/*

Copyright (c) 2003-2012, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/pch.hpp"

#include <vector>
#include <boost/limits.hpp>
#include <boost/bind.hpp>
#include <boost/cstdint.hpp>
#include <stdarg.h> // for va_start, va_end

#include "libtorrent/peer_connection.hpp"
#include "libtorrent/identify_client.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/version.hpp"
#include "libtorrent/extensions.hpp"
#include "libtorrent/aux_/session_interface.hpp"
#include "libtorrent/policy.hpp"
#include "libtorrent/socket_type.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/broadcast_socket.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/peer_info.hpp"
#include "libtorrent/bt_peer_connection.hpp"
#include "libtorrent/network_thread_pool.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/alloca.hpp"
#include "libtorrent/disk_interface.hpp"
#include "libtorrent/bandwidth_manager.hpp"
#include "libtorrent/request_blocks.hpp" // for request_a_block

#ifdef TORRENT_DEBUG
#include <set>
#endif

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
#include "libtorrent/escape_string.hpp"
#include "libtorrent/socket_io.hpp"
#endif

//#define TORRENT_CORRUPT_DATA

using boost::shared_ptr;

namespace libtorrent
{

	enum
	{
		// the limits of the download queue size
		min_request_queue = 2,
	};

	int round_up8(int v)
	{
		return ((v & 7) == 0) ? v : v + (8 - (v & 7));
	}

#if defined TORRENT_REQUEST_LOGGING
	void write_request_log(FILE* f, sha1_hash const& ih
		, peer_connection* p, peer_request const& r)
	{
		// the event format in the log is:
		// uint64_t timestamp (microseconds)
		// uint64_t info-hash prefix
		// uint32_t peer identifier
		// uint32_t piece
		// uint32_t start offset
		// uint32_t length
		char event[32];
		char* ptr = event;
		detail::write_uint64(total_microseconds((time_now_hires() - min_time())), ptr);
		memcpy(ptr, &ih[0], 8);
		ptr += 8;
		detail::write_uint32(uintptr_t(p), ptr);
		detail::write_uint32(r.piece, ptr);
		detail::write_uint32(r.start, ptr);
		detail::write_uint32(r.length, ptr);

		int ret = fwrite(event, 1, sizeof(event), f);
		if (ret != sizeof(event))
		{
			fprintf(stderr, "ERROR writing to request log: (%d) %s\n"
				, errno, strerror(errno));
		}
	}
#endif

	// outbound connection
	peer_connection::peer_connection(
		aux::session_interface& ses
		, aux::session_settings const& sett
		, buffer_allocator_interface& allocator
		, disk_interface& disk_thread
		, io_service& ios
		, boost::weak_ptr<torrent> tor
		, shared_ptr<socket_type> s
		, tcp::endpoint const& endp
		, torrent_peer* peerinfo
		, bool outgoing)
		:
#ifdef TORRENT_DEBUG
		m_last_choke(time_now() - hours(1))
		,
#endif
		  m_ses(ses)
		, m_disk_thread(disk_thread)
		, m_settings(sett)
		, m_allocator(allocator)
		, m_ios(ios)
		, m_max_out_request_queue(m_settings.get_int(settings_pack::max_out_request_queue))
		, m_work(m_ios)
		, m_last_piece(time_now())
		, m_last_request(time_now())
		, m_last_incoming_request(min_time())
		, m_last_unchoke(time_now())
		, m_last_unchoked(time_now())
		, m_last_receive(time_now())
		, m_last_sent(time_now())
		, m_requested(min_time())
		, m_remote_dl_update(time_now())
		, m_connect(time_now())
		, m_became_uninterested(time_now())
		, m_became_uninteresting(time_now())
		, m_downloaded_at_last_unchoke(0)
		, m_uploaded_at_last_unchoke(0)
		, m_disk_recv_buffer(allocator, 0)
		, m_socket(s)
		, m_remote(endp)
		, m_torrent(tor)
		, m_receiving_block(piece_block::invalid)
		, m_last_seen_complete(0)
		, m_timeout_extend(0)
		, m_outstanding_bytes(0)
		, m_extension_outstanding_bytes(0)
		, m_queued_time_critical(0)
		, m_num_pieces(0)
		, m_timeout(m_settings.get_int(settings_pack::peer_timeout))
		, m_packet_size(0)
		, m_soft_packet_size(0)
		, m_recv_pos(0)
		, m_recv_end(0)
		, m_recv_start(0)
		, m_disk_recv_buffer_size(0)
		, m_reading_bytes(0)
		, m_picker_options(0)
		, m_num_invalid_requests(0)
		, m_peer_info(peerinfo)
		, m_speed(slow)
		, m_connection_ticket(-1)
		, m_remote_bytes_dled(0)
		, m_remote_dl_rate(0)
		, m_outstanding_writing_bytes(0)
		, m_download_rate_peak(0)
		, m_upload_rate_peak(0)
		, m_rtt(0)
		, m_prefer_whole_pieces(0)
		, m_desired_queue_size(2)
		, m_choke_rejects(0)
		, m_disk_read_failures(0)
		, m_outstanding_piece_verification(0)
		, m_fast_reconnect(false)
		, m_outgoing(outgoing)
		, m_received_listen_port(false)
		, m_peer_interested(false)
		, m_peer_choked(true)
		, m_interesting(false)
		, m_choked(true)
		, m_failed(false)
		, m_have_all(false)
		, m_disconnecting(false)
		, m_connecting(outgoing)
		, m_queued(outgoing)
		, m_request_large_blocks(false)
		, m_share_mode(false)
		, m_upload_only(false)
		, m_snubbed(false)
		, m_bitfield_received(false)
		, m_no_download(false)
		, m_endgame_mode(false)
		, m_sent_suggests(false)
		, m_holepunch_mode(false)
		, m_ignore_stats(false)
		, m_corked(false)
		, m_need_interest_update(false)
		, m_has_metadata(true)
		, m_queued_for_connection(false)
		, m_exceeded_limit(false)
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		, m_in_constructor(true)
		, m_disconnect_started(false)
		, m_initialized(false)
		, m_in_use(1337)
		, m_received_in_piece(0)
		, m_destructed(false)
		, m_socket_is_writing(false)
#endif
	{
		m_ses.inc_stats_counter(aux::session_interface::num_tcp_peers + m_socket->type() - 1);

		m_superseed_piece[0] = -1;
		m_superseed_piece[1] = -1;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		// if t is NULL, we better not be connecting, since
		// we can't decrement the connecting counter
		TORRENT_ASSERT(t || !m_connecting);
		if (m_connecting && t) t->inc_num_connecting();
		m_est_reciprocation_rate = m_settings.get_int(settings_pack::default_est_reciprocation_rate);

#if TORRENT_USE_I2P
		if (peerinfo && peerinfo->is_i2p_addr)
		{
			// quadruple the timeout for i2p peers
			m_timeout *= 4;
		}
#endif

		m_channel_state[upload_channel] = peer_info::bw_idle;
		m_channel_state[download_channel] = peer_info::bw_idle;

		m_quota[0] = 0;
		m_quota[1] = 0;

		TORRENT_ASSERT(peerinfo == 0 || peerinfo->banned == false);
#ifndef TORRENT_DISABLE_RESOLVE_COUNTRIES
		std::fill(m_country, m_country + 2, 0);
#ifndef TORRENT_DISABLE_GEO_IP
		if (m_ses.has_country_db())
		{
			char const *country = m_ses.country_for_ip(m_remote.address());
			if (country != 0)
			{
				m_country[0] = country[0];
				m_country[1] = country[1];
			}
		}
#endif
#endif
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		error_code ec;
		TORRENT_ASSERT(m_socket->remote_endpoint(ec) == m_remote || ec);
		std::string log_name = m_remote.address().to_string(ec) + "_"
			+ to_string(m_remote.port()).elems;

		if (t) log_name = combine_path(to_hex(t->info_hash().to_string())
			, log_name);

		m_logger = m_ses.create_log(log_name, m_ses.listen_port());
		peer_log("%s [ ep: %s type: %s seed: %d p: %p local: %s]"
			, m_outgoing ? ">>> OUTGOING_CONNECTION" : "<<< INCOMING CONNECTION"
			, print_endpoint(m_remote).c_str()
			, m_socket->type_name()
			, m_peer_info ? m_peer_info->seed : 0, m_peer_info
			, print_endpoint(m_socket->local_endpoint(ec)).c_str());
#endif
#ifndef TORRENT_DISABLE_GEO_IP
		m_inet_as_name = m_ses.as_name_for_ip(m_remote.address());
#endif
#ifdef TORRENT_DEBUG
		piece_failed = false;
#endif
		std::fill(m_peer_id.begin(), m_peer_id.end(), 0);
	}

#ifdef TORRENT_BUFFER_STATS
	void peer_connection::log_buffer_usage(char* buffer, int size, char const* label)
	{
		if (m_disk_thread.is_disk_buffer(buffer))
			m_disk_thread.rename_buffer(buffer, label);
	
		m_ses.buffer_usage_logger() << log_time() << " append_send_buffer: " << size << std::endl;
		m_ses.log_buffer_usage();
	}
#endif

	void peer_connection::increase_est_reciprocation_rate()
	{
		m_est_reciprocation_rate += m_est_reciprocation_rate
			* m_settings.get_int(settings_pack::increase_est_reciprocation_rate) / 100;
	}

	void peer_connection::decrease_est_reciprocation_rate()
	{
		m_est_reciprocation_rate -= m_est_reciprocation_rate
			* m_settings.get_int(settings_pack::decrease_est_reciprocation_rate) / 100;
	}

	bool peer_connection::bittyrant_unchoke_compare(
		peer_connection const* p) const
	{
		TORRENT_ASSERT(p);
		peer_connection const& rhs = *p;

		size_type d1, d2, u1, u2;

		// first compare how many bytes they've sent us
		d1 = m_statistics.total_payload_download() - m_downloaded_at_last_unchoke;
		d2 = rhs.m_statistics.total_payload_download() - rhs.m_downloaded_at_last_unchoke;
		// divided by the number of bytes we've sent them
		u1 = m_statistics.total_payload_upload() - m_uploaded_at_last_unchoke;
		u2 = rhs.m_statistics.total_payload_upload() - rhs.m_uploaded_at_last_unchoke;

		// take torrent priority into account
		d1 *= get_priority(upload_channel);
		d2 *= rhs.get_priority(upload_channel);

		d1 = d1 * 1000 / (std::max)(size_type(1), u1);
		d2 = d2 * 1000 / (std::max)(size_type(1), u2);
		if (d1 > d2) return true;
		if (d1 < d2) return false;

		// if both peers are still in their send quota or not in their send quota
		// prioritize the one that has waited the longest to be unchoked
		return m_last_unchoke < rhs.m_last_unchoke;
	}

	bool peer_connection::unchoke_compare(peer_connection const* p) const
	{
		TORRENT_ASSERT(p);
		peer_connection const& rhs = *p;

		// if one peer belongs to a higher priority torrent than the other one
		// that one should be unchoked.
		boost::shared_ptr<torrent> t1 = m_torrent.lock();
		TORRENT_ASSERT(t1);
		boost::shared_ptr<torrent> t2 = rhs.associated_torrent().lock();
		TORRENT_ASSERT(t2);

		int prio1 = get_priority(upload_channel);
		int prio2 = rhs.get_priority(upload_channel);

		if (prio1 != prio2)
			return prio1 > prio2;

		// compare how many bytes they've sent us
		size_type c1;
		size_type c2;
		c1 = m_statistics.total_payload_download() - m_downloaded_at_last_unchoke;
		c2 = rhs.m_statistics.total_payload_download() - rhs.m_downloaded_at_last_unchoke;

		if (c1 != c2) return c1 > c2;

		if (m_settings.get_int(settings_pack::seed_choking_algorithm)
			== settings_pack::round_robin)
		{
			// if they are equal, compare how much we have uploaded
			c1 = m_statistics.total_payload_upload() - m_uploaded_at_last_unchoke;
			c2 = rhs.m_statistics.total_payload_upload() - rhs.m_uploaded_at_last_unchoke;

			// in order to not switch back and forth too often,
			// unchoked peers must be at least one piece ahead
			// of a choked peer to be sorted at a lower unchoke-priority
			int pieces = m_settings.get_int(settings_pack::seeding_piece_quota);
			bool c1_done = is_choked() || c1 > (std::max)(t1->torrent_file().piece_length() * pieces, 256 * 1024);
			bool c2_done = rhs.is_choked() || c2 > (std::max)(t2->torrent_file().piece_length() * pieces, 256 * 1024);

			if (!c1_done && c2_done) return true;
			if (c1_done && !c2_done) return false;
		}	
		else if (m_settings.get_int(settings_pack::seed_choking_algorithm)
			== settings_pack::fastest_upload)
		{
			c1 = m_statistics.total_payload_upload() - m_uploaded_at_last_unchoke;
			c2 = rhs.m_statistics.total_payload_upload() - rhs.m_uploaded_at_last_unchoke;
		
			// take torrent priority into account
			c1 *= prio1;
			c2 *= prio2;

			if (c1 > c2) return true;
			if (c2 > c1) return false;
		}
		else if (m_settings.get_int(settings_pack::seed_choking_algorithm)
			== settings_pack::anti_leech)
		{
			// the anti-leech seeding algorithm ranks peers based on how many pieces
			// they have, prefering to unchoke peers that just started and peers that
			// are close to completing. Like this:
			//   ^
			//   | \                       / |
			//   |  \                     /  |
			//   |   \                   /   |
			// s |    \                 /    |
			// c |     \               /     |
			// o |      \             /      |
			// r |       \           /       |
			// e |        \         /        |
			//   |         \       /         |
			//   |          \     /          |
			//   |           \   /           |
			//   |            \ /            |
			//   |             V             |
			//   +---------------------------+
			//   0%    num have pieces     100%
			int t1_total = t1->torrent_file().num_pieces();
			int t2_total = t2->torrent_file().num_pieces();
			int score1 = (num_have_pieces() < t1_total / 2
				? t1_total - num_have_pieces() : num_have_pieces()) * 1000 / t1_total;
			int score2 = (rhs.num_have_pieces() < t2_total / 2
				? t2_total - rhs.num_have_pieces() : rhs.num_have_pieces()) * 1000 / t2_total;
			if (score1 > score2) return true;
			if (score2 > score1) return false;
		}
		
		// if both peers have are still in their send quota or not in their send quota
		// prioritize the one that has waited the longest to be unchoked
		return m_last_unchoke < rhs.m_last_unchoke;
	}

	int peer_connection::get_priority(int channel) const
	{
		TORRENT_ASSERT(channel >= 0 && channel < 2);
		int prio = 1;
		for (int i = 0; i < num_classes(); ++i)
		{
			int class_prio = m_ses.peer_classes().at(class_at(i))->priority[channel];
			if (prio < class_prio) prio = class_prio;
		}

		boost::shared_ptr<torrent> t = associated_torrent().lock();

		if (t)
		{
			for (int i = 0; i < t->num_classes(); ++i)
			{
				int class_prio = m_ses.peer_classes().at(t->class_at(i))->priority[channel];
				if (prio < class_prio) prio = class_prio;
			}
		}
		return prio;
	}

	bool peer_connection::upload_rate_compare(peer_connection const* p) const
	{
		size_type c1;
		size_type c2;

		c1 = m_statistics.total_payload_upload() - m_uploaded_at_last_unchoke;
		c2 = p->m_statistics.total_payload_upload() - p->m_uploaded_at_last_unchoke;
		
		// take torrent priority into account
		c1 *= get_priority(upload_channel);
		c2 *= p->get_priority(upload_channel);

		return c1 > c2;
	}

	void peer_connection::reset_choke_counters()
	{
		m_downloaded_at_last_unchoke = m_statistics.total_payload_download();
		m_uploaded_at_last_unchoke = m_statistics.total_payload_upload();
	}

	void peer_connection::start()
	{
		TORRENT_ASSERT(m_peer_info == 0 || m_peer_info->connection == this);
		boost::shared_ptr<torrent> t = m_torrent.lock();

		if (!m_outgoing)
		{
			tcp::socket::non_blocking_io ioc(true);
			error_code ec;
			m_socket->io_control(ioc, ec);
			if (ec)
			{
				disconnect(ec);
				return;
			}
			m_remote = m_socket->remote_endpoint(ec);
			if (ec)
			{
				disconnect(ec);
				return;
			}
			m_local = m_socket->local_endpoint(ec);
			if (ec)
			{
				disconnect(ec);
				return;
			}
			TORRENT_ASSERT(m_remote.address() != address_v4::any());
			if (m_remote.address().is_v4())
			{
				m_socket->set_option(type_of_service(m_settings.get_int(settings_pack::peer_tos)), ec);
#if defined TORRENT_VERBOSE_LOGGING
				peer_log(">>> SET_TOS[ tos: %d e: %s ]", m_settings.get_int(settings_pack::peer_tos), ec.message().c_str());
#endif
			}
		}

#if defined TORRENT_VERBOSE_LOGGING
		peer_log("*** SET_PEER_CLASS [ a: %s ]", print_address(m_remote.address()).c_str());
#endif

		m_ses.set_peer_classes(this, m_remote.address(), m_socket->type());

#if defined TORRENT_VERBOSE_LOGGING
		for (int i = 0; i < num_classes(); ++i)
		{
			peer_log("*** CLASS [ %s ]", m_ses.peer_classes().at(class_at(i))->label.c_str());
		}
#endif
		if (t && t->ready_for_connections())
		{
			init();
		}
	}

	void peer_connection::update_interest()
	{
		if (!m_need_interest_update)
		{
			// we're the first to request an interest update
			// post a message in order to delay it enough for
			// any potential other messages already in the queue
			// to not trigger another one. This effectively defer
			// the update until the current message queue is
			// flushed
			m_ios.post(boost::bind(&peer_connection::do_update_interest, self()));
		}
		m_need_interest_update = true;
	}

	void peer_connection::do_update_interest()
	{
		TORRENT_ASSERT(m_need_interest_update);
		m_need_interest_update = false;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;

		// if m_have_piece is 0, it means the connections
		// have not been initialized yet. The interested
		// flag will be updated once they are.
		if (m_have_piece.size() == 0) return;
		if (!t->ready_for_connections()) return;

		bool interested = false;
		if (!t->is_upload_only())
		{
			t->need_picker();
			piece_picker const& p = t->picker();
			int num_pieces = p.num_pieces();
			for (int j = 0; j != num_pieces; ++j)
			{
				if (m_have_piece[j]
					&& t->piece_priority(j) > 0
					&& !p.has_piece_passed(j))
				{
					interested = true;
					break;
				}
			}
		}
		if (!interested) send_not_interested();
		else t->peer_is_interesting(*this);

		TORRENT_ASSERT(in_handshake() || is_interesting() == interested);
	}

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
	void peer_connection::peer_log(char const* fmt, ...) const
	{
		if (!m_logger) return;

		va_list v;	
		va_start(v, fmt);
	
		char usr[400];
		vsnprintf(usr, sizeof(usr), fmt, v);
		va_end(v);
		char buf[450];
		snprintf(buf, sizeof(buf), "%s: %s\n", time_now_string(), usr);
		(*m_logger) << buf;
	}
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
	void peer_connection::add_extension(boost::shared_ptr<peer_plugin> ext)
	{
		m_extensions.push_back(ext);
	}

	peer_plugin const* peer_connection::find_plugin(char const* type)
	{
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if (strcmp((*i)->type(), type) == 0) return (*i).get();
		}
		return 0;
	}
#endif

	void peer_connection::send_allowed_set()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (t->super_seeding())
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** SKIPPING ALLOWED SET BECAUSE OF SUPER SEEDING");
#endif
			return;
		}

		if (upload_only())
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** SKIPPING ALLOWED SET BECAUSE PEER IS UPLOAD ONLY");
#endif
			return;
		}

		int num_allowed_pieces = m_settings.get_int(settings_pack::allowed_fast_set_size);
		if (num_allowed_pieces == 0) return;

		int num_pieces = t->torrent_file().num_pieces();

		if (num_allowed_pieces >= num_pieces)
		{
			// this is a special case where we have more allowed
			// fast pieces than pieces in the torrent. Just send
			// an allowed fast message for every single piece
			for (int i = 0; i < num_pieces; ++i)
			{
				// there's no point in offering fast pieces
				// that the peer already has
				if (has_piece(i)) continue;

#ifdef TORRENT_VERBOSE_LOGGING
				peer_log("==> ALLOWED_FAST [ %d ]", i);
#endif
				write_allow_fast(i);
				TORRENT_ASSERT(std::find(m_accept_fast.begin()
					, m_accept_fast.end(), i)
					== m_accept_fast.end());
				if (m_accept_fast.empty()) m_accept_fast.reserve(10);
				m_accept_fast.push_back(i);
			}
			return;
		}

		std::string x;
		address const& addr = m_remote.address();
		if (addr.is_v4())
		{
			address_v4::bytes_type bytes = addr.to_v4().to_bytes();
			x.assign((char*)&bytes[0], bytes.size());
		}
#if TORRENT_USE_IPV6
		else
		{
			address_v6::bytes_type bytes = addr.to_v6().to_bytes();
			x.assign((char*)&bytes[0], bytes.size());
		}
#endif
		x.append((char*)&t->torrent_file().info_hash()[0], 20);

		sha1_hash hash = hasher(x.c_str(), x.size()).final();
		for (;;)
		{
			char* p = (char*)&hash[0];
			for (int i = 0; i < 5; ++i)
			{
				int piece = detail::read_uint32(p) % num_pieces;
				if (std::find(m_accept_fast.begin(), m_accept_fast.end(), piece)
					== m_accept_fast.end())
				{
#ifdef TORRENT_VERBOSE_LOGGING
					peer_log("==> ALLOWED_FAST [ %d ]", piece);
#endif
					write_allow_fast(piece);
					if (m_accept_fast.empty()) m_accept_fast.reserve(10);
					m_accept_fast.push_back(piece);
					if (int(m_accept_fast.size()) >= num_allowed_pieces
						|| int(m_accept_fast.size()) == num_pieces) return;
				}
			}
			hash = hasher((char*)&hash[0], 20).final();
		}
	}

	void peer_connection::on_metadata_impl()
	{
		boost::shared_ptr<torrent> t = associated_torrent().lock();
		m_have_piece.resize(t->torrent_file().num_pieces(), m_have_all);
		m_num_pieces = m_have_piece.count();

		// now that we know how many pieces there are
		// remove any invalid allowed_fast and suggest pieces
		// now that we know what the number of pieces are
		for (std::vector<int>::iterator i = m_allowed_fast.begin();
			i != m_allowed_fast.end();)
		{
			if (*i < m_num_pieces)
			{
				++i;
				continue;
			}
			i = m_allowed_fast.erase(i);
		}

		for (std::vector<int>::iterator i = m_suggested_pieces.begin();
			i != m_suggested_pieces.end();)
		{
			if (*i < m_num_pieces)
			{
				++i;
				continue;
			}
			i = m_suggested_pieces.erase(i);
		}
		on_metadata();
		if (m_disconnecting) return;
	}

	void peer_connection::init()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		TORRENT_ASSERT(t->valid_metadata());
		TORRENT_ASSERT(t->ready_for_connections());

		m_have_piece.resize(t->torrent_file().num_pieces(), m_have_all);

		if (m_have_all) m_num_pieces = t->torrent_file().num_pieces();
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(!m_initialized);
		m_initialized = true;
#endif
		// now that we have a piece_picker,
		// update it with this peer's pieces

		TORRENT_ASSERT(m_num_pieces == m_have_piece.count());

		if (m_num_pieces == int(m_have_piece.size()))
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** THIS IS A SEED [ p: %p ]", m_peer_info);
#endif
			// if this is a web seed. we don't have a peer_info struct
			t->set_seed(m_peer_info, true);
			m_upload_only = true;

			t->peer_has_all(this);
			if (t->is_upload_only()) send_not_interested();
			else t->peer_is_interesting(*this);
			return;
		}

		// if we're a seed, we don't keep track of piece availability
		if (t->has_picker())
		{
			t->peer_has(m_have_piece, this);
			bool interesting = false;
			for (int i = 0; i < int(m_have_piece.size()); ++i)
			{
				if (m_have_piece[i])
				{
					// if the peer has a piece and we don't, the peer is interesting
					if (!t->have_piece(i)
						&& t->picker().piece_priority(i) != 0)
						interesting = true;
				}
			}
			if (interesting) t->peer_is_interesting(*this);
			else send_not_interested();
		}
		else
		{
			update_interest();
		}
	}

	peer_connection::~peer_connection()
	{
		m_ses.inc_stats_counter(aux::session_interface::num_tcp_peers + m_socket->type() - 1, -1);

		TORRENT_ASSERT(!m_queued_for_connection);
//		INVARIANT_CHECK;
		TORRENT_ASSERT(!m_in_constructor);
		TORRENT_ASSERT(m_disconnecting);
		TORRENT_ASSERT(m_disconnect_started);
		TORRENT_ASSERT(!m_destructed);
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		m_destructed = true;
#endif
		TORRENT_ASSERT(m_ses.is_single_thread());

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		m_in_use = 0;
#endif

		// defensive
		boost::shared_ptr<torrent> t = m_torrent.lock();
		// if t is NULL, we better not be connecting, since
		// we can't decrement the connecting counter
		TORRENT_ASSERT(t || !m_connecting);

		// we should really have dealt with this already
		TORRENT_ASSERT(!m_connecting);
		if (m_connecting && t)
		{
			t->dec_num_connecting();
			m_connecting = false;
		}

		m_disk_recv_buffer_size = 0;

#ifndef TORRENT_DISABLE_EXTENSIONS
		m_extensions.clear();
#endif

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		peer_log("*** CONNECTION CLOSED");
#endif
		TORRENT_ASSERT(m_request_queue.empty());
		TORRENT_ASSERT(m_download_queue.empty());

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		if (m_peer_info)
			TORRENT_ASSERT(m_peer_info->connection == 0);
#endif
	}

	bool peer_connection::on_parole() const
	{ return peer_info_struct() && peer_info_struct()->on_parole; }

	int peer_connection::picker_options() const
	{
		int ret = m_picker_options; 

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		if (!t) return 0;

		if (t->is_sequential_download())
		{
			ret |= piece_picker::sequential | piece_picker::ignore_whole_pieces;
		}
		else if (t->num_have() < m_settings.get_int(settings_pack::initial_picker_threshold))
		{
			// if we have fewer pieces than a certain threshols
			// don't pick rare pieces, just pick random ones,
			// and prioritize finishing them
			ret |= piece_picker::prioritize_partials;
		}
		else
		{
			ret |= piece_picker::rarest_first | piece_picker::speed_affinity;
		}

		if (m_snubbed)
		{
			// snubbed peers should request
			// the common pieces first, just to make
			// it more likely for all snubbed peers to
			// request blocks from the same piece
			ret |= piece_picker::reverse;
		}

		if (m_settings.get_bool(settings_pack::prioritize_partial_pieces))
			ret |= piece_picker::prioritize_partials;

		if (on_parole()) ret |= piece_picker::on_parole
			| piece_picker::prioritize_partials;

		// only one of rarest_first, common_first and sequential can be set.
		TORRENT_ASSERT((ret & piece_picker::rarest_first) ? 1 : 0
			+ (ret & piece_picker::sequential) ? 1 : 0 <= 1);
		return ret;
	}

	void peer_connection::fast_reconnect(bool r)
	{
		if (!peer_info_struct() || peer_info_struct()->fast_reconnects > 1)
			return;
		m_fast_reconnect = r;
		peer_info_struct()->last_connected = m_ses.session_time();
		int rewind = m_settings.get_int(settings_pack::min_reconnect_time)
			* m_settings.get_int(settings_pack::max_failcount);
		if (peer_info_struct()->last_connected < rewind) peer_info_struct()->last_connected = 0;
		else peer_info_struct()->last_connected -= rewind;

		if (peer_info_struct()->fast_reconnects < 15)
			++peer_info_struct()->fast_reconnects;
	}

	void peer_connection::received_piece(int index)
	{
		// dont announce during handshake
		if (in_handshake()) return;

#if defined TORRENT_VERBOSE_LOGGING
		peer_log("<<< RECEIVED [ piece: %d ]", index);
#endif

		// remove suggested pieces once we have them
		std::vector<int>::iterator i = std::find(
			m_suggested_pieces.begin(), m_suggested_pieces.end(), index);
		if (i != m_suggested_pieces.end()) m_suggested_pieces.erase(i);

		// remove allowed fast pieces
		i = std::find(m_allowed_fast.begin(), m_allowed_fast.end(), index);
		if (i != m_allowed_fast.end()) m_allowed_fast.erase(i);

		if (has_piece(index))
		{
			// if we got a piece that this peer has
			// it might have been the last interesting
			// piece this peer had. We might not be
			// interested anymore
			update_interest();
			if (is_disconnecting()) return;
		}

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
#endif
	}

	void peer_connection::announce_piece(int index)
	{
		// dont announce during handshake
		if (in_handshake()) return;

		if (has_piece(index))
		{
			// optimization, don't send have messages
			// to peers that already have the piece
			if (!m_settings.get_bool(settings_pack::send_redundant_have))
			{
#ifdef TORRENT_VERBOSE_LOGGING
				peer_log("==> HAVE    [ piece: %d ] SUPRESSED", index);
#endif
				return;
			}
		}

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("==> HAVE    [ piece: %d ]", index);
#endif
		write_have(index);
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
#endif
	}

	bool peer_connection::has_piece(int i) const
	{
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		TORRENT_ASSERT(t->valid_metadata());
		TORRENT_ASSERT(i >= 0);
		TORRENT_ASSERT(i < t->torrent_file().num_pieces());
		return m_have_piece[i];
	}

	std::vector<pending_block> const& peer_connection::request_queue() const
	{
		return m_request_queue;
	}
	
	std::vector<pending_block> const& peer_connection::download_queue() const
	{
		return m_download_queue;
	}
	
	std::vector<peer_request> const& peer_connection::upload_queue() const
	{
		return m_requests;
	}

	time_duration peer_connection::download_queue_time(int extra_bytes) const
	{
		int rate = m_statistics.transfer_rate(stat::download_payload)
			+ m_statistics.transfer_rate(stat::download_protocol);
		// avoid division by zero
		if (rate < 50) rate = 50;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		return seconds((m_outstanding_bytes + m_queued_time_critical * t->block_size()) / rate);
	}

	void peer_connection::add_stat(size_type downloaded, size_type uploaded)
	{
		m_statistics.add_stat(downloaded, uploaded);
	}

	void peer_connection::received_bytes(int bytes_payload, int bytes_protocol)
	{
		m_statistics.received_bytes(bytes_payload, bytes_protocol);
		if (m_ignore_stats) return;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->received_bytes(bytes_payload, bytes_protocol);
	}

	void peer_connection::sent_bytes(int bytes_payload, int bytes_protocol)
	{
		m_statistics.sent_bytes(bytes_payload, bytes_protocol);
		if (m_ignore_stats) return;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->sent_bytes(bytes_payload, bytes_protocol);
	}

	void peer_connection::trancieve_ip_packet(int bytes, bool ipv6)
	{
		m_statistics.trancieve_ip_packet(bytes, ipv6);
		if (m_ignore_stats) return;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->trancieve_ip_packet(bytes, ipv6);
	}

	void peer_connection::sent_syn(bool ipv6)
	{
		m_statistics.sent_syn(ipv6);
		if (m_ignore_stats) return;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->sent_syn(ipv6);
	}

	void peer_connection::received_synack(bool ipv6)
	{
		m_statistics.received_synack(ipv6);
		if (m_ignore_stats) return;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;
		t->received_synack(ipv6);
	}

	bitfield const& peer_connection::get_bitfield() const
	{
		return m_have_piece;
	}

	void peer_connection::received_valid_data(int index)
	{
		// this fails because we haven't had time to disconnect
		// seeds yet, and we might have just become one
//		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			TORRENT_TRY {
				(*i)->on_piece_pass(index);
			} TORRENT_CATCH(std::exception&) {}
		}
#endif
	}

	bool peer_connection::received_invalid_data(int index, bool single_peer)
	{
		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			TORRENT_TRY {
				(*i)->on_piece_failed(index);
			} TORRENT_CATCH(std::exception&) {}
		}
#endif
		return true;
	}

	// verifies a piece to see if it is valid (is within a valid range)
	// and if it can correspond to a request generated by libtorrent.
	bool peer_connection::verify_piece(const peer_request& p) const
	{
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		TORRENT_ASSERT(t->valid_metadata());
		torrent_info const& ti = t->torrent_file();

		return p.piece >= 0
			&& p.piece < ti.num_pieces()
			&& p.start >= 0
			&& p.start < ti.piece_length()
			&& t->to_req(piece_block(p.piece, p.start / t->block_size())) == p;
	}

	void peer_connection::attach_to_torrent(sha1_hash const& ih, bool allow_encrypted)
	{
		INVARIANT_CHECK;

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_connect_time = time_now_hires();
#endif

		TORRENT_ASSERT(!m_disconnecting);
		TORRENT_ASSERT(m_torrent.expired());
		boost::weak_ptr<torrent> wpt = m_ses.find_torrent(ih);
		boost::shared_ptr<torrent> t = wpt.lock();

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		// now that we know which torrent this peer belongs
		// to. Move the log file into its directory

		error_code ec;
		std::string log_name = combine_path(to_hex(ih.to_string())
			, m_remote.address().to_string(ec) + "_"
				+ to_string(m_remote.port()).elems);
		m_logger->move_log_file(m_ses.get_log_path(), log_name, m_ses.listen_port());
#endif

		if (t && t->is_aborted())
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** the torrent has been aborted");
#endif
			t.reset();
		}

		if (!t)
		{
			t = m_ses.delay_load_torrent(ih, this);
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			if (t)
				peer_log("*** Delay loaded torrent: %s:", to_hex(ih.to_string()).c_str());
#endif
		}

		if (!t)
		{
			// we couldn't find the torrent!
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** couldn't find a torrent with the given info_hash: %s torrents:", to_hex(ih.to_string()).c_str());
			m_ses.log_all_torrents(this);
#endif
			disconnect(errors::invalid_info_hash, 1);
			return;
		}

		if (t->is_paused() && (!t->is_auto_managed()
			|| !m_settings.get_bool(settings_pack::incoming_starts_queued_torrents)))
		{
			// paused torrents will not accept
			// incoming connections unless they are auto managed
			// and inconing_starts_queued_torrents is true
			// torrents that have errors should always reject
			// incoming peers
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("rejected connection to paused torrent");
#endif
			disconnect(errors::torrent_paused, 2);
			return;
		}

#if TORRENT_USE_I2P
		i2p_stream* i2ps = m_socket->get<i2p_stream>();
		if (!i2ps && t->torrent_file().is_i2p()
			&& !m_settings.get_bool(settings_pack::allow_i2p_mixed))
		{
			// the torrent is an i2p torrent, the peer is a regular peer
			// and we don't allow mixed mode. Disconnect the peer.
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("rejected regular connection to i2p torrent");
#endif
			disconnect(errors::peer_banned, 2);
			return;
		}
#endif // TORRENT_USE_I2P

		TORRENT_ASSERT(m_torrent.expired());

		if (t->is_paused()
			&& m_settings.get_bool(settings_pack::incoming_starts_queued_torrents)
			&& !m_ses.is_paused()
			&& !t->is_aborted()
			&& !m_ses.is_aborted())
		{
			t->resume();
		}

		// check to make sure we don't have another connection with the same
		// info_hash and peer_id. If we do. close this connection.
		t->attach_peer(this);
		if (m_disconnecting) return;
		m_torrent = wpt;

		if (m_exceeded_limit)
		{
			// find a peer in some torrent (presumably the one with most peers)
			// and disconnect the lowest ranking peer
			boost::weak_ptr<torrent> torr = m_ses.find_disconnect_candidate_torrent();
			boost::shared_ptr<torrent> other_t = torr.lock();

			if (other_t)
			{
				if (other_t->num_peers() <= t->num_peers())
				{
					disconnect(errors::too_many_connections);
					return;
				}
				// find the lowest ranking peer and disconnect that
				peer_connection* p = other_t->find_lowest_ranking_peer();
				p->disconnect(errors::too_many_connections);
				peer_disconnected_other();
			}
			else
			{
				disconnect(errors::too_many_connections);
				return;
			}
		}

		TORRENT_ASSERT(!m_torrent.expired());

		// if the torrent isn't ready to accept
		// connections yet, we'll have to wait with
		// our initialization
		if (t->ready_for_connections()) init();

		TORRENT_ASSERT(!m_torrent.expired());

		// assume the other end has no pieces
		// if we don't have valid metadata yet,
		// leave the vector unallocated
		TORRENT_ASSERT(m_num_pieces == 0);
		m_have_piece.clear_all();
		TORRENT_ASSERT(!m_torrent.expired());
	}

	boost::uint32_t peer_connection::peer_rank() const
	{
		return m_peer_info->rank(m_ses.external_address()
			, m_ses.listen_port());
	}

	// message handlers

	// -----------------------------
	// --------- KEEPALIVE ---------
	// -----------------------------

	void peer_connection::incoming_keepalive()
	{
		INVARIANT_CHECK;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== KEEPALIVE");
#endif
	}

	// -----------------------------
	// ----------- CHOKE -----------
	// -----------------------------

	void peer_connection::incoming_choke()
	{
		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_choke()) return;
		}
#endif
		if (is_disconnecting()) return;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== CHOKE");
#endif
		m_peer_choked = true;
		set_endgame(false);

		clear_request_queue();
	}

	void peer_connection::clear_request_queue()
	{
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		if (!t->has_picker())
		{
			m_request_queue.clear();
			return;
		}

		// clear the requests that haven't been sent yet
		if (peer_info_struct() == 0 || !peer_info_struct()->on_parole)
		{
			// if the peer is not in parole mode, clear the queued
			// up block requests
			piece_picker& p = t->picker();
			for (std::vector<pending_block>::const_iterator i = m_request_queue.begin()
				, end(m_request_queue.end()); i != end; ++i)
			{
				p.abort_download(i->block, peer_info_struct());
			}
			m_request_queue.clear();
			m_queued_time_critical = 0;
		}
	}

	bool match_request(peer_request const& r, piece_block const& b, int block_size)
	{
		if (int(b.piece_index) != r.piece) return false;
		if (int(b.block_index) != r.start / block_size) return false;
		if (r.start % block_size != 0) return false;
		return true;
	}

	// -----------------------------
	// -------- REJECT PIECE -------
	// -----------------------------

	void peer_connection::incoming_reject_request(peer_request const& r)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== REJECT_PIECE [ piece: %d | s: %d | l: %d ]"
			, r.piece, r.start, r.length);
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_reject(r)) return;
		}
#endif

		if (is_disconnecting()) return;

		std::vector<pending_block>::iterator i = std::find_if(
			m_download_queue.begin(), m_download_queue.end()
			, boost::bind(match_request, boost::cref(r), boost::bind(&pending_block::block, _1)
			, t->block_size()));
	
		if (i != m_download_queue.end())
		{
			pending_block b = *i;
			bool remove_from_picker = !i->timed_out && !i->not_wanted;
			m_download_queue.erase(i);
			TORRENT_ASSERT(m_outstanding_bytes >= r.length);
			m_outstanding_bytes -= r.length;
			if (m_outstanding_bytes < 0) m_outstanding_bytes = 0;
			
			// if the peer is in parole mode, keep the request
			if (peer_info_struct() && peer_info_struct()->on_parole)
			{
				// we should only add it if the block is marked as
				// busy in the piece-picker
				if (remove_from_picker)
					m_request_queue.insert(m_request_queue.begin(), b);
			}
			else if (!t->is_seed() && remove_from_picker)
			{
				piece_picker& p = t->picker();
				p.abort_download(b.block, peer_info_struct());
			}
#if !defined TORRENT_DISABLE_INVARIANT_CHECKS && defined TORRENT_DEBUG
			check_invariant();
#endif
		}
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		else
		{
			peer_log("*** PIECE NOT IN REQUEST QUEUE");
		}
#endif
		if (has_peer_choked())
		{
			// if we're choked and we got a rejection of
			// a piece in the allowed fast set, remove it
			// from the allow fast set.
			std::vector<int>::iterator i = std::find(
				m_allowed_fast.begin(), m_allowed_fast.end(), r.piece);
			if (i != m_allowed_fast.end()) m_allowed_fast.erase(i);
		}
		else
		{
			std::vector<int>::iterator i = std::find(m_suggested_pieces.begin()
				, m_suggested_pieces.end(), r.piece);
			if (i != m_suggested_pieces.end())
				m_suggested_pieces.erase(i);
		}

		if (m_request_queue.empty() && m_download_queue.size() < 2)
		{
			m_ses.inc_stats_counter(aux::session_interface::reject_piece_picks);
			request_a_block(*t, *this);
			send_block_requests();
		}
	}
	
	// -----------------------------
	// ------- SUGGEST PIECE -------
	// -----------------------------

	void peer_connection::incoming_suggest(int index)
	{
		INVARIANT_CHECK;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== SUGGEST_PIECE [ piece: %d ]", index);
#endif
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_suggest(index)) return;
		}
#endif

		if (is_disconnecting()) return;
		if (index < 0)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("<== INVALID_SUGGEST_PIECE [ %d ]", index);
#endif
			return;
		}
		
		if (t->valid_metadata())
		{
			if (index >= int(m_have_piece.size()))
			{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
				peer_log("<== INVALID_ALLOWED_FAST [ %d | s: %d ]"
					, index, int(m_have_piece.size()));
#endif
				return;
			}

			// if we already have the piece, we can
			// ignore this message
			if (t->have_piece(index))
				return;
		}

		if (int(m_suggested_pieces.size()) > m_settings.get_int(settings_pack::max_suggest_pieces))
			m_suggested_pieces.erase(m_suggested_pieces.begin());

		m_suggested_pieces.push_back(index);

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("** SUGGEST_PIECE [ piece: %d added to set: %d ]", index, int(m_suggested_pieces.size()));
#endif
	}

	// -----------------------------
	// ---------- UNCHOKE ----------
	// -----------------------------

	void peer_connection::incoming_unchoke()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_unchoke_time = time_now_hires();
		t->debug_log("UNCHOKE [%p] (%d ms)", this, int(total_milliseconds(m_unchoke_time - m_bitfield_time)));
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_unchoke()) return;
		}
#endif

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== UNCHOKE");
#endif
		m_peer_choked = false;
		m_last_unchoked = time_now();
		if (is_disconnecting()) return;

		if (is_interesting())
		{
			m_ses.inc_stats_counter(aux::session_interface::unchoke_piece_picks);
			request_a_block(*t, *this);
			send_block_requests();
		}
	}

	// -----------------------------
	// -------- INTERESTED ---------
	// -----------------------------

	void peer_connection::incoming_interested()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_interested()) return;
		}
#endif

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== INTERESTED");
#endif
		m_peer_interested = true;
		if (is_disconnecting()) return;
	
		// if the peer is ready to download stuff, it must have metadata		
		m_has_metadata = true;

		disconnect_if_redundant();
		if (is_disconnecting()) return;

		if (is_choked())
		{
			if (ignore_unchoke_slots())
			{
#ifdef TORRENT_VERBOSE_LOGGING
				peer_log("ABOUT TO UNCHOKE [ peer ignores unchoke slots ]");
#endif
				// if this peer is expempted from the choker
				// just unchoke it immediately
				send_unchoke();
			}
			else if (m_ses.num_uploads() < m_settings.get_int(settings_pack::unchoke_slots_limit)
				|| m_settings.get_int(settings_pack::unchoke_slots_limit) < 0)
			{
				// if the peer is choked and we have upload slots left,
				// then unchoke it. Another condition that has to be met
				// is that the torrent doesn't keep track of the individual
				// up/down ratio for each peer (ratio == 0) or (if it does
				// keep track) this particular connection isn't a leecher.
				// If the peer was choked because it was leeching, don't
				// unchoke it again.
				// The exception to this last condition is if we're a seed.
				// In that case we don't care if people are leeching, they
				// can't pay for their downloads anyway.
				m_ses.unchoke_peer(*this);
			}
#if defined TORRENT_VERBOSE_LOGGING
			else
			{
				peer_log("DID NOT UNCHOKE [ the number of uploads (%d) "
					"is more than or equal to the limit (%d) ]"
					, m_ses.num_uploads(), m_settings.get_int(settings_pack::unchoke_slots_limit));
			}
#endif
		}
	}

	// -----------------------------
	// ------ NOT INTERESTED -------
	// -----------------------------

	void peer_connection::incoming_not_interested()
	{
		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_not_interested()) return;
		}
#endif

		m_became_uninterested = time_now();

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== NOT_INTERESTED");
#endif
		m_peer_interested = false;
		if (is_disconnecting()) return;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (!is_choked())
		{
			if (ignore_unchoke_slots())
			{
				send_choke();
			}
			else
			{
				if (m_peer_info && m_peer_info->optimistically_unchoked)
				{
					m_peer_info->optimistically_unchoked = false;
					m_ses.trigger_optimistic_unchoke();
				}
				m_ses.choke_peer(*this);
				m_ses.trigger_unchoke();
			}
		}

		if (t->super_seeding())
		{
			// maybe we need to try another piece, to see if the peer
			// is interested in us then
			superseed_piece(-1, t->get_piece_to_super_seed(m_have_piece));
		}
	}

	// -----------------------------
	// ----------- HAVE ------------
	// -----------------------------

	void peer_connection::incoming_have(int index)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_have(index)) return;
		}
#endif

		if (is_disconnecting()) return;

		// if we haven't received a bitfield, it was
		// probably omitted, which is the same as 'have_none'
		if (!m_bitfield_received) incoming_have_none();

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== HAVE    [ piece: %d ]", index);
#endif

		if (is_disconnecting()) return;

		if (!t->valid_metadata() && index >= int(m_have_piece.size()))
		{
			if (index < 131072)
			{
				// if we don't have metadata
				// and we might not have received a bitfield
				// extend the bitmask to fit the new
				// have message
				m_have_piece.resize(index + 1, false);
			}
			else
			{
				// unless the index > 64k, in which case
				// we just ignore it
				return;
			}
		}

		// if we got an invalid message, abort
		if (index >= int(m_have_piece.size()) || index < 0)
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** ERROR: [ have-metadata have_piece.size: %d ]", index, int(m_have_piece.size()));
#endif
			disconnect(errors::invalid_have, 2);
			return;
		}

		if (t->super_seeding() && !m_settings.get_bool(settings_pack::strict_super_seeding))
		{
			// if we're superseeding and the peer just told
			// us that it completed the piece we're superseeding
			// to it, change the superseeding piece for this peer
			// if the peer optimizes out redundant have messages
			// this will be handled when the peer sends not-interested
			// instead.
			if (super_seeded_piece(index))
			{
				superseed_piece(index, t->get_piece_to_super_seed(m_have_piece));
			}
		}

		if (m_have_piece[index])
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("   got redundant HAVE message for index: %d", index);
#endif
			return;
		}

		m_have_piece.set_bit(index);
		++m_num_pieces;

		// if the peer is downloading stuff, it must have metadata		
		m_has_metadata = true;

		// only update the piece_picker if
		// we have the metadata and if
		// we're not a seed (in which case
		// we won't have a piece picker)
		if (!t->valid_metadata()) return;

		t->peer_has(index, this);

		// this will disregard all have messages we get within
		// the first two seconds. Since some clients implements
		// lazy bitfields, these will not be reliable to use
		// for an estimated peer download rate.
		if (!peer_info_struct()
			|| m_ses.session_time() - peer_info_struct()->last_connected > 2)
		{
			// update bytes downloaded since last timer
			m_remote_bytes_dled += t->torrent_file().piece_size(index);
		}

		// it's important to not disconnect before we have
		// updated the piece picker, otherwise we will incorrectly
		// decrement the piece count without first incrementing it
		if (is_seed())
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** THIS IS A SEED [ p: %p ]", m_peer_info);
#endif
			t->seen_complete();
			t->set_seed(m_peer_info, true);
			m_upload_only = true;
			disconnect_if_redundant();
			if (is_disconnecting()) return;
		}

		if (!t->has_piece_passed(index)
			&& !t->is_seed()
			&& !is_interesting()
			&& (!t->has_picker() || t->picker().piece_priority(index) != 0))
			t->peer_is_interesting(*this);

		// if we're super seeding, this might mean that somebody
		// forwarded this piece. In which case we need to give
		// a new piece to that peer
		if (t->super_seeding()
			&& m_settings.get_bool(settings_pack::strict_super_seeding)
			&& (!super_seeded_piece(index) || t->num_peers() == 1))
		{
			for (torrent::peer_iterator i = t->begin()
				, end(t->end()); i != end; ++i)
			{
				peer_connection* p = *i;
				if (!p->super_seeded_piece(index)) continue;
				if (!p->has_piece(index)) continue;
				p->superseed_piece(index, t->get_piece_to_super_seed(p->get_bitfield()));
			}
		}
	}

	// -----------------------------
	// -------- DONT HAVE ----------
	// -----------------------------

	void peer_connection::incoming_dont_have(int index)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_dont_have(index)) return;
		}
#endif

		if (is_disconnecting()) return;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== DONT_HAVE [ piece: %d ]", index);
#endif

		if (is_disconnecting()) return;

		// if we got an invalid message, abort
		if (index >= int(m_have_piece.size()) || index < 0)
		{
			disconnect(errors::invalid_dont_have, 2);
			return;
		}

		if (!m_have_piece[index])
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("   got redundant DONT_HAVE message for index: %d", index);
#endif
			return;
		}

		bool was_seed = is_seed();
		m_have_piece.clear_bit(index);
		TORRENT_ASSERT(m_num_pieces > 0);
		--m_num_pieces;

		// only update the piece_picker if
		// we have the metadata and if
		// we're not a seed (in which case
		// we won't have a piece picker)
		if (!t->valid_metadata()) return;

		t->peer_lost(index, this);

		if (was_seed)
			t->set_seed(m_peer_info, false);
	}

	// -----------------------------
	// --------- BITFIELD ----------
	// -----------------------------

	void peer_connection::incoming_bitfield(bitfield const& bits)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_bitfield(bits)) return;
		}
#endif

		if (is_disconnecting()) return;

#ifdef TORRENT_VERBOSE_LOGGING
		std::string bitfield_str;
		bitfield_str.resize(bits.size());
		for (int i = 0; i < int(bits.size()); ++i)
			bitfield_str[i] = bits[i] ? '1' : '0';
		peer_log("<== BITFIELD [ %s ]", bitfield_str.c_str());
#endif

		// if we don't have the metedata, we cannot
		// verify the bitfield size
		if (t->valid_metadata()
			&& (bits.size() + 7) / 8 != (m_have_piece.size() + 7) / 8)
		{
			disconnect(errors::invalid_bitfield_size, 2);
			return;
		}

		if (m_bitfield_received)
		{
			// if we've already received a bitfield message
			// we first need to count down all the pieces
			// we believe the peer has first
			t->peer_lost(m_have_piece, this);
		}

		m_bitfield_received = true;

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_bitfield_time = time_now_hires();
		t->debug_log("HANDSHAKE [%p] (%d ms)", this, int(total_milliseconds(m_bitfield_time - m_connect_time)));
#endif
		// if we don't have metadata yet
		// just remember the bitmask
		// don't update the piecepicker
		// (since it doesn't exist yet)
		if (!t->ready_for_connections())
		{
#ifdef TORRENT_VERBOSE_LOGGING
			if (m_num_pieces == int(bits.size()))
				peer_log("*** THIS IS A SEED [ p: %p ]", m_peer_info);
#endif
			m_have_piece = bits;
			m_num_pieces = bits.count();
			t->set_seed(m_peer_info, m_num_pieces == int(bits.size()));
			return;
		}

		TORRENT_ASSERT(t->valid_metadata());
		
		int num_pieces = bits.count();
		if (num_pieces == int(m_have_piece.size()))
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** THIS IS A SEED [ p: %p ]", m_peer_info);
#endif
			// if this is a web seed. we don't have a peer_info struct
			t->set_seed(m_peer_info, true);
			m_upload_only = true;

			m_have_piece.set_all();
			m_num_pieces = num_pieces;
			t->peer_has_all(this);
			if (!t->is_upload_only())
				t->peer_is_interesting(*this);

			disconnect_if_redundant();

			return;
		}

		// let the torrent know which pieces the
		// peer has
		// if we're a seed, we don't keep track of piece availability
		bool interesting = false;
		t->peer_has(bits, this);

		if (!t->is_upload_only())
		{
			for (int i = 0; i < (int)m_have_piece.size(); ++i)
			{
				bool have = bits[i];
				if (!have || m_have_piece[i]) continue;
				// if we don't have a picker, the assumption is that the piece
				// priority is 1, or that we're a seed, but in that case have_piece
				// would have returned true.
				if (!t->have_piece(i) && (!t->has_picker() || t->picker().piece_priority(i) != 0))
					interesting = true;
			}
		}

		m_have_piece = bits;
		m_num_pieces = num_pieces;

		if (interesting) t->peer_is_interesting(*this);
		else if (upload_only()) disconnect(errors::upload_upload_connection);
	}

	bool peer_connection::disconnect_if_redundant()
	{
		if (m_disconnecting) return false;

		// we cannot disconnect in a constructor
		TORRENT_ASSERT(m_in_constructor == false);
		if (!m_settings.get_bool(settings_pack::close_redundant_connections)) return false;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return false;

		// if we don't have the metadata yet, don't disconnect
		// also, if the peer doesn't have metadata we shouldn't
		// disconnect it, since it may want to request the
		// metadata from us
		if (!t->valid_metadata() || !has_metadata()) return false;

		// don't close connections in share mode, we don't know if we need them
		if (t->share_mode()) return false;

		if (m_upload_only && t->is_upload_only())
		{
			disconnect(errors::upload_upload_connection);
			return true;
		}

		if (m_upload_only
			&& !m_interesting
			&& m_bitfield_received
			&& t->are_files_checked())
		{
			disconnect(errors::uninteresting_upload_peer);
			return true;
		}

#if defined TORRENT_DEBUG && !defined TORRENT_DISABLE_INVARIANT_CHECKS
		check_invariant();
#endif
		return false;
	}

	// -----------------------------
	// ---------- REQUEST ----------
	// -----------------------------

	void peer_connection::incoming_request(peer_request const& r)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		m_ses.inc_stats_counter(aux::session_interface::piece_requests);

#if defined TORRENT_VERBOSE_LOGGING
		peer_log("<== REQUEST [ piece: %d s: %d l: %d ]"
			, r.piece, r.start, r.length);
#endif

		if (t->super_seeding()
			&& !super_seeded_piece(r.piece))
		{
			m_ses.inc_stats_counter(aux::session_interface::invalid_piece_requests);
			++m_num_invalid_requests;
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** INVALID_REQUEST [ piece not superseeded "
				"i: %d t: %d n: %d h: %d ss1: %d ss2: %d ]"
				, m_peer_interested
				, int(t->torrent_file().piece_size(r.piece))
				, t->torrent_file().num_pieces()
				, t->has_piece_passed(r.piece)
				, m_superseed_piece[0]
				, m_superseed_piece[1]);
#endif

			write_reject_request(r);

			if (t->alerts().should_post<invalid_request_alert>())
			{
				t->alerts().post_alert(invalid_request_alert(
					t->get_handle(), m_remote, m_peer_id, r));
			}
			return;
		}

		// if we haven't received a bitfield, it was
		// probably omitted, which is the same as 'have_none'
		if (!m_bitfield_received) incoming_have_none();
		if (is_disconnecting()) return;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_request(r)) return;
		}
#endif
		if (is_disconnecting()) return;

		if (!t->valid_metadata())
		{
			m_ses.inc_stats_counter(aux::session_interface::invalid_piece_requests);
			// if we don't have valid metadata yet,
			// we shouldn't get a request
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** INVALID_REQUEST [ we don't have metadata yet ]");
			peer_log("==> REJECT_PIECE [ piece: %d s: %d l: %d ]"
				, r.piece , r.start , r.length);
#endif
			write_reject_request(r);
			return;
		}

		if (int(m_requests.size()) > m_settings.get_int(settings_pack::max_allowed_in_request_queue))
		{
			m_ses.inc_stats_counter(aux::session_interface::max_piece_requests);
			// don't allow clients to abuse our
			// memory consumption.
			// ignore requests if the client
			// is making too many of them.
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** INVALID_REQUEST [ incoming request queue full %d ]"
				, int(m_requests.size()));
			peer_log("==> REJECT_PIECE [ piece: %d s: %d l: %d ]"
				, r.piece , r.start , r.length);
#endif
			write_reject_request(r);
			return;
		}

		// make sure this request
		// is legal and that the peer
		// is not choked
		if (r.piece < 0
			|| r.piece >= t->torrent_file().num_pieces()
			|| (!t->has_piece_passed(r.piece) && !t->is_predictive_piece(r.piece))
			|| r.start < 0
			|| r.start >= t->torrent_file().piece_size(r.piece)
			|| r.length <= 0
			|| r.length + r.start > t->torrent_file().piece_size(r.piece)
			|| !m_peer_interested
			|| r.length > t->block_size())
		{
			m_ses.inc_stats_counter(aux::session_interface::invalid_piece_requests);
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** INVALID_REQUEST [ "
				"i: %d t: %d n: %d h: %d block_limit: %d ]"
				, m_peer_interested
				, int(t->torrent_file().piece_size(r.piece))
				, t->torrent_file().num_pieces()
				, t->has_piece_passed(r.piece)
				, t->block_size());

			peer_log("==> REJECT_PIECE [ piece: %d s: %d l: %d ]"
				, r.piece , r.start , r.length);
#endif

			write_reject_request(r);
			++m_num_invalid_requests;

			if (t->alerts().should_post<invalid_request_alert>())
			{
				t->alerts().post_alert(invalid_request_alert(
					t->get_handle(), m_remote, m_peer_id, r));
			}
			return;
		}

		// if we have choked the client
		// ignore the request
		if (m_choked && std::find(m_accept_fast.begin(), m_accept_fast.end()
				, r.piece) == m_accept_fast.end())
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** REJECTING REQUEST [ peer choked and piece not in allowed fast set ]");
			peer_log(" ==> REJECT_PIECE [ piece: %d | s: %d | l: %d ]"
				, r.piece, r.start, r.length);
#endif
			m_ses.inc_stats_counter(aux::session_interface::choked_piece_requests);
			write_reject_request(r);
			++m_choke_rejects;

			if (m_choke_rejects > m_settings.get_int(settings_pack::max_rejects))
			{
				disconnect(errors::too_many_requests_when_choked, 2);
				return;
			}
			else if ((m_choke_rejects & 0xf) == 0)
			{
				// tell the peer it's choked again
				// every 16 requests in a row
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
				peer_log("==> CHOKE [ peer keeps sending request when choked ]");
#endif
				write_choke();
			}
		}
		else
		{
#if defined TORRENT_REQUEST_LOGGING
			static ptime start = time_now_hires();
			// time-ms info-hash peer-id piece-index piece-offset size upload-rate-bytes-per-second
			char ih[5];
			to_hex((char const*)&t->info_hash()[0], 4, ih);
			char pid[5];
			to_hex((char const*)&m_peer_id[10], 4, pid);
			fprintf(m_ses.m_request_logger, "%d\t%s\t%s\t%d\t%d\t%d\t%d\n"
				, total_milliseconds(time_now_hires() - start), ih, pid
				, r.piece, r.start, r.length, m_statistics.upload_rate());
#endif
			m_choke_rejects = 0;
			m_requests.push_back(r);
#ifdef TORRENT_REQUEST_LOGGING
			if (m_ses.m_request_log)
				write_request_log(m_ses.m_request_log, t->info_hash(), this, r);
#endif
			m_last_incoming_request = time_now();
			fill_send_buffer();
		}
	}

	// reject all requests to this piece
	void peer_connection::reject_piece(int index)
	{
		for (std::vector<peer_request>::iterator i = m_requests.begin()
			, end(m_requests.end()); i != end; ++i)
		{
			peer_request const& r = *i;
			if (r.piece != index) continue;
			write_reject_request(r);
			i = m_requests.erase(i);
		}
	}

	void peer_connection::incoming_piece_fragment(int bytes)
	{
		m_last_piece = time_now();
		TORRENT_ASSERT(m_outstanding_bytes >= bytes);
		m_outstanding_bytes -= bytes;
		if (m_outstanding_bytes < 0) m_outstanding_bytes = 0;
		boost::shared_ptr<torrent> t = associated_torrent().lock();
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(m_received_in_piece + bytes <= t->block_size());
		m_received_in_piece += bytes;
#endif

		// progress of this torrent increased
		t->state_updated();

#if !defined TORRENT_DISABLE_INVARIANT_CHECKS && defined TORRENT_DEBUG
		check_invariant();
#endif
	}

	void peer_connection::start_receive_piece(peer_request const& r)
	{
#if !defined TORRENT_DISABLE_INVARIANT_CHECKS && defined TORRENT_DEBUG
		check_invariant();
#endif
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		buffer::const_interval recv_buffer = receive_buffer();
		int recv_pos = recv_buffer.end - recv_buffer.begin;
		TORRENT_ASSERT(recv_pos >= 9);
#endif

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		TORRENT_ASSERT(t);
		piece_block b(r.piece, r.start / t->block_size());
		m_receiving_block = b;

		if (!verify_piece(r))
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** INVALID_PIECE [ piece: %d s: %d l: %d ]"
				, r.piece, r.start, r.length);
#endif
			disconnect(errors::invalid_piece, 2);
			return;
		}

		bool in_req_queue = false;
		for (std::vector<pending_block>::const_iterator i = m_download_queue.begin()
			, end(m_download_queue.end()); i != end; ++i)
		{
			if (i->block != b) continue;
			in_req_queue = true;
			break;
		}

		// if this is not in the request queue, we have to
		// assume our outstanding bytes includes this piece too
		// if we're disconnecting, we shouldn't add pieces
		if (!in_req_queue && !m_disconnecting)
		{
			for (std::vector<pending_block>::iterator i = m_request_queue.begin()
				, end(m_request_queue.end()); i != end; ++i)
			{
				if (i->block != b) continue;
				in_req_queue = true;
				if (i - m_request_queue.begin() < m_queued_time_critical)
					--m_queued_time_critical;
				m_request_queue.erase(i);
				break;
			}

			m_download_queue.insert(m_download_queue.begin(), b);
			if (!in_req_queue)
			{
				if (t->alerts().should_post<unwanted_block_alert>())
				{
					t->alerts().post_alert(unwanted_block_alert(t->get_handle(), m_remote
						, m_peer_id, b.block_index, b.piece_index));
				}
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
				peer_log("*** The block we just got was not in the request queue ***");
#endif
				TORRENT_ASSERT(m_download_queue.front().block == b);
				m_download_queue.front().not_wanted = true;
			}
			m_outstanding_bytes += r.length;
		}
	}

#ifdef TORRENT_DEBUG
	struct check_postcondition
	{
		check_postcondition(boost::shared_ptr<torrent> const& t_
			, bool init_check = true): t(t_) { if (init_check) check(); }
	
		~check_postcondition() { check(); }
		
		void check()
		{
			if (!t->is_seed())
			{
				const int blocks_per_piece = static_cast<int>(
					(t->torrent_file().piece_length() + t->block_size() - 1) / t->block_size());

				std::vector<piece_picker::downloading_piece> const& dl_queue
					= t->picker().get_download_queue();

				for (std::vector<piece_picker::downloading_piece>::const_iterator i =
					dl_queue.begin(); i != dl_queue.end(); ++i)
				{
					TORRENT_ASSERT(i->finished <= blocks_per_piece);
				}
			}
		}
		
		shared_ptr<torrent> t;
	};
#endif


	// -----------------------------
	// ----------- PIECE -----------
	// -----------------------------

	void peer_connection::incoming_piece(peer_request const& p, char const* data)
	{
		bool exceeded = false;
		char* buffer = m_allocator.allocate_disk_buffer(exceeded, self(), "receive buffer");

		if (buffer == 0)
		{
			disconnect(errors::no_memory);
			return;
		}

		if (exceeded)
		{
			if ((m_channel_state[download_channel] & peer_info::bw_disk) == 0)
				m_ses.inc_disk_queue(download_channel);
			m_channel_state[download_channel] |= peer_info::bw_disk;
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** exceeded disk buffer watermark");
#endif
		}

		disk_buffer_holder holder(m_allocator, buffer);
		std::memcpy(buffer, data, p.length);
		incoming_piece(p, holder);
	}

	void peer_connection::incoming_piece(peer_request const& p, disk_buffer_holder& data)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		TORRENT_ASSERT(!m_disk_recv_buffer);
		TORRENT_ASSERT(m_disk_recv_buffer_size == 0);

		// we're not receiving any block right now
		m_receiving_block = piece_block::invalid;

#ifdef TORRENT_CORRUPT_DATA
		// corrupt all pieces from certain peers
		if (m_remote.address().is_v4()
			&& (m_remote.address().to_v4().to_ulong() & 0xf) == 0)
		{
			data.get()[0] = ~data.get()[0];
		}
#endif

		// if we haven't received a bitfield, it was
		// probably omitted, which is the same as 'have_none'
		if (!m_bitfield_received) incoming_have_none();
		if (is_disconnecting()) return;

		update_desired_queue_size();

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_piece(p, data))
			{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
				TORRENT_ASSERT(m_received_in_piece == p.length);
				m_received_in_piece = 0;
#endif
				return;
			}
		}
#endif
		if (is_disconnecting()) return;

#if defined TORRENT_DEBUG && !defined TORRENT_DISABLE_INVARIANT_CHECKS
		check_postcondition post_checker_(t);
#if defined TORRENT_EXPENSIVE_INVARIANT_CHECKS
		t->check_invariant();
#endif
#endif

#ifdef TORRENT_VERBOSE_LOGGING
		hasher h;
		h.update(data.get(), p.length);
		peer_log("<== PIECE        [ piece: %d | s: %d | l: %d | ds: %d | qs: %d | q: %d | hash: %s ]"
			, p.piece, p.start, p.length, statistics().download_rate()
			, int(m_desired_queue_size), int(m_download_queue.size())
			, to_hex(h.final().to_string()).c_str());
#endif

		if (p.length == 0)
		{
			if (t->alerts().should_post<peer_error_alert>())
			{
				t->alerts().post_alert(peer_error_alert(t->get_handle(), m_remote
					, m_peer_id, errors::peer_sent_empty_piece));
			}
			// This is used as a reject-request by bitcomet
			incoming_reject_request(p);
			return;
		}

		// if we're already seeding, don't bother,
		// just ignore it
		if (t->is_seed())
		{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT(m_received_in_piece == p.length);
			m_received_in_piece = 0;
#endif
			if (!m_download_queue.empty()) m_download_queue.erase(m_download_queue.begin());
			t->add_redundant_bytes(p.length, torrent::piece_seed);
			return;
		}

		ptime now = time_now();

		t->need_picker();

		piece_picker& picker = t->picker();

		std::vector<piece_block> finished_blocks;
		piece_block block_finished(p.piece, p.start / t->block_size());
		TORRENT_ASSERT(verify_piece(p));

		std::vector<pending_block>::iterator b
			= std::find_if(
				m_download_queue.begin()
				, m_download_queue.end()
				, has_block(block_finished));

		if (b == m_download_queue.end())
		{
			if (t->alerts().should_post<unwanted_block_alert>())
			{
				t->alerts().post_alert(unwanted_block_alert(t->get_handle(), m_remote
					, m_peer_id, block_finished.block_index, block_finished.piece_index));
			}
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** The block we just got was not in the request queue ***");
#endif
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT_VAL(m_received_in_piece == p.length, m_received_in_piece);
			m_received_in_piece = 0;
#endif
			t->add_redundant_bytes(p.length, torrent::piece_unknown);
			return;
		}

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		pending_block pending_b = *b;
#endif

		int block_index = b - m_download_queue.begin();
		TORRENT_ASSERT(m_download_queue[block_index] == pending_b);
		for (int i = 0; i < block_index; ++i)
		{
			pending_block& qe = m_download_queue[i];
			TORRENT_ASSERT(m_download_queue[block_index] == pending_b);
			TORRENT_ASSERT(i < block_index);

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** SKIPPED_PIECE [ piece: %d b: %d dqs: %d ]"
				, qe.block.piece_index, qe.block.block_index, int(m_desired_queue_size));
#endif

			++qe.skipped;
			// if the number of times a block is skipped by out of order
			// blocks exceeds the size of the outstanding queue, assume that
			// the other end dropped the request.
			if (m_settings.get_bool(settings_pack::drop_skipped_requests)
				&& qe.skipped > m_desired_queue_size)
			{
				if (t->alerts().should_post<request_dropped_alert>())
					t->alerts().post_alert(request_dropped_alert(t->get_handle()
						, remote(), pid(), qe.block.block_index, qe.block.piece_index));

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
				peer_log("*** DROPPED_PIECE [ piece: %d b: %d dqs: %d skip: %d ]"
					, qe.block.piece_index, qe.block.block_index
					, int(m_desired_queue_size), qe.skipped);
#endif
				if (!qe.timed_out && !qe.not_wanted)
					picker.abort_download(qe.block, peer_info_struct());

				TORRENT_ASSERT(m_outstanding_bytes >= t->to_req(qe.block).length);
				m_outstanding_bytes -= t->to_req(qe.block).length;
				if (m_outstanding_bytes < 0) m_outstanding_bytes = 0;
				TORRENT_ASSERT(m_download_queue[block_index] == pending_b);
				m_download_queue.erase(m_download_queue.begin() + i);
				--i;
				--block_index;
				TORRENT_ASSERT(m_download_queue[block_index] == pending_b);
#if !defined TORRENT_DISABLE_INVARIANT_CHECKS && defined TORRENT_DEBUG
				check_invariant();
#endif
			}
		}
		TORRENT_ASSERT(int(m_download_queue.size()) > block_index);
		b = m_download_queue.begin() + block_index;
		TORRENT_ASSERT(*b == pending_b);
		
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT_VAL(m_received_in_piece == p.length, m_received_in_piece);
		m_received_in_piece = 0;
#endif
		// if the block we got is already finished, then ignore it
		if (picker.is_downloaded(block_finished))
		{
			torrent::wasted_reason_t reason;
			if (b->timed_out) reason = torrent::piece_timed_out;
			else if (b->not_wanted) reason = torrent::piece_cancelled;
			else if (b->busy) reason = torrent::piece_end_game;
			else reason = torrent::piece_unknown;

			t->add_redundant_bytes(p.length, reason);

			m_download_queue.erase(b);
			m_timeout_extend = 0;

			if (m_disconnecting) return;

			if (!m_download_queue.empty())
				m_requested = now;

			m_ses.inc_stats_counter(aux::session_interface::incoming_redundant_piece_picks);
			request_a_block(*t, *this);
			send_block_requests();
			return;
		}
		
		if (total_seconds(now - m_requested)
			< m_settings.get_int(settings_pack::request_timeout)
			&& m_snubbed)
		{
			m_snubbed = false;
			if (t->alerts().should_post<peer_unsnubbed_alert>())
			{
				t->alerts().post_alert(peer_unsnubbed_alert(t->get_handle()
					, m_remote, m_peer_id));
			}
		}

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		t->debug_log("PIECE [%p] (%d ms) (%d)", this
			, int(total_milliseconds(time_now_hires() - m_unchoke_time)), t->num_have());
#endif

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("*** FILE ASYNC WRITE [ piece: %d | s: %d | l: %d ]"
			, p.piece, p.start, p.length);
#endif
		m_download_queue.erase(b);
		if (!t->need_loaded())
		{
			t->add_redundant_bytes(p.length, torrent::piece_unknown);
			return;
		}
		t->inc_refcount();
		m_disk_thread.async_write(&t->storage(), p, data
			, boost::bind(&peer_connection::on_disk_write_complete
			, self(), _1, p, t));
		m_outstanding_writing_bytes += p.length;

		if (!m_download_queue.empty())
		{
			m_timeout_extend = (std::max)(m_timeout_extend
				- m_settings.get_int(settings_pack::request_timeout), 0);
			m_requested += seconds(m_settings.get_int(settings_pack::request_timeout));
			if (m_requested > now) m_requested = now;
		}
		else
		{
			m_timeout_extend = 0;
		}

		bool was_finished = picker.is_piece_finished(p.piece);
		// did we request this block from any other peers?
		bool multi = picker.num_peers(block_finished) > 1;
//		fprintf(stderr, "peer_connection mark_as_writing peer: %p piece: %d block: %d\n"
//			, peer_info_struct(), block_finished.piece_index, block_finished.block_index);
		picker.mark_as_writing(block_finished, peer_info_struct());

		TORRENT_ASSERT(picker.num_peers(block_finished) == 0);
		// if we requested this block from other peers, cancel it now
		if (multi) t->cancel_block(block_finished);

		if (m_settings.get_int(settings_pack::predictive_piece_announce))
		{
			int piece = block_finished.piece_index;
			piece_picker::downloading_piece st;
			t->picker().piece_info(piece, st);

			int num_blocks = t->picker().blocks_in_piece(piece);
			if (st.requested > 0 && st.writing + st.finished + st.requested == num_blocks)
			{
				std::vector<void*> d;
				t->picker().get_requestors(d, piece);
				if (d.size() == 1)
				{
					// only make predicitons if all remaining
					// blocks are requested from the same peer
					torrent_peer* p = (torrent_peer*)d[0];
					if (p->connection)
					{
						// we have a connection. now, what is the current
						// download rate from this peer, and how many blocks
						// do we have left to download?
						boost::int64_t rate = p->connection->statistics().download_payload_rate();
						boost::int64_t bytes_left = boost::int64_t(st.requested) * t->block_size();
						// the settings unit is milliseconds, so calculate the
						// number of milliseconds worth of bytes left in the piece
						if (rate > 1000
							&& (bytes_left * 1000) / rate < m_settings.get_int(settings_pack::predictive_piece_announce))
						{
							// we predict we will complete this piece very soon.
							t->predicted_have_piece(piece, (bytes_left * 1000) / rate);
						}
					}
				}
			}
		}

		TORRENT_ASSERT(picker.num_peers(block_finished) == 0);

#if defined TORRENT_DEBUG && !defined TORRENT_DISABLE_INVARIANT_CHECKS \
	&& defined TORRENT_EXPENSIVE_INVARIANT_CHECKS
		t->check_invariant();
#endif

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		piece_picker::downloading_piece pi;
		picker.piece_info(p.piece, pi);
		int num_blocks = picker.blocks_in_piece(p.piece);
		TORRENT_ASSERT(pi.writing + pi.finished + pi.requested <= num_blocks);
		TORRENT_ASSERT(picker.is_piece_finished(p.piece) == (pi.writing + pi.finished == num_blocks));
#endif

		// did we just finish the piece?
		// this means all blocks are either written
		// to disk or are in the disk write cache
		if (picker.is_piece_finished(p.piece) && !was_finished)
		{
#if defined TORRENT_DEBUG && !defined TORRENT_DISABLE_INVARIANT_CHECKS
			check_postcondition post_checker2_(t, false);
#endif
			t->verify_piece(p.piece);
		}

		if (is_disconnecting()) return;

		m_ses.inc_stats_counter(aux::session_interface::incoming_piece_picks);
		request_a_block(*t, *this);
		send_block_requests();
	}

	void peer_connection::on_disk_write_complete(disk_io_job const* j
		, peer_request p, boost::shared_ptr<torrent> t)
	{
		torrent_ref_holder h(t.get());
		if (t) t->dec_refcount();
		TORRENT_ASSERT(m_ses.is_single_thread());

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("*** FILE ASYNC WRITE COMPLETE [ ret: %d | piece: %d | s: %d | l: %d | e: %s ]"
			, j->ret, p.piece, p.start, p.length, j->error.ec.message().c_str());
#endif

		m_outstanding_writing_bytes -= p.length;

		TORRENT_ASSERT(m_outstanding_writing_bytes >= 0);

		// flush send buffer at the end of
		// this burst of disk events
//		m_ses.cork_burst(this);

		INVARIANT_CHECK;

		if (!t)
		{
			disconnect(j->error.ec);
			return;
		}

		t->schedule_storage_tick();

		// in case the outstanding bytes just dropped down
		// to allow to receive more data
		setup_receive(read_async);

		piece_block block_finished(p.piece, p.start / t->block_size());

		if (j->ret < 0)
		{
			// handle_disk_error may disconnect us
			t->handle_disk_error(j, this);
			return;
		}

		TORRENT_ASSERT(j->ret == p.length);

		if (!t->has_picker()) return;

		piece_picker& picker = t->picker();

		TORRENT_ASSERT(p.piece == j->piece);
		TORRENT_ASSERT(p.start == j->d.io.offset);
		TORRENT_ASSERT(picker.num_peers(block_finished) == 0);

		if (j->ret == -1 && j->error.ec == boost::system::errc::operation_canceled)
		{
			picker.mark_as_canceled(block_finished, peer_info_struct());
			return;
		}
//		fprintf(stderr, "peer_connection mark_as_finished peer: %p piece: %d block: %d\n"
//			, peer_info_struct(), block_finished.piece_index, block_finished.block_index);
		picker.mark_as_finished(block_finished, peer_info_struct());

		t->maybe_done_flushing();

		if (t->alerts().should_post<block_finished_alert>())
		{
			t->alerts().post_alert(block_finished_alert(t->get_handle(), 
				remote(), pid(), block_finished.block_index, block_finished.piece_index));
		}

		disconnect_if_redundant();

		if (m_disconnecting) return;

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		if (t->has_picker())
		{
			const std::vector<piece_picker::downloading_piece>& q
				= picker.get_download_queue();

			for (std::vector<piece_picker::downloading_piece>::const_iterator
				i = q.begin(), end(q.end()); i != end; ++i)
			{
				if (i->index != block_finished.piece_index) continue;
				piece_picker::downloading_piece const& p = *i;
				TORRENT_ASSERT(p.info[block_finished.block_index].state == piece_picker::block_info::state_finished);
			}
		}
#endif
		if (t->is_aborted()) return;
	}

	// -----------------------------
	// ---------- CANCEL -----------
	// -----------------------------

	void peer_connection::incoming_cancel(peer_request const& r)
	{
		INVARIANT_CHECK;

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_cancel(r)) return;
		}
#endif
		if (is_disconnecting()) return;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== CANCEL  [ piece: %d | s: %d | l: %d ]", r.piece, r.start, r.length);
#endif

		std::vector<peer_request>::iterator i
			= std::find(m_requests.begin(), m_requests.end(), r);

		if (i != m_requests.end())
		{
			m_ses.inc_stats_counter(aux::session_interface::cancelled_piece_requests);
			m_requests.erase(i);
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("==> REJECT_PIECE [ piece: %d s: %d l: %d ]"
				, r.piece , r.start , r.length);
#endif
			write_reject_request(r);
		}
		else
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** GOT CANCEL NOT IN THE QUEUE");
#endif
		}
	}

	// -----------------------------
	// --------- DHT PORT ----------
	// -----------------------------

	void peer_connection::incoming_dht_port(int listen_port)
	{
		INVARIANT_CHECK;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== DHT_PORT [ p: %d ]", listen_port);
#endif
#ifndef TORRENT_DISABLE_DHT
		m_ses.add_dht_node(udp::endpoint(
			m_remote.address(), listen_port));
#endif
	}

	// -----------------------------
	// --------- HAVE ALL ----------
	// -----------------------------

	void peer_connection::incoming_have_all()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		// we cannot disconnect in a constructor, and
		// this function may end up doing that
		TORRENT_ASSERT(m_in_constructor == false);

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== HAVE_ALL");
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_have_all()) return;
		}
#endif
		if (is_disconnecting()) return;

		if (m_bitfield_received)
			t->peer_lost(m_have_piece, this);

		m_have_all = true;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("*** THIS IS A SEED [ p: %p ]", m_peer_info);
#endif

		t->set_seed(m_peer_info, true);
		m_upload_only = true;
		m_bitfield_received = true;

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_bitfield_time = time_now_hires();
		t->debug_log("HANDSHAKE [%p] (%d ms)", this, int(total_milliseconds(m_bitfield_time - m_connect_time)));
#endif

		// if we don't have metadata yet
		// just remember the bitmask
		// don't update the piecepicker
		// (since it doesn't exist yet)
		if (!t->ready_for_connections())
		{
			// assume seeds are interesting when we
			// don't even have the metadata
			t->peer_is_interesting(*this);

			disconnect_if_redundant();
			return;
		}

		TORRENT_ASSERT(!m_have_piece.empty());
		m_have_piece.set_all();
		m_num_pieces = m_have_piece.size();
		
		t->peer_has_all(this);

		// if we're finished, we're not interested
		if (t->is_upload_only()) send_not_interested();
		else t->peer_is_interesting(*this);

		disconnect_if_redundant();
	}
	
	// -----------------------------
	// --------- HAVE NONE ---------
	// -----------------------------

	void peer_connection::incoming_have_none()
	{
		INVARIANT_CHECK;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== HAVE_NONE");
#endif

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_have_none()) return;
		}
#endif
		if (is_disconnecting()) return;

		if (m_bitfield_received)
			t->peer_lost(m_have_piece, this);

		t->set_seed(m_peer_info, false);
		m_bitfield_received = true;

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_bitfield_time = time_now_hires();
		t->debug_log("HANDSHAKE [%p] (%d ms)", this, int(total_milliseconds(m_bitfield_time - m_connect_time)));
#endif
		m_have_piece.clear_all();
		m_num_pieces = 0;

		// if the peer is ready to download stuff, it must have metadata		
		m_has_metadata = true;

		// we're never interested in a peer that doesn't have anything
		send_not_interested();

		TORRENT_ASSERT(!m_have_piece.empty() || !t->ready_for_connections());
		disconnect_if_redundant();
	}

	// -----------------------------
	// ------- ALLOWED FAST --------
	// -----------------------------

	void peer_connection::incoming_allowed_fast(int index)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

#ifdef TORRENT_LOGGING
		{
			ptime now = time_now_hires();
			t->debug_log("ALLOW FAST [%p] (%d ms)", this, int(total_milliseconds(now - m_connect_time)));
			if (m_peer_choked) m_unchoke_time = now;
		}
#endif

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<== ALLOWED_FAST [ %d ]", index);
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_allowed_fast(index)) return;
		}
#endif
		if (is_disconnecting()) return;
		if (index < 0)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("<== INVALID_ALLOWED_FAST [ %d ]", index);
#endif
			return;
		}

		if (t->valid_metadata())
		{
			if (index >= int(m_have_piece.size()))
			{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
				peer_log("<== INVALID_ALLOWED_FAST [ %d | s: %d ]"
					, index, int(m_have_piece.size()));
#endif
				return;
			}

			// if we already have the piece, we can
			// ignore this message
			if (t->have_piece(index))
				return;
		}

		// if we don't have the metadata, we'll verify
		// this piece index later
		m_allowed_fast.push_back(index);

		// if the peer has the piece and we want
		// to download it, request it
		if (int(m_have_piece.size()) > index
			&& m_have_piece[index]
			&& !t->has_piece_passed(index)
			&& t->valid_metadata()
			&& t->has_picker()
			&& !t->has_piece_passed(index)
			&& t->picker().piece_priority(index) > 0)
		{
			t->peer_is_interesting(*this);
		}
	}

	std::vector<int> const& peer_connection::allowed_fast()
	{
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		// TODO: sort the allowed fast set in priority order
		return m_allowed_fast;
	}

	bool peer_connection::can_request_time_critical() const
	{
		if (has_peer_choked() || !is_interesting()) return false;
		if ((int)m_download_queue.size() + (int)m_request_queue.size()
			> m_desired_queue_size * 2) return false;
		if (on_parole()) return false; 
		if (m_disconnecting) return false;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		if (t->upload_mode()) return false;
		return true;
	}

	void peer_connection::make_time_critical(piece_block const& block)
	{
		std::vector<pending_block>::iterator rit = std::find_if(m_request_queue.begin()
			, m_request_queue.end(), has_block(block));
		if (rit == m_request_queue.end()) return;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);
		TORRENT_ASSERT(t->has_picker());
		TORRENT_ASSERT(t->picker().is_requested(block));
#endif
		// ignore it if it's already time critical
		if (rit - m_request_queue.begin() < m_queued_time_critical) return;
		pending_block b = *rit;
		m_request_queue.erase(rit);
		m_request_queue.insert(m_request_queue.begin() + m_queued_time_critical, b);
		++m_queued_time_critical;
	}

	bool peer_connection::add_request(piece_block const& block, int flags)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		TORRENT_ASSERT(!m_disconnecting);
		TORRENT_ASSERT(t->valid_metadata());

		TORRENT_ASSERT(block.block_index != piece_block::invalid.block_index);
		TORRENT_ASSERT(block.piece_index != piece_block::invalid.piece_index);
		TORRENT_ASSERT(int(block.piece_index) < t->torrent_file().num_pieces());
		TORRENT_ASSERT(int(block.block_index) < t->torrent_file().piece_size(block.piece_index));
		TORRENT_ASSERT(!t->picker().is_requested(block) || (t->picker().num_peers(block) > 0));
		TORRENT_ASSERT(!t->have_piece(block.piece_index));
		TORRENT_ASSERT(std::find_if(m_download_queue.begin(), m_download_queue.end()
			, has_block(block)) == m_download_queue.end());
		TORRENT_ASSERT(std::find(m_request_queue.begin(), m_request_queue.end()
			, block) == m_request_queue.end());

		if (t->upload_mode()) return false;
		if (m_disconnecting) return false;

		piece_picker::piece_state_t state;
		peer_speed_t speed = peer_speed();
		char const* speedmsg = 0;
		if (speed == fast)
		{
			speedmsg = "fast";
			state = piece_picker::fast;
		}
		else if (speed == medium)
		{
			speedmsg = "medium";
			state = piece_picker::medium;
		}
		else
		{
			speedmsg = "slow";
			state = piece_picker::slow;
		}

		if (flags & req_busy)
		{
			// this block is busy (i.e. it has been requested
			// from another peer already). Only allow one busy
			// request in the pipeline at the time
			for (std::vector<pending_block>::const_iterator i = m_download_queue.begin()
				, end(m_download_queue.end()); i != end; ++i)
			{
				if (i->busy) return false;
			}

			for (std::vector<pending_block>::const_iterator i = m_request_queue.begin()
				, end(m_request_queue.end()); i != end; ++i)
			{
				if (i->busy) return false;
			}
		}

		if (!t->picker().mark_as_downloading(block, peer_info_struct(), state))
			return false;

		if (t->alerts().should_post<block_downloading_alert>())
		{
			t->alerts().post_alert(block_downloading_alert(t->get_handle(), 
				remote(), pid(), speedmsg, block.block_index, block.piece_index));
		}

		pending_block pb(block);
		pb.busy = (flags & req_busy) ? true : false;
		if (flags & req_time_critical)
		{
			m_request_queue.insert(m_request_queue.begin() + m_queued_time_critical
				, pb);
			++m_queued_time_critical;
		}
		else
		{
			m_request_queue.push_back(pb);
		}
		return true;
	}

	void peer_connection::cancel_all_requests()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		// this peer might be disconnecting
		if (!t) return;

		TORRENT_ASSERT(t->valid_metadata());

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("*** CANCEL ALL REQUESTS");
#endif

		while (!m_request_queue.empty())
		{
			t->picker().abort_download(m_request_queue.back().block, peer_info_struct());
			m_request_queue.pop_back();
		}
		m_queued_time_critical = 0;

		// make a local temporary copy of the download queue, since it
		// may be modified when we call write_cancel (for peers that don't
		// support the FAST extensions).
		std::vector<pending_block> temp_copy = m_download_queue;

		for (std::vector<pending_block>::iterator i = temp_copy.begin()
			, end(temp_copy.end()); i != end; ++i)
		{
			piece_block b = i->block;

			int block_offset = b.block_index * t->block_size();
			int block_size
				= (std::min)(t->torrent_file().piece_size(b.piece_index)-block_offset,
					t->block_size());
			TORRENT_ASSERT(block_size > 0);
			TORRENT_ASSERT(block_size <= t->block_size());

			// we can't cancel the piece if we've started receiving it
			if (m_receiving_block == b) continue;

			peer_request r;
			r.piece = b.piece_index;
			r.start = block_offset;
			r.length = block_size;

#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("==> CANCEL  [ piece: %d s: %d l: %d b: %d ]"
				, b.piece_index, block_offset, block_size, b.block_index);
#endif
			write_cancel(r);
		}
	}

	void peer_connection::cancel_request(piece_block const& block, bool force)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		// this peer might be disconnecting
		if (!t) return;

		TORRENT_ASSERT(t->valid_metadata());

		TORRENT_ASSERT(block.block_index != piece_block::invalid.block_index);
		TORRENT_ASSERT(block.piece_index != piece_block::invalid.piece_index);
		TORRENT_ASSERT(int(block.piece_index) < t->torrent_file().num_pieces());
		TORRENT_ASSERT(int(block.block_index) < t->torrent_file().piece_size(block.piece_index));

		// if all the peers that requested this block has been
		// cancelled, then just ignore the cancel.
		if (!t->picker().is_requested(block)) return;

		std::vector<pending_block>::iterator it
			= std::find_if(m_download_queue.begin(), m_download_queue.end(), has_block(block));
		if (it == m_download_queue.end())
		{
			std::vector<pending_block>::iterator rit = std::find_if(m_request_queue.begin()
				, m_request_queue.end(), has_block(block));

			// when a multi block is received, it is cancelled
			// from all peers, so if this one hasn't requested
			// the block, just ignore to cancel it.
			if (rit == m_request_queue.end()) return;

			if (rit - m_request_queue.begin() < m_queued_time_critical)
				--m_queued_time_critical;

			t->picker().abort_download(block, peer_info_struct());
			m_request_queue.erase(rit);
			// since we found it in the request queue, it means it hasn't been
			// sent yet, so we don't have to send a cancel.
			return;
		}

		int block_offset = block.block_index * t->block_size();
		int block_size
			= (std::min)(t->torrent_file().piece_size(block.piece_index)-block_offset,
			t->block_size());
		TORRENT_ASSERT(block_size > 0);
		TORRENT_ASSERT(block_size <= t->block_size());

		if (force) t->picker().abort_download(block, peer_info_struct());

		if (m_outstanding_bytes < block_size) return;

		peer_request r;
		r.piece = block.piece_index;
		r.start = block_offset;
		r.length = block_size;

#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("==> CANCEL  [ piece: %d s: %d l: %d b: %d ]"
				, block.piece_index, block_offset, block_size, block.block_index);
#endif
		write_cancel(r);
	}

	bool peer_connection::send_choke()
	{
		INVARIANT_CHECK;

		if (m_peer_info && m_peer_info->optimistically_unchoked)
			m_peer_info->optimistically_unchoked = false;

		if (m_choked) return false;
#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("==> CHOKE");
#endif
		write_choke();
		m_choked = true;

#ifdef TORRENT_DEBUG
		m_last_choke = time_now();
#endif
		m_num_invalid_requests = 0;

		// reject the requests we have in the queue
		// except the allowed fast pieces
		for (std::vector<peer_request>::iterator i = m_requests.begin();
			i != m_requests.end();)
		{
			if (std::find(m_accept_fast.begin(), m_accept_fast.end(), i->piece)
				!= m_accept_fast.end())
			{
				++i;
				continue;
			}
			peer_request const& r = *i;
			m_ses.inc_stats_counter(aux::session_interface::choked_piece_requests);
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("==> REJECT_PIECE [ piece: %d s: %d l: %d ]"
				, r.piece , r.start , r.length);
#endif
			write_reject_request(r);
			i = m_requests.erase(i);
		}
		return true;
	}

	bool peer_connection::send_unchoke()
	{
		INVARIANT_CHECK;

		if (!m_choked) return false;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t->ready_for_connections()) return false;

		if (!m_sent_suggests)
		{
			std::vector<torrent::suggest_piece_t> const& ret
				= t->get_suggested_pieces();

			for (std::vector<torrent::suggest_piece_t>::const_iterator i = ret.begin()
				, end(ret.end()); i != end; ++i)
			{
				TORRENT_ASSERT(i->piece_index >= 0);
				send_suggest(i->piece_index);
			}

			m_sent_suggests = true;
		}

		m_last_unchoke = time_now();
		write_unchoke();
		m_choked = false;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("==> UNCHOKE");
#endif
		return true;
	}

	void peer_connection::send_interested()
	{
		if (m_interesting) return;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t->ready_for_connections()) return;
		m_interesting = true;
		write_interested();

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("==> INTERESTED");
#endif
	}

	void peer_connection::send_not_interested()
	{
		// we cannot disconnect in a constructor, and
		// this function may end up doing that
		TORRENT_ASSERT(m_in_constructor == false);

		if (!m_interesting)
		{
			disconnect_if_redundant();
			return;
		}

		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t->ready_for_connections()) return;
		m_interesting = false;
		write_not_interested();

		m_became_uninteresting = time_now();

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("==> NOT_INTERESTED");
#endif
		disconnect_if_redundant();
	}

	void peer_connection::send_suggest(int piece)
	{
		// we cannot suggest a piece we don't have!
		TORRENT_ASSERT(m_torrent.lock()->has_piece_passed(piece));

		if (m_connecting) return;
		if (in_handshake()) return;

		// don't suggest a piece that the peer already has
		// don't suggest anything to a peer that isn't interested
		if (has_piece(piece)
			|| !m_peer_interested)
			return;
	
		write_suggest(piece);
	}

	void peer_connection::send_block_requests()
	{
		INVARIANT_CHECK;
		
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (m_disconnecting) return;

		if (t->graceful_pause() && m_outstanding_bytes == 0)
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** GRACEFUL PAUSE [ NO MORE DOWNLOAD ]");
#endif
			disconnect(errors::torrent_paused);
			return;
		}

		// we can't download pieces in these states
		if (t->state() == torrent_status::checking_files
			|| t->state() == torrent_status::checking_resume_data
			|| t->state() == torrent_status::downloading_metadata
			|| t->state() == torrent_status::allocating)
			return;

		if ((int)m_download_queue.size() >= m_desired_queue_size
			|| t->upload_mode()) return;

		bool empty_download_queue = m_download_queue.empty();

		while (!m_request_queue.empty()
			&& ((int)m_download_queue.size() < m_desired_queue_size
				|| m_queued_time_critical > 0))
		{
			pending_block block = m_request_queue.front();

			m_request_queue.erase(m_request_queue.begin());
			if (m_queued_time_critical) --m_queued_time_critical;

			// if we're a seed, we don't have a piece picker
			// so we don't have to worry about invariants getting
			// out of sync with it
			if (!t->has_picker()) continue;

			// this can happen if a block times out, is re-requested and
			// then arrives "unexpectedly"
			if (t->picker().is_finished(block.block)
				|| t->picker().is_downloaded(block.block))
			{
				t->picker().abort_download(block.block, peer_info_struct());
				continue;
			}

			int block_offset = block.block.block_index * t->block_size();
			int block_size = (std::min)(t->torrent_file().piece_size(
				block.block.piece_index) - block_offset, t->block_size());
			TORRENT_ASSERT(block_size > 0);
			TORRENT_ASSERT(block_size <= t->block_size());

			peer_request r;
			r.piece = block.block.piece_index;
			r.start = block_offset;
			r.length = block_size;

			TORRENT_ASSERT(verify_piece(t->to_req(block.block)));
			m_download_queue.push_back(block);
			m_outstanding_bytes += block_size;
#if !defined TORRENT_DISABLE_INVARIANT_CHECKS && defined TORRENT_DEBUG
			check_invariant();
#endif

/*
#ifdef TORRENT_VERBOSE_LOGGING
			(*m_logger) << time_now_string()
				<< " *** REQUEST-QUEUE** [ "
				"piece: " << block.piece_index << " | "
				"block: " << block.block_index << " ]\n";
#endif
*/			
			// if we are requesting large blocks, merge the smaller
			// blocks that are in the same piece into larger requests
			if (m_request_large_blocks)
			{
				int blocks_per_piece = t->torrent_file().piece_length() / t->block_size();

				while (!m_request_queue.empty())
				{
					// check to see if this block is connected to the previous one
					// if it is, merge them, otherwise, break this merge loop
					pending_block const& front = m_request_queue.front();
					if (front.block.piece_index * blocks_per_piece + front.block.block_index
						!= block.block.piece_index * blocks_per_piece + block.block.block_index + 1)
						break;
					block = m_request_queue.front();
					m_request_queue.erase(m_request_queue.begin());
					TORRENT_ASSERT(verify_piece(t->to_req(block.block)));
					m_download_queue.push_back(block);
					if (m_queued_time_critical) --m_queued_time_critical;

#ifdef TORRENT_VERBOSE_LOGGING
					peer_log("*** MERGING REQUEST [ piece: %d block: %d ]"
						, block.block.piece_index, block.block.block_index);
#endif

					block_offset = block.block.block_index * t->block_size();
					block_size = (std::min)(t->torrent_file().piece_size(
						block.block.piece_index) - block_offset, t->block_size());
					TORRENT_ASSERT(block_size > 0);
					TORRENT_ASSERT(block_size <= t->block_size());

					r.length += block_size;
					m_outstanding_bytes += block_size;
#if !defined TORRENT_DISABLE_INVARIANT_CHECKS && defined TORRENT_DEBUG
					check_invariant();
#endif
				}
			}

			// the verification will fail for coalesced blocks
			TORRENT_ASSERT(verify_piece(r) || m_request_large_blocks);
			
#ifndef TORRENT_DISABLE_EXTENSIONS
			bool handled = false;
			for (extension_list_t::iterator i = m_extensions.begin()
				, end(m_extensions.end()); i != end; ++i)
			{
				if ((handled = (*i)->write_request(r))) break;
			}
			if (is_disconnecting()) return;
			if (!handled)
#endif
			{
				write_request(r);
				m_last_request = time_now();
			}

#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("==> REQUEST      [ piece: %d | s: %d | l: %d | ds: %d B/s | "
				"dqs: %d rqs: %d blk: %s ]"
				, r.piece, r.start, r.length, statistics().download_rate()
				, int(m_desired_queue_size), int(m_download_queue.size())
				, m_request_large_blocks?"large":"single");
#endif
		}
		m_last_piece = time_now();

		if (!m_download_queue.empty()
			&& empty_download_queue)
		{
			// This means we just added a request to this connection
			m_requested = time_now();
#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			t->debug_log("REQUEST [%p] (%d ms)", this, int(total_milliseconds(time_now_hires() - m_unchoke_time)));
#endif
		}
	}

	void peer_connection::on_connect_timeout()
	{
		m_queued_for_connection = false;
		TORRENT_ASSERT(m_ses.is_single_thread());

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		boost::shared_ptr<torrent> t = m_torrent.lock();
		t->debug_log("END queue peer (timed out) [%p]", this);
#endif
		connect_failed(errors::timed_out);
	}
	
	void peer_connection::connect_failed(error_code const& e)
	{
		TORRENT_ASSERT(e);

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		peer_log("CONNECTION FAILED: %s", print_endpoint(m_remote).c_str());
#endif
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_ses.session_log(" CONNECTION FAILED: %s", print_endpoint(m_remote).c_str());
#endif

		m_ses.inc_stats_counter(aux::session_interface::connect_timeouts);

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(!m_connecting || t);
		if (m_connecting && t)
		{
			t->dec_num_connecting();
			m_connecting = false;
		}

		if (m_connection_ticket != -1)
		{
			m_ses.half_open_done(m_connection_ticket);
		}

		// a connection attempt using uTP just failed
		// mark this peer as not supporting uTP
		// we'll never try it again (unless we're trying holepunch)
		if (is_utp(*m_socket)
			&& m_peer_info
			&& m_peer_info->supports_utp
			&& !m_holepunch_mode)
		{
			m_peer_info->supports_utp = false;
			// reconnect immediately using TCP
			torrent_peer* pi = peer_info_struct();
			boost::shared_ptr<torrent> t = m_torrent.lock();
			fast_reconnect(true);
			disconnect(e, 0);
			if (t && pi) t->connect_to_peer(pi, true);
			return;
		}

		if (m_holepunch_mode)
			fast_reconnect(true);

#ifndef TORRENT_DISABLE_EXTENSIONS
		if ((!is_utp(*m_socket)
				|| !m_settings.get_bool(settings_pack::enable_outgoing_tcp))
			&& m_peer_info
			&& m_peer_info->supports_holepunch
			&& !m_holepunch_mode)
		{
			boost::shared_ptr<torrent> t = m_torrent.lock();
			// see if we can try a holepunch
			bt_peer_connection* p = t->find_introducer(remote());
			if (p)
				p->write_holepunch_msg(bt_peer_connection::hp_rendezvous, remote(), 0);
		}
#endif

		disconnect(e, 1);
		return;
	}

	// the error argument defaults to 0, which means deliberate disconnect
	// 1 means unexpected disconnect/error
	// 2 protocol error (client sent something invalid)
	void peer_connection::disconnect(error_code const& ec, int error)
	{
		TORRENT_ASSERT(m_ses.is_single_thread());

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		m_disconnect_started = true;
#endif

		if (m_disconnecting) return;

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		switch (error)
		{
		case 0:
			peer_log("*** CONNECTION CLOSED %s", ec.message().c_str());
			break;
		case 1:
			peer_log("*** CONNECTION FAILED %s", ec.message().c_str());
			break;
		case 2:
			peer_log("*** PEER ERROR %s", ec.message().c_str());
			break;
		}
#endif

		if ((m_channel_state[upload_channel] & peer_info::bw_network) == 0)
		{
			// make sure we free up all send buffers that are owned
			// by the disk thread
			m_send_buffer.clear();
			m_disk_recv_buffer.reset();
		}

		// we cannot do this in a constructor
		TORRENT_ASSERT(m_in_constructor == false);
		if (error > 0) m_failed = true;
		if (m_disconnecting) return;

		// for incoming connections, we get invalid argument errors
		// when asking for the remote endpoint and the socket already
		// closed, which is an edge case, but possible to happen when
		// a peer makes a TCP and uTP connection in parallel.
		// for outgoing connections however, why would we get this?
//		TORRENT_ASSERT(ec != error::invalid_argument || !m_outgoing);

		m_ses.inc_stats_counter(aux::session_interface::disconnected_peers);
		if (error == 2) m_ses.inc_stats_counter(aux::session_interface::error_peers);
		if (ec == error::connection_reset) m_ses.inc_stats_counter(aux::session_interface::connreset_peers);
		else if (ec == error::eof) m_ses.inc_stats_counter(aux::session_interface::eof_peers);
		else if (ec == error::connection_refused) m_ses.inc_stats_counter(aux::session_interface::connrefused_peers);
		else if (ec == error::connection_aborted) m_ses.inc_stats_counter(aux::session_interface::connaborted_peers);
		else if (ec == error::no_permission) m_ses.inc_stats_counter(aux::session_interface::perm_peers);
		else if (ec == error::no_buffer_space) m_ses.inc_stats_counter(aux::session_interface::buffer_peers);
		else if (ec == error::host_unreachable) m_ses.inc_stats_counter(aux::session_interface::unreachable_peers);
		else if (ec == error::broken_pipe) m_ses.inc_stats_counter(aux::session_interface::broken_pipe_peers);
		else if (ec == error::address_in_use) m_ses.inc_stats_counter(aux::session_interface::addrinuse_peers);
		else if (ec == error::access_denied) m_ses.inc_stats_counter(aux::session_interface::no_access_peers);
		else if (ec == error::invalid_argument) m_ses.inc_stats_counter(aux::session_interface::invalid_arg_peers);
		else if (ec == error::operation_aborted) m_ses.inc_stats_counter(aux::session_interface::aborted_peers);
		else if (ec == error_code(errors::upload_upload_connection)
			|| ec == error_code(errors::uninteresting_upload_peer)
			|| ec == error_code(errors::torrent_aborted)
			|| ec == error_code(errors::self_connection)
			|| ec == error_code(errors::torrent_paused))
			m_ses.inc_stats_counter(aux::session_interface::uninteresting_peers);

		if (ec == error_code(errors::timed_out)
			|| ec == error::timed_out)
			m_ses.inc_stats_counter(aux::session_interface::transport_timeout_peers);
		
		if (ec == error_code(errors::timed_out_inactivity)
			|| ec == error_code(errors::timed_out_no_request)
			|| ec == error_code(errors::timed_out_no_interest))
			m_ses.inc_stats_counter(aux::session_interface::timeout_peers);

		if (ec == error_code(errors::no_memory))
			m_ses.inc_stats_counter(aux::session_interface::no_memory_peers);

		if (ec == error_code(errors::too_many_connections))
			m_ses.inc_stats_counter(aux::session_interface::too_many_peers);

		if (ec == error_code(errors::timed_out_no_handshake))
			m_ses.inc_stats_counter(aux::session_interface::connect_timeouts);

		if (is_utp(*m_socket)) m_ses.inc_stats_counter(aux::session_interface::error_utp_peers);
		else m_ses.inc_stats_counter(aux::session_interface::error_tcp_peers);

		if (m_outgoing) m_ses.inc_stats_counter(aux::session_interface::error_outgoing_peers);
		else m_ses.inc_stats_counter(aux::session_interface::error_incoming_peers);


#ifndef TORRENT_DISABLE_ENCRYPTION
		if (type() == bittorrent_connection)
		{
			bt_peer_connection* bt = static_cast<bt_peer_connection*>(this);
			if (bt->supports_encryption()) m_ses.inc_stats_counter(aux::session_interface::error_encrypted_peers);
			if (bt->rc4_encrypted() && bt->supports_encryption()) m_ses.inc_stats_counter(aux::session_interface::error_rc4_peers);
		}
#endif // TORRENT_DISABLE_ENCRYPTION

		boost::shared_ptr<peer_connection> me(self());

		INVARIANT_CHECK;

		if (m_channel_state[upload_channel] & peer_info::bw_disk)
		{
			m_ses.dec_disk_queue(upload_channel);
			m_channel_state[upload_channel] &= ~peer_info::bw_disk;
		}
		if (m_channel_state[download_channel] & peer_info::bw_disk)
		{
			m_ses.dec_disk_queue(download_channel);
			m_channel_state[download_channel] &= ~peer_info::bw_disk;
		}

		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (m_connecting && t)
		{
			t->dec_num_connecting();
			m_connecting = false;
		}
		if (m_connection_ticket >= 0)
		{
			m_ses.half_open_done(m_connection_ticket);
			m_connection_ticket = -1;
		}

		torrent_handle handle;
		if (t) handle = t->get_handle();

		if (ec == error::address_in_use
			&& m_settings.get_int(settings_pack::outgoing_port) != 0
			&& t)
		{
			if (t->alerts().should_post<performance_alert>())
				t->alerts().post_alert(performance_alert(
					handle, performance_alert::too_few_outgoing_ports));
		}

		if (t)
		{
			if (ec)
			{
				if ((error > 1 || ec.category() == socks_category)
					&& t->alerts().should_post<peer_error_alert>())
				{
					t->alerts().post_alert(
						peer_error_alert(handle, remote(), pid(), ec));
				}
				else if (error <= 1 && t->alerts().should_post<peer_disconnected_alert>())
				{
					t->alerts().post_alert(
						peer_disconnected_alert(handle, remote(), pid(), ec));
				}
			}

			// make sure we keep all the stats!
			if (!m_ignore_stats)
			{
				// report any partially received payload as redundant
				boost::optional<piece_block_progress> pbp = downloading_piece_progress();
				if (pbp
					&& pbp->bytes_downloaded > 0
					&& pbp->bytes_downloaded < pbp->full_block_bytes)
				{
					t->add_redundant_bytes(pbp->bytes_downloaded, torrent::piece_closing);
				}
			}

			if (t->has_picker())
			{
				piece_picker& picker = t->picker();

				while (!m_download_queue.empty())
				{
					pending_block& qe = m_download_queue.back();
					if (!qe.timed_out && !qe.not_wanted)
						picker.abort_download(qe.block, peer_info_struct());
					m_outstanding_bytes -= t->to_req(qe.block).length;
					if (m_outstanding_bytes < 0) m_outstanding_bytes = 0;
					m_download_queue.pop_back();
				}
				while (!m_request_queue.empty())
				{
					picker.abort_download(m_request_queue.back().block, peer_info_struct());
					m_request_queue.pop_back();
				}
			}
			else
			{
				m_download_queue.clear();
				m_request_queue.clear();
				m_outstanding_bytes = 0;
			}
			m_queued_time_critical = 0;

#if !defined TORRENT_DISABLE_INVARIANT_CHECKS && defined TORRENT_DEBUG
			check_invariant();
#endif
			t->remove_peer(this);
		}
		else
		{
			TORRENT_ASSERT(m_download_queue.empty());
			TORRENT_ASSERT(m_request_queue.empty());
		}

#if defined TORRENT_DEBUG && defined TORRENT_EXPENSIVE_INVARIANT_CHECKS
		// since this connection doesn't have a torrent reference
		// no torrent should have a reference to this connection either
		TORRENT_ASSERT(!m_ses.any_torrent_has_peer(this));
#endif

		m_disconnecting = true;
		error_code e;

		async_shutdown(*m_socket, m_socket);

		m_ses.close_connection(this, ec, m_queued_for_connection);
		m_queued_for_connection = false;
	}

	bool peer_connection::ignore_unchoke_slots() const
	{
		if (num_classes() == 0) return true;

		if (m_ses.ignore_unchoke_slots_set(*this)) return true;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (t && m_ses.ignore_unchoke_slots_set(*t)) return true;
		return false;
	}

	// defined in upnp.cpp
	bool is_local(address const& a);

	bool peer_connection::on_local_network() const
	{
		if (libtorrent::is_local(m_remote.address())
			|| is_loopback(m_remote.address())) return true;
		return false;
	}

	void peer_connection::get_peer_info(peer_info& p) const
	{
		TORRENT_ASSERT(!associated_torrent().expired());

		ptime now = time_now();

		p.download_rate_peak = m_download_rate_peak;
		p.upload_rate_peak = m_upload_rate_peak;
		p.rtt = m_rtt;
		p.down_speed = statistics().download_rate();
		p.up_speed = statistics().upload_rate();
		p.payload_down_speed = statistics().download_payload_rate();
		p.payload_up_speed = statistics().upload_payload_rate();
		p.pid = pid();
		p.ip = remote();
		p.pending_disk_bytes = m_outstanding_writing_bytes;
		p.pending_disk_read_bytes = m_reading_bytes;
		p.send_quota = m_quota[upload_channel];
		p.receive_quota = m_quota[download_channel];
		p.num_pieces = m_num_pieces;
		if (m_download_queue.empty()) p.request_timeout = -1;
		else p.request_timeout = total_seconds(m_requested - now)
			+ m_settings.get_int(settings_pack::request_timeout)
			+ m_timeout_extend;
#ifndef TORRENT_DISABLE_GEO_IP
		p.inet_as_name = m_inet_as_name;
#endif

		p.download_queue_time = download_queue_time();
		p.queue_bytes = m_outstanding_bytes;
		
#ifndef TORRENT_DISABLE_RESOLVE_COUNTRIES	
		p.country[0] = m_country[0];
		p.country[1] = m_country[1];
#endif

		p.total_download = statistics().total_payload_download();
		p.total_upload = statistics().total_payload_upload();
#ifndef TORRENT_NO_DEPRECATE
		p.upload_limit = -1;
		p.download_limit = -1;
		p.load_balancing = 0;
#endif

		p.download_queue_length = int(download_queue().size() + m_request_queue.size());
		p.requests_in_buffer = int(m_requests_in_buffer.size() + m_request_queue.size());
		p.target_dl_queue_length = int(desired_queue_size());
		p.upload_queue_length = int(upload_queue().size());
		p.timed_out_requests = 0;
		p.busy_requests = 0;
		for (std::vector<pending_block>::const_iterator i = m_download_queue.begin()
			, end(m_download_queue.end()); i != end; ++i)
		{
			if (i->timed_out) ++p.timed_out_requests;
			if (i->busy) ++p.busy_requests;
		}

		if (boost::optional<piece_block_progress> ret = downloading_piece_progress())
		{
			p.downloading_piece_index = ret->piece_index;
			p.downloading_block_index = ret->block_index;
			p.downloading_progress = ret->bytes_downloaded;
			p.downloading_total = ret->full_block_bytes;
		}
		else
		{
			p.downloading_piece_index = -1;
			p.downloading_block_index = -1;
			p.downloading_progress = 0;
			p.downloading_total = 0;
		}

		p.pieces = get_bitfield();
		p.last_request = now - m_last_request;
		p.last_active = now - (std::max)(m_last_sent, m_last_receive);

		// this will set the flags so that we can update them later
		p.flags = 0;
		get_specific_peer_info(p);

		p.flags |= is_seed() ? peer_info::seed : 0;
		p.flags |= m_snubbed ? peer_info::snubbed : 0;
		p.flags |= m_upload_only ? peer_info::upload_only : 0;
		p.flags |= m_endgame_mode ? peer_info::endgame_mode : 0;
		p.flags |= m_holepunch_mode ? peer_info::holepunched : 0;
		if (peer_info_struct())
		{
			torrent_peer* pi = peer_info_struct();
			TORRENT_ASSERT(pi->in_use);
			p.source = pi->source;
			p.failcount = pi->failcount;
			p.num_hashfails = pi->hashfails;
			p.flags |= pi->on_parole ? peer_info::on_parole : 0;
			p.flags |= pi->optimistically_unchoked ? peer_info::optimistic_unchoke : 0;
#ifndef TORRENT_DISABLE_GEO_IP
			p.inet_as = pi->inet_as ? pi->inet_as->first : 0xffff;
#endif
		}
		else
		{
			p.source = 0;
			p.failcount = 0;
			p.num_hashfails = 0;
#ifndef TORRENT_DISABLE_GEO_IP
			p.inet_as = 0xffff;
#endif
		}

		p.remote_dl_rate = m_remote_dl_rate;
		p.send_buffer_size = m_send_buffer.capacity();
		p.used_send_buffer = m_send_buffer.size();
		p.receive_buffer_size = m_recv_buffer.capacity() + m_disk_recv_buffer_size;
		p.used_receive_buffer = m_recv_pos;
		p.write_state = m_channel_state[upload_channel];
		p.read_state = m_channel_state[download_channel];
		
		// pieces may be empty if we don't have metadata yet
		if (p.pieces.size() == 0)
		{
			p.progress = 0.f;
			p.progress_ppm = 0;
		}
		else
		{
#if TORRENT_NO_FPU
			p.progress = 0.f;
#else
			p.progress = (float)p.pieces.count() / (float)p.pieces.size();
#endif
			p.progress_ppm = boost::uint64_t(p.pieces.count()) * 1000000 / p.pieces.size();
		}

		p.estimated_reciprocation_rate = m_est_reciprocation_rate;

		error_code ec;
		p.local_endpoint = get_socket()->local_endpoint(ec);
	}

	// allocates a disk buffer of size 'disk_buffer_size' and replaces the
	// end of the current receive buffer with it. i.e. the receive pos
	// must be <= packet_size - disk_buffer_size
	// the disk buffer can be accessed through release_disk_receive_buffer()
	// when it is queried, the responsibility to free it is transferred
	// to the caller
	bool peer_connection::allocate_disk_receive_buffer(int disk_buffer_size)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(m_packet_size > 0);
		TORRENT_ASSERT(m_recv_pos <= m_packet_size - disk_buffer_size);
		TORRENT_ASSERT(!m_disk_recv_buffer);
		TORRENT_ASSERT(disk_buffer_size <= 16 * 1024);

		if (disk_buffer_size == 0) return true;

		if (disk_buffer_size > 16 * 1024)
		{
			disconnect(errors::invalid_piece_size, 2);
			return false;
		}

		// first free the old buffer
		m_disk_recv_buffer.reset();
		// then allocate a new one

		bool exceeded = false;
		m_disk_recv_buffer.reset(m_allocator.allocate_disk_buffer(exceeded, self(), "receive buffer"));

		if (!m_disk_recv_buffer)
		{
			disconnect(errors::no_memory);
			return false;
		}

		if (exceeded)
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** exceeded disk buffer watermark");
#endif
			if ((m_channel_state[download_channel] & peer_info::bw_disk) == 0)
				m_ses.inc_disk_queue(download_channel);
			m_channel_state[download_channel] |= peer_info::bw_disk;
		}

		m_disk_recv_buffer_size = disk_buffer_size;
		return true;
	}

	char* peer_connection::release_disk_receive_buffer()
	{
		if (!m_disk_recv_buffer) return 0;

		TORRENT_ASSERT(m_disk_recv_buffer_size <= m_recv_end);
		TORRENT_ASSERT(m_recv_start <= m_recv_end - m_disk_recv_buffer_size);
		m_recv_end -= m_disk_recv_buffer_size;
		m_disk_recv_buffer_size = 0;
		return m_disk_recv_buffer.release();
	}
	
	// size = the packet size to remove from the receive buffer
	// packet_size = the next packet size to receive in the buffer
	void peer_connection::cut_receive_buffer(int size, int packet_size, int offset)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(packet_size > 0);
		TORRENT_ASSERT(int(m_recv_buffer.size()) >= size);
		TORRENT_ASSERT(int(m_recv_buffer.size()) >= m_recv_pos);
		TORRENT_ASSERT(m_recv_pos >= size + offset);
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(m_recv_buffer.size() >= m_recv_end);

		if (offset > 0)
		{
			TORRENT_ASSERT(m_recv_start - size <= m_recv_end);

			if (size > 0)
				std::memmove(&m_recv_buffer[0] + offset, &m_recv_buffer[0] + offset + size, m_recv_end - size - offset);

			m_recv_pos -= size;
			m_recv_end -= size;

#ifdef TORRENT_DEBUG
			std::fill(m_recv_buffer.begin() + m_recv_end, m_recv_buffer.end(), 0xcc);
#endif
		}
		else
		{
			TORRENT_ASSERT(m_recv_start + size <= m_recv_end);
			m_recv_start += size;
			m_recv_pos -= size;
		}

		m_packet_size = packet_size;
	}

	void peer_connection::normalize_receive_buffer()
	{
		TORRENT_ASSERT(m_recv_end >= m_recv_start);
		if (m_recv_start == 0) return;

		if (m_recv_end > m_recv_start)
			std::memmove(&m_recv_buffer[0], &m_recv_buffer[0] + m_recv_start, m_recv_end - m_recv_start);

		m_recv_end -= m_recv_start;
		m_recv_start = 0;

#ifdef TORRENT_DEBUG
		std::fill(m_recv_buffer.begin() + m_recv_end, m_recv_buffer.end(), 0xcc);
#endif
	}

	void peer_connection::superseed_piece(int replace_piece, int new_piece)
	{
		if (new_piece == -1)
		{
			if (m_superseed_piece[0] == -1) return;
			m_superseed_piece[0] = -1;
			m_superseed_piece[1] = -1;
			
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** ending super seed mode");
#endif
			boost::shared_ptr<torrent> t = m_torrent.lock();
			assert(t);

			// this will either send a full bitfield or
			// a have-all message, effectively terminating
			// super-seeding, since the peer may pick any piece
			write_bitfield();
			
			return;
		}

		assert(!has_piece(new_piece));
		
#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("==> HAVE    [ piece: %d ] (super seed)", new_piece);
#endif
		write_have(new_piece);

		if (replace_piece >= 0)
		{
			// move the piece we're replacing to the tail
			if (m_superseed_piece[0] == replace_piece)
				std::swap(m_superseed_piece[0], m_superseed_piece[1]);
		}

		m_superseed_piece[1] = m_superseed_piece[0];
		m_superseed_piece[0] = new_piece;
	}

	void peer_connection::update_desired_queue_size()
	{
		if (m_snubbed)
		{
			m_desired_queue_size = 1;
			return;
		}
	
		int download_rate = statistics().download_rate();

		// calculate the desired download queue size
		const int queue_time = m_settings.get_int(settings_pack::request_queue_time);
		// (if the latency is more than this, the download will stall)
		// so, the queue size is queue_time * down_rate / 16 kiB
		// (16 kB is the size of each request)
		// the minimum number of requests is 2 and the maximum is 48
		// the block size doesn't have to be 16. So we first query the
		// torrent for it
		boost::shared_ptr<torrent> t = m_torrent.lock();
		const int block_size = t->block_size();

		TORRENT_ASSERT(block_size > 0);
		
		m_desired_queue_size = queue_time * download_rate / block_size;

		if (m_desired_queue_size > m_max_out_request_queue)
			m_desired_queue_size = m_max_out_request_queue;
		if (m_desired_queue_size < min_request_queue)
			m_desired_queue_size = min_request_queue;
	}

	void peer_connection::second_tick(int tick_interval_ms)
	{
		ptime now = time_now();
		boost::shared_ptr<peer_connection> me(self());

		// the invariant check must be run before me is destructed
		// in case the peer got disconnected
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();

		int warning = 0;
		// drain the IP overhead from the bandwidth limiters
		if (m_settings.get_bool(settings_pack::rate_limit_ip_overhead) && t)
		{
			warning |= m_ses.use_quota_overhead(*this, m_statistics.download_ip_overhead()
				, m_statistics.upload_ip_overhead());
			warning |= m_ses.use_quota_overhead(*t, m_statistics.download_ip_overhead()
				, m_statistics.upload_ip_overhead());
		}

		if (warning && t->alerts().should_post<performance_alert>())
		{
			for (int channel = 0; channel < 2; ++channel)
			{
				if ((warning & (1 << channel)) == 0) return;
				t->alerts().post_alert(performance_alert(t->get_handle()
					, channel == peer_connection::download_channel
					? performance_alert::download_limit_too_low
					: performance_alert::upload_limit_too_low));
			}
		}

		if (!t || m_disconnecting)
		{
			m_ses.half_open_done(m_connection_ticket);
			if (m_connection_ticket >= -1) m_connection_ticket = -1;
			TORRENT_ASSERT(t || !m_connecting);
			if (m_connecting && t)
			{
				t->dec_num_connecting();
				m_connecting = false;
			}
			disconnect(errors::torrent_aborted);
			return;
		}

		if (m_endgame_mode
			&& m_interesting
			&& m_download_queue.empty()
			&& m_request_queue.empty()
			&& total_seconds(now - m_last_request) >= 5)
		{
			// this happens when we're in strict end-game
			// mode and the peer could not request any blocks
			// because they were all taken but there were still
			// unrequested blocks. Now, 5 seconds later, there
			// might not be any unrequested blocks anymore, so
			// we should try to pick another block to see
			// if we can pick a busy one
			m_ses.inc_stats_counter(aux::session_interface::end_game_piece_picks);
			m_last_request = now;
			request_a_block(*t, *this);
			if (m_disconnecting) return;
		}

		on_tick();

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			(*i)->tick();
		}
		if (is_disconnecting()) return;
#endif

		// if the peer hasn't said a thing for a certain
		// time, it is considered to have timed out
		time_duration d;
		d = (std::min)(now - m_last_receive, now - m_last_sent);

		// if we can't read, it means we're blocked on the rate-limiter
		// or the disk, not the peer itself. In this case, don't blame
		// the peer and disconnect it
		bool may_timeout = (m_channel_state[download_channel] & peer_info::bw_network);

		if (may_timeout && d > seconds(m_timeout) && !m_connecting && m_reading_bytes == 0)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** LAST ACTIVITY [ %d seconds ago ] ***", int(total_seconds(d)));
#endif
			disconnect(errors::timed_out_inactivity);
			return;
		}

		// do not stall waiting for a handshake
		if (may_timeout
			&& !m_connecting
			&& in_handshake()
			&& d > seconds(m_settings.get_int(settings_pack::handshake_timeout)))
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** NO HANDSHAKE [ waited %d seconds ] ***", int(total_seconds(d)));
#endif
			disconnect(errors::timed_out_no_handshake);
			return;
		}

		// disconnect peers that we unchoked, but
		// they didn't send a request within 20 seconds.
		// but only if we're a seed
		d = now - (std::max)(m_last_unchoke, m_last_incoming_request);
		if (may_timeout
			&& !m_connecting
			&& m_requests.empty()
			&& m_reading_bytes == 0
			&& !m_choked
			&& m_peer_interested
			&& t && t->is_upload_only()
			&& d > seconds(20))
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** NO REQUEST [ waited %d seconds ] ***", int(total_seconds(d)));
#endif
			disconnect(errors::timed_out_no_request);
			return;
		}

		// if the peer hasn't become interested and we haven't
		// become interested in the peer for 10 minutes, it
		// has also timed out.
		time_duration d1;
		time_duration d2;
		d1 = now - m_became_uninterested;
		d2 = now - m_became_uninteresting;
		time_duration time_limit = seconds(
			m_settings.get_int(settings_pack::inactivity_timeout));

		// don't bother disconnect peers we haven't been interested
		// in (and that hasn't been interested in us) for a while
		// unless we have used up all our connection slots
		if (may_timeout
			&& !m_interesting
			&& !m_peer_interested
			&& d1 > time_limit
			&& d2 > time_limit
			&& (m_ses.num_connections() >= m_settings.get_int(settings_pack::connections_limit)
			|| (t && t->num_peers() >= t->max_connections())))
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** MUTUAL NO INTEREST [ t1: %d t2: %d ]"
				, total_seconds(d1), total_seconds(d2));
#endif
			disconnect(errors::timed_out_no_interest);
			return;
		}

		if (may_timeout
			&& !m_download_queue.empty()
			&& m_quota[download_channel] > 0
			&& now > m_requested + seconds(m_settings.get_int(settings_pack::request_timeout)
			+ m_timeout_extend))
		{
			snub_peer();
		}

		// if we haven't sent something in too long, send a keep-alive
		keep_alive();

		m_statistics.second_tick(tick_interval_ms);

		if (m_statistics.upload_payload_rate() > m_upload_rate_peak)
		{
			m_upload_rate_peak = m_statistics.upload_payload_rate();
		}
		if (m_statistics.download_payload_rate() > m_download_rate_peak)
		{
			m_download_rate_peak = m_statistics.download_payload_rate();
#ifndef TORRENT_DISABLE_GEO_IP
			if (peer_info_struct())
			{
				std::pair<const int, int>* as_stats = peer_info_struct()->inet_as;
				if (as_stats && as_stats->second < m_download_rate_peak)
					as_stats->second = m_download_rate_peak;
			}
#endif
		}
		if (is_disconnecting()) return;

		if (!t->ready_for_connections()) return;

		update_desired_queue_size();

		if (m_desired_queue_size == m_max_out_request_queue 
				&& t->alerts().should_post<performance_alert>())
		{
			t->alerts().post_alert(performance_alert(t->get_handle()
				, performance_alert::outstanding_request_limit_reached));
		}

		int piece_timeout = m_settings.get_int(settings_pack::piece_timeout);

		if (!m_download_queue.empty()
			&& m_quota[download_channel] > 0
			&& now - m_last_piece > seconds(piece_timeout + m_timeout_extend))
		{
			// this peer isn't sending the pieces we've
			// requested (this has been observed by BitComet)
			// in this case we'll clear our download queue and
			// re-request the blocks.
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** PIECE_REQUEST TIMED OUT [ %d time: %d to: %d extend: %d ]"
				, (int)m_download_queue.size(), total_seconds(now - m_last_piece)
				, piece_timeout, m_timeout_extend);
#endif

			snub_peer();
		}

		// update once every minute
		if (now - m_remote_dl_update >= seconds(60))
		{
			if (m_remote_dl_rate > 0)
				m_remote_dl_rate = (m_remote_dl_rate * 2 / 3) + 
					((m_remote_bytes_dled / 3) / 60);
			else
				m_remote_dl_rate = m_remote_bytes_dled / 60;
			
			m_remote_bytes_dled = 0;
			m_remote_dl_update = now;
		}

		fill_send_buffer();
	}

	void peer_connection::snub_peer()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		if (!m_snubbed)
		{
			m_snubbed = true;
			if (t->alerts().should_post<peer_snubbed_alert>())
			{
				t->alerts().post_alert(peer_snubbed_alert(t->get_handle()
					, m_remote, m_peer_id));
			}
		}
		m_desired_queue_size = 1;

		if (on_parole())
		{
			m_timeout_extend += m_settings.get_int(settings_pack::request_timeout);
			return;
		}
		if (!t->has_picker()) return;
		piece_picker& picker = t->picker();

		// first, if we have any unsent requests, just
		// wipe those out
		while (!m_request_queue.empty())
		{
			t->picker().abort_download(m_request_queue.back().block, peer_info_struct());
			m_request_queue.pop_back();
		}
		m_queued_time_critical = 0;

		TORRENT_ASSERT(!m_download_queue.empty());

		// request a new block before removing the previous
		// one, in order to prevent it from
		// picking the same block again, stalling the
		// same piece indefinitely.
		m_desired_queue_size = 2;
		m_ses.inc_stats_counter(aux::session_interface::snubbed_piece_picks);
		request_a_block(*t, *this);

		// the block we just picked (potentially)
		// hasn't been put in m_download_queue yet.
		// it's in m_request_queue and will be sent
		// once send_block_requests() is called.

		m_desired_queue_size = 1;

		// time out the last request eligible
		// block in the queue
		int i = m_download_queue.size() - 1;
		for (; i >= 0; --i)
		{
			if (!m_download_queue[i].timed_out
				&& !m_download_queue[i].not_wanted)
				break;
		}

		if (i >= 0)
		{
			pending_block& qe = m_download_queue[i];
			piece_block r = qe.block;

			// only time out a request if it blocks the piece
			// from being completed (i.e. no free blocks to
			// request from it)
			piece_picker::downloading_piece p;
			picker.piece_info(qe.block.piece_index, p);
			int free_blocks = picker.blocks_in_piece(qe.block.piece_index)
				- p.finished - p.writing - p.requested;
			if (free_blocks > 0)
			{
				m_timeout_extend += m_settings.get_int(settings_pack::request_timeout);
				return;
			}

			if (t->alerts().should_post<block_timeout_alert>())
			{
				t->alerts().post_alert(block_timeout_alert(t->get_handle()
					, remote(), pid(), qe.block.block_index, qe.block.piece_index));
			}
			qe.timed_out = true;
			picker.abort_download(r, peer_info_struct());
		}

		send_block_requests();
	}

	int peer_connection::preferred_caching() const
	{
		int line_size = 0;
		if (m_settings.get_bool(settings_pack::guided_read_cache))
		{
			boost::shared_ptr<torrent> t = m_torrent.lock();
			int upload_rate = m_statistics.upload_payload_rate();
			if (upload_rate == 0) upload_rate = 1;

			int num_uploads = m_ses.num_uploads();
			if (num_uploads == 0) num_uploads = 1;

			// assume half of the cache is write cache if we're downloading
			// this torrent as well
			int cache_size = m_settings.get_int(settings_pack::cache_size) / num_uploads;
			if (!t->is_upload_only()) cache_size /= 2;
			// cache_size is the amount of cache we have per peer. The
			// cache line should not be greater than this

			line_size = cache_size;
		}
		return line_size;
	}

	void peer_connection::fill_send_buffer()
	{
#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
		INVARIANT_CHECK;
#endif

		bool sent_a_piece = false;
		boost::shared_ptr<torrent> t = m_torrent.lock();
		if (!t) return;

		// only add new piece-chunks if the send buffer is small enough
		// otherwise there will be no end to how large it will be!
		
		boost::uint64_t upload_rate = int(m_statistics.upload_rate());

		int buffer_size_watermark = upload_rate
			* m_settings.get_int(settings_pack::send_buffer_watermark_factor) / 100;

		if (buffer_size_watermark < m_settings.get_int(settings_pack::send_buffer_low_watermark))
		{
			buffer_size_watermark = m_settings.get_int(settings_pack::send_buffer_low_watermark);
		}
		else if (buffer_size_watermark > m_settings.get_int(settings_pack::send_buffer_watermark))
		{
			buffer_size_watermark = m_settings.get_int(settings_pack::send_buffer_watermark);
		}

		// don't just pop the front element here, since in seed mode one request may
		// be blocked because we have to verify the hash first, so keep going with the
		// next request. However, only let each peer have one hash verification outstanding
		// at any given time
		for (int i = 0; i < m_requests.size()
			&& (send_buffer_size() + m_reading_bytes < buffer_size_watermark); ++i)
		{
			TORRENT_ASSERT(t->ready_for_connections());
			peer_request& r = m_requests[i];
			
			TORRENT_ASSERT(r.piece >= 0);
			TORRENT_ASSERT(r.piece < (int)m_have_piece.size());
//			TORRENT_ASSERT(t->have_piece(r.piece));
			TORRENT_ASSERT(r.start + r.length <= t->torrent_file().piece_size(r.piece));
			TORRENT_ASSERT(r.length > 0 && r.start >= 0);

			if (t->seed_mode() && !t->verified_piece(r.piece))
			{
				// we're still verifying the hash of this piece
				// so we can't return it yet.
				if (t->verifying_piece(r.piece)) continue;

				// only have three outstanding hash check per peer
				if (m_outstanding_piece_verification >= 3) continue;

				++m_outstanding_piece_verification;

#ifdef TORRENT_VERBOSE_LOGGING
				peer_log("*** FILE ASYNC HASH [ piece: %d ]", r.piece);
#endif
				// this means we're in seed mode and we haven't yet
				// verified this piece (r.piece)
				if (!t->need_loaded()) return;
				t->inc_refcount();
				m_disk_thread.async_hash(&t->storage(), r.piece, 0
					, boost::bind(&peer_connection::on_seed_mode_hashed, self(), _1)
					, this);
				t->verifying(r.piece);
				continue;
			}

			// in seed mode, we might end up accepting a request
			// which it later turns out we cannot serve, if we ended
			// up not having that piece
			if (!t->has_piece_passed(r.piece))
			{
				// we don't have this piece yet, but we anticipate to have
				// it very soon, so we have told our peers we have it.
				// hold off on sending it. If the piece fails later
				// we will reject this request
				if (t->is_predictive_piece(r.piece)) continue;
#if defined TORRENT_VERBOSE_LOGGING
				peer_log("==> REJECT_PIECE [ piece: %d s: %d l: %d ]"
					, r.piece , r.start , r.length);
#endif
				write_reject_request(r);
			}
			else
			{
#ifdef TORRENT_VERBOSE_LOGGING
				peer_log("*** FILE ASYNC READ [ piece: %d | s: %d | l: %d ]"
					, r.piece, r.start, r.length);
#endif
				m_reading_bytes += r.length;
				sent_a_piece = true;

				// the callback function may be called immediately, instead of being posted
				if (!t->need_loaded()) return;
				t->inc_refcount();
				m_disk_thread.async_read(&t->storage(), r
					, boost::bind(&peer_connection::on_disk_read_complete
					, self(), _1, r), this);
			}
			m_requests.erase(m_requests.begin() + i);
			--i;
		}

		if (t->share_mode() && sent_a_piece)
			t->recalc_share_mode();
	}

	// this is called when a previously unchecked piece has been
	// checked, while in seed-mode
	void peer_connection::on_seed_mode_hashed(disk_io_job const* j)
	{
		TORRENT_ASSERT(m_ses.is_single_thread());
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		torrent_ref_holder h(t.get());
		if (t) t->dec_refcount();

		TORRENT_ASSERT(m_outstanding_piece_verification > 0);
		--m_outstanding_piece_verification;

		if (!t) return;

		// we're using the piece hashes here, we need the torrent to be loaded
		if (!t->need_loaded()) return;

		if (!m_settings.get_bool(settings_pack::disable_hash_checks)
			&& sha1_hash(j->d.piece_hash) != t->torrent_file().hash_for_piece(j->piece))
		{
			t->leave_seed_mode(false);
		}
		else
		{
			TORRENT_ASSERT(t->verifying_piece(j->piece));
			if (t->seed_mode()) t->verified(j->piece);

			if (t)
			{
				if (t->seed_mode() && t->all_verified())
					t->leave_seed_mode(true);
			}
		}

		// try to service the requests again, now that the piece
		// has been verified
		fill_send_buffer();
	}

	void peer_connection::on_disk_read_complete(disk_io_job const* j, peer_request r)
	{
		// return value:
		// 0: success, piece passed hash check
		// -1: disk failure

		TORRENT_ASSERT(m_ses.is_single_thread());

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("*** FILE ASYNC READ COMPLETE [ ret: %d | piece: %d | s: %d | l: %d"
			" | b: %p | c: %s | e: %s ]"
			, j->ret, r.piece, r.start, r.length, j->buffer
			, (j->flags & disk_io_job::cache_hit ? "cache hit" : "cache miss")
			, j->error.ec.message().c_str());
#endif
		m_reading_bytes -= r.length;

		boost::shared_ptr<torrent> t = m_torrent.lock();
		torrent_ref_holder h(t.get());
		if (t) t->dec_refcount();

		if (j->ret < 0)
		{
			if (!t)
			{
				disconnect(j->error.ec);
				return;
			}
		
			TORRENT_ASSERT(j->buffer == 0);
			write_dont_have(r.piece);
			write_reject_request(r);
			if (t->alerts().should_post<file_error_alert>())
				t->alerts().post_alert(file_error_alert(j->error.ec
					, t->resolve_filename(j->error.file)
					, j->error.operation_str(), t->get_handle()));

			++m_disk_read_failures;
			if (m_disk_read_failures > 100) disconnect(j->error.ec);
			return;
		}

		// we're only interested in failures in a row.
		// if we every now and then successfully send a
		// block, the peer is still useful
		m_disk_read_failures = 0;

		TORRENT_ASSERT(j->ret == r.length);

		// even if we're disconnecting, we need to free this block
		// otherwise the disk thread will hang, waiting for the network
		// thread to be done with it
		disk_buffer_holder buffer(m_allocator, *j);

		if (m_disconnecting) return;

		// flush send buffer at the end of
		// this burst of disk events
//		m_ses.cork_burst(this);

#if TORRENT_BUFFER_STATS
		if (j->buffer && j->d.io.ref.storage == 0)
			m_disk_thread.rename_buffer(j->buffer, "received send buffer");
#endif

		if (!t)
		{
			disconnect(j->error.ec);
			return;
		}
		
		if (j->ret != r.length)
		{
			// handle_disk_error may disconnect us
			t->handle_disk_error(j, this);
			return;
		}

#if defined TORRENT_VERBOSE_LOGGING
		peer_log("==> PIECE   [ piece: %d s: %d l: %d ]"
			, r.piece, r.start, r.length);
#endif

#if TORRENT_BUFFER_STATS
		if (j->buffer && j->d.io.ref.storage == 0)
			m_disk_thread.rename_buffer(j->buffer, "dispatched send buffer");
#endif

		// we probably just pulled this piece into the cache.
		// if it's rare enough to make it into the suggested piece
		// push another piece out
		if (m_settings.get_int(settings_pack::suggest_mode) == settings_pack::suggest_read_cache
			&& (j->flags & disk_io_job::cache_hit) == 0)
		{
			t->add_suggest_piece(r.piece);
		}
		write_piece(r, buffer);
	}

	void peer_connection::assign_bandwidth(int channel, int amount)
	{
#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("%s ASSIGN BANDWIDHT [ bytes: %d ]"
			, channel == upload_channel ? ">>>" : "<<<", amount);
#endif

		TORRENT_ASSERT(amount > 0);
		m_quota[channel] += amount;
		TORRENT_ASSERT(m_channel_state[channel] & peer_info::bw_limit);
		m_channel_state[channel] &= ~peer_info::bw_limit;
		if (channel == upload_channel)
		{
			setup_send();
		}
		else if (channel == download_channel)
		{
			setup_receive();
		}
	}

	int peer_connection::request_upload_bandwidth()
	{
		shared_ptr<torrent> t = m_torrent.lock();

		int priority;
		if (t && m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::bittyrant_choker
			&& !t->upload_mode() && !t->is_upload_only())
		{
			// when we use the bittyrant choker, the priority of a peer
			// is decided based on the estimated reciprocation rate and
			// the share it represents of the total upload rate capacity
			// the torrent priority is taken into account when unchoking peers
			int upload_capacity = m_settings.get_int(settings_pack::upload_rate_limit);
			if (upload_capacity == 0)
			{
				// we don't know at what rate we can upload. If we have a
				// measurement of the peak, use that + 10kB/s, otherwise
				// assume 20 kB/s
				upload_capacity = (std::max)(20000, m_ses.peak_up_rate() + 10000);
			}
			int estimated_reciprocation_rate = m_est_reciprocation_rate;
			// we cannot send faster than our upload rate anyway
			if (estimated_reciprocation_rate < upload_capacity)
				estimated_reciprocation_rate = upload_capacity;

			priority = (boost::uint64_t(estimated_reciprocation_rate) << 14) / upload_capacity;
			if (priority > 0xffff) priority = 0xffff;
		}
		else
		{
			priority = 1 + is_interesting() * 2 + m_requests_in_buffer.size();
			if (priority > 255) priority = 255;
			priority |= get_priority(upload_channel) << 8;
		}
		TORRENT_ASSERT(priority <= 0xffff);

		int bytes = (std::max)(m_send_buffer.size()
			, (m_statistics.upload_rate() * 2 * m_settings.get_int(settings_pack::tick_interval)) / 1000);

		// peers that we are not interested in are non-prioritized
		return request_bandwidth(upload_channel, priority, bytes);
	}

	int peer_connection::request_download_bandwidth(int bytes)
	{
		shared_ptr<torrent> t = m_torrent.lock();

		TORRENT_ASSERT(m_outstanding_bytes >= 0);

		if (bytes == 0)
		{
			bytes = (std::max)((std::max)(m_outstanding_bytes, m_packet_size - m_recv_pos) + 30
				, m_statistics.download_rate() * 2
				/ (1000 / m_settings.get_int(settings_pack::tick_interval)));
		}
		return request_bandwidth(download_channel, get_priority(download_channel), bytes);
	}

	int peer_connection::request_bandwidth(int channel, int priority, int bytes)
	{
		shared_ptr<torrent> t = m_torrent.lock();

		int max_channels = num_classes() + (t ? t->num_classes() : 0) + 2;
		bandwidth_channel** channels = TORRENT_ALLOCA(bandwidth_channel*, max_channels);

		// collect the pointers to all bandwidth channels
		// that apply to this torrent
		int c = 0;

		c += m_ses.copy_pertinent_channels(*this, channel
			, channels + c, max_channels - c);
		if (t)
		{
			c += m_ses.copy_pertinent_channels(*t, channel
				, channels + c, max_channels - c);
		}

#ifdef TORRENT_DEBUG
		// make sure we don't have duplicates
		std::set<bandwidth_channel*> unique_classes;
		for (int i = 0; i < c; ++i)
		{
			TORRENT_ASSERT(unique_classes.count(channels[i]) == 0);
			unique_classes.insert(channels[i]);
		}
#endif

#ifdef TORRENT_VERBOSE_LOGGING
		if (channel == download_channel)
		{
			peer_log("<<< REQUEST_BANDWIDTH [ download: %d prio: %d "
				"num_channels: %d ]", int(m_download_queue.size() * 16 * 1024 + 30), priority, c);
		}
		else
		{
			peer_log(">>> REQUEST_BANDWIDTH [ upload: %d prio: %d "
				"num_channels: %d ]", int(m_send_buffer.size()), priority, c);
		}
#endif

		TORRENT_ASSERT((m_channel_state[channel] & peer_info::bw_limit) == 0);

		bandwidth_manager* manager = m_ses.get_bandwidth_manager(channel);

		int ret = manager->request_bandwidth(self()
			, bytes, priority, channels, c);

		if (ret == 0) m_channel_state[channel] |= peer_info::bw_limit;

		return ret;
	}

	void peer_connection::uncork_socket()
	{
		if (!m_corked) return;
		m_corked = false;
		setup_send();
	}

	void peer_connection::setup_send()
	{
		if (m_disconnecting) return;

		if (m_channel_state[upload_channel] & (peer_info::bw_network | peer_info::bw_limit)) return;
		
		if (m_quota[upload_channel] == 0
			&& !m_send_buffer.empty()
			&& !m_connecting)
		{
			int ret = request_upload_bandwidth();
			if (ret == 0)
			{
				m_channel_state[upload_channel] |= peer_info::bw_limit;
				return;
			}

			// we were just assigned 'ret' quota
			TORRENT_ASSERT(ret > 0);
			m_quota[upload_channel] += ret;

#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("<<< ASSIGN BANDWIDTH [ bytes: %d ]", ret);
#endif
		}

		int quota_left = m_quota[upload_channel];

		if (m_send_buffer.empty()
			&& m_reading_bytes > 0
			&& quota_left > 0)
		{
			if ((m_channel_state[upload_channel] & peer_info::bw_disk) == 0)
				m_ses.inc_disk_queue(upload_channel);
			m_channel_state[upload_channel] |= peer_info::bw_disk;
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("*** exceeded disk buffer watermark");
#endif

			if (!m_connecting
				&& !m_requests.empty()
				&& m_reading_bytes > m_settings.get_int(settings_pack::send_buffer_watermark) - 0x4000)
			{
				boost::shared_ptr<torrent> t = m_torrent.lock();

				// we're stalled on the disk. We want to write and we can write
				// but our send buffer is empty, waiting to be refilled from the disk
				// this either means the disk is slower than the network connection
				// or that our send buffer watermark is too small, because we can
				// send it all before the disk gets back to us. That's why we only
				// trigger this if we've also filled the allowed send buffer. The
				// first request would not fill it all the way up because of the
				// upload rate being virtually 0. If m_requests is empty, it doesn't
				// matter anyway, because we don't have any more requests from the
				// peer to hang on to the disk
				if (t && t->alerts().should_post<performance_alert>())
				{
					t->alerts().post_alert(performance_alert(t->get_handle()
						, performance_alert::send_buffer_watermark_too_low));
				}
			}
		}
		else
		{
			if (m_channel_state[upload_channel] & peer_info::bw_disk)
				m_ses.dec_disk_queue(upload_channel);
			m_channel_state[upload_channel] &= ~peer_info::bw_disk;
		}

		if (!can_write())
		{
#ifdef TORRENT_VERBOSE_LOGGING
			if (m_send_buffer.empty())
			{
				peer_log(">>> SEND BUFFER DEPLETED ["
					" quota: %d buf: %d connecting: %s disconnecting: %s pending_disk: %d ]"
					, m_quota[upload_channel]
					, int(m_send_buffer.size()), m_connecting?"yes":"no"
					, m_disconnecting?"yes":"no", m_reading_bytes);
			}
			else
			{
				peer_log(">>> CANNOT WRITE ["
					" quota: %d buf: %d connecting: %s disconnecting: %s pending_disk: %d ]"
					, m_quota[upload_channel]
					, int(m_send_buffer.size()), m_connecting?"yes":"no"
					, m_disconnecting?"yes":"no", m_reading_bytes);
			}
#endif
			return;
		}

		// send the actual buffer
		int amount_to_send = m_send_buffer.size();
		if (amount_to_send > quota_left)
			amount_to_send = quota_left;

		TORRENT_ASSERT(amount_to_send > 0);

		if (m_corked)
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log(">>> CORKED WRITE [ bytes: %d ]", amount_to_send);
#endif
			return;
		}

		TORRENT_ASSERT((m_channel_state[upload_channel] & peer_info::bw_network) == 0);
#ifdef TORRENT_VERBOSE_LOGGING
		peer_log(">>> ASYNC_WRITE [ bytes: %d ]", amount_to_send);
#endif
		std::vector<asio::const_buffer> const& vec = m_send_buffer.build_iovec(amount_to_send);
#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("peer_connection::on_send_data");
#endif

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(!m_socket_is_writing);
		m_socket_is_writing = true;
#endif
		if (is_utp(*m_socket))
		{
			m_socket->async_write_some(vec, make_write_handler(boost::bind(
				&peer_connection::on_send_data, self(), _1, _2)));
		}
		else
		{
			write_some_job j;
			j.vec = &vec;
			j.peer = self();
			m_ses.post_socket_write_job(j);
		}

		m_channel_state[upload_channel] |= peer_info::bw_network;
	}

	void peer_connection::on_disk()
	{
		if ((m_channel_state[download_channel] & peer_info::bw_disk) == 0) return;
		boost::shared_ptr<peer_connection> me(self());
	
#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("*** dropped below disk buffer watermark");
#endif
		m_ses.dec_disk_queue(download_channel);
		m_channel_state[download_channel] &= ~peer_info::bw_disk;
		setup_receive(read_async);
	}

	void peer_connection::setup_receive(sync_t sync)
	{
		INVARIANT_CHECK;

		if (m_disconnecting) return;
		if (m_channel_state[download_channel] & (peer_info::bw_network | peer_info::bw_limit)) return;

		if (m_quota[download_channel] == 0
			&& !m_connecting)
		{
			int ret = request_download_bandwidth();
			if (ret == 0)
			{
				m_channel_state[download_channel] |= peer_info::bw_limit;
				return;
			}

			// we were just assigned 'ret' quota
			TORRENT_ASSERT(ret > 0);
			m_quota[download_channel] += ret;

#ifdef TORRENT_VERBOSE_LOGGING
			peer_log(">>> ASSIGN BANDWIDTH [ bytes: %d ]", ret);
#endif
		}
		
		if (!can_read())
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("<<< CANNOT READ [ quota: %d  "
				"can-write-to-disk: %s queue-limit: %d disconnecting: %s ]"
				, m_quota[download_channel]
				, ((m_channel_state[download_channel] & peer_info::bw_disk)?"no":"yes")
				, m_settings.get_int(settings_pack::max_queued_disk_bytes)
				, (m_disconnecting?"yes":"no"));
#endif
			// if we block reading, waiting for the disk, we will wake up
			// by the disk_io_thread posting a message every time it drops
			// from being at or exceeding the limit down to below the limit
			return;
		}
		error_code ec;

		try_read(read_async, ec);
	}

	size_t peer_connection::try_read(sync_t s, error_code& ec)
	{
		if (m_quota[download_channel] == 0)
		{
			ec = asio::error::would_block;
			return 0;
		}
		
		if (!can_read())
		{
			ec = asio::error::would_block;
			return 0;
		}

		int max_receive = m_packet_size - m_recv_pos;

		boost::array<asio::mutable_buffer, 2> vec;
		int num_bufs = 0;
		// only apply the contiguous receive buffer when we don't have any
		// outstanding requests. When we're likely to receive pieces, we'll
		// save more time from avoiding copying data from the socket
		if ((m_settings.get_bool(settings_pack::contiguous_recv_buffer)
			|| m_download_queue.empty()) && !m_disk_recv_buffer)
		{
			if (s == read_sync)
			{
				ec = asio::error::would_block;
				return 0;
			}

			TORRENT_ASSERT((m_channel_state[download_channel] & peer_info::bw_network) == 0);
			m_channel_state[download_channel] |= peer_info::bw_network;
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("<<< ASYNC_READ      [ ]");
#endif

#if defined TORRENT_ASIO_DEBUGGING
			add_outstanding_async("peer_connection::on_receive_data");
#endif
			m_socket->async_read_some(asio::null_buffers(), make_read_handler(
				boost::bind(&peer_connection::on_receive_data, self(), _1, _2, true)));
			return 0;
		}

		TORRENT_ASSERT(m_packet_size > 0);
		TORRENT_ASSERT(max_receive >= 0);

		if (m_recv_pos >= m_soft_packet_size) m_soft_packet_size = 0;
		if (m_soft_packet_size && max_receive > m_soft_packet_size - m_recv_pos)
			max_receive = m_soft_packet_size - m_recv_pos;
		int quota_left = m_quota[download_channel];
		if (max_receive > quota_left)
			max_receive = quota_left;

		if (max_receive == 0)
		{
			ec = asio::error::would_block;
			return 0;
		}

		TORRENT_ASSERT(m_recv_pos >= 0);
		TORRENT_ASSERT(m_packet_size > 0);

		int regular_buffer_size = m_packet_size - m_disk_recv_buffer_size;

		if (int(m_recv_buffer.size()) < regular_buffer_size)
			m_recv_buffer.resize(round_up8(regular_buffer_size));

		if (!m_disk_recv_buffer || regular_buffer_size >= m_recv_pos + max_receive)
		{
			// only receive into regular buffer
			TORRENT_ASSERT(m_recv_pos + max_receive <= int(m_recv_buffer.size()));
			vec[0] = asio::buffer(&m_recv_buffer[m_recv_pos], max_receive);
			num_bufs = 1;
		}
		else if (m_recv_pos >= regular_buffer_size)
		{
			// only receive into disk buffer
			TORRENT_ASSERT(m_recv_pos - regular_buffer_size >= 0);
			TORRENT_ASSERT(m_recv_pos - regular_buffer_size + max_receive <= m_disk_recv_buffer_size);
			vec[0] = asio::buffer(m_disk_recv_buffer.get() + m_recv_pos - regular_buffer_size, max_receive);
			num_bufs = 1;
		}
		else
		{
			// receive into both regular and disk buffer
			TORRENT_ASSERT(max_receive + m_recv_pos > regular_buffer_size);
			TORRENT_ASSERT(m_recv_pos < regular_buffer_size);
			TORRENT_ASSERT(max_receive - regular_buffer_size
				+ m_recv_pos <= m_disk_recv_buffer_size);

			vec[0] = asio::buffer(&m_recv_buffer[m_recv_pos]
				, regular_buffer_size - m_recv_pos);
			vec[1] = asio::buffer(m_disk_recv_buffer.get()
				, max_receive - regular_buffer_size + m_recv_pos);
			num_bufs = 2;
		}

		if (s == read_async)
		{
			TORRENT_ASSERT((m_channel_state[download_channel] & peer_info::bw_network) == 0);
			m_channel_state[download_channel] |= peer_info::bw_network;
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("<<< ASYNC_READ      [ max: %d bytes ]", max_receive);
#endif

#if defined TORRENT_ASIO_DEBUGGING
			add_outstanding_async("peer_connection::on_receive_data");
#endif
			if (num_bufs == 1)
			{
				m_socket->async_read_some(
					asio::mutable_buffers_1(vec[0]), make_read_handler(
						boost::bind(&peer_connection::on_receive_data, self(), _1, _2, false)));
			}
			else
			{
				m_socket->async_read_some(
					vec, make_read_handler(
						boost::bind(&peer_connection::on_receive_data, self(), _1, _2, false)));
			}
			return 0;
		}

		size_t ret = 0;
		if (num_bufs == 1)
		{
			ret = m_socket->read_some(asio::mutable_buffers_1(vec[0]), ec);
		}
		else
		{
			ret = m_socket->read_some(vec, ec);
		}
		// this is weird. You would imagine read_some() would do this
		if (ret == 0 && !ec) ec = asio::error::eof;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<<< SYNC_READ [ max: %d ret: %d e: %s ]", max_receive, ret, ec ? ec.message().c_str() : "");
#endif
		return ret;
	}

#ifndef TORRENT_DISABLE_ENCRYPTION

	// returns the last 'bytes' from the receive buffer
	std::pair<buffer::interval, buffer::interval> peer_connection::wr_recv_buffers(int bytes)
	{
		TORRENT_ASSERT(bytes <= m_recv_pos);

		std::pair<buffer::interval, buffer::interval> vec;
		int regular_buffer_size = m_packet_size - m_disk_recv_buffer_size;
		TORRENT_ASSERT(regular_buffer_size >= 0);
		if (!m_disk_recv_buffer || regular_buffer_size >= m_recv_pos)
		{
			vec.first = buffer::interval(&m_recv_buffer[0] + m_recv_start
				+ m_recv_pos - bytes, &m_recv_buffer[0] + m_recv_start + m_recv_pos);
			vec.second = buffer::interval(0,0);
		}
		else if (m_recv_pos - bytes >= regular_buffer_size)
		{
			vec.first = buffer::interval(m_disk_recv_buffer.get() + m_recv_pos
				- regular_buffer_size - bytes, m_disk_recv_buffer.get() + m_recv_pos
				- regular_buffer_size);
			vec.second = buffer::interval(0,0);
		}
		else
		{
			TORRENT_ASSERT(m_recv_pos - bytes < regular_buffer_size);
			TORRENT_ASSERT(m_recv_pos > regular_buffer_size);
			vec.first = buffer::interval(&m_recv_buffer[0] + m_recv_start + m_recv_pos - bytes
				, &m_recv_buffer[0] + m_recv_start + regular_buffer_size);
			vec.second = buffer::interval(m_disk_recv_buffer.get()
				, m_disk_recv_buffer.get() + m_recv_pos - regular_buffer_size);
		}
		TORRENT_ASSERT(vec.first.left() + vec.second.left() == bytes);
		return vec;
	}
#endif

	void peer_connection::reset_recv_buffer(int packet_size)
	{
		TORRENT_ASSERT(m_recv_buffer.size() >= m_recv_end);
		TORRENT_ASSERT(packet_size > 0);
		if (m_recv_end > m_packet_size)
		{
			cut_receive_buffer(m_packet_size, packet_size);
			return;
		}

		m_recv_pos = 0;
		m_recv_start = 0;
		m_recv_end = 0;
		m_packet_size = packet_size;
	}

	void peer_connection::append_send_buffer(char* buffer, int size
		, boost::function<void(char*)> const& destructor, bool encrypted)
	{
#if defined TORRENT_BUFFER_STATS
		log_buffer_usage(buffer, size, "queued send buffer");
#endif
		// bittorrent connections should never use this function, since
		// they might be encrypted and this would circumvent the actual
		// encryption. bt_peer_connection overrides this function with
		// its own version.
		TORRENT_ASSERT(encrypted || type() != bittorrent_connection);
		m_send_buffer.append_buffer(buffer, size, size, destructor);
	}

	void peer_connection::append_const_send_buffer(char const* buffer, int size
		, boost::function<void(char*)> const& destructor)
	{
		m_send_buffer.append_buffer((char*)buffer, size, size, destructor);
#if defined TORRENT_STATS && defined TORRENT_BUFFER_STATS
		m_ses.buffer_usage_logger() << log_time() << " append_const_send_buffer: " << size << std::endl;
		m_ses.log_buffer_usage();
#endif
	}

	void peer_connection::send_buffer(char const* buf, int size, int flags
		, void (*fun)(char*, int, void*), void* userdata)
	{
		if (flags == message_type_request)
			m_requests_in_buffer.push_back(m_send_buffer.size() + size);

		int free_space = m_send_buffer.space_in_last_buffer();
		if (free_space > size) free_space = size;
		if (free_space > 0)
		{
			char* dst = m_send_buffer.append(buf, free_space);
			TORRENT_ASSERT(dst != 0);
			if (fun) fun(dst, free_space, userdata);
			size -= free_space;
			buf += free_space;
#if defined TORRENT_STATS && defined TORRENT_BUFFER_STATS
			m_ses.buffer_usage_logger() << log_time() << " send_buffer: "
				<< free_space << std::endl;
			m_ses.log_buffer_usage();
#endif
		}
		if (size <= 0) return;

#if defined TORRENT_STATS && defined TORRENT_BUFFER_STATS
		m_ses.buffer_usage_logger() << log_time() << " send_buffer_alloc: " << size << std::endl;
		m_ses.log_buffer_usage();
#endif
		int i = 0;
		while (size > 0)
		{
			char* chain_buf = m_ses.allocate_buffer();
			if (chain_buf == 0)
			{
				disconnect(errors::no_memory);
				return;
			}

			const int alloc_buf_size = m_ses.send_buffer_size();
			int buf_size = (std::min)(alloc_buf_size, size);
			memcpy(chain_buf, buf, buf_size);
			if (fun) fun(chain_buf, buf_size, userdata);
			buf += buf_size;
			size -= buf_size;
			m_send_buffer.append_buffer(chain_buf, alloc_buf_size, buf_size
				, boost::bind(&aux::session_interface::free_buffer, boost::ref(m_ses), _1));
			++i;
		}
		setup_send();
	}

	template<class T>
	struct set_to_zero
	{
		set_to_zero(T& v, bool cond): m_val(v), m_cond(cond) {}
		void fire() { if (!m_cond) return; m_cond = false; m_val = 0; }
		~set_to_zero() { if (m_cond) m_val = 0; }
	private:
		T& m_val;
		bool m_cond;
	};

	// --------------------------
	// RECEIVE DATA
	// --------------------------

	// nb is true if this callback is due to a null_buffers()
	// invocation of async_read_some(). In that case, we need
	// to disregard bytes_transferred.
	void peer_connection::on_receive_data(const error_code& error
		, std::size_t bytes_transferred, bool nb)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("peer_connection::on_receive_data");
#endif
#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<<< ON_RECEIVE_DATA [ bytes: %d error: %s ]"
			, bytes_transferred, error.message().c_str());
#endif

		// leave this bit set until we're done looping, reading from the socket.
		// that way we don't trigger any async read calls until the end of this
		// function.
		TORRENT_ASSERT(m_channel_state[download_channel] & peer_info::bw_network);

		TORRENT_ASSERT(m_ses.is_single_thread());

		// submit all disk jobs later
		m_ses.deferred_submit_jobs();

		// keep ourselves alive in until this function exits in
		// case we disconnect
		// this needs to be created before the invariant check,
		// to keep the object alive through the exit check
		boost::shared_ptr<peer_connection> me(self());

		// flush the send buffer at the end of this function
		cork _c(*this);

		INVARIANT_CHECK;

		int bytes_in_loop = bytes_transferred;

		if (error)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("*** ERROR [ in peer_connection::on_receive_data error: %s ]"
				, error.message().c_str());
#endif
			trancieve_ip_packet(bytes_in_loop, m_remote.address().is_v6());
			on_receive(error, bytes_transferred);
			disconnect(error);
			return;
		}

		// nb is short for null_buffers. In this mode we don't actually
		// allocate a receive buffer up-front, but get notified when
		// we can read from the socket, and then determine how much there
		// is to read.
		if (nb)
		{
			error_code ec;
			std::size_t buffer_size = m_socket->available(ec);
			if (ec)
			{
				disconnect(ec);
				return;
			}

#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("<<< READ_AVAILABLE [ bytes: %d ]", buffer_size);
#endif

			// at this point the ioctl told us the socket doesn't have any
			// pending bytes. This probably means some error happened.
			// in order to find out though, we need to initiate a read
			// operation
			if (buffer_size == 0)
			{
				// try to read one byte. The socket is non-blocking anyway
				// so worst case, we'll failed with EWOULDBLOCK
				buffer_size = 1;
			}
			else
			{
				if (buffer_size > m_quota[download_channel])
				{
					if ((m_channel_state[download_channel] & peer_info::bw_limit) == 0)
					{
						int ret = request_download_bandwidth(buffer_size - m_quota[download_channel]);
						if (ret > 0)
						{
							m_quota[download_channel] += ret;
							m_channel_state[download_channel] &= ~peer_info::bw_limit;
						}
					}
	
					buffer_size = m_quota[download_channel];
				}
				// we're already waiting to get some more
				// quota from the bandwidth manager
				if (buffer_size == 0) return;
			}

			// TODO: cap the size of the receive buffer? Maybe we can just rely
			// on the OS buffer size limit on TCP buffers and assume it won't
			// buffer more than necessary.
			m_recv_buffer.resize(m_recv_pos + buffer_size);
			TORRENT_ASSERT(m_recv_start == 0);
			bytes_transferred = m_socket->read_some(asio::buffer(&m_recv_buffer[m_recv_pos]
				, buffer_size), ec);
			if (ec)
			{
				disconnect(ec);
				return;
			}
		}

		m_ses.inc_stats_counter(aux::session_interface::on_read_counter);
		m_ses.received_buffer(bytes_transferred);

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("<<< ON_RECEIVE_DATA [ bytes: %d error: %s ]"
			, bytes_transferred, error.message().c_str());
#endif

		if (m_extension_outstanding_bytes > 0)
			m_extension_outstanding_bytes -= (std::min)(m_extension_outstanding_bytes, int(bytes_transferred));

		int num_loops = 0;
		do
		{
#ifdef TORRENT_VERBOSE_LOGGING
			peer_log("<<< read %d bytes", int(bytes_transferred));
#endif
			// correct the dl quota usage, if not all of the buffer was actually read
			TORRENT_ASSERT(int(bytes_transferred) <= m_quota[download_channel]);
			m_quota[download_channel] -= bytes_transferred;

			if (m_disconnecting)
			{
				trancieve_ip_packet(bytes_in_loop, m_remote.address().is_v6());
				return;
			}
	
			TORRENT_ASSERT(m_packet_size > 0);
			TORRENT_ASSERT(bytes_transferred > 0);

			m_recv_end += bytes_transferred;
			TORRENT_ASSERT(m_recv_pos <= int(m_recv_buffer.size()
				+ m_disk_recv_buffer_size));

			int bytes = bytes_transferred;
			int sub_transferred = 0;
			do {
				INVARIANT_CHECK;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
				size_type cur_payload_dl = m_statistics.last_payload_downloaded();
				size_type cur_protocol_dl = m_statistics.last_protocol_downloaded();
#endif
				int packet_size = m_soft_packet_size ? m_soft_packet_size : m_packet_size;
				int limit = packet_size > m_recv_pos ? packet_size - m_recv_pos : packet_size;
				sub_transferred = (std::min)(bytes, limit);
				m_recv_pos += sub_transferred;
				on_receive(error, sub_transferred);
				bytes -= sub_transferred;
				TORRENT_ASSERT(sub_transferred > 0);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
				TORRENT_ASSERT(m_statistics.last_payload_downloaded() - cur_payload_dl >= 0);
				TORRENT_ASSERT(m_statistics.last_protocol_downloaded() - cur_protocol_dl >= 0);
				size_type stats_diff = m_statistics.last_payload_downloaded() - cur_payload_dl +
					m_statistics.last_protocol_downloaded() - cur_protocol_dl;
				TORRENT_ASSERT(stats_diff == int(sub_transferred));
#endif
			if (m_disconnecting) return;

			} while (bytes > 0 && sub_transferred > 0);

			normalize_receive_buffer();

			TORRENT_ASSERT(m_recv_pos == m_recv_end);

			TORRENT_ASSERT(m_packet_size > 0);

			if (m_peer_choked
				&& m_recv_pos == 0
				&& (m_recv_buffer.capacity() - m_packet_size) > 128)
			{
				// round up to an even 8 bytes since that's the RC4 blocksize
				buffer(round_up8(m_packet_size)).swap(m_recv_buffer);
			}

			if (m_recv_pos >= m_soft_packet_size) m_soft_packet_size = 0;

			if (num_loops > 10 || nb) break;

			error_code ec;
			bytes_transferred = try_read(read_sync, ec);
			TORRENT_ASSERT(bytes_transferred > 0 || ec);
			if (ec && ec != asio::error::would_block)
			{
				trancieve_ip_packet(bytes_in_loop, m_remote.address().is_v6());
				disconnect(ec);
				return;
			}
			if (ec == asio::error::would_block) break;
			bytes_in_loop += bytes_transferred;
			++num_loops;
		}
		while (bytes_transferred > 0);

		m_last_receive = time_now();

		if (is_seed())
		{
			boost::shared_ptr<torrent> t = m_torrent.lock();
			if (t) t->seen_complete();
		}

		trancieve_ip_packet(bytes_in_loop, m_remote.address().is_v6());

		// allow reading from the socket again
		TORRENT_ASSERT(m_channel_state[download_channel] & peer_info::bw_network);
		m_channel_state[download_channel] &= ~peer_info::bw_network;

		setup_receive(read_async);
	}

	bool peer_connection::can_write() const
	{
		// if we have requests or pending data to be sent or announcements to be made
		// we want to send data
		return !m_send_buffer.empty()
			&& m_quota[upload_channel] > 0
			&& !m_connecting;
	}

	bool peer_connection::can_read() const
	{
		boost::shared_ptr<torrent> t = m_torrent.lock();

		bool bw_limit = m_quota[download_channel] > 0;

		if (!bw_limit) return false;

		if (m_outstanding_bytes > 0)
		{
			// if we're expecting to download piece data, we might not
			// want to read from the socket in case we're out of disk
			// cache space right now

			if (m_channel_state[download_channel] & peer_info::bw_disk) return false;

			// if we already have a disk buffer, we might as well use it
			// if contiguous recv buffer is true, don't apply this logic, but
			// actually wait until we try to allocate a buffer and exceed the limit
			if (!m_disk_recv_buffer
				&& !m_settings.get_bool(settings_pack::contiguous_recv_buffer)
				&& m_ses.exceeded_cache_use())
			{
				m_ses.inc_disk_queue(download_channel);
				const_cast<peer_connection*>(this)->m_channel_state[download_channel] |= peer_info::bw_disk;

				// make sure we know when the cache has been flushed
				// enough so we can start downloading again
				m_ses.subscribe_to_disk(const_cast<peer_connection*>(this)->self());

#ifdef TORRENT_VERBOSE_LOGGING
				peer_log("*** exceeded disk buffer watermark");
#endif
				return false;
			}
		}

		return !m_connecting && !m_disconnecting;
	}

	void peer_connection::on_allow_connect(int ticket)
	{
		TORRENT_ASSERT(m_queued_for_connection);
		m_queued_for_connection = false;

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		{
			boost::shared_ptr<torrent> t = m_torrent.lock();
			t->debug_log("END queue peer [%p]", this);
		}
#endif

		TORRENT_ASSERT(m_ses.is_single_thread());
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		// in case we disconnect here, we need to
		// keep the connection alive until the
		// exit invariant check is run
		boost::shared_ptr<peer_connection> me(self());
#endif
		INVARIANT_CHECK;

		error_code ec;
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_ses.session_log("ON_CONNECT: %s", print_endpoint(m_remote).c_str());
#endif

		if (ticket == -1)
		{
			disconnect(asio::error::operation_aborted);
			return;		
		}

		m_connection_ticket = ticket;
		boost::shared_ptr<torrent> t = m_torrent.lock();

		m_queued = false;

		if (!t)
		{
			TORRENT_ASSERT(!m_connecting);
			disconnect(errors::torrent_aborted);
			return;
		}

		TORRENT_ASSERT(m_connecting);

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		peer_log(">>> OPEN [ protocol: %s ]", (m_remote.address().is_v4()?"IPv4":"IPv6"));
#endif
		m_socket->open(m_remote.protocol(), ec);
		if (ec)
		{
			disconnect(ec);
			return;
		}

		tcp::endpoint bind_interface = m_ses.get_interface();
	
		if (m_settings.get_int(settings_pack::outgoing_port) > 0)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log(">>> SET_REUSE_ADDRESS");
#endif
			m_socket->set_option(socket_acceptor::reuse_address(true), ec);
			// ignore errors because the underlying socket may not
			// be opened yet. This happens when we're routing through
			// a proxy. In that case, we don't yet know the address of
			// the proxy server, and more importantly, we don't know
			// the address family of its address. This means we can't
			// open the socket yet. The socks abstraction layer defers
			// opening it.
			ec.clear();
			bind_interface.port(m_ses.next_port());
		}

		// if we're not binding to a specific interface, bind
		// to the same protocol family as the target endpoint
		if (is_any(bind_interface.address()))
		{
#if TORRENT_USE_IPV6
			if (m_remote.address().is_v6())
				bind_interface.address(address_v6::any());
			else
#endif
				bind_interface.address(address_v4::any());
		}

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
		peer_log(">>> BIND [ ep: %s ]", print_endpoint(bind_interface).c_str());
#endif
		m_socket->bind(bind_interface, ec);
		if (ec)
		{
			disconnect(ec);
			return;
		}
#if defined TORRENT_VERBOSE_LOGGING
		peer_log(">>> ASYNC_CONNECT [ dst: %s ]", print_endpoint(m_remote).c_str());
#endif
#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("peer_connection::on_connection_complete");
#endif

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		t->debug_log("START connect [%p] (%d)", this, int(t->num_peers()));
#endif

		m_socket->async_connect(m_remote
			, boost::bind(&peer_connection::on_connection_complete, self(), _1));
		m_connect = time_now_hires();

		sent_syn(m_remote.address().is_v6());

		if (t->alerts().should_post<peer_connect_alert>())
		{
			t->alerts().post_alert(peer_connect_alert(
				t->get_handle(), remote(), pid()));
		}
#if defined TORRENT_VERBOSE_LOGGING
		peer_log("*** LOCAL ENDPOINT[ e: %s ]", print_endpoint(m_socket->local_endpoint(ec)).c_str());
#endif
	}
	
	void peer_connection::on_connection_complete(error_code const& e)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("peer_connection::on_connection_complete");
#endif
		ptime completed = time_now_hires();

		TORRENT_ASSERT(m_ses.is_single_thread());

		INVARIANT_CHECK;

		m_rtt = total_milliseconds(completed - m_connect);

#if defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		{
			boost::shared_ptr<torrent> t = m_torrent.lock();
			t->debug_log("END connect [%p] (%d ms)", this, m_rtt);
			m_connect_time = completed;
		}
#endif

#ifdef TORRENT_USE_OPENSSL
		// add this RTT to the PRNG seed, to add more unpredictability
		boost::uint64_t now = total_microseconds(completed - m_connect);
		// assume 12 bits of entropy (i.e. about 8 milliseconds)
		RAND_add(&now, 8, 1.5);
#endif
		
		if (m_disconnecting) return;

		if (e)
		{
			connect_failed(e);
			return;
		}

		error_code ec;
		m_local = m_socket->local_endpoint(ec);
		if (ec)
		{
			disconnect(ec);
			return;
		}

		// if t is NULL, we better not be connecting, since
		// we can't decrement the connecting counter
		boost::shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t || !m_connecting);
		if (m_connecting && t)
		{
			t->dec_num_connecting();
			m_connecting = false;
		}
		m_ses.half_open_done(m_connection_ticket);
		m_connection_ticket = -1;

		if (m_disconnecting) return;
		m_last_receive = time_now();

		if (is_utp(*m_socket) && m_peer_info)
		{
			m_peer_info->confirmed_supports_utp = true;
			m_peer_info->supports_utp = false;
		}

		// this means the connection just succeeded

		received_synack(m_remote.address().is_v6());

		TORRENT_ASSERT(m_socket);
#if defined TORRENT_VERBOSE_LOGGING
		peer_log(">>> COMPLETED [ ep: %s rtt: %d ]", print_endpoint(m_remote).c_str(), m_rtt);
#endif

		// set the socket to non-blocking, so that we can
		// read the entire buffer on each read event we get
		tcp::socket::non_blocking_io ioc(true);
#if defined TORRENT_VERBOSE_LOGGING
		peer_log("*** SET NON-BLOCKING");
#endif
		m_socket->io_control(ioc, ec);
		if (ec)
		{
			disconnect(ec);
			return;
		}

		if (m_remote == m_socket->local_endpoint(ec))
		{
			// if the remote endpoint is the same as the local endpoint, we're connected
			// to ourselves
			if (m_peer_info && t) t->ban_peer(m_peer_info);
			disconnect(errors::self_connection, 1);
			return;
		}

		if (m_remote.address().is_v4())
		{
			error_code ec;
			m_socket->set_option(type_of_service(m_settings.get_int(settings_pack::peer_tos)), ec);
#if defined TORRENT_VERBOSE_LOGGING
			peer_log(">>> SET_TOS[ tos: %d e: %s ]", m_settings.get_int(settings_pack::peer_tos), ec.message().c_str());
#endif
		}

		on_connected();
		setup_send();
		setup_receive();
	}
	
	// --------------------------
	// SEND DATA
	// --------------------------

	void peer_connection::on_send_data(error_code const& error
		, std::size_t bytes_transferred)
	{
		m_ses.inc_stats_counter(aux::session_interface::on_write_counter);
		m_ses.sent_buffer(bytes_transferred);
		TORRENT_ASSERT(m_ses.is_single_thread());

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(m_socket_is_writing);
		m_socket_is_writing = false;
#endif

		// submit all disk jobs when we've processed all messages
		// in the current message queue
		m_ses.deferred_submit_jobs();

#if defined TORRENT_VERBOSE_LOGGING 
		peer_log("*** ON_SEND_DATA [ bytes: %d error: %s ]"
			, int(bytes_transferred), error.message().c_str());
#endif

		INVARIANT_CHECK;

#if defined TORRENT_ASIO_DEBUGGING
		complete_async("peer_connection::on_send_data");
#endif
		// keep ourselves alive in until this function exits in
		// case we disconnect
		boost::shared_ptr<peer_connection> me(self());

		TORRENT_ASSERT(m_channel_state[upload_channel] & peer_info::bw_network);

		m_send_buffer.pop_front(bytes_transferred);

		for (std::vector<int>::iterator i = m_requests_in_buffer.begin()
			, end(m_requests_in_buffer.end()); i != end; ++i)
			*i -= bytes_transferred;

		while (!m_requests_in_buffer.empty()
			&& m_requests_in_buffer.front() <= 0)
			m_requests_in_buffer.erase(m_requests_in_buffer.begin());
		
		m_channel_state[upload_channel] &= ~peer_info::bw_network;

		TORRENT_ASSERT(int(bytes_transferred) <= m_quota[upload_channel]);
		m_quota[upload_channel] -= bytes_transferred;

		trancieve_ip_packet(bytes_transferred, m_remote.address().is_v6());

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log(">>> wrote %d bytes", int(bytes_transferred));
#endif

		if (error)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_ERROR_LOGGING
			peer_log("**ERROR**: %s [in peer_connection::on_send_data]", error.message().c_str());
#endif
			disconnect(error);
			return;
		}
		if (m_disconnecting)
		{
			// make sure we free up all send buffers that are owned
			// by the disk thread
			m_send_buffer.clear();
			m_disk_recv_buffer.reset();
			return;
		}

		TORRENT_ASSERT(!m_connecting);
		TORRENT_ASSERT(bytes_transferred > 0);

		m_last_sent = time_now();

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		size_type cur_payload_ul = m_statistics.last_payload_uploaded();
		size_type cur_protocol_ul = m_statistics.last_protocol_uploaded();
#endif
		on_sent(error, bytes_transferred);
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(m_statistics.last_payload_uploaded() - cur_payload_ul >= 0);
		TORRENT_ASSERT(m_statistics.last_protocol_uploaded() - cur_protocol_ul >= 0);
		size_type stats_diff = m_statistics.last_payload_uploaded() - cur_payload_ul
			+ m_statistics.last_protocol_uploaded() - cur_protocol_ul;
		TORRENT_ASSERT(stats_diff == int(bytes_transferred));
#endif

		fill_send_buffer();

		setup_send();
	}

#if defined TORRENT_DEBUG && !defined TORRENT_DISABLE_INVARIANT_CHECKS
	struct peer_count_t
	{
		peer_count_t(): num_peers(0), num_peers_with_timeouts(0), num_peers_with_nowant(0), num_not_requested(0) {}
		int num_peers;
		int num_peers_with_timeouts;
		int num_peers_with_nowant;
		int num_not_requested;
//		std::vector<peer_connection const*> peers;
	};

	void peer_connection::check_invariant() const
	{
		TORRENT_ASSERT(m_in_use == 1337);
		TORRENT_ASSERT(m_queued_time_critical <= int(m_request_queue.size()));
		TORRENT_ASSERT(m_recv_end >= m_recv_start);

		TORRENT_ASSERT(bool(m_disk_recv_buffer) == (m_disk_recv_buffer_size > 0));

		boost::shared_ptr<torrent> t = m_torrent.lock();

		if (!m_disconnect_started && m_initialized)
		{
			// none of this matters if we're disconnecting anyway
			if (t->is_finished())
				TORRENT_ASSERT(!is_interesting() || m_need_interest_update);
			if (is_seed())
				TORRENT_ASSERT(upload_only());
		}

		if (m_disconnecting)
		{
			TORRENT_ASSERT(m_download_queue.empty());
			TORRENT_ASSERT(m_request_queue.empty());
			TORRENT_ASSERT(m_disconnect_started);
		}
		else if (!m_in_constructor)
		{
			TORRENT_ASSERT(m_ses.has_peer((peer_connection*)this));
		}

		TORRENT_ASSERT(m_outstanding_bytes >= 0);
		if (t && t->valid_metadata() && !m_disconnecting)
		{
			torrent_info const& ti = t->torrent_file();
			// if the piece is fully downloaded, we might have popped it from the
			// download queue already
			int outstanding_bytes = 0;
			bool in_download_queue = false;
			int block_size = t->block_size();
			piece_block last_block(ti.num_pieces()-1
				, (ti.piece_size(ti.num_pieces()-1) + block_size - 1) / block_size);
			for (std::vector<pending_block>::const_iterator i = m_download_queue.begin()
				, end(m_download_queue.end()); i != end; ++i)
			{
				TORRENT_ASSERT(i->block.piece_index <= last_block.piece_index);
				TORRENT_ASSERT(i->block.piece_index < last_block.piece_index
					|| i->block.block_index <= last_block.block_index);
				if (m_received_in_piece && i == m_download_queue.begin())
				{
					in_download_queue = true;
					// this assert is not correct since block may have different sizes
					// and may not be returned in the order they were requested
//					TORRENT_ASSERT(t->to_req(i->block).length >= m_received_in_piece);
					outstanding_bytes += t->to_req(i->block).length - m_received_in_piece;
				}
				else
				{
					outstanding_bytes += t->to_req(i->block).length;
				}
			}
			//if (p && p->bytes_downloaded < p->full_block_bytes) TORRENT_ASSERT(in_download_queue);

			TORRENT_ASSERT(m_outstanding_bytes == outstanding_bytes);
		}
/*
		if (m_channel_state[download_channel] == peer_info::bw_limit)
			TORRENT_ASSERT(m_quota[download_channel] == 0);
		if (m_channel_state[upload_channel] == peer_info::bw_limit)
			TORRENT_ASSERT(m_quota[upload_channel] == 0);
*/
		std::set<piece_block> unique;
		std::transform(m_download_queue.begin(), m_download_queue.end()
			, std::inserter(unique, unique.begin()), boost::bind(&pending_block::block, _1));
		std::transform(m_request_queue.begin(), m_request_queue.end()
			, std::inserter(unique, unique.begin()), boost::bind(&pending_block::block, _1));
		TORRENT_ASSERT(unique.size() == m_download_queue.size() + m_request_queue.size());
		if (m_peer_info)
		{
			TORRENT_ASSERT(m_peer_info->prev_amount_upload == 0);
			TORRENT_ASSERT(m_peer_info->prev_amount_download == 0);
			TORRENT_ASSERT(m_peer_info->connection == this
				|| m_peer_info->connection == 0);

			if (m_peer_info->optimistically_unchoked)
				TORRENT_ASSERT(!is_choked());
		}

		TORRENT_ASSERT(m_have_piece.count() == m_num_pieces);

		if (!t)
		{
#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
			// since this connection doesn't have a torrent reference
			// no torrent should have a reference to this connection either
			TORRENT_ASSERT(!m_ses.any_torrent_has_peer(this));
#endif
			return;
		}

		if (t->ready_for_connections() && m_initialized)
			TORRENT_ASSERT(t->torrent_file().num_pieces() == int(m_have_piece.size()));

		// in share mode we don't close redundant connections
		if (m_settings.get_bool(settings_pack::close_redundant_connections) && !t->share_mode())
		{
			// make sure upload only peers are disconnected
			if (t->is_upload_only()
				&& m_upload_only
				&& t->valid_metadata()
				&& has_metadata())
				TORRENT_ASSERT(m_disconnect_started || t->graceful_pause() || t->has_error());

			if (m_upload_only
				&& !m_interesting
				&& m_bitfield_received
				&& t->are_files_checked()
				&& t->valid_metadata()
				&& has_metadata())
				TORRENT_ASSERT(m_disconnect_started);
		}

		if (!m_disconnect_started && m_initialized
			&& m_ses.settings().get_bool(settings_pack::close_redundant_connections))
		{
			// none of this matters if we're disconnecting anyway
			if (t->is_upload_only())
				TORRENT_ASSERT(!m_interesting || t->graceful_pause() || t->has_error());
			if (is_seed())
				TORRENT_ASSERT(m_upload_only);
		}

#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
		if (t->has_picker())
		{
			std::map<piece_block, peer_count_t> num_requests;
			for (torrent::const_peer_iterator i = t->begin(); i != t->end(); ++i)
			{
				// make sure this peer is not a dangling pointer
				TORRENT_ASSERT(m_ses.has_peer(*i));
				peer_connection const& p = *(*i);
				for (std::vector<pending_block>::const_iterator i = p.request_queue().begin()
					, end(p.request_queue().end()); i != end; ++i)
				{
					++num_requests[i->block].num_peers;
					++num_requests[i->block].num_peers_with_timeouts;
					++num_requests[i->block].num_peers_with_nowant;
					++num_requests[i->block].num_not_requested;
//					num_requests[i->block].peers.push_back(&p);
				}
				for (std::vector<pending_block>::const_iterator i = p.download_queue().begin()
					, end(p.download_queue().end()); i != end; ++i)
				{
					if (!i->not_wanted && !i->timed_out) ++num_requests[i->block].num_peers;
					if (i->timed_out) ++num_requests[i->block].num_peers_with_timeouts;
					if (i->not_wanted) ++num_requests[i->block].num_peers_with_nowant;
//					num_requests[i->block].peers.push_back(&p);
				}
			}
			for (std::map<piece_block, peer_count_t>::iterator i = num_requests.begin()
				, end(num_requests.end()); i != end; ++i)
			{
				piece_block b = i->first;
				peer_count_t const& pc = i->second;
				int count = pc.num_peers;
				int count_with_timeouts = pc.num_peers_with_timeouts;
				int count_with_nowant = pc.num_peers_with_nowant;
				(void)count_with_timeouts;
				(void)count_with_nowant;
				int picker_count = t->picker().num_peers(b);
				if (!t->picker().is_downloaded(b))
					TORRENT_ASSERT(picker_count == count);
			}
		}
		if (m_peer_info && type() == bittorrent_connection)
		{
			policy::const_iterator i = t->get_policy().begin_peer();
			policy::const_iterator end = t->get_policy().end_peer();
			for (; i != end; ++i)
			{
				if (*i == m_peer_info) break;
			}
			TORRENT_ASSERT(i != end);
		}
#endif
		if (t->has_picker() && !t->is_aborted())
		{
			// make sure that pieces that have completed the download
			// of all their blocks are in the disk io thread's queue
			// to be checked.
			const std::vector<piece_picker::downloading_piece>& dl_queue
				= t->picker().get_download_queue();
			for (std::vector<piece_picker::downloading_piece>::const_iterator i =
				dl_queue.begin(); i != dl_queue.end(); ++i)
			{
				const int blocks_per_piece = t->picker().blocks_in_piece(i->index);

				bool complete = true;
				for (int j = 0; j < blocks_per_piece; ++j)
				{
					if (i->info[j].state == piece_picker::block_info::state_finished)
						continue;
					complete = false;
					break;
				}
/*
// this invariant is not valid anymore since the completion event
// might be queued in the io service
				if (complete && !piece_failed)
				{
					disk_io_job ret = m_ses.m_disk_thread.find_job(
						&t->storage(), -1, i->index);
					TORRENT_ASSERT(ret.action == disk_io_job::hash || ret.action == disk_io_job::write);
					TORRENT_ASSERT(ret.piece == i->index);
				}
*/
			}
		}

// extremely expensive invariant check
/*
		if (!t->is_seed())
		{
			piece_picker& p = t->picker();
			const std::vector<piece_picker::downloading_piece>& dlq = p.get_download_queue();
			const int blocks_per_piece = static_cast<int>(
				t->torrent_file().piece_length() / t->block_size());

			for (std::vector<piece_picker::downloading_piece>::const_iterator i =
				dlq.begin(); i != dlq.end(); ++i)
			{
				for (int j = 0; j < blocks_per_piece; ++j)
				{
					if (std::find(m_request_queue.begin(), m_request_queue.end()
						, piece_block(i->index, j)) != m_request_queue.end()
						||
						std::find(m_download_queue.begin(), m_download_queue.end()
						, piece_block(i->index, j)) != m_download_queue.end())
					{
						TORRENT_ASSERT(i->info[j].peer == m_remote);
					}
					else
					{
						TORRENT_ASSERT(i->info[j].peer != m_remote || i->info[j].finished);
					}
				}
			}
		}
*/
	}
#endif

	peer_connection::peer_speed_t peer_connection::peer_speed()
	{
		shared_ptr<torrent> t = m_torrent.lock();
		TORRENT_ASSERT(t);

		int download_rate = int(statistics().download_payload_rate());
		int torrent_download_rate = int(t->statistics().download_payload_rate());

		if (download_rate > 512 && download_rate > torrent_download_rate / 16)
			m_speed = fast;
		else if (download_rate > 4096 && download_rate > torrent_download_rate / 64)
			m_speed = medium;
		else if (download_rate < torrent_download_rate / 15 && m_speed == fast)
			m_speed = medium;
		else
			m_speed = slow;

		return m_speed;
	}

	void peer_connection::keep_alive()
	{
#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
		INVARIANT_CHECK;
#endif

		time_duration d;
		d = time_now() - m_last_sent;
		if (total_seconds(d) < m_timeout / 2) return;
		
		if (m_connecting) return;
		if (in_handshake()) return;

		// if the last send has not completed yet, do not send a keep
		// alive
		if (m_channel_state[upload_channel] & peer_info::bw_network) return;

#ifdef TORRENT_VERBOSE_LOGGING
		peer_log("==> KEEPALIVE");
#endif

		m_last_sent = time_now();
		write_keepalive();
	}

	bool peer_connection::is_seed() const
	{
		// if m_num_pieces == 0, we probably don't have the
		// metadata yet.
		boost::shared_ptr<torrent> t = m_torrent.lock();
		return m_num_pieces == (int)m_have_piece.size() && m_num_pieces > 0 && t && t->valid_metadata();
	}

	void peer_connection::set_share_mode(bool u)
	{
		// if the peer is a seed, ignore share mode messages
		if (is_seed()) return;

		m_share_mode = u;
	}

	void peer_connection::set_upload_only(bool u)
	{
		// if the peer is a seed, don't allow setting
		// upload_only to false
		if (m_upload_only || is_seed()) return;

		m_upload_only = u;
		boost::shared_ptr<torrent> t = associated_torrent().lock();
		t->set_seed(m_peer_info, u);
		disconnect_if_redundant();
	}

}

