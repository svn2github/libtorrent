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

#ifndef TORRENT_ALERT_TYPES_HPP_INCLUDED
#define TORRENT_ALERT_TYPES_HPP_INCLUDED

#include "libtorrent/alert.hpp"
#include "libtorrent/torrent_handle.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/config.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/identify_client.hpp"
#include "libtorrent/address.hpp"
#include "libtorrent/stat.hpp"
#include "libtorrent/rss.hpp" // for feed_handle

namespace libtorrent
{

	// maps an operation id (from peer_erro_alert and peer_disconnected_alert)
	// to its name. See peer_connection for the constants
	TORRENT_EXPORT char const* operation_name(int op);

	// user defined alerts should use IDs greater than this
	const static int user_alert_id = 10000;

	struct TORRENT_EXPORT torrent_alert: alert
	{
		torrent_alert(torrent_handle const& h);
		
		const static int alert_type = 0;
		virtual std::string message() const;

		torrent_handle handle;
		std::string name;
	};

	struct TORRENT_EXPORT peer_alert: torrent_alert
	{
		peer_alert(torrent_handle const& h, tcp::endpoint const& ip_
			, peer_id const& pid_)
			: torrent_alert(h)
			, ip(ip_)
			, pid(pid_)
		{}

		const static int alert_type = 1;
		const static int static_category = alert::peer_notification;
		virtual int category() const { return static_category; }
		virtual std::string message() const;

		tcp::endpoint ip;
		peer_id pid;
	};

	struct TORRENT_EXPORT tracker_alert: torrent_alert
	{
		tracker_alert(torrent_handle const& h
			, std::string const& url_)
			: torrent_alert(h)
			, url(url_)
		{}

		const static int alert_type = 2;
		const static int static_category = alert::tracker_notification;
		virtual int category() const { return static_category; }
		virtual std::string message() const;

		std::string url;
	};

#define TORRENT_DEFINE_ALERT(name, seq) \
	const static int alert_type = seq; \
	virtual int type() const { return alert_type; } \
	virtual std::auto_ptr<alert> clone() const \
	{ return std::auto_ptr<alert>(new name(*this)); } \
	virtual int category() const { return static_category; } \
	virtual char const* what() const { return #name; }

	struct TORRENT_EXPORT torrent_added_alert: torrent_alert
	{
		torrent_added_alert(torrent_handle const& h)
			: torrent_alert(h)
		{}

		TORRENT_DEFINE_ALERT(torrent_added_alert, 3);
		const static int static_category = alert::status_notification;
		virtual std::string message() const;
	};

	struct TORRENT_EXPORT torrent_removed_alert: torrent_alert
	{
		torrent_removed_alert(torrent_handle const& h, sha1_hash const& ih)
			: torrent_alert(h)
			, info_hash(ih)
		{}

		TORRENT_DEFINE_ALERT(torrent_removed_alert, 4);
		const static int static_category = alert::status_notification;
		virtual std::string message() const;
		sha1_hash info_hash;
	};

	struct TORRENT_EXPORT read_piece_alert: torrent_alert
	{
		read_piece_alert(torrent_handle const& h
			, int p, boost::shared_array<char> d, int s)
			: torrent_alert(h)
			, buffer(d)
			, piece(p)
			, size(s)
		{}

		read_piece_alert(torrent_handle h, int p, error_code e)
			: torrent_alert(h)
			, ec(e)
			, piece(p)
			, size(0)
		{}

		TORRENT_DEFINE_ALERT(read_piece_alert, 5);

		const static int static_category = alert::storage_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		error_code ec;
		boost::shared_array<char> buffer;
		int piece;
		int size;
	};

	struct TORRENT_EXPORT file_completed_alert: torrent_alert
	{
		file_completed_alert(torrent_handle const& h
			, int index_)
			: torrent_alert(h)
			, index(index_)
		{}

		TORRENT_DEFINE_ALERT(file_completed_alert, 6);

		const static int static_category = alert::progress_notification;
		virtual std::string message() const;

		int index;
	};

	struct TORRENT_EXPORT file_renamed_alert: torrent_alert
	{
		file_renamed_alert(torrent_handle const& h
			, std::string const& name_
			, int index_)
			: torrent_alert(h)
			, name(name_)
			, index(index_)
		{}

		TORRENT_DEFINE_ALERT(file_renamed_alert, 7);

		const static int static_category = alert::storage_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		std::string name;
		int index;
	};

	struct TORRENT_EXPORT file_rename_failed_alert: torrent_alert
	{
		file_rename_failed_alert(torrent_handle const& h
			, int index_
			, error_code ec_)
			: torrent_alert(h)
			, index(index_)
			, error(ec_)
		{}

		TORRENT_DEFINE_ALERT(file_rename_failed_alert, 8);

		const static int static_category = alert::storage_notification;

		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		int index;
		error_code error;
	};

	struct TORRENT_EXPORT performance_alert: torrent_alert
	{
		enum performance_warning_t
		{
			outstanding_disk_buffer_limit_reached,
			outstanding_request_limit_reached,
			upload_limit_too_low,
			download_limit_too_low,
			send_buffer_watermark_too_low,
			too_many_optimistic_unchoke_slots,
			bittyrant_with_no_uplimit,
			too_high_disk_queue_limit,
			aio_limit_reached,
			too_few_outgoing_ports,
			too_few_file_descriptors,





			num_warnings
		};

		performance_alert(torrent_handle const& h
			, performance_warning_t w)
			: torrent_alert(h)
			, warning_code(w)
		{}

		TORRENT_DEFINE_ALERT(performance_alert, 9);

		const static int static_category = alert::performance_warning;

		virtual std::string message() const;

		performance_warning_t warning_code;
	};

	struct TORRENT_EXPORT state_changed_alert: torrent_alert
	{
		state_changed_alert(torrent_handle const& h
			, torrent_status::state_t state_
			, torrent_status::state_t prev_state_)
			: torrent_alert(h)
			, state(state_)
			, prev_state(prev_state_)
		{}

		TORRENT_DEFINE_ALERT(state_changed_alert, 10);

		const static int static_category = alert::status_notification;

		virtual std::string message() const;

		torrent_status::state_t state;
		torrent_status::state_t prev_state;
	};

	struct TORRENT_EXPORT tracker_error_alert: tracker_alert
	{
		tracker_error_alert(torrent_handle const& h
			, int times
			, int status
			, std::string const& url_
			, error_code const& e
			, std::string const& m)
			: tracker_alert(h, url_)
			, times_in_row(times)
			, status_code(status)
			, error(e)
			, msg(m)
		{
			TORRENT_ASSERT(!url.empty());
		}

		TORRENT_DEFINE_ALERT(tracker_error_alert, 11);

		const static int static_category = alert::tracker_notification | alert::error_notification;
		virtual std::string message() const;

		int times_in_row;
		int status_code;
		error_code error;
		std::string msg;
	};

	struct TORRENT_EXPORT tracker_warning_alert: tracker_alert
	{
		tracker_warning_alert(torrent_handle const& h
			, std::string const& url_
			, std::string const& msg_)
			: tracker_alert(h, url_)
			, msg(msg_)
		{ TORRENT_ASSERT(!url.empty()); }

		TORRENT_DEFINE_ALERT(tracker_warning_alert, 12);

		const static int static_category = alert::tracker_notification | alert::error_notification;
		virtual std::string message() const;

		std::string msg;
	};

	struct TORRENT_EXPORT scrape_reply_alert: tracker_alert
	{
		scrape_reply_alert(torrent_handle const& h
			, int incomplete_
			, int complete_
			, std::string const& url_)
			: tracker_alert(h, url_)
			, incomplete(incomplete_)
			, complete(complete_)
		{ TORRENT_ASSERT(!url.empty()); }

		TORRENT_DEFINE_ALERT(scrape_reply_alert, 13);

		virtual std::string message() const;

		int incomplete;
		int complete;
	};

	struct TORRENT_EXPORT scrape_failed_alert: tracker_alert
	{
		scrape_failed_alert(torrent_handle const& h
			, std::string const& url_
			, error_code const& e)
			: tracker_alert(h, url_)
			, msg(e.message())
		{ TORRENT_ASSERT(!url.empty()); }

		scrape_failed_alert(torrent_handle const& h
			, std::string const& url_
			, std::string const& msg_)
			: tracker_alert(h, url_)
			, msg(msg_)
		{ TORRENT_ASSERT(!url.empty()); }

		TORRENT_DEFINE_ALERT(scrape_failed_alert, 14);

		const static int static_category = alert::tracker_notification | alert::error_notification;
		virtual std::string message() const;

		std::string msg;
	};

	struct TORRENT_EXPORT tracker_reply_alert: tracker_alert
	{
		tracker_reply_alert(torrent_handle const& h
			, int np
			, std::string const& url_)
			: tracker_alert(h, url_)
			, num_peers(np)
		{ TORRENT_ASSERT(!url.empty()); }

		TORRENT_DEFINE_ALERT(tracker_reply_alert, 15);

		virtual std::string message() const;

		int num_peers;
	};

	struct TORRENT_EXPORT dht_reply_alert: tracker_alert
	{
		dht_reply_alert(torrent_handle const& h
			, int np)
			: tracker_alert(h, "")
			, num_peers(np)
		{}

		TORRENT_DEFINE_ALERT(dht_reply_alert, 16);

		virtual std::string message() const;

		int num_peers;
	};

	struct TORRENT_EXPORT tracker_announce_alert: tracker_alert
	{
		tracker_announce_alert(torrent_handle const& h
			, std::string const& url_, int event_)
			: tracker_alert(h, url_)
			, event(event_)
		{ TORRENT_ASSERT(!url.empty()); }

		TORRENT_DEFINE_ALERT(tracker_announce_alert, 17);

		virtual std::string message() const;

		int event;
	};
	
	struct TORRENT_EXPORT hash_failed_alert: torrent_alert
	{
		hash_failed_alert(
			torrent_handle const& h
			, int index)
			: torrent_alert(h)
			, piece_index(index)
		{ TORRENT_ASSERT(index >= 0);}

		TORRENT_DEFINE_ALERT(hash_failed_alert, 18);

		const static int static_category = alert::status_notification;
		virtual std::string message() const;

		int piece_index;
	};

	struct TORRENT_EXPORT peer_ban_alert: peer_alert
	{
		peer_ban_alert(torrent_handle h, tcp::endpoint const& ep
			, peer_id const& peer_id)
			: peer_alert(h, ep, peer_id)
		{}

		TORRENT_DEFINE_ALERT(peer_ban_alert, 19);

		virtual std::string message() const;
	};

	struct TORRENT_EXPORT peer_unsnubbed_alert: peer_alert
	{
		peer_unsnubbed_alert(torrent_handle h, tcp::endpoint const& ep
			, peer_id const& peer_id)
			: peer_alert(h, ep, peer_id)
		{}

		TORRENT_DEFINE_ALERT(peer_unsnubbed_alert, 20);

		virtual std::string message() const;
	};

	struct TORRENT_EXPORT peer_snubbed_alert: peer_alert
	{
		peer_snubbed_alert(torrent_handle h, tcp::endpoint const& ep
			, peer_id const& peer_id)
			: peer_alert(h, ep, peer_id)
		{}

		TORRENT_DEFINE_ALERT(peer_snubbed_alert, 21);

		virtual std::string message() const;
	};

	struct TORRENT_EXPORT peer_error_alert: peer_alert
	{
		peer_error_alert(torrent_handle const& h, tcp::endpoint const& ep
			, peer_id const& peer_id, int op, error_code const& e)
			: peer_alert(h, ep, peer_id)
			, operation(op)
			, error(e)
		{
#ifndef TORRENT_NO_DEPRECATE
			msg = error.message();
#endif
		}

		TORRENT_DEFINE_ALERT(peer_error_alert, 22);

		const static int static_category = alert::peer_notification;
		virtual std::string message() const;

		int operation;
		error_code error;

#ifndef TORRENT_NO_DEPRECATE
		std::string msg;
#endif
	};

	struct TORRENT_EXPORT peer_connect_alert: peer_alert
	{
		peer_connect_alert(torrent_handle h, tcp::endpoint const& ep
			, peer_id const& peer_id, int type)
			: peer_alert(h, ep, peer_id)
			, socket_type(type)
		{}

		TORRENT_DEFINE_ALERT(peer_connect_alert, 23);

		const static int static_category = alert::debug_notification;
		virtual std::string message() const;;

		int socket_type;
	};

	struct TORRENT_EXPORT peer_disconnected_alert: peer_alert
	{
		peer_disconnected_alert(torrent_handle const& h, tcp::endpoint const& ep
			, peer_id const& peer_id, int op, error_code const& e)
			: peer_alert(h, ep, peer_id)
			, operation(op)
			, error(e)
		{
#ifndef TORRENT_NO_DEPRECATE
			msg = error.message();
#endif
		}

		TORRENT_DEFINE_ALERT(peer_disconnected_alert, 24);

		const static int static_category = alert::debug_notification;
		virtual std::string message() const;

		int operation;
		error_code error;

#ifndef TORRENT_NO_DEPRECATE
		std::string msg;
#endif
	};

	struct TORRENT_EXPORT invalid_request_alert: peer_alert
	{
		invalid_request_alert(torrent_handle const& h, tcp::endpoint const& ep
			, peer_id const& peer_id, peer_request const& r)
			: peer_alert(h, ep, peer_id)
			, request(r)
		{}

		TORRENT_DEFINE_ALERT(invalid_request_alert, 25);

		virtual std::string message() const;

		peer_request request;
	};

	struct TORRENT_EXPORT torrent_finished_alert: torrent_alert
	{
		torrent_finished_alert(
			const torrent_handle& h)
			: torrent_alert(h)
		{}

		TORRENT_DEFINE_ALERT(torrent_finished_alert, 26);

		const static int static_category = alert::status_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " torrent finished downloading"; }
	};

	struct TORRENT_EXPORT piece_finished_alert: torrent_alert
	{
		piece_finished_alert(
			const torrent_handle& h
			, int piece_num)
			: torrent_alert(h)
			, piece_index(piece_num)
		{ TORRENT_ASSERT(piece_index >= 0);}

		TORRENT_DEFINE_ALERT(piece_finished_alert, 27);

		const static int static_category = alert::progress_notification;
		virtual std::string message() const;

		int piece_index;
	};

	struct TORRENT_EXPORT request_dropped_alert: peer_alert
	{
		request_dropped_alert(const torrent_handle& h, tcp::endpoint const& ep
			, peer_id const& peer_id, int block_num, int piece_num)
			: peer_alert(h, ep, peer_id)
			, block_index(block_num)
			, piece_index(piece_num)
		{ TORRENT_ASSERT(block_index >= 0 && piece_index >= 0);}

		TORRENT_DEFINE_ALERT(request_dropped_alert, 28);

		const static int static_category = alert::progress_notification
			| alert::peer_notification;
		virtual std::string message() const;

		int block_index;
		int piece_index;
	};

	struct TORRENT_EXPORT block_timeout_alert: peer_alert
	{
		block_timeout_alert(const torrent_handle& h, tcp::endpoint const& ep
			, peer_id const& peer_id, int block_num, int piece_num)
			: peer_alert(h, ep, peer_id)
			, block_index(block_num)
			, piece_index(piece_num)
		{ TORRENT_ASSERT(block_index >= 0 && piece_index >= 0);}

		TORRENT_DEFINE_ALERT(block_timeout_alert, 29);

		const static int static_category = alert::progress_notification
			| alert::peer_notification;
		virtual std::string message() const;

		int block_index;
		int piece_index;
	};

	struct TORRENT_EXPORT block_finished_alert: peer_alert
	{
		block_finished_alert(const torrent_handle& h, tcp::endpoint const& ep
			, peer_id const& peer_id, int block_num, int piece_num)
			: peer_alert(h, ep, peer_id)
			, block_index(block_num)
			, piece_index(piece_num)
		{ TORRENT_ASSERT(block_index >= 0 && piece_index >= 0);}

		TORRENT_DEFINE_ALERT(block_finished_alert, 30);

		const static int static_category = alert::progress_notification;
		virtual std::string message() const;

		int block_index;
		int piece_index;
	};

	struct TORRENT_EXPORT block_downloading_alert: peer_alert
	{
		block_downloading_alert(const torrent_handle& h, tcp::endpoint const& ep
			, peer_id const& peer_id, char const* speedmsg, int block_num, int piece_num)
			: peer_alert(h, ep, peer_id)
			, peer_speedmsg(speedmsg)
			, block_index(block_num)
			, piece_index(piece_num)
		{ TORRENT_ASSERT(block_index >= 0 && piece_index >= 0); }

		TORRENT_DEFINE_ALERT(block_downloading_alert, 31);

		const static int static_category = alert::progress_notification;
		virtual std::string message() const;

		char const* peer_speedmsg;
		int block_index;
		int piece_index;
	};

	struct TORRENT_EXPORT unwanted_block_alert: peer_alert
	{
		unwanted_block_alert(const torrent_handle& h, tcp::endpoint const& ep
			, peer_id const& peer_id, int block_num, int piece_num)
			: peer_alert(h, ep, peer_id)
			, block_index(block_num)
			, piece_index(piece_num)
		{ TORRENT_ASSERT(block_index >= 0 && piece_index >= 0);}

		TORRENT_DEFINE_ALERT(unwanted_block_alert, 32);

		virtual std::string message() const;

		int block_index;
		int piece_index;
	};

	struct TORRENT_EXPORT storage_moved_alert: torrent_alert
	{
		storage_moved_alert(torrent_handle const& h, std::string const& path_)
			: torrent_alert(h)
			, path(path_)
		{}
	
		TORRENT_DEFINE_ALERT(storage_moved_alert, 33);

		const static int static_category = alert::storage_notification;
		virtual std::string message() const
		{
			return torrent_alert::message() + " moved storage to: "
				+ path;
		}

		std::string path;
	};

	struct TORRENT_EXPORT storage_moved_failed_alert: torrent_alert
	{
		storage_moved_failed_alert(torrent_handle const& h
			, error_code const& ec
			, std::string const& file
			, char const* op)
			: torrent_alert(h)
			, error(ec)
			, file(file)
			, operation(op)
		{}
	
		TORRENT_DEFINE_ALERT(storage_moved_failed_alert, 34);

		const static int static_category = alert::storage_notification;
		virtual std::string message() const
		{
			return torrent_alert::message() + " storage move failed. "
				+ (operation?operation:"") + " (" + file + "): "
				+ error.message();
		}

		error_code error;
		std::string file;
		char const* operation;
	};

	struct TORRENT_EXPORT torrent_deleted_alert: torrent_alert
	{
		torrent_deleted_alert(torrent_handle const& h, sha1_hash const& ih)
			: torrent_alert(h)
		{ info_hash = ih; }
	
		TORRENT_DEFINE_ALERT(torrent_deleted_alert, 35);

		const static int static_category = alert::storage_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " deleted"; }
		virtual bool discardable() const { return false; }

		sha1_hash info_hash;
	};

	struct TORRENT_EXPORT torrent_delete_failed_alert: torrent_alert
	{
		torrent_delete_failed_alert(torrent_handle const& h, error_code const& e)
			: torrent_alert(h)
			, error(e)
		{
#ifndef TORRENT_NO_DEPRECATE
			msg = error.message();
#endif
		}
	
		TORRENT_DEFINE_ALERT(torrent_delete_failed_alert, 36);

		const static int static_category = alert::storage_notification
			| alert::error_notification;
		virtual std::string message() const
		{
			return torrent_alert::message() + " torrent deletion failed: "
				+ error.message();
		}
		virtual bool discardable() const { return false; }

		error_code error;

#ifndef TORRENT_NO_DEPRECATE
		std::string msg;
#endif
	};

	struct TORRENT_EXPORT save_resume_data_alert: torrent_alert
	{
		save_resume_data_alert(boost::shared_ptr<entry> const& rd
			, torrent_handle const& h)
			: torrent_alert(h)
			, resume_data(rd)
		{}
	
		TORRENT_DEFINE_ALERT(save_resume_data_alert, 37);

		const static int static_category = alert::storage_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " resume data generated"; }
		virtual bool discardable() const { return false; }

		boost::shared_ptr<entry> resume_data;
	};

	struct TORRENT_EXPORT save_resume_data_failed_alert: torrent_alert
	{
		save_resume_data_failed_alert(torrent_handle const& h
			, error_code const& e)
			: torrent_alert(h)
			, error(e)
		{
#ifndef TORRENT_NO_DEPRECATE
			msg = error.message();
#endif
		}
	
		TORRENT_DEFINE_ALERT(save_resume_data_failed_alert, 38);

		const static int static_category = alert::storage_notification
			| alert::error_notification;
		virtual std::string message() const
		{
			return torrent_alert::message() + " resume data was not generated: "
				+ error.message();
		}
		virtual bool discardable() const { return false; }

		error_code error;

#ifndef TORRENT_NO_DEPRECATE
		std::string msg;
#endif
	};

	struct TORRENT_EXPORT torrent_paused_alert: torrent_alert
	{
		torrent_paused_alert(torrent_handle const& h)
			: torrent_alert(h)
		{}
	
		TORRENT_DEFINE_ALERT(torrent_paused_alert, 39);

		const static int static_category = alert::status_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " paused"; }
	};

	struct TORRENT_EXPORT torrent_resumed_alert: torrent_alert
	{
		torrent_resumed_alert(torrent_handle const& h)
			: torrent_alert(h) {}

		TORRENT_DEFINE_ALERT(torrent_resumed_alert, 40);

		const static int static_category = alert::status_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " resumed"; }
	};

	struct TORRENT_EXPORT torrent_checked_alert: torrent_alert
	{
		torrent_checked_alert(torrent_handle const& h)
			: torrent_alert(h)
		{}

		TORRENT_DEFINE_ALERT(torrent_checked_alert, 41);

		const static int static_category = alert::status_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " checked"; }
	};

	struct TORRENT_EXPORT url_seed_alert: torrent_alert
	{
		url_seed_alert(
			torrent_handle const& h
			, std::string const& url_
			, error_code const& e)
			: torrent_alert(h)
			, url(url_)
			, msg(e.message())
		{}

		url_seed_alert(
			torrent_handle const& h
			, std::string const& url_
			, std::string const& msg_)
			: torrent_alert(h)
			, url(url_)
			, msg(msg_)
		{}

		TORRENT_DEFINE_ALERT(url_seed_alert, 42);

		const static int static_category = alert::peer_notification | alert::error_notification;
		virtual std::string message() const
		{
			return torrent_alert::message() + " url seed ("
				+ url + ") failed: " + msg;
		}

		std::string url;
		std::string msg;
	};

	struct TORRENT_EXPORT file_error_alert: torrent_alert
	{
		file_error_alert(
			error_code const& ec
			, std::string const& file
			, char const* op
			, torrent_handle const& h)
			: torrent_alert(h)
			, file(file)
			, error(ec)
			, operation(op)
		{
#ifndef TORRENT_NO_DEPRECATE
			msg = error.message();
#endif
		}

		TORRENT_DEFINE_ALERT(file_error_alert, 43);

		const static int static_category = alert::status_notification
			| alert::error_notification
			| alert::storage_notification;
		virtual std::string message() const
		{
			return torrent_alert::message() + " "
				+ (operation?operation:"") + " (" + file
				+ ") error: " + error.message();
		}

		std::string file;
		error_code error;
		char const* operation;

#ifndef TORRENT_NO_DEPRECATE
		std::string msg;
#endif
	};

	struct TORRENT_EXPORT metadata_failed_alert: torrent_alert
	{
		metadata_failed_alert(const torrent_handle& h, error_code const& ec)
			: torrent_alert(h)
			, error(ec)
		{}

		TORRENT_DEFINE_ALERT(metadata_failed_alert, 44);

		const static int static_category = alert::error_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " invalid metadata received: " + error.message(); }

		error_code error;
	};
	
	struct TORRENT_EXPORT metadata_received_alert: torrent_alert
	{
		metadata_received_alert(
			const torrent_handle& h)
			: torrent_alert(h)
		{}

		TORRENT_DEFINE_ALERT(metadata_received_alert, 45);

		const static int static_category = alert::status_notification;
		virtual std::string message() const
		{ return torrent_alert::message() + " metadata successfully received"; }
	};

	struct TORRENT_EXPORT udp_error_alert: alert
	{
		udp_error_alert(
			udp::endpoint const& ep
			, error_code const& ec)
			: endpoint(ep)
			, error(ec)
		{}

		TORRENT_DEFINE_ALERT(udp_error_alert, 46);

		const static int static_category = alert::error_notification;
		virtual std::string message() const
		{
			error_code ec;
			return "UDP error: " + error.message() + " from: " + endpoint.address().to_string(ec);
		}

		udp::endpoint endpoint;
		error_code error;
	};

	struct TORRENT_EXPORT external_ip_alert: alert
	{
		external_ip_alert(address const& ip)
			: external_address(ip)
		{}

		TORRENT_DEFINE_ALERT(external_ip_alert, 47);

		const static int static_category = alert::status_notification;
		virtual std::string message() const
		{
			error_code ec;
			return "external IP received: " + external_address.to_string(ec);
		}

		address external_address;
	};

	struct TORRENT_EXPORT listen_failed_alert: alert
	{
		listen_failed_alert(
			tcp::endpoint const& ep
			, int op
			, error_code const& ec)
			: endpoint(ep)
			, error(ec)
			, operation(op)
		{}

		TORRENT_DEFINE_ALERT(listen_failed_alert, 48);

		const static int static_category = alert::status_notification | alert::error_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		tcp::endpoint endpoint;
		error_code error;
		enum op_t
		{
			parse_addr, open, bind, listen, get_peer_name, accept
		};
		int operation;
	};

	struct TORRENT_EXPORT listen_succeeded_alert: alert
	{
		listen_succeeded_alert(tcp::endpoint const& ep)
			: endpoint(ep)
		{}

		TORRENT_DEFINE_ALERT(listen_succeeded_alert, 49);

		const static int static_category = alert::status_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		tcp::endpoint endpoint;
	};

	struct TORRENT_EXPORT portmap_error_alert: alert
	{
		portmap_error_alert(int i, int t, error_code const& e)
			:  mapping(i), map_type(t), error(e)
		{
#ifndef TORRENT_NO_DEPRECATE
			msg = error.message();
#endif
		}

		TORRENT_DEFINE_ALERT(portmap_error_alert, 50);

		const static int static_category = alert::port_mapping_notification
			| alert::error_notification;
		virtual std::string message() const;

		int mapping;
		int map_type;
		error_code error;
#ifndef TORRENT_NO_DEPRECATE
		std::string msg;
#endif
	};

	struct TORRENT_EXPORT portmap_alert: alert
	{
		portmap_alert(int i, int port, int t)
			: mapping(i), external_port(port), map_type(t)
		{}

		TORRENT_DEFINE_ALERT(portmap_alert, 51);

		const static int static_category = alert::port_mapping_notification;
		virtual std::string message() const;

		int mapping;
		int external_port;
		int map_type;
	};

	struct TORRENT_EXPORT portmap_log_alert: alert
	{
		portmap_log_alert(int t, std::string const& m)
			: map_type(t), msg(m)
		{}

		TORRENT_DEFINE_ALERT(portmap_log_alert, 52);

		const static int static_category = alert::port_mapping_notification;
		virtual std::string message() const;

		int map_type;
		std::string msg;
	};

	struct TORRENT_EXPORT fastresume_rejected_alert: torrent_alert
	{
		fastresume_rejected_alert(torrent_handle const& h
			, error_code const& ec
			, std::string const& file
			, char const* op)
			: torrent_alert(h)
			, error(ec)
			, file(file)
			, operation(op)
		{
#ifndef TORRENT_NO_DEPRECATE
			msg = error.message();
#endif
		}

		TORRENT_DEFINE_ALERT(fastresume_rejected_alert, 53);

		const static int static_category = alert::status_notification
			| alert::error_notification;
		virtual std::string message() const
		{
			return torrent_alert::message() + " fast resume rejected. "
				+ (operation?operation:"") + "(" + file + "): " + error.message();
		}

		error_code error;
		std::string file;
		char const* operation;

#ifndef TORRENT_NO_DEPRECATE
		std::string msg;
#endif
	};

	struct TORRENT_EXPORT peer_blocked_alert: torrent_alert
	{
		peer_blocked_alert(torrent_handle const& h, address const& ip_)
			: torrent_alert(h)
			, ip(ip_)
		{}
		
		TORRENT_DEFINE_ALERT(peer_blocked_alert, 54);

		const static int static_category = alert::ip_block_notification;
		virtual std::string message() const
		{
			error_code ec;
			return torrent_alert::message() + ": blocked peer: " + ip.to_string(ec);
		}

		address ip;
	};

	struct TORRENT_EXPORT dht_announce_alert: alert
	{
		dht_announce_alert(address const& ip_, int port_
			, sha1_hash const& info_hash_)
			: ip(ip_)
			, port(port_)
			, info_hash(info_hash_)
		{}
		
		TORRENT_DEFINE_ALERT(dht_announce_alert, 55);

		const static int static_category = alert::dht_notification;
		virtual std::string message() const;

		address ip;
		int port;
		sha1_hash info_hash;
	};

	struct TORRENT_EXPORT dht_get_peers_alert: alert
	{
		dht_get_peers_alert(sha1_hash const& info_hash_)
			: info_hash(info_hash_)
		{}

		TORRENT_DEFINE_ALERT(dht_get_peers_alert, 56);

		const static int static_category = alert::dht_notification;
		virtual std::string message() const;

		sha1_hash info_hash;
	};

	struct TORRENT_EXPORT stats_alert: torrent_alert
	{
		stats_alert(torrent_handle const& h, int interval
			, stat const& s);

		TORRENT_DEFINE_ALERT(stats_alert, 57);

		const static int static_category = alert::stats_notification;
		virtual std::string message() const;

		enum stats_channel
		{
			upload_payload,
			upload_protocol,
			download_payload,
			download_protocol,
#ifndef TORRENT_DISABLE_FULL_STATS
			upload_ip_protocol,
			upload_dht_protocol,
			upload_tracker_protocol,
			download_ip_protocol,
			download_dht_protocol,
			download_tracker_protocol,
#endif
			num_channels
		};

		int transferred[num_channels];
		int interval;
	};

	struct TORRENT_EXPORT cache_flushed_alert: torrent_alert
	{
		cache_flushed_alert(torrent_handle const& h);

		TORRENT_DEFINE_ALERT(cache_flushed_alert, 58);

		const static int static_category = alert::storage_notification;
	};

	struct TORRENT_EXPORT anonymous_mode_alert: torrent_alert
	{
		anonymous_mode_alert(torrent_handle const& h
			, int kind_, std::string const& str_)
			: torrent_alert(h)
			, kind(kind_)
			, str(str_)
		{}

		TORRENT_DEFINE_ALERT(anonymous_mode_alert, 59);

		const static int static_category = alert::error_notification;
		virtual std::string message() const;

		enum kind_t
		{
			tracker_not_anonymous = 0
		};

		int kind;
		std::string str;
	};

	struct TORRENT_EXPORT lsd_peer_alert: peer_alert
	{
		lsd_peer_alert(torrent_handle const& h
			, tcp::endpoint const& ip_)
			: peer_alert(h, ip_, peer_id(0))
		{}

		TORRENT_DEFINE_ALERT(lsd_peer_alert, 60);

		const static int static_category = alert::peer_notification;
		virtual std::string message() const;
	};

	struct TORRENT_EXPORT trackerid_alert: tracker_alert
	{
		trackerid_alert(torrent_handle const& h
			, std::string const& url_
                        , const std::string& id)
			: tracker_alert(h, url_)
			, trackerid(id)
		{}

		TORRENT_DEFINE_ALERT(trackerid_alert, 61);

		const static int static_category = alert::status_notification;
		virtual std::string message() const;

		std::string trackerid;
	};

	struct TORRENT_EXPORT dht_bootstrap_alert: alert
	{
		dht_bootstrap_alert() {}
		
		TORRENT_DEFINE_ALERT(dht_bootstrap_alert, 62);

		const static int static_category = alert::dht_notification;
		virtual std::string message() const;
	};

	struct TORRENT_EXPORT rss_alert: alert
	{
		rss_alert(feed_handle h, std::string const& url_, int state_, error_code const& ec)
			: handle(h), url(url_), state(state_), error(ec)
		{}

		TORRENT_DEFINE_ALERT(rss_alert, 63);

		const static int static_category = alert::rss_notification;
		virtual std::string message() const;

		enum state_t
		{
			state_updating, state_updated, state_error
		};

		feed_handle handle;
		std::string url;
		int state;
		error_code error;
	};

	struct TORRENT_EXPORT torrent_error_alert: torrent_alert
	{
		torrent_error_alert(torrent_handle const& h
			, error_code const& e, std::string const& f)
			: torrent_alert(h)
			, error(e)
			, error_file(f)
		{}

		TORRENT_DEFINE_ALERT(torrent_error_alert, 64);

		const static int static_category = alert::error_notification | alert::status_notification;
		virtual std::string message() const;

		error_code error;
		std::string error_file;
	};

	struct TORRENT_EXPORT torrent_need_cert_alert: torrent_alert
	{
		torrent_need_cert_alert(torrent_handle const& h)
			: torrent_alert(h)
		{}

		TORRENT_DEFINE_ALERT(torrent_need_cert_alert, 65);

		const static int static_category = alert::status_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		error_code error;
	};

	struct TORRENT_EXPORT incoming_connection_alert: alert
	{
		incoming_connection_alert(int type_, tcp::endpoint const& ip_)
			: socket_type(type_)
			, ip(ip_)
		{}

		TORRENT_DEFINE_ALERT(incoming_connection_alert, 66);

		const static int static_category = alert::peer_notification;
		virtual std::string message() const;

		int socket_type;
		tcp::endpoint ip;
	};

	struct TORRENT_EXPORT add_torrent_alert : torrent_alert
	{
		add_torrent_alert(torrent_handle h, add_torrent_params const& p, error_code ec)
			: torrent_alert(h)
			, params(p)
			, error(ec)
		{}

		TORRENT_DEFINE_ALERT(add_torrent_alert, 67);

		const static int static_category = alert::status_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		add_torrent_params params;
		error_code error;
	};

	struct TORRENT_EXPORT state_update_alert : alert
	{
		TORRENT_DEFINE_ALERT(state_update_alert, 68);

		const static int static_category = alert::status_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		std::vector<torrent_status> status;
	};
	
	struct TORRENT_EXPORT mmap_cache_alert : alert
	{
		mmap_cache_alert(error_code const& ec): error(ec) {}
		TORRENT_DEFINE_ALERT(mmap_cache_alert, 69);

		const static int static_category = alert::error_notification;
		virtual std::string message() const;

		error_code error;
	};

	struct TORRENT_EXPORT session_stats_alert : alert
	{
		session_stats_alert() {}
		TORRENT_DEFINE_ALERT(session_stats_alert, 70);

		const static int static_category = alert::stats_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		// microseconds since session start
		boost::uint64_t timestamp;

		std::vector<boost::uint64_t> values;
	};

	struct TORRENT_EXPORT torrent_update_alert : torrent_alert
	{
		torrent_update_alert(torrent_handle h, sha1_hash const& old_hash, sha1_hash const& new_hash)
			: torrent_alert(h)
			, old_ih(old_hash)
			, new_ih(new_hash)
		{}

		TORRENT_DEFINE_ALERT(torrent_update_alert, 71);

		const static int static_category = alert::status_notification;
		virtual std::string message() const;
		virtual bool discardable() const { return false; }

		sha1_hash old_ih;
		sha1_hash new_ih;
	};

	struct TORRENT_EXPORT rss_item_alert : alert
	{
		rss_item_alert(feed_handle h, feed_item const& item)
			: handle(h)
			, item(item)
		{}

		TORRENT_DEFINE_ALERT(rss_item_alert, 72);

		const static int static_category = alert::rss_notification;
		virtual std::string message() const;

		feed_handle handle;
		feed_item item;
	};


#undef TORRENT_DEFINE_ALERT

	enum { num_alert_types = 73 };
}


#endif
