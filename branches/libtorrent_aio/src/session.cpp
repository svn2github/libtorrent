/*

Copyright (c) 2006-2012, Arvid Norberg, Magnus Jonsson
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

#include <ctime>
#include <algorithm>
#include <set>
#include <deque>
#include <cctype>
#include <algorithm>

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/limits.hpp>
#include <boost/bind.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "libtorrent/extensions/ut_pex.hpp"
#include "libtorrent/extensions/ut_metadata.hpp"
#include "libtorrent/extensions/lt_trackers.hpp"
#include "libtorrent/extensions/smart_ban.hpp"
#include "libtorrent/peer_id.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/tracker_manager.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/fingerprint.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/bt_peer_connection.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/aux_/session_impl.hpp"
#include "libtorrent/kademlia/dht_tracker.hpp"
#include "libtorrent/natpmp.hpp"
#include "libtorrent/upnp.hpp"
#include "libtorrent/magnet_uri.hpp"

using boost::shared_ptr;
using boost::weak_ptr;
using libtorrent::aux::session_impl;

#ifdef TORRENT_MEMDEBUG
void start_malloc_debug();
void stop_malloc_debug();
#endif

namespace libtorrent
{
#ifdef _MSC_VER
	namespace aux
	{
		eh_initializer::eh_initializer()
		{
			::_set_se_translator(straight_to_debugger);
		}
	}
#endif

	TORRENT_EXPORT void TORRENT_LINK_TEST_NAME() {}

	TORRENT_EXPORT void min_memory_usage(settings_pack& set)
	{
		// receive data directly into disk buffers
		// this yields more system calls to read() and
		// kqueue(), but saves RAM.
		set.set_bool(settings_pack::contiguous_recv_buffer, false);

		set.set_int(settings_pack::disk_io_write_mode, settings_pack::disable_os_cache);
		set.set_int(settings_pack::disk_io_read_mode, settings_pack::disable_os_cache);

		// keep 2 blocks outstanding when hashing
		set.set_int(settings_pack::checking_mem_usage, 2);

		// don't use any extra threads to do SHA-1 hashing
		set.set_int(settings_pack::hashing_threads, 0);
		set.set_int(settings_pack::network_threads, 0);
		set.set_int(settings_pack::aio_threads, 1);

		set.set_int(settings_pack::alert_queue_size, 100);

		// setting this to a low limit, means more
		// peers are more likely to request from the
		// same piece. Which means fewer partial
		// pieces and fewer entries in the partial
		// piece list
		set.set_int(settings_pack::whole_pieces_threshold, 2);
		set.set_bool(settings_pack::use_parole_mode, false);
		set.set_bool(settings_pack::prioritize_partial_pieces, true);

		// connect to 5 peers per second
		set.set_int(settings_pack::connection_speed, 5);

		// be extra nice on the hard drive when running
		// on embedded devices. This might slow down
		// torrent checking
		set.set_int(settings_pack::file_checks_delay_per_block, 5);

		// only have 4 files open at a time
		set.set_int(settings_pack::file_pool_size, 4);

		// we want to keep the peer list as small as possible
		set.set_bool(settings_pack::allow_multiple_connections_per_ip, false);
		set.set_int(settings_pack::max_failcount, 2);
		set.set_int(settings_pack::inactivity_timeout, 120);

		// whenever a peer has downloaded one block, write
		// it to disk, and don't read anything from the
		// socket until the disk write is complete
		set.set_int(settings_pack::max_queued_disk_bytes, 1);

		// don't keep track of all upnp devices, keep
		// the device list small
		set.set_bool(settings_pack::upnp_ignore_nonrouters, true);

		// never keep more than one 16kB block in
		// the send buffer
		set.set_int(settings_pack::send_buffer_watermark, 9);

		// don't use any disk cache
		set.set_int(settings_pack::cache_size, 0);
		set.set_int(settings_pack::cache_buffer_chunk_size, 1);
		set.set_bool(settings_pack::use_read_cache, false);
		set.set_bool(settings_pack::use_disk_read_ahead, false);

		set.set_bool(settings_pack::close_redundant_connections, true);

		set.set_int(settings_pack::max_peerlist_size, 500);
		set.set_int(settings_pack::max_paused_peerlist_size, 50);

		// udp trackers are cheaper to talk to
		set.set_bool(settings_pack::prefer_udp_trackers, true);

		set.set_int(settings_pack::max_rejects, 10);

		set.set_int(settings_pack::recv_socket_buffer_size, 16 * 1024);
		set.set_int(settings_pack::send_socket_buffer_size, 16 * 1024);

		// use less memory when reading and writing
		// whole pieces
		set.set_bool(settings_pack::coalesce_reads, false);
		set.set_bool(settings_pack::coalesce_writes, false);

		// disallow the buffer size to grow for the uTP socket
		set.set_bool(settings_pack::utp_dynamic_sock_buf, false);
	}

	TORRENT_EXPORT void high_performance_seed(settings_pack& set)
	{
		// don't throttle TCP, assume there is
		// plenty of bandwidth
		set.set_int(settings_pack::mixed_mode_algorithm, settings_pack::prefer_tcp);

		set.set_int(settings_pack::max_out_request_queue, 1500);
		set.set_int(settings_pack::max_allowed_in_request_queue, 2000);

		// we will probably see a high rate of alerts, make it less
		// likely to loose alerts
		set.set_int(settings_pack::alert_queue_size, 10000);

		// allow 500 files open at a time
		set.set_int(settings_pack::file_pool_size, 500);

		// don't update access time for each read/write
		set.set_bool(settings_pack::no_atime_storage, true);

		// as a seed box, we must accept multiple peers behind
		// the same NAT
//		set.set_bool(settings_pack::allow_multiple_connections_per_ip, true);

		// connect to 50 peers per second
		set.set_int(settings_pack::connection_speed, 500);

		// allow 8000 peer connections
		set.set_int(settings_pack::connections_limit, 8000);

		// allow lots of peers to try to connect simultaneously
		set.set_int(settings_pack::listen_queue_size, 3000);

		// unchoke many peers
		set.set_int(settings_pack::unchoke_slots_limit, 2000);

		// we need more DHT capacity to ping more peers
		// candidates before trying to connect
		set.set_int(settings_pack::dht_upload_rate_limit, 100000);

		// use 1 GB of cache
		set.set_int(settings_pack::cache_size, 32768 * 2);
		set.set_bool(settings_pack::use_read_cache, true);
		set.set_int(settings_pack::cache_buffer_chunk_size, 0);
		set.set_int(settings_pack::read_cache_line_size, 32);
		set.set_int(settings_pack::write_cache_line_size, 256);
		set.set_bool(settings_pack::low_prio_disk, false);
		// 30 seconds expiration to save cache
		// space for active pieces
		set.set_int(settings_pack::cache_expiry, 30);
		// this is expensive and could add significant
		// delays when freeing a large number of buffers
		set.set_bool(settings_pack::lock_disk_cache, false);

		// in case the OS we're running on doesn't support
		// readv/writev, allocate contiguous buffers for
		// reads and writes
		// disable, since it uses a lot more RAM and a significant
		// amount of CPU to copy it around
		set.set_bool(settings_pack::coalesce_reads, false);
		set.set_bool(settings_pack::coalesce_writes, false);

		// the max number of bytes pending write before we throttle
		// download rate
		set.set_int(settings_pack::max_queued_disk_bytes, 7 * 1024 * 1024);

		set.set_bool(settings_pack::explicit_read_cache, false);
		// prevent fast pieces to interfere with suggested pieces
		// since we unchoke everyone, we don't need fast pieces anyway
		set.set_int(settings_pack::allowed_fast_set_size, 0);

		// suggest pieces in the read cache for higher cache hit rate
		set.set_int(settings_pack::suggest_mode, session_settings::suggest_read_cache);

		set.set_bool(settings_pack::close_redundant_connections, true);

		set.set_int(settings_pack::max_rejects, 10);

		// don't let connections linger for too long
		set.set_int(settings_pack::request_timeout, 10);
		set.set_int(settings_pack::peer_timeout, 20);
		set.set_int(settings_pack::inactivity_timeout, 20);

		set.set_int(settings_pack::active_limit, 2000);
		set.set_int(settings_pack::active_tracker_limit, 2000);
		set.set_int(settings_pack::active_dht_limit, 600);
		set.set_int(settings_pack::active_seeds, 2000);

		set.set_int(settings_pack::choking_algorithm, settings_pack::fixed_slots_choker);

		// of 500 ms, and a send rate of 4 MB/s, the upper
		// limit should be 2 MB
		set.set_int(settings_pack::send_buffer_watermark, 3 * 1024 * 1024);

		// put 1.5 seconds worth of data in the send buffer
		// this gives the disk I/O more heads-up on disk
		// reads, and can maximize throughput
		set.set_int(settings_pack::send_buffer_watermark_factor, 150);

		// always stuff at least 1 MiB down each peer
		// pipe, to quickly ramp up send rates
 		set.set_int(settings_pack::send_buffer_low_watermark, 1 * 1024 * 1024);

		// don't retry peers if they fail once. Let them
		// connect to us if they want to
		set.set_int(settings_pack::max_failcount, 1);

		// allow the buffer size to grow for the uTP socket
		set.set_bool(settings_pack::utp_dynamic_sock_buf, true);

		// we're likely to have more than 4 cores on a high
		// performance machine. One core is needed for the
		// network thread
		set.set_int(settings_pack::hashing_threads, 4);

		// the number of threads to use to call async_write_some
		// and read_some on peer sockets
		set.set_int(settings_pack::network_threads, 4);

		// number of disk threads for low level file operations
		set.set_int(settings_pack::aio_threads, 8);

		// keep 5 MiB outstanding when checking hashes
		// of a resumed file
		set.set_int(settings_pack::checking_mem_usage, 320);

		// the disk cache performs better with the pool allocator
		set.set_bool(settings_pack::use_disk_cache_pool, true);
	}

#ifndef TORRENT_NO_DEPRECATE
	// this function returns a session_settings object
	// which will optimize libtorrent for minimum memory
	// usage, with no consideration of performance.
	TORRENT_EXPORT session_settings min_memory_usage()
	{
		aux::session_settings def;
		initialize_default_settings(def);
		settings_pack pack;
		min_memory_usage(pack);
		apply_pack(&pack, def, 0);
		session_settings ret;
		load_struct_from_settings(def, ret);
		return ret;
	}

	TORRENT_EXPORT session_settings high_performance_seed()
	{
		aux::session_settings def;
		initialize_default_settings(def);
		settings_pack pack;
		high_performance_seed(pack);
		apply_pack(&pack, def, 0);
		session_settings ret;
		load_struct_from_settings(def, ret);
		return ret;
	}
#endif

	// wrapper around a function that's executed in the network thread
	// ans synchronized in the client thread
	template <class R>
	void fun_ret(R* ret, bool* done, condition_variable* e, mutex* m, boost::function<R(void)> f)
	{
		*ret = f();
		mutex::scoped_lock l(*m);
		*done = true;
		e->notify_all();
	}

	void fun_wrap(bool* done, condition_variable* e, mutex* m, boost::function<void(void)> f)
	{
		f();
		mutex::scoped_lock l(*m);
		*done = true;
		e->notify_all();
	}

#define TORRENT_ASYNC_CALL(x) \
	m_impl->m_io_service.post(boost::bind(&session_impl:: x, m_impl.get()))

#define TORRENT_ASYNC_CALL1(x, a1) \
	m_impl->m_io_service.post(boost::bind(&session_impl:: x, m_impl.get(), a1))

#define TORRENT_ASYNC_CALL2(x, a1, a2) \
	m_impl->m_io_service.post(boost::bind(&session_impl:: x, m_impl.get(), a1, a2))

#define TORRENT_WAIT \
	mutex::scoped_lock l(m_impl->mut); \
	while (!done) { m_impl->cond.wait(l); };

#define TORRENT_SYNC_CALL(x) \
	bool done = false; \
	m_impl->m_io_service.post(boost::bind(&fun_wrap, &done, &m_impl->cond, &m_impl->mut, boost::function<void(void)>(boost::bind(&session_impl:: x, m_impl.get())))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL1(x, a1) \
	bool done = false; \
	m_impl->m_io_service.post(boost::bind(&fun_wrap, &done, &m_impl->cond, &m_impl->mut, boost::function<void(void)>(boost::bind(&session_impl:: x, m_impl.get(), a1)))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL2(x, a1, a2) \
	bool done = false; \
	m_impl->m_io_service.post(boost::bind(&fun_wrap, &done, &m_impl->cond, &m_impl->mut, boost::function<void(void)>(boost::bind(&session_impl:: x, m_impl.get(), a1, a2)))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL3(x, a1, a2, a3) \
	bool done = false; \
	m_impl->m_io_service.post(boost::bind(&fun_wrap, &done, &m_impl->cond, &m_impl->mut, boost::function<void(void)>(boost::bind(&session_impl:: x, m_impl.get(), a1, a2, a3)))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL4(x, a1, a2, a3, a4) \
	bool done = false; \
	m_impl->m_io_service.post(boost::bind(&fun_wrap, &done, &m_impl->cond, &m_impl->mut, boost::function<void(void)>(boost::bind(&session_impl:: x, m_impl.get(), a1, a2, a3, a4)))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL_RET(type, x) \
	bool done = false; \
	type r; \
	m_impl->m_io_service.post(boost::bind(&fun_ret<type>, &r, &done, &m_impl->cond, &m_impl->mut, boost::function<type(void)>(boost::bind(&session_impl:: x, m_impl.get())))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL_RET1(type, x, a1) \
	bool done = false; \
	type r; \
	m_impl->m_io_service.post(boost::bind(&fun_ret<type>, &r, &done, &m_impl->cond, &m_impl->mut, boost::function<type(void)>(boost::bind(&session_impl:: x, m_impl.get(), a1)))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL_RET2(type, x, a1, a2) \
	bool done = false; \
	type r; \
	m_impl->m_io_service.post(boost::bind(&fun_ret<type>, &r, &done, &m_impl->cond, &m_impl->mut, boost::function<type(void)>(boost::bind(&session_impl:: x, m_impl.get(), a1, a2)))); \
	TORRENT_WAIT

#define TORRENT_SYNC_CALL_RET3(type, x, a1, a2, a3) \
	bool done = false; \
	type r; \
	m_impl->m_io_service.post(boost::bind(&fun_ret<type>, &r, &done, &m_impl->cond, &m_impl->mut, boost::function<type(void)>(boost::bind(&session_impl:: x, m_impl.get(), a1, a2, a3)))); \
	TORRENT_WAIT

#ifndef TORRENT_CFG
#error TORRENT_CFG is not defined!
#endif

	// this is a dummy function that's exported and named based
	// on the configuration. The session.hpp file will reference
	// it and if the library and the client are built with different
	// configurations this will give a link error
	void TORRENT_EXPORT TORRENT_CFG() {}

	void session::init(std::pair<int, int> listen_range, char const* listen_interface
		, fingerprint const& id, boost::uint32_t alert_mask)
	{
		m_impl.reset(new session_impl(listen_range, id, listen_interface, alert_mask));

#ifdef TORRENT_MEMDEBUG
		start_malloc_debug();
#endif
	}

	void session::set_log_path(std::string const& p)
	{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_impl->set_log_path(p);
#endif
	}

	void session::start(int flags)
	{
#ifndef TORRENT_DISABLE_EXTENSIONS
		if (flags & add_default_plugins)
		{
			add_extension(create_ut_pex_plugin);
			add_extension(create_ut_metadata_plugin);
			add_extension(create_lt_trackers_plugin);
			add_extension(create_smart_ban_plugin);
		}
#endif

		m_impl->start_session();

		if (flags & start_default_features)
		{
			start_upnp();
			start_natpmp();
#ifndef TORRENT_DISABLE_DHT
			start_dht();
#endif
			start_lsd();
		}
	}

	session::~session()
	{
#ifdef TORRENT_MEMDEBUG
		stop_malloc_debug();
#endif
		TORRENT_ASSERT(m_impl);
		// if there is at least one destruction-proxy
		// abort the session and let the destructor
		// of the proxy to syncronize
		if (!m_impl.unique())
		{
			TORRENT_ASYNC_CALL(abort);
		}
	}

	void session::save_state(entry& e, boost::uint32_t flags) const
	{
		TORRENT_SYNC_CALL2(save_state, &e, flags);
	}

	void session::load_state(lazy_entry const& e)
	{
		// this needs to be synchronized since the lifespan
		// of e is tied to the caller
		TORRENT_SYNC_CALL1(load_state, &e);
	}

	feed_handle session::add_feed(feed_settings const& feed)
	{
		// if you have auto-download enabled, you must specify a download directory!
		TORRENT_ASSERT(!feed.auto_download || !feed.add_args.save_path.empty());
		TORRENT_SYNC_CALL_RET1(feed_handle, add_feed, feed);
		return r;
	}

	void session::remove_feed(feed_handle h)
	{
		TORRENT_ASYNC_CALL1(remove_feed, h);
	}

	void session::get_feeds(std::vector<feed_handle>& f) const
	{
		f.clear();
		TORRENT_SYNC_CALL1(get_feeds, &f);
	}

	void session::set_load_function(user_load_function_t fun)
	{
		TORRENT_ASYNC_CALL1(set_load_function, fun);
	}

	void session::add_extension(boost::function<boost::shared_ptr<torrent_plugin>(torrent*, void*)> ext)
	{
#ifndef TORRENT_DISABLE_EXTENSIONS
		TORRENT_ASYNC_CALL1(add_extension, ext);
#endif
	}

	void session::add_extension(boost::shared_ptr<plugin> ext)
	{
#ifndef TORRENT_DISABLE_EXTENSIONS
		TORRENT_ASYNC_CALL1(add_ses_extension, ext);
#endif
	}

#ifndef TORRENT_DISABLE_GEO_IP
	void session::load_asnum_db(char const* file)
	{
		TORRENT_ASYNC_CALL1(load_asnum_db, std::string(file));
	}

	void session::load_country_db(char const* file)
	{
		TORRENT_ASYNC_CALL1(load_country_db, std::string(file));
	}

	int session::as_for_ip(address const& addr)
	{
		return m_impl->as_for_ip(addr);
	}

#if TORRENT_USE_WSTRING
	void session::load_asnum_db(wchar_t const* file)
	{
		TORRENT_ASYNC_CALL1(load_asnum_dbw, std::wstring(file));
	}

	void session::load_country_db(wchar_t const* file)
	{
		TORRENT_ASYNC_CALL1(load_country_dbw, std::wstring(file));
	}
#endif // TORRENT_USE_WSTRING
#endif // TORRENT_DISABLE_GEO_IP

#ifndef TORRENT_NO_DEPRECATE
	void session::load_state(entry const& ses_state)
	{
		if (ses_state.type() == entry::undefined_t) return;
		std::vector<char> buf;
		bencode(std::back_inserter(buf), ses_state);
		lazy_entry e;
		error_code ec;
		int ret = lazy_bdecode(&buf[0], &buf[0] + buf.size(), e, ec);
		TORRENT_ASSERT(ret == 0);
#ifndef BOOST_NO_EXCEPTIONS
		if (ret != 0) throw libtorrent_exception(ec);
#endif
		TORRENT_SYNC_CALL1(load_state, &e);
	}

	entry session::state() const
	{
		entry ret;
		TORRENT_SYNC_CALL2(save_state, &ret, 0xffffffff);
		return ret;
	}
#endif

	void session::set_ip_filter(ip_filter const& f)
	{
		TORRENT_ASYNC_CALL1(set_ip_filter, f);
	}
	
	ip_filter session::get_ip_filter() const
	{
		TORRENT_SYNC_CALL_RET(ip_filter, get_ip_filter);
		return r;
	}

	void session::set_port_filter(port_filter const& f)
	{
		TORRENT_ASYNC_CALL1(set_port_filter, f);
	}

	void session::set_peer_id(peer_id const& id)
	{
		TORRENT_ASYNC_CALL1(set_peer_id, id);
	}
	
	peer_id session::id() const
	{
		TORRENT_SYNC_CALL_RET(peer_id, get_peer_id);
		return r;
	}

	io_service& session::get_io_service()
	{
		return m_impl->m_io_service;
	}

	void session::set_key(int key)
	{
		TORRENT_ASYNC_CALL1(set_key, key);
	}

	void session::get_torrent_status(std::vector<torrent_status>* ret
		, boost::function<bool(torrent_status const&)> const& pred
		, boost::uint32_t flags) const
	{
		TORRENT_SYNC_CALL3(get_torrent_status, ret, boost::ref(pred), flags);
	}

	void session::refresh_torrent_status(std::vector<torrent_status>* ret
		, boost::uint32_t flags) const
	{
		TORRENT_SYNC_CALL2(refresh_torrent_status, ret, flags);
	}

	void session::post_torrent_updates()
	{
		TORRENT_ASYNC_CALL(post_torrent_updates);
	}

	std::vector<stats_metric> session_stats_metrics()
	{
		std::vector<stats_metric> ret;
		// defined in session_stats.cpp
		extern void get_stats_metric_map(std::vector<stats_metric>& stats);
		get_stats_metric_map(ret);
		return ret;
	}

	void session::post_session_stats()
	{
		TORRENT_ASYNC_CALL(post_session_stats);
	}

	std::vector<torrent_handle> session::get_torrents() const
	{
		TORRENT_SYNC_CALL_RET(std::vector<torrent_handle>, get_torrents);
		return r;
	}
	
	torrent_handle session::find_torrent(sha1_hash const& info_hash) const
	{
		TORRENT_SYNC_CALL_RET1(torrent_handle, find_torrent_handle, info_hash);
		return r;
	}

#ifndef BOOST_NO_EXCEPTIONS
	torrent_handle session::add_torrent(add_torrent_params const& params)
	{
		error_code ec;
		TORRENT_SYNC_CALL_RET2(torrent_handle, add_torrent, params, boost::ref(ec));
		if (ec) throw libtorrent_exception(ec);
		return r;
	}
#endif

	torrent_handle session::add_torrent(add_torrent_params const& params, error_code& ec)
	{
		ec.clear();
		TORRENT_SYNC_CALL_RET2(torrent_handle, add_torrent, params, boost::ref(ec));
		return r;
	}

	void session::async_add_torrent(add_torrent_params const& params)
	{
		add_torrent_params* p = new add_torrent_params(params);
		if (params.resume_data) p->resume_data = new std::vector<char>(*params.resume_data);
		TORRENT_ASYNC_CALL1(async_add_torrent, p);
	}

#ifndef BOOST_NO_EXCEPTIONS
#ifndef TORRENT_NO_DEPRECATE
	// if the torrent already exists, this will throw duplicate_torrent
	torrent_handle session::add_torrent(
		torrent_info const& ti
		, std::string const& save_path
		, entry const& resume_data
		, storage_mode_t storage_mode
		, bool paused
		, storage_constructor_type sc)
	{
		boost::intrusive_ptr<torrent_info> tip(new torrent_info(ti));
		add_torrent_params p(sc);
		p.ti = tip;
		p.save_path = save_path;
		std::vector<char> buf;
		if (resume_data.type() != entry::undefined_t)
		{
			bencode(std::back_inserter(buf), resume_data);
			p.resume_data = &buf;
		}
		p.storage_mode = storage_mode;
		p.paused = paused;
		return add_torrent(p);
	}

	torrent_handle session::add_torrent(
		boost::intrusive_ptr<torrent_info> ti
		, std::string const& save_path
		, entry const& resume_data
		, storage_mode_t storage_mode
		, bool paused
		, storage_constructor_type sc
		, void* userdata)
	{
		add_torrent_params p(sc);
		p.ti = ti;
		p.save_path = save_path;
		std::vector<char> buf;
		if (resume_data.type() != entry::undefined_t)
		{
			bencode(std::back_inserter(buf), resume_data);
			p.resume_data = &buf;
		}
		p.storage_mode = storage_mode;
		p.paused = paused;
		p.userdata = userdata;
		return add_torrent(p);
	}

	torrent_handle session::add_torrent(
		char const* tracker_url
		, sha1_hash const& info_hash
		, char const* name
		, std::string const& save_path
		, entry const& e
		, storage_mode_t storage_mode
		, bool paused
		, storage_constructor_type sc
		, void* userdata)
	{
		add_torrent_params p(sc);
		p.tracker_url = tracker_url;
		p.info_hash = info_hash;
		p.save_path = save_path;
		p.paused = paused;
		p.userdata = userdata;
		return add_torrent(p);
	}
#endif // TORRENT_NO_DEPRECATE
#endif // BOOST_NO_EXCEPTIONS

	void session::remove_torrent(const torrent_handle& h, int options)
	{
		if (!h.is_valid())
#ifdef BOOST_NO_EXCEPTIONS
			return;
#else
			throw_invalid_handle();
#endif
		TORRENT_ASYNC_CALL2(remove_torrent, h, options);
	}

#ifndef TORRENT_NO_DEPRECATE
	bool session::listen_on(
		std::pair<int, int> const& port_range
		, const char* net_interface, int flags)
	{
		error_code ec;
		listen_on(port_range, ec, net_interface, flags);
		return ec;
	}

	void session::listen_on(
		std::pair<int, int> const& port_range
		, error_code& ec
		, const char* net_interface, int flags)
	{
		settings_pack p;
		std::string interfaces_str;
		if (net_interface == NULL || strlen(net_interface) == 0)
			net_interface = "0.0.0.0";

		interfaces_str = print_endpoint(tcp::endpoint(address::from_string(net_interface, ec), port_range.first));
		if (ec) return;

		p.set_str(settings_pack::listen_interfaces, interfaces_str);
		p.set_int(settings_pack::max_retry_port_bind, port_range.second - port_range.first);
		p.set_bool(settings_pack::listen_system_port_fallback, (flags & session::listen_no_system_port) == 0);
		apply_settings(p);
	}

	void session::use_interfaces(char const* interfaces)
	{
		settings_pack pack;
		pack.set_str(settings_pack::outgoing_interfaces, interfaces);
		apply_settings(pack);
	}
#endif

	unsigned short session::listen_port() const
	{
		TORRENT_SYNC_CALL_RET(unsigned short, listen_port);
		return r;
	}

	session_status session::status() const
	{
		TORRENT_SYNC_CALL_RET(session_status, status);
		return r;
	}

	void session::pause()
	{
		TORRENT_ASYNC_CALL(pause);
	}

	void session::resume()
	{
		TORRENT_ASYNC_CALL(resume);
	}

	bool session::is_paused() const
	{
		TORRENT_SYNC_CALL_RET(bool, is_paused);
		return r;
	}

#ifndef TORRENT_NO_DEPRECATE
	void session::get_cache_info(sha1_hash const& ih
		, std::vector<cached_piece_info>& ret) const
	{
		cache_status st;
		get_cache_info(&st, find_torrent(ih));
		ret.swap(st.pieces);
	}

	cache_status session::get_cache_status() const
	{
		cache_status st;
		get_cache_info(&st);
		return st;
	}
#endif

	void session::get_cache_info(cache_status* ret
		, torrent_handle h, int flags) const
	{
		piece_manager* st = 0;
		boost::shared_ptr<torrent> t = h.m_torrent.lock();
		if (t)
		{
			if (t->has_storage())
				st = &t->storage();
			else
				flags = session::disk_cache_no_pieces;
		}
		m_impl->m_disk_thread.get_cache_info(ret, flags & session::disk_cache_no_pieces, st);
	}

	void session::start_dht()
	{
#ifndef TORRENT_DISABLE_DHT
		// the state is loaded in load_state()
		TORRENT_ASYNC_CALL(start_dht);
#endif
	}

	void session::stop_dht()
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_ASYNC_CALL(stop_dht);
#endif
	}

	void session::set_dht_settings(dht_settings const& settings)
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_ASYNC_CALL1(set_dht_settings, settings);
#endif
	}

#ifndef TORRENT_NO_DEPRECATE
	void session::start_dht(entry const& startup_state)
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_ASYNC_CALL1(start_dht, startup_state);
#endif
	}

	entry session::dht_state() const
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_SYNC_CALL_RET(entry, dht_state);
		return r;
#else
		return entry();
#endif
	}
#endif // TORRENT_NO_DEPRECATE
	
	void session::add_dht_node(std::pair<std::string, int> const& node)
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_ASYNC_CALL1(add_dht_node_name, node);
#endif
	}

	void session::add_dht_router(std::pair<std::string, int> const& node)
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_ASYNC_CALL1(add_dht_router, node);
#endif
	}

	bool session::is_dht_running() const
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_SYNC_CALL_RET(bool, is_dht_running);
		return r;
#else
		return false;
#endif
	}

#ifndef TORRENT_DISABLE_ENCRYPTION
	void session::set_pe_settings(pe_settings const& settings)
	{
		TORRENT_ASYNC_CALL1(set_pe_settings, settings);
	}

	pe_settings session::get_pe_settings() const
	{
		TORRENT_SYNC_CALL_RET(pe_settings, get_pe_settings);
		return r;
	}
#endif

	void session::set_peer_class_filter(ip_filter const& f)
	{
		TORRENT_ASYNC_CALL1(set_peer_class_filter, f);
	}

	void session::set_peer_class_type_filter(peer_class_type_filter const& f)
	{
		TORRENT_ASYNC_CALL1(set_peer_class_type_filter, f);
	}

	int session::create_peer_class(char const* name)
	{
		TORRENT_SYNC_CALL_RET1(int, create_peer_class, name);
		return r;
	}

	void session::delete_peer_class(int cid)
	{
		TORRENT_ASYNC_CALL1(delete_peer_class, cid);
	}

	peer_class_info session::get_peer_class(int cid)
	{
		TORRENT_SYNC_CALL_RET1(peer_class_info, get_peer_class, cid);
		return r;
	}

	void session::set_peer_class(int cid, peer_class_info const& pci)
	{
		TORRENT_ASYNC_CALL2(set_peer_class, cid, pci);
	}

	bool session::is_listening() const
	{
		TORRENT_SYNC_CALL_RET(bool, is_listening);
		return r;
	}

#ifndef TORRENT_NO_DEPRECATE
	void session::set_settings(session_settings const& s)
	{
		TORRENT_ASYNC_CALL1(set_settings, s);
	}

	session_settings session::settings() const
	{
		TORRENT_SYNC_CALL_RET(session_settings, deprecated_settings);
		return r;
	}
#endif

	void session::apply_settings(settings_pack const& s)
	{
		settings_pack* copy = new settings_pack(s);
		TORRENT_ASYNC_CALL1(apply_settings_pack, copy);
	}

	aux::session_settings session::get_settings() const
	{
		TORRENT_SYNC_CALL_RET(aux::session_settings, settings);
		return r;
	}

	void session::set_proxy(proxy_settings const& s)
	{
		TORRENT_ASYNC_CALL1(set_proxy, s);
	}

	proxy_settings session::proxy() const
	{
		TORRENT_SYNC_CALL_RET(proxy_settings, proxy);
		return r;
	}

#ifndef TORRENT_NO_DEPRECATE
	void session::set_peer_proxy(proxy_settings const& s)
	{
		TORRENT_ASYNC_CALL1(set_peer_proxy, s);
	}

	void session::set_web_seed_proxy(proxy_settings const& s)
	{
		TORRENT_ASYNC_CALL1(set_web_seed_proxy, s);
	}

	void session::set_tracker_proxy(proxy_settings const& s)
	{
		TORRENT_ASYNC_CALL1(set_tracker_proxy, s);
	}

	proxy_settings session::peer_proxy() const
	{
		TORRENT_SYNC_CALL_RET(proxy_settings, peer_proxy);
		return r;
	}

	proxy_settings session::web_seed_proxy() const
	{
		TORRENT_SYNC_CALL_RET(proxy_settings, web_seed_proxy);
		return r;
	}

	proxy_settings session::tracker_proxy() const
	{
		TORRENT_SYNC_CALL_RET(proxy_settings, tracker_proxy);
		return r;
	}


	void session::set_dht_proxy(proxy_settings const& s)
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_ASYNC_CALL1(set_dht_proxy, s);
#endif
	}

	proxy_settings session::dht_proxy() const
	{
#ifndef TORRENT_DISABLE_DHT
		TORRENT_SYNC_CALL_RET(proxy_settings, dht_proxy);
		return r;
#else
		return proxy_settings();
#endif
	}
#endif // TORRENT_NO_DEPRECATE

#if TORRENT_USE_I2P
	void session::set_i2p_proxy(proxy_settings const& s)
	{
		TORRENT_ASYNC_CALL1(set_i2p_proxy, s);
	}
	
	proxy_settings session::i2p_proxy() const
	{
		TORRENT_SYNC_CALL_RET(proxy_settings, i2p_proxy);
		return r;
	}
#endif

#ifdef TORRENT_STATS
	void session::enable_stats_logging(bool s)
	{
		TORRENT_ASYNC_CALL1(enable_stats_logging, s);
	}
#endif

#ifndef TORRENT_NO_DEPRECATE
	int session::max_uploads() const
	{
		TORRENT_SYNC_CALL_RET(int, max_uploads);
		return r;
	}

	void session::set_max_uploads(int limit)
	{
		TORRENT_ASYNC_CALL1(set_max_uploads, limit);
	}

	int session::max_connections() const
	{
		TORRENT_SYNC_CALL_RET(int, max_connections);
		return r;
	}

	void session::set_max_connections(int limit)
	{
		TORRENT_ASYNC_CALL1(set_max_connections, limit);
	}

	int session::max_half_open_connections() const
	{
		TORRENT_SYNC_CALL_RET(int, max_half_open_connections);
		return r;
	}

	void session::set_max_half_open_connections(int limit)
	{
		TORRENT_ASYNC_CALL1(set_max_half_open_connections, limit);
	}

	int session::local_upload_rate_limit() const
	{
		TORRENT_SYNC_CALL_RET(int, local_upload_rate_limit);
		return r;
	}

	int session::local_download_rate_limit() const
	{
		TORRENT_SYNC_CALL_RET(int, local_download_rate_limit);
		return r;
	}

	int session::upload_rate_limit() const
	{
		TORRENT_SYNC_CALL_RET(int, upload_rate_limit);
		return r;
	}

	int session::download_rate_limit() const
	{
		TORRENT_SYNC_CALL_RET(int, download_rate_limit);
		return r;
	}

	void session::set_local_upload_rate_limit(int bytes_per_second)
	{
		TORRENT_ASYNC_CALL1(set_local_upload_rate_limit, bytes_per_second);
	}

	void session::set_local_download_rate_limit(int bytes_per_second)
	{
		TORRENT_ASYNC_CALL1(set_local_download_rate_limit, bytes_per_second);
	}

	void session::set_upload_rate_limit(int bytes_per_second)
	{
		TORRENT_ASYNC_CALL1(set_upload_rate_limit, bytes_per_second);
	}

	void session::set_download_rate_limit(int bytes_per_second)
	{
		TORRENT_ASYNC_CALL1(set_download_rate_limit, bytes_per_second);
	}

	int session::num_uploads() const
	{
		TORRENT_SYNC_CALL_RET(int, num_uploads);
		return r;
	}

	int session::num_connections() const
	{
		TORRENT_SYNC_CALL_RET(int, num_connections);
		return r;
	}
#endif // TORRENT_NO_DEPRECATE

	void session::set_alert_dispatch(boost::function<void(std::auto_ptr<alert>)> const& fun)
	{
		TORRENT_ASYNC_CALL1(set_alert_dispatch, fun);
	}

	std::auto_ptr<alert> session::pop_alert()
	{
		return m_impl->pop_alert();
	}

	void session::pop_alerts(std::deque<alert*>* alerts)
	{
		for (std::deque<alert*>::iterator i = alerts->begin()
			, end(alerts->end()); i != end; ++i)
			delete *i;
		alerts->clear();
		m_impl->pop_alerts(alerts);
	}

	alert const* session::wait_for_alert(time_duration max_wait)
	{
		return m_impl->wait_for_alert(max_wait);
	}

	void session::set_alert_mask(boost::uint32_t m)
	{
		TORRENT_ASYNC_CALL1(set_alert_mask, m);
	}

#ifndef TORRENT_NO_DEPRECATE
	size_t session::set_alert_queue_size_limit(size_t queue_size_limit_)
	{
		TORRENT_SYNC_CALL_RET1(size_t, set_alert_queue_size_limit, queue_size_limit_);
		return r;
	}

	void session::set_severity_level(alert::severity_t s)
	{
		int m = 0;
		switch (s)
		{
			case alert::debug: m = alert::all_categories; break;
			case alert::info: m = alert::all_categories & ~(alert::debug_notification
				| alert::progress_notification | alert::dht_notification); break;
			case alert::warning: m = alert::all_categories & ~(alert::debug_notification
				| alert::status_notification | alert::progress_notification
				| alert::dht_notification); break;
			case alert::critical: m = alert::error_notification | alert::storage_notification; break;
			case alert::fatal: m = alert::error_notification; break;
			default: break;
		}

		TORRENT_ASYNC_CALL1(set_alert_mask, m);
	}
#endif

	void session::start_lsd()
	{
		TORRENT_ASYNC_CALL(start_lsd);
	}
	
	natpmp* session::start_natpmp()
	{
		TORRENT_SYNC_CALL_RET(natpmp*, start_natpmp);
		return r;
	}
	
	upnp* session::start_upnp()
	{
		TORRENT_SYNC_CALL_RET(upnp*, start_upnp);
		return r;
	}
	
	void session::stop_lsd()
	{
		TORRENT_ASYNC_CALL(stop_lsd);
	}
	
	void session::stop_natpmp()
	{
		TORRENT_ASYNC_CALL(stop_natpmp);
	}
	
	void session::stop_upnp()
	{
		TORRENT_ASYNC_CALL(stop_upnp);
	}
	
	connection_queue& session::get_connection_queue()
	{
		return m_impl->m_half_open;
	}

#ifndef TORRENT_NO_DEPRECATE
	session_settings::session_settings(std::string const& user_agent_)
	{
		aux::session_settings def;
		initialize_default_settings(def);
		def.set_str(settings_pack::user_agent, user_agent_);
		load_struct_from_settings(def, *this);
	}

	session_settings::~session_settings() {}
#endif // TORRENT_NO_DEPRECATE

}

