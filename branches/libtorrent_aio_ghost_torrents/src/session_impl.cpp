/*

Copyright (c) 2006, Arvid Norberg, Magnus Jonsson
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
#include <cctype>
#include <algorithm>

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/limits.hpp>
#include <boost/bind.hpp>
#include <boost/function_equal.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

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
#ifndef TORRENT_DISABLE_DHT
#include "libtorrent/kademlia/dht_tracker.hpp"
#endif
#include "libtorrent/enum_net.hpp"
#include "libtorrent/config.hpp"
#include "libtorrent/utf8.hpp"
#include "libtorrent/upnp.hpp"
#include "libtorrent/natpmp.hpp"
#include "libtorrent/lsd.hpp"
#include "libtorrent/instantiate_connection.hpp"
#include "libtorrent/peer_info.hpp"
#include "libtorrent/settings.hpp"
#include "libtorrent/build_config.hpp"
#include "libtorrent/extensions.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/magnet_uri.hpp"
#include "libtorrent/aux_/session_settings.hpp"

#if defined TORRENT_STATS && defined __MACH__
#include <mach/task.h>
#endif

#ifndef TORRENT_WINDOWS
#include <sys/resource.h>
#endif

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING

// for logging stat layout
#include "libtorrent/stat.hpp"
#include "libtorrent/struct_debug.hpp"

// for logging the size of DHT structures
#ifndef TORRENT_DISABLE_DHT
#include <libtorrent/kademlia/find_data.hpp>
#include <libtorrent/kademlia/refresh.hpp>
#include <libtorrent/kademlia/node.hpp>
#include <libtorrent/kademlia/observer.hpp>
#endif // TORRENT_DISABLE_DHT

#include "libtorrent/http_tracker_connection.hpp"
#include "libtorrent/udp_tracker_connection.hpp"

#include "libtorrent/debug.hpp"

#if TORRENT_USE_IOSTREAM
namespace libtorrent {
std::ofstream logger::log_file;
std::string logger::open_filename;
mutex logger::file_mutex;
}
#endif // TORRENT_USE_IOSTREAM

#endif

#ifdef TORRENT_USE_GCRYPT

extern "C" {
GCRY_THREAD_OPTION_PTHREAD_IMPL;
}

namespace
{
	// libgcrypt requires this to initialize the library
	struct gcrypt_setup
	{
		gcrypt_setup()
		{
			gcry_check_version(0);
			gcry_error_t e = gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
			if (e != 0) fprintf(stderr, "libcrypt ERROR: %s\n", gcry_strerror(e));
			e = gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
			if (e != 0) fprintf(stderr, "initialization finished error: %s\n", gcry_strerror(e));
		}
	} gcrypt_global_constructor;
}

#endif // TORRENT_USE_GCRYPT

#ifdef TORRENT_USE_OPENSSL

#include <openssl/crypto.h>

namespace
{
	// openssl requires this to clean up internal
	// structures it allocates
	struct openssl_cleanup
	{
		~openssl_cleanup() { CRYPTO_cleanup_all_ex_data(); }
	} openssl_global_destructor;
}

#endif // TORRENT_USE_OPENSSL

#ifdef TORRENT_WINDOWS
// for ERROR_SEM_TIMEOUT
#include <winerror.h>
#endif

using boost::shared_ptr;
using boost::weak_ptr;
using libtorrent::aux::session_impl;

#ifdef BOOST_NO_EXCEPTIONS
namespace boost {
	void throw_exception(std::exception const& e) { ::abort(); }
}
#endif

namespace libtorrent {

#if defined TORRENT_ASIO_DEBUGGING
	std::map<std::string, async_t> _async_ops;
	int _async_ops_nthreads = 0;
	mutex _async_ops_mutex;
#endif

write_some_job::~write_some_job() {}

void network_thread_pool::process_job(write_some_job const& j, bool post)
{
	TORRENT_ASSERT(j.peer->m_socket_is_writing);
	j.peer->get_socket()->async_write_some(
		*j.vec, j.peer->make_write_handler(boost::bind(
			&peer_connection::on_send_data, j.peer, _1, _2)));
}

namespace detail
{
	std::string generate_auth_string(std::string const& user
		, std::string const& passwd)
	{
		if (user.empty()) return std::string();
		return user + ":" + passwd;
	}
}

namespace aux {

#ifdef TORRENT_STATS
	void get_vm_stats(vm_statistics_data_t* vm_stat, error_code& ec)
	{
		memset(vm_stat, 0, sizeof(*vm_stat));
#if defined __MACH__
		ec.clear();
		mach_port_t host_port = mach_host_self();
		mach_msg_type_number_t host_count = HOST_VM_INFO_COUNT;
		kern_return_t error = host_statistics(host_port, HOST_VM_INFO,
			(host_info_t)vm_stat, &host_count);
#elif defined TORRENT_LINUX
		ec.clear();
		char string[4096];
		boost::uint32_t value;
		FILE* f = fopen("/proc/vmstat", "r");
		int ret = 0;
		if (f == 0)
		{
			ec.assign(errno, boost::system::get_system_category());
			return;
		}
		while ((ret = fscanf(f, "%s %u\n", string, &value)) != EOF)
		{
			if (ret != 2) continue;
			if (strcmp(string, "nr_active_anon") == 0) vm_stat->active_count += value;
			else if (strcmp(string, "nr_active_file") == 0) vm_stat->active_count += value;
			else if (strcmp(string, "nr_inactive_anon") == 0) vm_stat->inactive_count += value;
			else if (strcmp(string, "nr_inactive_file") == 0) vm_stat->inactive_count += value;
			else if (strcmp(string, "nr_free_pages") == 0) vm_stat->free_count = value;
			else if (strcmp(string, "nr_unevictable") == 0) vm_stat->wire_count = value;
			else if (strcmp(string, "pswpin") == 0) vm_stat->pageins = value;
			else if (strcmp(string, "pswpout") == 0) vm_stat->pageouts = value;
			else if (strcmp(string, "pgfault") == 0) vm_stat->faults = value;
		}
		fclose(f);
#else
		ec = asio::error::operation_not_supported;
#endif
// TOOD: windows?
	}

	void get_thread_cpu_usage(thread_cpu_usage* tu)
	{
#if defined __MACH__
		task_thread_times_info t_info;
		mach_msg_type_number_t t_info_count = TASK_THREAD_TIMES_INFO_COUNT;
		task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t)&t_info, &t_info_count);

		tu->user_time = min_time()
			+ seconds(t_info.user_time.seconds)
			+ microsec(t_info.user_time.microseconds);
		tu->system_time = min_time()
			+ seconds(t_info.system_time.seconds)
			+ microsec(t_info.system_time.microseconds);
#elif defined TORRENT_LINUX
		struct rusage ru;
		getrusage(RUSAGE_THREAD, &ru);
		tu->user_time = min_time()
			+ seconds(ru.ru_utime.tv_sec)
			+ microsec(ru.ru_utime.tv_usec);
		tu->system_time = min_time()
			+ seconds(ru.ru_stime.tv_sec)
			+ microsec(ru.ru_stime.tv_usec);
#elif defined TORRENT_WINDOWS
		FILETIME system_time;
		FILETIME user_time;
		FILETIME creation_time;
		FILETIME exit_time;
		GetThreadTimes(GetCurrentThread(), &creation_time, &exit_time, &user_time, &system_time);

		boost::uint64_t utime = (boost::uint64_t(user_time.dwHighDateTime) << 32)
			+ user_time.dwLowDateTime;
		boost::uint64_t stime = (boost::uint64_t(system_time.dwHighDateTime) << 32)
			+ system_time.dwLowDateTime;

		tu->user_time = min_time() + microsec(utime / 10);
		tu->system_time = min_time() + microsec(stime / 10);
#endif
	}
		
#endif // TORRENT_STATS

	struct seed_random_generator
	{
		seed_random_generator()
		{
			random_seed((unsigned int)total_microseconds(time_now_hires() - min_time()));
		}
	};

#define TORRENT_SETTING(t, x) {#x, offsetof(proxy_settings,x), t},

	bencode_map_entry proxy_settings_map[] =
	{
		TORRENT_SETTING(std_string, hostname)
		TORRENT_SETTING(integer, port)
		TORRENT_SETTING(std_string, username)
		TORRENT_SETTING(std_string, password)
		TORRENT_SETTING(integer, type)
		TORRENT_SETTING(boolean, proxy_hostnames)
		TORRENT_SETTING(boolean, proxy_peer_connections)
	};
#undef TORRENT_SETTING

#ifndef TORRENT_DISABLE_DHT
#define TORRENT_SETTING(t, x) {#x, offsetof(dht_settings,x), t},
	bencode_map_entry dht_settings_map[] =
	{
		TORRENT_SETTING(integer, max_peers_reply)
		TORRENT_SETTING(integer, search_branching)
#ifndef TORRENT_NO_DEPRECATE
		TORRENT_SETTING(integer, service_port)
#endif
		TORRENT_SETTING(integer, max_fail_count)
		TORRENT_SETTING(integer, max_torrent_search_reply)
	};
#undef TORRENT_SETTING
#endif

#ifndef TORRENT_DISABLE_ENCRYPTION
#define TORRENT_SETTING(t, x) {#x, offsetof(pe_settings,x), t},
	bencode_map_entry pe_settings_map[] = 
	{
		TORRENT_SETTING(integer, out_enc_policy)
		TORRENT_SETTING(integer, in_enc_policy)
		TORRENT_SETTING(integer, allowed_enc_level)
		TORRENT_SETTING(boolean, prefer_rc4)
	};
#undef TORRENT_SETTING
#endif

	struct session_category
	{
		char const* name;
		bencode_map_entry const* map;
		int num_entries;
		int flag;
		int offset;
		int default_offset;
	};

	// the names in here need to match the names in session_impl
	// to make the macro simpler
	struct all_default_values
	{
		session_settings m_settings;
		proxy_settings m_proxy;
#ifndef TORRENT_DISABLE_ENCRYPTION
		pe_settings m_pe_settings;
#endif
#ifndef TORRENT_DISABLE_DHT
		dht_settings m_dht_settings;
#endif
	};

#define lenof(x) sizeof(x)/sizeof(x[0])
#define TORRENT_CATEGORY(name, flag, member, map) \
	{ name, map, lenof(map), session:: flag , offsetof(session_impl, member), offsetof(all_default_values, member) },

	session_category all_settings[] =
	{
//		TORRENT_CATEGORY("settings", save_settings, m_settings, session_settings_map)
#ifndef TORRENT_DISABLE_DHT
		TORRENT_CATEGORY("dht", save_dht_settings, m_dht_settings, dht_settings_map)
#endif
		TORRENT_CATEGORY("proxy", save_proxy, m_proxy, proxy_settings_map)
#if TORRENT_USE_I2P
//		TORRENT_CATEGORY("i2p", save_i2p_proxy, m_i2p_proxy, proxy_settings_map)
#endif
#ifndef TORRENT_DISABLE_ENCRYPTION
		TORRENT_CATEGORY("encryption", save_encryption_settings, m_pe_settings, pe_settings_map)
#endif
	};
/*
	std::pair<bencode_map_entry*, int> settings_map()
	{
		return std::make_pair(session_settings_map, lenof(session_settings_map));
	}
*/
#undef lenof

#ifdef TORRENT_STATS
	int session_impl::logging_allocator::allocations = 0;
	int session_impl::logging_allocator::allocated_bytes = 0;
#endif

	void session_impl::init_peer_class_filter(bool unlimited_local)
	{
		// set the default peer_class_filter to use the local peer class
		// for peers on local networks
		boost::uint32_t lfilter = 1 << m_local_peer_class;
		boost::uint32_t gfilter = 1 << m_global_class;

		struct class_mapping
		{
			char const* first;
			char const* last;
			boost::uint32_t filter;
		};

		const static class_mapping v4_classes[] =
		{
			// everything
			{"0.0.0.0", "255.255.255.255", gfilter},
			// local networks
			{"10.0.0.0", "10.255.255.255", lfilter},
			{"172.16.0.0", "172.16.255.255", lfilter},
			{"192.168.0.0", "192.168.255.255", lfilter},
			// link-local
			{"169.254.0.0", "169.254.255.255", lfilter},
			// loop-back
			{"127.0.0.0", "127.255.255.255", lfilter},
		};

		const static class_mapping v6_classes[] =
		{
			// everything
			{"::0", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", gfilter},
			// link-local
			{"fe80::", "febf::ffff:ffff:ffff:ffff:ffff:ffff:ffff", lfilter},
			// loop-back
			{"::1", "::1", lfilter},
		};

		class_mapping const* p = v4_classes;
		int len = sizeof(v4_classes) / sizeof(v4_classes[0]);
		if (!unlimited_local) len = 1;
		for (int i = 0; i < len; ++i)
		{
			error_code ec;
			address_v4 begin = address_v4::from_string(p[i].first, ec);
			address_v4 end = address_v4::from_string(p[i].last, ec);
			if (ec) continue;
			m_peer_class_filter.add_rule(begin, end, p[i].filter);
		}
		p = v6_classes;
		len = sizeof(v6_classes) / sizeof(v6_classes[0]);
		if (!unlimited_local) len = 1;
		for (int i = 0; i < len; ++i)
		{
			error_code ec;
			address_v6 begin = address_v6::from_string(p[i].first, ec);
			address_v6 end = address_v6::from_string(p[i].last, ec);
			if (ec) continue;
			m_peer_class_filter.add_rule(begin, end, p[i].filter);
		}
	}

#if defined TORRENT_USE_OPENSSL && BOOST_VERSION >= 104700 && OPENSSL_VERSION_NUMBER >= 0x90812f
	// when running bittorrent over SSL, the SNI (server name indication)
	// extension is used to know which torrent the incoming connection is
	// trying to connect to. The 40 first bytes in the name is expected to
	// be the hex encoded info-hash
	int servername_callback(SSL *s, int *ad, void *arg)
	{
		session_impl* ses = (session_impl*)arg;
		const char* servername = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
	
		if (!servername || strlen(servername) < 40)
			return SSL_TLSEXT_ERR_ALERT_FATAL;

		sha1_hash info_hash;
		bool valid = from_hex(servername, 40, (char*)&info_hash[0]);

		// the server name is not a valid hex-encoded info-hash
		if (!valid)
			return SSL_TLSEXT_ERR_ALERT_FATAL;

		// see if there is a torrent with this info-hash
		boost::shared_ptr<torrent> t = ses->find_torrent(info_hash).lock();

		// if there isn't, fail
		if (!t) return SSL_TLSEXT_ERR_ALERT_FATAL;

		// if the torrent we found isn't an SSL torrent, also fail.
		// the torrent doesn't have an SSL context and should not allow
		// incoming SSL connections
		if (!t->is_ssl_torrent()) return SSL_TLSEXT_ERR_ALERT_FATAL;

		// use this torrent's certificate
		SSL_set_SSL_CTX(s, t->ssl_ctx()->native_handle());

		return SSL_TLSEXT_ERR_OK;
	}
#endif

	session_impl::session_impl(
		std::pair<int, int> listen_port_range
		, fingerprint const& cl_fprint
		, char const* listen_interface
		, boost::uint32_t alert_mask
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		, std::string const& logpath
#endif
		)
		: m_ipv4_peer_pool(500)
#if TORRENT_USE_IPV6
		, m_ipv6_peer_pool(500)
#endif
#ifndef TORRENT_DISABLE_POOL_ALLOCATOR
		, m_send_buffers(send_buffer_size())
#endif
		, m_io_service()
#ifdef TORRENT_USE_OPENSSL
		, m_ssl_ctx(m_io_service, asio::ssl::context::sslv23)
#endif
		, m_alerts(m_io_service, m_settings.get_int(settings_pack::alert_queue_size), alert_mask)
		, m_disk_thread(m_io_service, this, this)
		, m_half_open(m_io_service)
		, m_download_rate(peer_connection::download_channel)
#ifdef TORRENT_VERBOSE_BANDWIDTH_LIMIT
		, m_upload_rate(peer_connection::upload_channel, true)
#else
		, m_upload_rate(peer_connection::upload_channel)
#endif
		, m_tracker_manager(*this, m_proxy)
		, m_work(io_service::work(m_io_service))
		, m_max_queue_pos(-1)
		, m_key(0)
		, m_listen_port_retries(listen_port_range.second - listen_port_range.first)
#if TORRENT_USE_I2P
		, m_i2p_conn(m_io_service)
#endif
		, m_abort(false)
		, m_paused(false)
		, m_allowed_upload_slots(8)
		, m_num_unchoked(0)
		, m_unchoke_time_scaler(0)
		, m_auto_manage_time_scaler(0)
		, m_optimistic_unchoke_time_scaler(0)
		, m_disconnect_time_scaler(90)
		, m_auto_scrape_time_scaler(180)
		, m_next_explicit_cache_torrent(0)
		, m_cache_rotation_timer(0)
		, m_next_suggest_torrent(0)
		, m_suggest_timer(0)
		, m_peak_up_rate(0)
		, m_peak_down_rate(0)
		, m_incoming_connection(false)
		, m_created(time_now_hires())
		, m_last_tick(m_created)
		, m_last_second_tick(m_created - milliseconds(900))
		, m_last_disk_performance_warning(min_time())
		, m_last_disk_queue_performance_warning(min_time())
		, m_last_choke(m_created)
		, m_next_rss_update(min_time())
#ifndef TORRENT_DISABLE_DHT
		, m_dht_announce_timer(m_io_service)
#endif
		, m_external_udp_port(0)
		, m_udp_socket(m_io_service, m_half_open)
		, m_utp_socket_manager(m_settings, m_udp_socket
			, boost::bind(&session_impl::incoming_connection, this, _1))
		, m_boost_connections(0)
		, m_timer(m_io_service)
		, m_lsd_announce_timer(m_io_service)
		, m_host_resolver(m_io_service)
		, m_download_connect_attempts(0)
		, m_tick_residual(0)
		, m_non_filtered_torrents(0)
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		, m_logpath(logpath)
#endif
#ifndef TORRENT_DISABLE_GEO_IP
		, m_asnum_db(0)
		, m_country_db(0)
#endif
		, m_total_failed_bytes(0)
		, m_total_redundant_bytes(0)
		, m_deferred_submit_disk_jobs(false)
		, m_writing_bytes(0)
#if (defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS) && defined BOOST_HAS_PTHREADS
		, m_network_thread(0)
#endif
	{
		memset(m_redundant_bytes, 0, sizeof(m_redundant_bytes));
		m_udp_socket.set_rate_limit(m_settings.get_int(settings_pack::dht_upload_rate_limit));

		m_udp_socket.subscribe(&m_tracker_manager);
		m_udp_socket.subscribe(&m_utp_socket_manager);
		m_udp_socket.subscribe(this);

		m_disk_queues[0] = 0;
		m_disk_queues[1] = 0;

#ifdef TORRENT_REQUEST_LOGGING
		char log_filename[200];
		snprintf(log_filename, sizeof(log_filename), "requests-%d.log", getpid());
		m_request_log = fopen(log_filename, "w+");
		if (m_request_log == 0)
		{
			fprintf(stderr, "failed to open request log file: (%d) %s\n", errno, strerror(errno));
		}
#endif

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_logger = create_log("main_session", listen_port(), false);
		(*m_logger) << time_now_string() << "\n";
#endif

#if defined TORRENT_REQUEST_LOGGING
		m_request_logger = fopen("requests.log", "w+");
#endif

		error_code ec;
#ifdef TORRENT_USE_OPENSSL
		m_ssl_ctx.set_verify_mode(asio::ssl::context::verify_none, ec);
#if BOOST_VERSION >= 104700
#if OPENSSL_VERSION_NUMBER >= 0x90812f
		SSL_CTX_set_tlsext_servername_callback(m_ssl_ctx.native_handle(), servername_callback);
		SSL_CTX_set_tlsext_servername_arg(m_ssl_ctx.native_handle(), this);
#endif // OPENSSL_VERSION_NUMBER
#endif // BOOST_VERSION
#endif

#ifndef TORRENT_DISABLE_DHT
		m_next_dht_torrent = m_torrents.begin();
#endif
		m_next_lsd_torrent = m_torrents.begin();
		m_next_downloading_connect_torrent = 0;
		m_next_finished_connect_torrent = 0;
		m_next_scrape_torrent = 0;
		m_next_disk_peer = m_connections.begin();

		if (!listen_interface) listen_interface = "0.0.0.0";
		m_listen_interface = tcp::endpoint(address::from_string(listen_interface, ec), listen_port_range.first);
		TORRENT_ASSERT_VAL(!ec, ec);

		m_tcp_mapping[0] = -1;
		m_tcp_mapping[1] = -1;
		m_udp_mapping[0] = -1;
		m_udp_mapping[1] = -1;
#ifdef TORRENT_USE_OPENSSL
		m_ssl_mapping[0] = -1;
		m_ssl_mapping[1] = -1;
#endif
#ifdef WIN32
		// windows XP has a limit on the number of
		// simultaneous half-open TCP connections
		// here's a table:

		// windows version       half-open connections limit
		// --------------------- ---------------------------
		// XP sp1 and earlier    infinite
		// earlier than vista    8
		// vista sp1 and earlier 5
		// vista sp2 and later   infinite

		// windows release                     version number
		// ----------------------------------- --------------
		// Windows 7                           6.1
		// Windows Server 2008 R2              6.1
		// Windows Server 2008                 6.0
		// Windows Vista                       6.0
		// Windows Server 2003 R2              5.2
		// Windows Home Server                 5.2
		// Windows Server 2003                 5.2
		// Windows XP Professional x64 Edition 5.2
		// Windows XP                          5.1
		// Windows 2000                        5.0

 		OSVERSIONINFOEX osv;
		memset(&osv, 0, sizeof(osv));
		osv.dwOSVersionInfoSize = sizeof(osv);
		GetVersionEx((OSVERSIONINFO*)&osv);

		// the low two bytes of windows_version is the actual
		// version.
		boost::uint32_t windows_version
			= ((osv.dwMajorVersion & 0xff) << 16)
			| ((osv.dwMinorVersion & 0xff) << 8)
			| (osv.wServicePackMajor & 0xff);

		// this is the format of windows_version
		// xx xx xx
		// |  |  |
		// |  |  + service pack version
		// |  + minor version
		// + major version

		// the least significant byte is the major version
		// and the most significant one is the minor version
		if (windows_version >= 0x060100)
		{
			// windows 7 and up doesn't have a half-open limit
			m_half_open.limit(0);
		}
		else if (windows_version >= 0x060002)
		{
			// on vista SP 2 and up, there's no limit
			m_half_open.limit(0);
		}
		else if (windows_version >= 0x060000)
		{
			// on vista the limit is 5 (in home edition)
			m_half_open.limit(4);
		}
		else if (windows_version >= 0x050102)
		{
			// on XP SP2 the limit is 10	
			m_half_open.limit(9);
		}
		else
		{
			// before XP SP2, there was no limit
			m_half_open.limit(0);
		}
		m_settings.set_int(settings_pack::half_open_limit, m_half_open.limit());
#endif

		m_global_class = m_classes.new_peer_class("global");
		m_tcp_peer_class = m_classes.new_peer_class("tcp");
		m_local_peer_class = m_classes.new_peer_class("local");
		m_classes.at(m_local_peer_class)->ignore_unchoke_slots = true;

		TORRENT_ASSERT(m_global_class == session::global_peer_class_id);
		TORRENT_ASSERT(m_tcp_peer_class == session::tcp_peer_class_id);
		TORRENT_ASSERT(m_local_peer_class == session::local_peer_class_id);

		init_peer_class_filter(true);

		// TCP, SSL/TCP and I2P connections should be assigned the TCP peer class
		m_peer_class_type_filter.add(peer_class_type_filter::tcp_socket, m_tcp_peer_class);
		m_peer_class_type_filter.add(peer_class_type_filter::ssl_tcp_socket, m_tcp_peer_class);
		m_peer_class_type_filter.add(peer_class_type_filter::i2p_socket, m_tcp_peer_class);

		// TODO: there's no rule here to make uTP connections not have the global or
		// local rate limits apply to it. This used to be the default.

#ifdef TORRENT_UPNP_LOGGING
		m_upnp_log.open("upnp.log", std::ios::in | std::ios::out | std::ios::trunc);
#endif

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING

		char tmp[300];
		snprintf(tmp, sizeof(tmp), "libtorrent configuration: %s\n"
			"libtorrent version: %s\n"
			"libtorrent revision: %s\n\n"
		  	, TORRENT_CFG_STRING
			, LIBTORRENT_VERSION
			, LIBTORRENT_REVISION);
		(*m_logger) << tmp;

		logger& l = *m_logger;

		int temp = 0;
		int prev_size = 0;

		PRINT_SIZEOF(piece_picker::piece_pos)

		PRINT_SIZEOF(cached_piece_entry)
		PRINT_OFFSETOF(cached_piece_entry, prev)
		PRINT_OFFSETOF(cached_piece_entry, next)
		PRINT_OFFSETOF(cached_piece_entry, storage)
		PRINT_OFFSETOF(cached_piece_entry, hash)
		PRINT_OFFSETOF(cached_piece_entry, blocks)
		PRINT_OFFSETOF(cached_piece_entry, jobs)
		PRINT_OFFSETOF(cached_piece_entry, expire)
		PRINT_OFFSETOF(cached_piece_entry, refcount)
		PRINT_OFFSETOF_END(cached_piece_entry)

		PRINT_SIZEOF(announce_entry)
		PRINT_OFFSETOF(announce_entry, url)
		PRINT_OFFSETOF(announce_entry, trackerid)
		PRINT_OFFSETOF(announce_entry, message)
		PRINT_OFFSETOF(announce_entry, last_error)
		PRINT_OFFSETOF(announce_entry, next_announce)
		PRINT_OFFSETOF(announce_entry, min_announce)
		PRINT_OFFSETOF(announce_entry, tier)
		PRINT_OFFSETOF(announce_entry, fail_limit)
		PRINT_OFFSETOF_END(announce_entry)

		PRINT_SIZEOF(torrent_info)
		PRINT_OFFSETOF(torrent_info, m_refs)
		PRINT_OFFSETOF(torrent_info, m_merkle_first_leaf)
		PRINT_OFFSETOF(torrent_info, m_files)
		PRINT_OFFSETOF(torrent_info, m_orig_files)
		PRINT_OFFSETOF(torrent_info, m_urls)
		PRINT_OFFSETOF(torrent_info, m_web_seeds)
		PRINT_OFFSETOF(torrent_info, m_nodes)
		PRINT_OFFSETOF(torrent_info, m_merkle_tree)
		PRINT_OFFSETOF(torrent_info, m_info_section)
		PRINT_OFFSETOF(torrent_info, m_piece_hashes)
		PRINT_OFFSETOF(torrent_info, m_comment)
		PRINT_OFFSETOF(torrent_info, m_created_by)
#ifdef TORRENT_USE_OPENSSL
		PRINT_OFFSETOF(torrent_info, m_ssl_root_cert)
#endif
		PRINT_OFFSETOF(torrent_info, m_info_dict)
		PRINT_OFFSETOF(torrent_info, m_creation_date)
		PRINT_OFFSETOF(torrent_info, m_info_hash)
		PRINT_OFFSETOF_END(torrent_info)

		PRINT_SIZEOF(union_endpoint)
		PRINT_SIZEOF(request_callback)
		PRINT_SIZEOF(stat)
		PRINT_SIZEOF(bandwidth_channel)
		PRINT_SIZEOF(policy)
		(*m_logger) << "sizeof(utp_socket_impl): " << socket_impl_size() << "\n";

		PRINT_SIZEOF(file_entry)
		PRINT_SIZEOF(internal_file_entry)
		PRINT_OFFSETOF(internal_file_entry, name)
		PRINT_OFFSETOF(internal_file_entry, path_index)
		PRINT_OFFSETOF_END(internal_file_entry)

		PRINT_SIZEOF(disk_io_job)
		PRINT_OFFSETOF(disk_io_job, next)
		PRINT_OFFSETOF(disk_io_job, buffer)
		PRINT_OFFSETOF(disk_io_job, storage)
		PRINT_OFFSETOF(disk_io_job, callback)
		PRINT_OFFSETOF(disk_io_job, error.ec)
//		PRINT_OFFSETOF(disk_io_job, error.file)
//		PRINT_OFFSETOF(disk_io_job, error.operation)
		PRINT_OFFSETOF(disk_io_job, d.io.ref.storage)
		PRINT_OFFSETOF(disk_io_job, d.io.ref.piece)
		PRINT_OFFSETOF(disk_io_job, d.io.ref.block)
		PRINT_OFFSETOF(disk_io_job, d.io.offset)
		PRINT_OFFSETOF(disk_io_job, d.io.buffer_size)
//		PRINT_OFFSETOF(disk_io_job, piece)
//		PRINT_OFFSETOF(disk_io_job, action)
		PRINT_OFFSETOF(disk_io_job, flags)
		PRINT_OFFSETOF_END(disk_io_job)

		PRINT_SIZEOF(file_storage)
		PRINT_OFFSETOF(file_storage, m_files)
		PRINT_OFFSETOF(file_storage, m_file_hashes)
		PRINT_OFFSETOF(file_storage, m_symlinks)
		PRINT_OFFSETOF(file_storage, m_mtime)
		PRINT_OFFSETOF(file_storage, m_file_base)
		PRINT_OFFSETOF(file_storage, m_paths)
		PRINT_OFFSETOF(file_storage, m_name)
		PRINT_OFFSETOF(file_storage, m_total_size)
		PRINT_OFFSETOF(file_storage, m_num_pieces)
		PRINT_OFFSETOF(file_storage, m_piece_length)
		PRINT_OFFSETOF_END(file_storage)

//		PRINT_SIZEOF(stat_channel)
//		PRINT_OFFSETOF(stat_channel, m_counter)
//		PRINT_OFFSETOF(stat_channel, m_average)
//		PRINT_OFFSETOF(stat_channel, m_total_counter)

		torrent::print_size(*m_logger);

		PRINT_SIZEOF(peer_connection)
		PRINT_SIZEOF(bt_peer_connection)
		PRINT_SIZEOF(address)
		PRINT_SIZEOF(address_v4)
		PRINT_SIZEOF(address_v4::bytes_type)
#if TORRENT_USE_IPV6
		PRINT_SIZEOF(address_v6)
		PRINT_SIZEOF(address_v6::bytes_type)
#endif
		PRINT_SIZEOF(void*)
#ifndef TORRENT_DISABLE_DHT
		PRINT_SIZEOF(dht::node_entry)
#endif

		PRINT_SIZEOF(policy::peer)
		PRINT_OFFSETOF(policy::peer, prev_amount_upload)
		PRINT_OFFSETOF(policy::peer, prev_amount_download)
		PRINT_OFFSETOF(policy::peer, connection)
#ifndef TORRENT_DISABLE_GEO_IP
#ifdef TORRENT_DEBUG
		PRINT_OFFSETOF(policy::peer, inet_as_num)
#endif
		PRINT_OFFSETOF(policy::peer, inet_as)
#endif
		PRINT_OFFSETOF(policy::peer, last_optimistically_unchoked)
		PRINT_OFFSETOF(policy::peer, last_connected)
		PRINT_OFFSETOF(policy::peer, port)
		PRINT_OFFSETOF(policy::peer, hashfails)
		PRINT_OFFSETOF_END(policy::peer)

		PRINT_SIZEOF(policy::ipv4_peer)
#if TORRENT_USE_IPV6
		PRINT_SIZEOF(policy::ipv6_peer)
#endif

		PRINT_SIZEOF(udp_socket)
		PRINT_OFFSETOF(udp_socket, m_ipv4_sock)
		PRINT_OFFSETOF(udp_socket, m_buf)
#if TORRENT_USE_IPV6
		PRINT_OFFSETOF(udp_socket, m_ipv6_sock)
#endif
		PRINT_OFFSETOF(udp_socket, m_bind_port)
		PRINT_OFFSETOF(udp_socket, m_v4_outstanding)
#if TORRENT_USE_IPV6
		PRINT_OFFSETOF(udp_socket, m_v6_outstanding)
#endif
		PRINT_OFFSETOF(udp_socket, m_socks5_sock)
		PRINT_OFFSETOF(udp_socket, m_connection_ticket)
		PRINT_OFFSETOF(udp_socket, m_proxy_settings)
#ifndef _MSC_VER
		PRINT_OFFSETOF(udp_socket, m_cc)
#endif
		PRINT_OFFSETOF(udp_socket, m_resolver)
		PRINT_OFFSETOF(udp_socket, m_tmp_buf)
		PRINT_OFFSETOF(udp_socket, m_queue_packets)
		PRINT_OFFSETOF(udp_socket, m_tunnel_packets)
		PRINT_OFFSETOF(udp_socket, m_abort)
		PRINT_OFFSETOF(udp_socket, m_proxy_addr)
		PRINT_OFFSETOF(udp_socket, m_queue)
		PRINT_OFFSETOF(udp_socket, m_outstanding_ops)
#ifdef TORRENT_DEBUG
		PRINT_OFFSETOF(udp_socket, m_started)
		PRINT_OFFSETOF(udp_socket, m_magic)
		PRINT_OFFSETOF(udp_socket, m_outstanding_when_aborted)
#endif
		PRINT_OFFSETOF_END(udp_socket)

		PRINT_SIZEOF(tracker_connection)
		PRINT_SIZEOF(http_tracker_connection)

		PRINT_SIZEOF(udp_tracker_connection)
		PRINT_OFFSETOF(udp_tracker_connection, m_refs)

		PRINT_OFFSETOF(udp_tracker_connection, m_start_time)
		PRINT_OFFSETOF(udp_tracker_connection, m_read_time)
		PRINT_OFFSETOF(udp_tracker_connection, m_timeout)
		PRINT_OFFSETOF(udp_tracker_connection, m_completion_timeout)
		PRINT_OFFSETOF(udp_tracker_connection, m_read_timeout)
		PRINT_OFFSETOF(udp_tracker_connection, m_mutex)
		PRINT_OFFSETOF(udp_tracker_connection, m_requester)
#ifndef _MSC_VER
		PRINT_OFFSETOF(udp_tracker_connection, m_man)
#endif

		PRINT_OFFSETOF(udp_tracker_connection, m_req)

		PRINT_OFFSETOF(udp_tracker_connection, m_abort)
		PRINT_OFFSETOF(udp_tracker_connection, m_hostname)
		PRINT_OFFSETOF(udp_tracker_connection, m_target)
		PRINT_OFFSETOF(udp_tracker_connection, m_endpoints)
		PRINT_OFFSETOF(udp_tracker_connection, m_transaction_id)
#ifndef _MSC_VER
		PRINT_OFFSETOF(udp_tracker_connection, m_ses)
#endif
		PRINT_OFFSETOF(udp_tracker_connection, m_attempts)
		PRINT_OFFSETOF(udp_tracker_connection, m_state)
		PRINT_OFFSETOF(udp_tracker_connection, m_proxy)
		PRINT_OFFSETOF_END(udp_tracker_connection)

#ifndef TORRENT_DISABLE_DHT
		PRINT_SIZEOF(dht::find_data_observer)
		PRINT_SIZEOF(dht::announce_observer)
		PRINT_SIZEOF(dht::null_observer)
#endif
#undef PRINT_OFFSETOF_END
#undef PRINT_OFFSETOF
#undef PRINT_SIZEOF

#endif

#ifdef TORRENT_STATS

		m_stats_logger = 0;
		m_log_seq = 0;
		m_stats_logging_enabled = true;

		memset(&m_last_cache_status, 0, sizeof(m_last_cache_status));
		vm_statistics_data_t vst;
		get_vm_stats(&vst, ec);
		if (!ec) m_last_vm_stat = vst;

		m_last_failed = 0;
		m_last_redundant = 0;
		m_last_uploaded = 0;
		m_last_downloaded = 0;
		get_thread_cpu_usage(&m_network_thread_cpu_usage);

		reset_stat_counters();
		rotate_stats_log();
#endif
#ifdef TORRENT_BUFFER_STATS
		m_buffer_usage_logger.open("buffer_stats.log", std::ios::trunc);
		m_buffer_allocations = 0;
#endif

#if TORRENT_USE_RLIMIT
		// ---- auto-cap max connections ----

		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			(*m_logger) << time_now_string() << " max number of open files: " << rl.rlim_cur << "\n";
#endif
			// deduct some margin for epoll/kqueue, log files,
			// futexes, shared objects etc.
			rl.rlim_cur -= 20;

			// 80% of the available file descriptors should go to connections
			m_settings.set_int(settings_pack::connections_limit, (std::min)(
				m_settings.get_int(settings_pack::connections_limit)
				, int(rl.rlim_cur * 8 / 10)));
			// 20% goes towards regular files (see disk_io_thread)
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			(*m_logger) << time_now_string() << "   max connections: " << m_settings.get_int(settings_pack::connections_limit) << "\n";
			(*m_logger) << time_now_string() << "   max files: " << int(rl.rlim_cur * 2 / 10) << "\n";
#endif
		}
#endif // TORRENT_USE_RLIMIT


		// ---- generate a peer id ----
		static seed_random_generator seeder;

		m_key = random() + (random() << 15) + (random() << 30);
		std::string print = cl_fprint.to_string();
		TORRENT_ASSERT_VAL(print.length() <= 20, print.length());

		// the client's fingerprint
		std::copy(
			print.begin()
			, print.begin() + print.length()
			, m_peer_id.begin());

		url_random((char*)&m_peer_id[print.length()], (char*)&m_peer_id[0] + 20);

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		(*m_logger) << time_now_string() << " generated peer ID: " << m_peer_id.to_string() << "\n";
#endif

		update_half_open();
#ifndef TORRENT_NO_DEPRECATE
		update_local_download_rate();
		update_local_upload_rate();
#endif
		update_download_rate();
		update_upload_rate();
		update_connections_limit();
		update_choking_algorithm();
		update_disk_threads();
		update_network_threads();
	}

#ifdef TORRENT_STATS
	void session_impl::rotate_stats_log()
	{
		if (m_stats_logger)
		{
			++m_log_seq;
			fclose(m_stats_logger);
		}

		// make these cumulative for easier reading of graphs
		// reset them every time the log is rotated though,
		// to make them cumulative per one-hour graph
		memset(m_stats_counter, 0, sizeof(m_stats_counter));

		error_code ec;
		char filename[100];
		create_directory("session_stats", ec);
		snprintf(filename, sizeof(filename), "session_stats/%d.%04d.log", int(getpid()), m_log_seq);
		m_stats_logger = fopen(filename, "w+");
		m_last_log_rotation = time_now();
		if (m_stats_logger == 0)
		{
			fprintf(stderr, "Failed to create session stats log file \"%s\": (%d) %s\n"
				, filename, errno, strerror(errno));
			return;
		}
			
		fputs("second"
			":uploaded bytes"
			":downloaded bytes"
			":downloading torrents"
			":seeding torrents"
			":peers"
			":connecting peers"
			":disk block buffers"
			":num list peers"
			":peer allocations"
			":peer storage bytes"
			":checking torrents"
			":stopped torrents"
			":upload-only torrents"
			":queued seed torrents"
			":queued download torrents"
			":peers bw-up"
			":peers bw-down"
			":peers disk-up"
			":peers disk-down"
			":upload rate"
			":download rate"
			":disk write queued bytes"
			":peers down 0"
			":peers down 0-2"
			":peers down 2-5"
			":peers down 5-10"
			":peers down 10-50"
			":peers down 50-100"
			":peers down 100-"
			":peers up 0"
			":peers up 0-2"
			":peers up 2-5"
			":peers up 5-10"
			":peers up 10-50"
			":peers up 50-100"
			":peers up 100-"
			":error peers"
			":peers down interesting"
			":peers down unchoked"
			":peers down requests"
			":peers up interested"
			":peers up unchoked"
			":peers up requests"
			":peer disconnects"
			":peers eof"
			":peers connection reset"
			":outstanding requests"
			":outstanding end-game requests"
			":outstanding writing blocks"
			":end game piece picker blocks"
			":piece picker blocks"
			":piece picker loops"
			":piece picks"
			":reject piece picks"
			":unchoke piece picks"
			":incoming redundant piece picks"
			":incoming piece picks"
			":end game piece picks"
			":snubbed piece picks"
			":connect timeouts"
			":uninteresting peers disconnect"
			":timeout peers"
			":% failed payload bytes"
			":% wasted payload bytes"
			":% protocol bytes"
			":disk read time"
			":disk write time"
			":disk queue size"
			":queued disk bytes"
			":read cache hits"
			":disk block read"
			":disk block written"
			":failed bytes"
			":redundant bytes"
			":error torrents"
			":read disk cache size"
			":disk cache size"
			":disk buffer allocations"
			":disk hash time"
			":connection attempts"
			":banned peers"
			":banned for hash failure"
			":cache size"
			":max connections"
			":connect candidates"
			":cache trim low watermark"
			":% read time"
			":% write time"
			":% hash time"
			":disk read back"
			":% read back"
			":disk read queue size"
			":tick interval"
			":tick residual"
			":max unchoked"
			":smooth upload rate"
			":smooth download rate"
			":num end-game peers"
			":TCP up rate"
			":TCP down rate"
			":TCP up limit"
			":TCP down limit"
			":uTP up rate"
			":uTP down rate"
			":uTP peak send delay"
			":uTP avg send delay"
			":uTP peak recv delay"
			":uTP avg recv delay"
			":read ops/s"
			":write ops/s"
			":active resident pages"
			":inactive resident pages"
			":pinned resident pages"
			":free pages"
			":pageins"
			":pageouts"
			":page faults"
			":smooth read ops/s"
			":smooth write ops/s"
			":pinned blocks"
			":num partial pieces"
			":num downloading partial pieces"
			":num full partial pieces"
			":num finished partial pieces"
			":allocated jobs"
			":allocated read jobs"
			":allocated write jobs"
			":pending reading bytes"
			":read_counter"
			":write_counter"
			":tick_counter"
			":lsd_counter"
			":lsd_peer_counter"
			":udp_counter"
			":accept_counter"
			":disk_queue_counter"
			":disk_counter"
			":up 8:up 16:up 32:up 64:up 128:up 256:up 512:up 1024:up 2048:up 4096:up 8192:up 16384:up 32768:up 65536:up 131072:up 262144:up 524288:up 1048576"
			":down 8:down 16:down 32:down 64:down 128:down 256:down 512:down 1024:down 2048:down 4096:down 8192:down 16384:down 32768:down 65536:down 131072:down 262144:down 524288:down 1048576"
			":network thread system time"
			":network thread user+system time"

			":redundant timed-out"
			":redundant cancelled"
			":redundant unknown"
			":redundant seed"
			":redundant end-game"
			":redundant closing"
			":no memory peer errors"
			":too many peers"
			":transport timeout peers"
			
			":arc LRU write pieces"
			":arc LRU volatile pieces"
			":arc LRU pieces"
			":arc LRU ghost pieces"
			":arc LFU pieces"
			":arc LFU ghost pieces"

			":uTP idle"
			":uTP syn-sent"
			":uTP connected"
			":uTP fin-sent"
			":uTP close-wait"

			":tcp peers"
			":utp peers"

			":connection refused peers"
			":connection aborted peers"
			":permission denied peers"
			":no buffer peers"
			":host unreachable peers"
			":broken pipe peers"
			":address in use peers"
			":access denied peers"
			":invalid argument peers"
			":operation aborted peers"

			":error incoming peers"
			":error outgoing peers"
			":error rc4 peers"
			":error encrypted peers"
			":error tcp peers"
			":error utp peers"

			":total peers"
			":pending incoming block requests"
			":average pending incoming block requests"

			":torrents want more peers"
			":average peers per limit"

			":piece requests"
			":max piece requests"
			":invalid piece requests"
			":choked piece requests"
			":cancelled piece requests"
			":piece rejects"

			":total pieces"
			":pieces flushed"
			":pieces passed"

			":peers up send buffer"

			":loaded torrents"
			":loaded torrent churn"

			"\n\n", m_stats_logger);
	}
#endif

	void session_impl::queue_async_resume_data(boost::shared_ptr<torrent> const& t)
	{
		int loaded_limit = m_settings.get_int(settings_pack::active_loaded_limit);
		if (m_num_save_resume + m_num_queued_resume >= loaded_limit)
		{
			TORRENT_ASSERT(t);
			// do loaded torrents first, otherwise they'll just be
			// evicted and have to be loaded again
			if (t->is_loaded())
				m_save_resume_queue.push_front(t);
			else
				m_save_resume_queue.push_back(t);
			return;
		}

		++m_num_save_resume;
		t->do_async_save_resume_data();
	}

	// this is called whenever a save_resume_data comes back
	// from the disk thread
	void session_impl::done_async_resume()
	{
		TORRENT_ASSERT(m_num_save_resume > 0);
		--m_num_save_resume;
		++m_num_queued_resume;
	}

	// this is called when one or all save resume alerts are
	// popped off the alert queue
	void session_impl::async_resume_dispatched(bool all)
	{
		if (all)
		{
			if (m_num_queued_resume == 0) return;
			m_num_queued_resume = 0;
		}
		else
		{
			TORRENT_ASSERT(m_num_queued_resume > 0);
			--m_num_queued_resume;
		}

		int loaded_limit = m_settings.get_int(settings_pack::active_loaded_limit);
		while (!m_save_resume_queue.empty()
			&& m_num_save_resume + m_num_queued_resume < loaded_limit)
		{
			boost::shared_ptr<torrent> t = m_save_resume_queue.front();
			m_save_resume_queue.erase(m_save_resume_queue.begin());
			++m_num_save_resume;
			t->do_async_save_resume_data();
		}
	}

	void session_impl::start_session()
	{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		(*m_logger) << time_now_string() << " spawning network thread\n";
#endif
		m_thread.reset(new thread(boost::bind(&session_impl::main_thread, this)));
	}

	void session_impl::init()
	{
#if defined TORRENT_LOGGING || defined TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << " *** session thread init\n";
#endif

		// this is where we should set up all async operations. This
		// is called from within the network thread as opposed to the
		// constructor which is called from the main thread

#if defined TORRENT_ASIO_DEBUGGING
		async_inc_threads();
		add_outstanding_async("session_impl::on_tick");
#endif
		error_code ec;
		m_io_service.post(boost::bind(&session_impl::on_tick, this, ec));

#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_lsd_announce");
#endif
		int delay = (std::max)(m_settings.get_int(settings_pack::local_service_announce_interval)
			/ (std::max)(int(m_torrents.size()), 1), 1);
		m_lsd_announce_timer.expires_from_now(seconds(delay), ec);
		m_lsd_announce_timer.async_wait(
			boost::bind(&session_impl::on_lsd_announce, this, _1));
		TORRENT_ASSERT(!ec);

#ifndef TORRENT_DISABLE_DHT

#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_dht_announce");
#endif
		delay = (std::max)(m_settings.get_int(settings_pack::dht_announce_interval)
			/ (std::max)(int(m_torrents.size()), 1), 1);
		m_dht_announce_timer.expires_from_now(seconds(delay), ec);
		m_dht_announce_timer.async_wait(
			boost::bind(&session_impl::on_dht_announce, this, _1));
		TORRENT_ASSERT(!ec);
#endif

#if defined TORRENT_LOGGING || defined TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << " open listen port\n";
#endif
		// no reuse_address and allow system defined port
		open_listen_port(0, ec);
#if defined TORRENT_LOGGING || defined TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << " done starting session\n";
#endif
	}

	void session_impl::save_state(entry* eptr, boost::uint32_t flags) const
	{
		TORRENT_ASSERT(is_single_thread());

		entry& e = *eptr;

		all_default_values def;

		for (int i = 0; i < int(sizeof(all_settings)/sizeof(all_settings[0])); ++i)
		{
			session_category const& c = all_settings[i];
			if ((flags & c.flag) == 0) continue;
			save_struct(e[c.name], reinterpret_cast<char const*>(this) + c.offset
				, c.map, c.num_entries, reinterpret_cast<char const*>(&def) + c.default_offset);
		}

		entry::dictionary_type& sett = e["settings"].dict();
		save_settings_to_dict(m_settings, sett);

#ifndef TORRENT_DISABLE_DHT
		if (m_dht && (flags & session::save_dht_state))
		{
			e["dht state"] = m_dht->state();
		}
#endif

#if TORRENT_USE_I2P
		if (flags & session::save_i2p_proxy)
		{
			save_struct(e["i2p"], &i2p_proxy(), proxy_settings_map
				, sizeof(proxy_settings_map)/sizeof(proxy_settings_map[0])
				, &def.m_proxy);
		}
#endif
#ifndef TORRENT_DISABLE_GEO_IP
		if (flags & session::save_as_map)
		{
			entry::dictionary_type& as_map = e["AS map"].dict();
			char buf[10];
			for (std::map<int, int>::const_iterator i = m_as_peak.begin()
				, end(m_as_peak.end()); i != end; ++i)
			{
				if (i->second == 0) continue;
					sprintf(buf, "%05d", i->first);
				as_map[buf] = i->second;
			}
		}
#endif

		if (flags & session::save_feeds)
		{
			entry::list_type& feeds = e["feeds"].list();
			for (std::vector<boost::shared_ptr<feed> >::const_iterator i =
				m_feeds.begin(), end(m_feeds.end()); i != end; ++i)
			{
				feeds.push_back(entry());
				(*i)->save_state(feeds.back());
			}
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (ses_extension_list_t::const_iterator i = m_ses_extensions.begin()
			, end(m_ses_extensions.end()); i != end; ++i)
		{
			TORRENT_TRY {
				(*i)->save_state(*eptr);
			} TORRENT_CATCH(std::exception&) {}
		}
#endif
	}
	
	void session_impl::set_proxy(proxy_settings const& s)
	{
		TORRENT_ASSERT(is_single_thread());

		m_proxy = s;
		// in case we just set a socks proxy, we might have to
		// open the socks incoming connection
		if (!m_socks_listen_socket) open_new_incoming_socks_connection();
		m_udp_socket.set_proxy_settings(m_proxy);
	}

	void session_impl::load_state(lazy_entry const* e)
	{
		TORRENT_ASSERT(is_single_thread());

		lazy_entry const* settings;
	  
		if (e->type() != lazy_entry::dict_t) return;

		for (int i = 0; i < int(sizeof(all_settings)/sizeof(all_settings[0])); ++i)
		{
			session_category const& c = all_settings[i];
			settings = e->dict_find_dict(c.name);
			if (!settings) continue;
			load_struct(*settings, reinterpret_cast<char*>(this) + c.offset, c.map, c.num_entries);
		}
		
		settings = e->dict_find_dict("settings");
		if (settings)
		{
			settings_pack* pack = load_pack_from_dict(settings);
			apply_settings_pack(pack);
		}

		// in case we just set a socks proxy, we might have to
		// open the socks incoming connection
		if (!m_socks_listen_socket) open_new_incoming_socks_connection();
		m_udp_socket.set_proxy_settings(m_proxy);

#ifndef TORRENT_DISABLE_DHT
		settings = e->dict_find_dict("dht state");
		if (settings)
		{
			m_dht_state = *settings;
		}
#endif

#if TORRENT_USE_I2P
		settings = e->dict_find_dict("i2p");
		if (settings)
		{
			proxy_settings s;
			load_struct(*settings, &s, proxy_settings_map
				, sizeof(proxy_settings_map)/sizeof(proxy_settings_map[0]));
			set_i2p_proxy(s);
		}
#endif
#ifndef TORRENT_DISABLE_GEO_IP
		settings  = e->dict_find_dict("AS map");
		if (settings)
		{
			for (int i = 0; i < settings->dict_size(); ++i)
			{
				std::pair<std::string, lazy_entry const*> item = settings->dict_at(i);
				int as_num = atoi(item.first.c_str());
				if (item.second->type() != lazy_entry::int_t || item.second->int_value() == 0) continue;
				int& peak = m_as_peak[as_num];
				if (peak < item.second->int_value()) peak = item.second->int_value();
			}
		}
#endif

		settings = e->dict_find_list("feeds");
		if (settings)
		{
			m_feeds.reserve(settings->list_size());
			for (int i = 0; i < settings->list_size(); ++i)
			{
				if (settings->list_at(i)->type() != lazy_entry::dict_t) continue;
				boost::shared_ptr<feed> f(new_feed(*this, feed_settings()));
				f->load_state(*settings->list_at(i));
				f->update_feed();
				m_feeds.push_back(f);
			}
			update_rss_feeds();
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (ses_extension_list_t::iterator i = m_ses_extensions.begin()
			, end(m_ses_extensions.end()); i != end; ++i)
		{
			TORRENT_TRY {
				(*i)->load_state(*e);
			} TORRENT_CATCH(std::exception&) {}
		}
#endif
	}

#ifndef TORRENT_DISABLE_GEO_IP
	namespace
	{
		struct free_ptr
		{
			void* ptr_;
			free_ptr(void* p): ptr_(p) {}
			~free_ptr() { free(ptr_); }
		};
	}

	char const* session_impl::country_for_ip(address const& a)
	{
		TORRENT_ASSERT(is_single_thread());

		if (!a.is_v4() || m_country_db == 0) return 0;
		return GeoIP_country_code_by_ipnum(m_country_db, a.to_v4().to_ulong());
	}

	int session_impl::as_for_ip(address const& a)
	{
		TORRENT_ASSERT(is_single_thread());

		if (!a.is_v4() || m_asnum_db == 0) return 0;
		char* name = GeoIP_name_by_ipnum(m_asnum_db, a.to_v4().to_ulong());
		if (name == 0) return 0;
		free_ptr p(name);
		// GeoIP returns the name as AS??? where ? is the AS-number
		return atoi(name + 2);
	}

	std::string session_impl::as_name_for_ip(address const& a)
	{
		TORRENT_ASSERT(is_single_thread());

		if (!a.is_v4() || m_asnum_db == 0) return std::string();
		char* name = GeoIP_name_by_ipnum(m_asnum_db, a.to_v4().to_ulong());
		if (name == 0) return std::string();
		free_ptr p(name);
		char* tmp = std::strchr(name, ' ');
		if (tmp == 0) return std::string();
		return tmp + 1;
	}

	std::pair<const int, int>* session_impl::lookup_as(int as)
	{
		TORRENT_ASSERT(is_single_thread());

		std::map<int, int>::iterator i = m_as_peak.lower_bound(as);

		if (i == m_as_peak.end() || i->first != as)
		{
			// we don't have any data for this AS, insert a new entry
			i = m_as_peak.insert(i, std::pair<int, int>(as, 0));
		}
		return &(*i);
	}

	void session_impl::load_asnum_db(std::string file)
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_asnum_db) GeoIP_delete(m_asnum_db);
		m_asnum_db = GeoIP_open(file.c_str(), GEOIP_STANDARD);
//		return m_asnum_db;
	}

#if TORRENT_USE_WSTRING
	void session_impl::load_asnum_dbw(std::wstring file)
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_asnum_db) GeoIP_delete(m_asnum_db);
		std::string utf8;
		wchar_utf8(file, utf8);
		m_asnum_db = GeoIP_open(utf8.c_str(), GEOIP_STANDARD);
//		return m_asnum_db;
	}

	void session_impl::load_country_dbw(std::wstring file)
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_country_db) GeoIP_delete(m_country_db);
		std::string utf8;
		wchar_utf8(file, utf8);
		m_country_db = GeoIP_open(utf8.c_str(), GEOIP_STANDARD);
//		return m_country_db;
	}
#endif // TORRENT_USE_WSTRING

	void session_impl::load_country_db(std::string file)
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_country_db) GeoIP_delete(m_country_db);
		m_country_db = GeoIP_open(file.c_str(), GEOIP_STANDARD);
//		return m_country_db;
	}

#endif // TORRENT_DISABLE_GEO_IP

#ifndef TORRENT_DISABLE_EXTENSIONS
	void session_impl::add_extension(
		boost::function<boost::shared_ptr<torrent_plugin>(torrent*, void*)> ext)
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT_VAL(ext, ext);

		typedef boost::shared_ptr<torrent_plugin>(*function_t)(torrent*, void*);
		function_t const* f = ext.target<function_t>();

		if (f)
		{
			for (extension_list_t::iterator i = m_extensions.begin(); i != m_extensions.end(); ++i)
				if (function_equal(*i, *f)) return;
		}

		m_extensions.push_back(ext);
	}

	void session_impl::add_ses_extension(boost::shared_ptr<plugin> ext)
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT_VAL(ext, ext);

		m_ses_extensions.push_back(ext);
		m_alerts.add_extension(ext);
		ext->added(shared_from_this());
	}
#endif

#ifndef TORRENT_DISABLE_DHT
	void session_impl::add_dht_node(udp::endpoint n)
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_dht) m_dht->add_node(n);
	}

	bool session_impl::has_dht() const
	{
		return m_dht;
	}
#endif

	feed_handle session_impl::add_feed(feed_settings const& sett)
	{
		TORRENT_ASSERT(is_single_thread());

		// look for duplicates. If we already have a feed with this
		// URL, return a handle to the existing one
		for (std::vector<boost::shared_ptr<feed> >::const_iterator i
			= m_feeds.begin(), end(m_feeds.end()); i != end; ++i)
		{
			if (sett.url != (*i)->m_settings.url) continue;
			return feed_handle(*i);
		}

		boost::shared_ptr<feed> f(new_feed(*this, sett));
		m_feeds.push_back(f);
		update_rss_feeds();
		return feed_handle(f);
	}

	void session_impl::remove_feed(feed_handle h)
	{
		TORRENT_ASSERT(is_single_thread());

		boost::shared_ptr<feed> f = h.m_feed_ptr.lock();
		if (!f) return;

		std::vector<boost::shared_ptr<feed> >::iterator i
			= std::find(m_feeds.begin(), m_feeds.end(), f);

		if (i == m_feeds.end()) return;

		m_feeds.erase(i);
	}

	void session_impl::get_feeds(std::vector<feed_handle>* ret) const
	{
		TORRENT_ASSERT(is_single_thread());

		ret->clear();
		ret->reserve(m_feeds.size());
		for (std::vector<boost::shared_ptr<feed> >::const_iterator i = m_feeds.begin()
			, end(m_feeds.end()); i != end; ++i)
			ret->push_back(feed_handle(*i));
	}

	void session_impl::pause()
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_paused) return;
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING
		(*m_logger) << time_now_string() << " *** session paused ***\n";
#endif
		m_paused = true;
		for (torrent_map::iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			torrent& t = *i->second;
			t.do_pause();
		}
	}

	void session_impl::resume()
	{
		TORRENT_ASSERT(is_single_thread());

		if (!m_paused) return;
		m_paused = false;
		for (torrent_map::iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			torrent& t = *i->second;
			t.do_resume();
			if (t.should_check_files()) t.start_checking();
		}
	}
	
	void session_impl::abort()
	{
		TORRENT_ASSERT(is_single_thread());

		if (m_abort) return;
#if defined TORRENT_LOGGING
		(*m_logger) << time_now_string() << " *** ABORT CALLED ***\n";
#endif
		// abort the main thread
		m_abort = true;
		error_code ec;
#if TORRENT_USE_I2P
		m_i2p_conn.close(ec);
#endif
		stop_lsd();
		stop_upnp();
		stop_natpmp();
#ifndef TORRENT_DISABLE_DHT
		stop_dht();
		m_dht_announce_timer.cancel(ec);
#endif
		m_lsd_announce_timer.cancel(ec);

		// close the listen sockets
		for (std::list<listen_socket_t>::iterator i = m_listen_sockets.begin()
			, end(m_listen_sockets.end()); i != end; ++i)
		{
			i->sock->close(ec);
			TORRENT_ASSERT(!ec);
		}
		m_listen_sockets.clear();
		if (m_socks_listen_socket && m_socks_listen_socket->is_open())
			m_socks_listen_socket->close();
		m_socks_listen_socket.reset();
		if (m_i2p_listen_socket && m_i2p_listen_socket->is_open())
			m_i2p_listen_socket->close();
		m_i2p_listen_socket.reset();

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " aborting all torrents (" << m_torrents.size() << ")\n";
#endif
		// abort all torrents
		for (torrent_map::iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			i->second->abort();
		}

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " aborting all tracker requests\n";
#endif
		m_tracker_manager.abort_all_requests();

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " sending event=stopped to trackers\n";
#endif
		for (torrent_map::iterator i = m_torrents.begin();
			i != m_torrents.end(); ++i)
		{
			torrent& t = *i->second;
			t.abort();
		}

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " aborting all connections (" << m_connections.size() << ")\n";
#endif
		m_half_open.close();

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " connection queue: " << m_half_open.size() << "\n";
#endif

		// abort all connections
		while (!m_connections.empty())
		{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			int conn = m_connections.size();
#endif
			(*m_connections.begin())->disconnect(errors::stopping_torrent);
			TORRENT_ASSERT_VAL(conn == int(m_connections.size()) + 1, conn);
		}

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " connection queue: " << m_half_open.size() << "\n";
#endif

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " shutting down connection queue\n";
#endif

		m_download_rate.close();
		m_upload_rate.close();

		// #error closing the udp socket here means that
		// the uTP connections cannot be closed gracefully
		m_udp_socket.close();
		m_external_udp_port = 0;

		m_undead_peers.clear();

#ifndef TORRENT_DISABLE_GEO_IP
		if (m_asnum_db) GeoIP_delete(m_asnum_db);
		if (m_country_db) GeoIP_delete(m_country_db);
		m_asnum_db = 0;
		m_country_db = 0;
#endif

		m_disk_thread.set_num_threads(0);
	}

	void session_impl::set_port_filter(port_filter const& f)
	{
		m_port_filter = f;
		// TODO: recalculate all connect candidates for all torrents
	}

	void session_impl::set_ip_filter(ip_filter const& f)
	{
		INVARIANT_CHECK;

		m_ip_filter = f;

		// Close connections whose endpoint is filtered
		// by the new ip-filter
		for (torrent_map::iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
			i->second->ip_filter_updated();
	}

	ip_filter const& session_impl::get_ip_filter() const
	{
		return m_ip_filter;
	}

	int session_impl::create_peer_class(char const* name)
	{
		return m_classes.new_peer_class(name);
	}

	void session_impl::delete_peer_class(int cid)
	{
		// if you hit this assert, you're deleting a non-existent peer class
		TORRENT_ASSERT(m_classes.at(cid));
		if (m_classes.at(cid) == 0) return;
		m_classes.decref(cid);
	}

	peer_class_info session_impl::get_peer_class(int cid)
	{
		peer_class_info ret;
		peer_class* pc = m_classes.at(cid);
		// if you hit this assert, you're passing in an invalid cid
		TORRENT_ASSERT(pc);
		if (pc == 0)
		{
#ifdef TORRENT_DEBUG
			// make it obvious that the return value is undefined
			ret.upload_limit = rand();
			ret.download_limit = rand();
			ret.label.resize(20);
			url_random(&ret.label[0], &ret.label[0] + 20);
			ret.ignore_unchoke_slots = false;
#endif
			return ret;
		}

		pc->get_info(&ret);
		return ret;
	}

	void session_impl::set_peer_class(int cid, peer_class_info const& pci)
	{
		peer_class* pc = m_classes.at(cid);
		// if you hit this assert, you're passing in an invalid cid
		TORRENT_ASSERT(pc);
		if (pc == 0) return;

		pc->set_info(&pci);
	}

	void session_impl::set_peer_class_filter(ip_filter const& f)
	{
		INVARIANT_CHECK;
		m_peer_class_filter = f;
	}

	ip_filter const& session_impl::get_peer_class_filter() const
	{
		return m_peer_class_filter;
	}

	void session_impl::set_peer_class_type_filter(peer_class_type_filter f)
	{
		m_peer_class_type_filter = f;
	}

	peer_class_type_filter session_impl::get_peer_class_type_filter()
	{
		return m_peer_class_type_filter;
	}

	void session_impl::set_peer_classes(peer_class_set* s, address const& a, int st)
	{
		boost::uint32_t peer_class_mask = m_peer_class_filter.access(a);

		// assign peer class based on socket type
		const static int mapping[] = { 0, 0, 0, 0, 1, 4, 2, 2, 2, 3};
		int socket_type = mapping[st];
		// filter peer classes based on type
		peer_class_mask = m_peer_class_type_filter.apply(socket_type, peer_class_mask);

		for (peer_class_t i = 0; peer_class_mask; peer_class_mask >>= 1, ++i)
		{
			if ((peer_class_mask & 1) == 0) continue;

			// if you hit this assert, your peer class filter contains
			// a bitmask referencing a non-existent peer class
			TORRENT_ASSERT(m_classes.at(i));

			if (m_classes.at(i) == 0) continue;
			s->add_class(m_classes, i);
		}
	}

	bool session_impl::ignore_unchoke_slots_set(peer_class_set const& set) const
	{
		int num = set.num_classes();
		for (int i = 0; i < num; ++i)
		{
			peer_class const* pc = m_classes.at(set.class_at(i));
			if (pc == 0) continue;
			if (pc->ignore_unchoke_slots) return true;
		}
		return false;
	}

	bandwidth_manager* session_impl::get_bandwidth_manager(int channel)
	{
		return (channel == peer_connection::download_channel)
			? &m_download_rate : &m_upload_rate;
	}

	void session_impl::bump_torrent(torrent* t)
	{
		// if t is the only torrent in the LRU list, both
		// its prev and next links will be NULL, even though
		// it's already in the list. Cover this case by also
		// checking to see if it's the first item
		if (t->next != NULL || t->prev != NULL || m_torrent_lru.front() == t)
		{
			// this torrent is in the list already.
			// first remove it
			m_torrent_lru.erase(t);
		}

		// pinned torrents should not be part of the LRU, since
		// the LRU is only used to evict torrents
		if (t->is_pinned()) return;

		m_torrent_lru.push_back(t);
	}

	void session_impl::evict_torrent(torrent* ignore)
	{
		if (!m_user_load_torrent) return;

		int loaded_limit = m_settings.get_int(settings_pack::active_loaded_limit);
		while (m_torrent_lru.size() >= loaded_limit)
		{
			// we're at the limit of loaded torrents. Find the least important
			// torrent and unload it. This is done with an LRU.
			torrent* i = (torrent*)m_torrent_lru.front();

			// ignore t. That's the torrent we're putting in
			if (i == ignore) i = (torrent*)i->next;

			// if there are no other torrents, we can't do anything
			if (i == NULL) break;

#ifdef TORRENT_STATS
			inc_stats_counter(torrent_evicted_counter);
#endif
			TORRENT_ASSERT(i->is_pinned() == false);
			i->unload();
			m_torrent_lru.erase(i);
		}
	}

	bool session_impl::load_torrent(torrent* t)
	{
		evict_torrent(t);

		// now, load t into RAM
		std::vector<char> buffer;
		error_code ec;
		m_user_load_torrent(t->info_hash(), buffer, ec);
		if (ec)
		{
			t->set_error(ec, -1);
			return false;
		}
		bool ret = t->load(buffer);
		bump_torrent(t);
		return ret;
	}

	void session_impl::deferred_submit_jobs()
	{
		if (m_deferred_submit_disk_jobs) return;
		m_deferred_submit_disk_jobs = true;
		m_io_service.post(boost::bind(&session_impl::submit_disk_jobs, this));
	}

	void session_impl::submit_disk_jobs()
	{
		TORRENT_ASSERT(m_deferred_submit_disk_jobs);
		m_deferred_submit_disk_jobs = false;
		m_disk_thread.submit_jobs();
	}

	// copies pointers to bandwidth channels from the peer classes
	// into the array. Only bandwidth channels with a bandwidth limit
	// is considered pertinent and copied
	// returns the number of pointers copied
	// channel is upload_channel or download_channel
	int session_impl::copy_pertinent_channels(peer_class_set const& set
		, int channel, bandwidth_channel** dst, int max)
	{
		int num_channels = set.num_classes();
		int num_copied = 0;
		for (int i = 0; i < num_channels; ++i)
		{
			peer_class* pc = m_classes.at(set.class_at(i));
			TORRENT_ASSERT(pc);
			if (pc == 0) continue;
			bandwidth_channel* chan = &pc->channel[channel];
			// no need to include channels that don't have any bandwidth limits
			if (chan->throttle() == 0) continue;
			dst[num_copied] = chan;
			++num_copied;
			if (num_copied == max) break;
		}
		return num_copied;
	}

	bool session_impl::use_quota_overhead(bandwidth_channel* ch, int channel, int amount)
	{
		ch->use_quota(amount);
		return (ch->throttle() > 0 && ch->throttle() < amount);
	}

	int session_impl::use_quota_overhead(peer_class_set& set, int amount_down, int amount_up)
	{
		int ret = 0;
		int num = set.num_classes();
		for (int i = 0; i < num; ++i)
		{
			peer_class* p = m_classes.at(set.class_at(i));
			if (p == 0) continue;
			bandwidth_channel* ch = &p->channel[peer_connection::download_channel];
			if (use_quota_overhead(ch, peer_connection::download_channel, amount_down))
				ret |= 1 << peer_connection::download_channel;
			ch = &p->channel[peer_connection::upload_channel];
			if (use_quota_overhead(ch, peer_connection::upload_channel, amount_up))
				ret |= 1 << peer_connection::upload_channel;
		}
		return ret;
	}

	// session_impl is responsible for deleting 'pack', but it
	// will pass it on to the disk io thread, which will take
	// over ownership of it
	void session_impl::apply_settings_pack(settings_pack* pack)
	{
		apply_pack(pack, m_settings, this);
		m_disk_thread.set_settings(pack);
		delete pack;
	}

#ifndef TORRENT_NO_DEPRECATE
	void session_impl::set_settings(libtorrent::session_settings const& s)
	{
		INVARIANT_CHECK;
		TORRENT_ASSERT(is_single_thread());
		settings_pack* p = load_pack_from_struct(m_settings, s);
		apply_settings_pack(p);
	}

	libtorrent::session_settings session_impl::deprecated_settings() const
	{
		libtorrent::session_settings ret;

		load_struct_from_settings(m_settings, ret);
		return ret;
	}
#endif

	tcp::endpoint session_impl::get_ipv6_interface() const
	{
		return m_ipv6_interface;
	}

	tcp::endpoint session_impl::get_ipv4_interface() const
	{
		return m_ipv4_interface;
	}

	void session_impl::setup_listener(listen_socket_t* s, tcp::endpoint ep
		, int retries, bool v6_only, int flags, error_code& ec)
	{
		s->sock.reset(new socket_acceptor(m_io_service));
		s->sock->open(ep.protocol(), ec);
		if (ec)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			(*m_logger) << "failed to open socket: " << print_endpoint(ep)
				<< ": " << ec.message() << "\n" << "\n";
#endif
			return;
		}

		error_code err; // ignore errors here
		s->sock->set_option(socket_acceptor::reuse_address(true), err);

#if TORRENT_USE_IPV6
		if (ep.protocol() == tcp::v6())
		{
			error_code err; // ignore errors here
			s->sock->set_option(v6only(v6_only), err);
#ifdef TORRENT_WINDOWS

#ifndef PROTECTION_LEVEL_UNRESTRICTED
#define PROTECTION_LEVEL_UNRESTRICTED 10
#endif
			// enable Teredo on windows
			s->sock->set_option(v6_protection_level(PROTECTION_LEVEL_UNRESTRICTED), err);
#endif
		}
#endif
		s->sock->bind(ep, ec);
		while (ec && retries > 0)
		{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			char msg[200];
			snprintf(msg, 200, "failed to bind to interface \"%s\": %s"
				, print_endpoint(ep).c_str(), ec.message().c_str());
			(*m_logger) << time_now_string() << " " << msg << "\n";
#endif
			ec.clear();
			TORRENT_ASSERT_VAL(!ec, ec);
			--retries;
			ep.port(ep.port() + 1);
			s->sock->bind(ep, ec);
		}
		if (ec && !(flags & session::listen_no_system_port))
		{
			// instead of giving up, trying
			// let the OS pick a port
			ep.port(0);
			ec = error_code();
			s->sock->bind(ep, ec);
		}
		if (ec)
		{
			// not even that worked, give up
			if (m_alerts.should_post<listen_failed_alert>())
				m_alerts.post_alert(listen_failed_alert(ep, ec));
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			char msg[200];
			snprintf(msg, 200, "cannot bind to interface \"%s\": %s"
				, print_endpoint(ep).c_str(), ec.message().c_str());
			(*m_logger) << time_now_string() << msg << "\n";
#endif
			return;
		}
		s->external_port = s->sock->local_endpoint(ec).port();
		if (!ec) s->sock->listen(m_settings.get_int(settings_pack::listen_queue_size), ec);
		if (ec)
		{
			if (m_alerts.should_post<listen_failed_alert>())
				m_alerts.post_alert(listen_failed_alert(ep, ec));
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			char msg[200];
			snprintf(msg, 200, "cannot listen on interface \"%s\": %s"
				, print_endpoint(ep).c_str(), ec.message().c_str());
			(*m_logger) << time_now_string() << msg << "\n";
#endif
			return;
		}

		// if we asked the system to listen on port 0, which
		// socket did it end up choosing?
		if (ep.port() == 0)
			ep.port(s->sock->local_endpoint(ec).port());

		if (m_alerts.should_post<listen_succeeded_alert>())
			m_alerts.post_alert(listen_succeeded_alert(ep));

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		(*m_logger) << time_now_string() << " listening on: " << ep
			<< " external port: " << s->external_port << "\n";
#endif
	}
	
	void session_impl::open_listen_port(int flags, error_code& ec)
	{
		TORRENT_ASSERT(is_single_thread());

		// close the open listen sockets
		m_listen_sockets.clear();
		m_incoming_connection = false;

		m_ipv6_interface = tcp::endpoint();
		m_ipv4_interface = tcp::endpoint();

#ifdef TORRENT_USE_OPENSSL
		tcp::endpoint ssl_interface = m_listen_interface;
		ssl_interface.port(m_settings.get_int(settings_pack::ssl_listen));
#endif
	
		if (is_any(m_listen_interface.address()))
		{
			// this means we should open two listen sockets
			// one for IPv4 and one for IPv6
		
			listen_socket_t s;
			setup_listener(&s, tcp::endpoint(address_v4::any(), m_listen_interface.port())
				, m_listen_port_retries, false, flags, ec);

			if (s.sock)
			{
				// update the listen_interface member with the
				// actual port we ended up listening on, so that the other
				// sockets can be bound to the same one
				m_listen_interface.port(s.external_port);

				m_listen_sockets.push_back(s);
				async_accept(s.sock, s.ssl);
			}

#ifdef TORRENT_USE_OPENSSL
			if (m_settings.get_int(settings_pack::ssl_listen))
			{
				listen_socket_t s;
				s.ssl = true;
				setup_listener(&s, ssl_interface, 10, false, flags, ec);

				if (s.sock)
				{
					m_listen_sockets.push_back(s);
					async_accept(s.sock, s.ssl);
				}
			}
#endif

#if TORRENT_USE_IPV6
			// only try to open the IPv6 port if IPv6 is installed
			if (supports_ipv6())
			{
				setup_listener(&s, tcp::endpoint(address_v6::any(), m_listen_interface.port())
					, m_listen_port_retries, true, flags, ec);

				if (s.sock)
				{
					m_listen_sockets.push_back(s);
					async_accept(s.sock, s.ssl);
				}

#ifdef TORRENT_USE_OPENSSL
				if (m_settings.get_int(settings_pack::ssl_listen))
				{
					listen_socket_t s;
					s.ssl = true;
					setup_listener(&s, tcp::endpoint(address_v6::any(), ssl_interface.port())
						, 10, false, flags, ec);

					if (s.sock)
					{
						m_listen_sockets.push_back(s);
						async_accept(s.sock, s.ssl);
					}
				}
#endif // TORRENT_USE_OPENSSL
			}
#endif // TORRENT_USE_IPV6

			// set our main IPv4 and IPv6 interfaces
			// used to send to the tracker
			std::vector<ip_interface> ifs = enum_net_interfaces(m_io_service, ec);
			for (std::vector<ip_interface>::const_iterator i = ifs.begin()
					, end(ifs.end()); i != end; ++i)
			{
				address const& addr = i->interface_address;
				if (addr.is_v6() && !is_local(addr) && !is_loopback(addr))
					m_ipv6_interface = tcp::endpoint(addr, m_listen_interface.port());
				else if (addr.is_v4() && !is_local(addr) && !is_loopback(addr))
					m_ipv4_interface = tcp::endpoint(addr, m_listen_interface.port());
			}
		}
		else
		{
			// we should only open a single listen socket, that
			// binds to the given interface

			listen_socket_t s;
			setup_listener(&s, m_listen_interface, m_listen_port_retries, false, flags, ec);

			if (s.sock)
			{
				m_listen_sockets.push_back(s);
				async_accept(s.sock, s.ssl);

				if (m_listen_interface.address().is_v6())
					m_ipv6_interface = m_listen_interface;
				else
					m_ipv4_interface = m_listen_interface;
			}

#ifdef TORRENT_USE_OPENSSL
			if (m_settings.get_int(settings_pack::ssl_listen))
			{
				listen_socket_t s;
				s.ssl = true;
				setup_listener(&s, ssl_interface, 10, false, flags, ec);

				if (s.sock)
				{
					m_listen_sockets.push_back(s);
					async_accept(s.sock, s.ssl);
				}
			}
#endif
		}

		m_udp_socket.bind(udp::endpoint(m_listen_interface.address(), m_listen_interface.port()), ec);
		if (ec)
		{
			if (m_alerts.should_post<listen_failed_alert>())
				m_alerts.post_alert(listen_failed_alert(m_listen_interface, ec));
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			char msg[200];
			snprintf(msg, sizeof(msg), "cannot bind to UDP interface \"%s\": %s"
				, print_endpoint(m_listen_interface).c_str(), ec.message().c_str());
			(*m_logger) << msg << "\n";
#endif
		}
		else
		{
			m_external_udp_port = m_udp_socket.local_port();
			maybe_update_udp_mapping(0, m_listen_interface.port(), m_listen_interface.port());
			maybe_update_udp_mapping(1, m_listen_interface.port(), m_listen_interface.port());
		}

		m_udp_socket.set_option(type_of_service(m_settings.get_int(settings_pack::peer_tos)), ec);
#if defined TORRENT_VERBOSE_LOGGING
		(*m_logger) << ">>> SET_TOS[ udp_socket tos: " << m_settings.get_int(settings_pack::peer_tos) << " e: " << ec.message() << " ]\n";
#endif
		ec.clear();

		open_new_incoming_socks_connection();
#if TORRENT_USE_I2P
		open_new_incoming_i2p_connection();
#endif

		if (!m_listen_sockets.empty())
		{
			tcp::endpoint local = m_listen_sockets.front().sock->local_endpoint(ec);
			if (!ec) remap_tcp_ports(3, local.port(), ssl_listen_port());
		}

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_logger = create_log("main_session", listen_port(), false);
#endif
	}

	void session_impl::remap_tcp_ports(boost::uint32_t mask, int tcp_port, int ssl_port)
	{
		if ((mask & 1) && m_natpmp.get())
		{
			if (m_tcp_mapping[0] != -1) m_natpmp->delete_mapping(m_tcp_mapping[0]);
			m_tcp_mapping[0] = m_natpmp->add_mapping(natpmp::tcp, tcp_port, tcp_port);
#ifdef TORRENT_USE_OPENSSL
			if (m_ssl_mapping[0] != -1) m_natpmp->delete_mapping(m_ssl_mapping[0]);
			m_ssl_mapping[0] = m_natpmp->add_mapping(natpmp::tcp, ssl_port, ssl_port);
#endif
		}
		if ((mask & 2) && m_upnp.get())
		{
			if (m_tcp_mapping[1] != -1) m_upnp->delete_mapping(m_tcp_mapping[1]);
			m_tcp_mapping[1] = m_upnp->add_mapping(upnp::tcp, tcp_port, tcp_port);
#ifdef TORRENT_USE_OPENSSL
			if (m_ssl_mapping[1] != -1) m_upnp->delete_mapping(m_ssl_mapping[1]);
			m_ssl_mapping[1] = m_upnp->add_mapping(upnp::tcp, ssl_port, ssl_port);
#endif
		}
	}

	void session_impl::open_new_incoming_socks_connection()
	{
		if (m_proxy.type != proxy_settings::socks5
			&& m_proxy.type != proxy_settings::socks5_pw
			&& m_proxy.type != proxy_settings::socks4)
			return;
		
		if (m_socks_listen_socket) return;

		m_socks_listen_socket = boost::shared_ptr<socket_type>(new socket_type(m_io_service));
		bool ret = instantiate_connection(m_io_service, m_proxy
			, *m_socks_listen_socket);
		TORRENT_ASSERT_VAL(ret, ret);

#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_socks_accept");
#endif
		socks5_stream& s = *m_socks_listen_socket->get<socks5_stream>();
		s.set_command(2); // 2 means BIND (as opposed to CONNECT)
		m_socks_listen_port = m_listen_interface.port();
		if (m_socks_listen_port == 0) m_socks_listen_port = 2000 + random() % 60000;
		s.async_connect(tcp::endpoint(address_v4::any(), m_socks_listen_port)
			, boost::bind(&session_impl::on_socks_accept, this, m_socks_listen_socket, _1));
	}

#if TORRENT_USE_I2P
	void session_impl::on_i2p_open(error_code const& ec)
	{
		open_new_incoming_i2p_connection();
	}

	void session_impl::open_new_incoming_i2p_connection()
	{
		if (!m_i2p_conn.is_open()) return;

		if (m_i2p_listen_socket) return;

		m_i2p_listen_socket = boost::shared_ptr<socket_type>(new socket_type(m_io_service));
		bool ret = instantiate_connection(m_io_service, m_i2p_conn.proxy()
			, *m_i2p_listen_socket);
		TORRENT_ASSERT_VAL(ret, ret);

#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_i2p_accept");
#endif
		i2p_stream& s = *m_i2p_listen_socket->get<i2p_stream>();
		s.set_command(i2p_stream::cmd_accept);
		s.set_session_id(m_i2p_conn.session_id());
		s.async_connect(tcp::endpoint(address_v4::any(), m_listen_interface.port())
			, boost::bind(&session_impl::on_i2p_accept, this, m_i2p_listen_socket, _1));
	}

	void session_impl::on_i2p_accept(boost::shared_ptr<socket_type> const& s
		, error_code const& e)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("session_impl::on_i2p_accept");
#endif
		m_i2p_listen_socket.reset();
		if (e == asio::error::operation_aborted) return;
		if (e)
		{
			if (m_alerts.should_post<listen_failed_alert>())
				m_alerts.post_alert(listen_failed_alert(tcp::endpoint(
					address_v4::any(), m_listen_interface.port()), e));
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
			char msg[200];
			snprintf(msg, sizeof(msg), "cannot bind to port %d: %s"
				, m_listen_interface.port(), e.message().c_str());
			(*m_logger) << msg << "\n";
#endif
			return;
		}
		open_new_incoming_i2p_connection();
		incoming_connection(s);
	}
#endif

	bool session_impl::incoming_packet(error_code const& ec
		, udp::endpoint const& ep, char const* buf, int size)
	{
#ifdef TORRENT_STATS
		inc_stats_counter(on_udp_counter);
#endif

		if (ec)
		{
			// don't bubble up operation aborted errors to the user
			if (ec != asio::error::operation_aborted
				&& m_alerts.should_post<udp_error_alert>())
				m_alerts.post_alert(udp_error_alert(ep, ec));
		}
		return false;
	}

	void session_impl::async_accept(boost::shared_ptr<socket_acceptor> const& listener, bool ssl)
	{
		TORRENT_ASSERT(!m_abort);
		shared_ptr<socket_type> c(new socket_type(m_io_service));
		stream_socket* str = 0;

#ifdef TORRENT_USE_OPENSSL
		if (ssl)
		{
			// accept connections initializing the SSL connection to
			// use the generic m_ssl_ctx context. However, since it has
			// the servername callback set on it, we will switch away from
			// this context into a specific torrent once we start handshaking
			c->instantiate<ssl_stream<stream_socket> >(m_io_service, &m_ssl_ctx);
			str = &c->get<ssl_stream<stream_socket> >()->next_layer();
		}
		else
#endif
		{
			c->instantiate<stream_socket>(m_io_service);
			str = c->get<stream_socket>();
		}


#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_accept_connection");
#endif
		listener->async_accept(*str
			, boost::bind(&session_impl::on_accept_connection, this, c
			, boost::weak_ptr<socket_acceptor>(listener), _1, ssl));
	}

	void session_impl::on_accept_connection(shared_ptr<socket_type> const& s
		, weak_ptr<socket_acceptor> listen_socket, error_code const& e, bool ssl)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("session_impl::on_accept_connection");
#endif
#ifdef TORRENT_STATS
		inc_stats_counter(on_accept_counter);
#endif
		TORRENT_ASSERT(is_single_thread());
		boost::shared_ptr<socket_acceptor> listener = listen_socket.lock();
		if (!listener) return;
		
		if (e == asio::error::operation_aborted) return;

		if (m_abort) return;

		error_code ec;
		if (e)
		{
			tcp::endpoint ep = listener->local_endpoint(ec);
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			std::string msg = "error accepting connection on '"
				+ print_endpoint(ep) + "' " + e.message();
			(*m_logger) << msg << "\n";
#endif
#ifdef TORRENT_WINDOWS
			// Windows sometimes generates this error. It seems to be
			// non-fatal and we have to do another async_accept.
			if (e.value() == ERROR_SEM_TIMEOUT)
			{
				async_accept(listener, ssl);
				return;
			}
#endif
#ifdef TORRENT_BSD
			// Leopard sometimes generates an "invalid argument" error. It seems to be
			// non-fatal and we have to do another async_accept.
			if (e.value() == EINVAL)
			{
				async_accept(listener, ssl);
				return;
			}
#endif
			if (e == boost::system::errc::too_many_files_open)
			{
				// if we failed to accept an incoming connection
				// because we have too many files open, try again
				// and lower the number of file descriptors used
				// elsewere.
				if (m_settings.get_int(settings_pack::connections_limit) > 10)
				{
					// now, disconnect a random peer
					torrent_map::iterator i = std::max_element(m_torrents.begin()
						, m_torrents.end(), boost::bind(&torrent::num_peers
							, boost::bind(&torrent_map::value_type::second, _1)));

					if (m_alerts.should_post<performance_alert>())
						m_alerts.post_alert(performance_alert(
							torrent_handle(), performance_alert::too_few_file_descriptors));

					if (i != m_torrents.end())
					{
						i->second->disconnect_peers(1, e);
					}

					m_settings.set_int(settings_pack::connections_limit, m_connections.size());
				}
				// try again, but still alert the user of the problem
				async_accept(listener, ssl);
			}
			if (m_alerts.should_post<listen_failed_alert>())
				m_alerts.post_alert(listen_failed_alert(ep, e));
			return;
		}
		async_accept(listener, ssl);

#ifdef TORRENT_USE_OPENSSL
		if (ssl)
		{
			// for SSL connections, incoming_connection() is called
			// after the handshake is done
			s->get<ssl_stream<stream_socket> >()->async_accept_handshake(
				boost::bind(&session_impl::ssl_handshake, this, _1, s));
		}
		else
#endif
		{
			incoming_connection(s);
		}
	}

#ifdef TORRENT_USE_OPENSSL

	// to test SSL connections, one can use this openssl command template:
	// 
	// openssl s_client -cert <client-cert>.pem -key <client-private-key>.pem \ 
	//   -CAfile <torrent-cert>.pem  -debug -connect 127.0.0.1:4433 -tls1 \ 
	//   -servername <hex-encoded-info-hash>

	void session_impl::ssl_handshake(error_code const& ec, boost::shared_ptr<socket_type> s)
	{
		error_code e;
		tcp::endpoint endp = s->remote_endpoint(e);
		if (e) return;

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " *** peer SSL handshake done [ ip: "
			<< endp << " ec: " << ec.message() << " socket: " << s->type_name() << "]\n";
#endif

		if (ec)
		{
			if (m_alerts.should_post<peer_error_alert>())
			{
				m_alerts.post_alert(peer_error_alert(torrent_handle(), endp
					, peer_id(), ec));
			}
			return;
		}

		incoming_connection(s);
	}

#endif // TORRENT_USE_OPENSSL

	void session_impl::incoming_connection(boost::shared_ptr<socket_type> const& s)
	{
		TORRENT_ASSERT(is_single_thread());

#ifdef TORRENT_USE_OPENSSL
		// add the current time to the PRNG, to add more unpredictability
		boost::uint64_t now = total_microseconds(time_now_hires() - min_time());
		// assume 12 bits of entropy (i.e. about 8 milliseconds)
		RAND_add(&now, 8, 1.5);
#endif

		if (m_paused)
		{
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			(*m_logger) << time_now_string() << " <== INCOMING CONNECTION [ ignored, paused ]\n";
#endif
			return;
		}

		error_code ec;
		// we got a connection request!
		tcp::endpoint endp = s->remote_endpoint(ec);

		if (ec)
		{
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			(*m_logger) << endp << " <== INCOMING CONNECTION FAILED, could "
				"not retrieve remote endpoint " << ec.message() << "\n";
#endif
			return;
		}

		TORRENT_ASSERT(endp.address() != address_v4::any());

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " <== INCOMING CONNECTION " << endp
			<< " type: " << s->type_name() << "\n";
#endif

		if (m_alerts.should_post<incoming_connection_alert>())
		{
			m_alerts.post_alert(incoming_connection_alert(s->type(), endp));
		}

		if (!m_settings.get_bool(settings_pack::enable_incoming_utp)
			&& s->get<utp_stream>())
		{
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			(*m_logger) << "    rejected uTP connection\n";
#endif
			if (m_alerts.should_post<peer_blocked_alert>())
				m_alerts.post_alert(peer_blocked_alert(torrent_handle(), endp.address()));
			return;
		}

		if (!m_settings.get_bool(settings_pack::enable_incoming_tcp)
			&& s->get<stream_socket>())
		{
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			(*m_logger) << "    rejected TCP connection\n";
#endif
			if (m_alerts.should_post<peer_blocked_alert>())
				m_alerts.post_alert(peer_blocked_alert(torrent_handle(), endp.address()));
			return;
		}

		// local addresses do not count, since it's likely
		// coming from our own client through local service discovery
		// and it does not reflect whether or not a router is open
		// for incoming connections or not.
		if (!is_local(endp.address()))
			m_incoming_connection = true;

		// this filter is ignored if a single torrent
		// is set to ignore the filter, since this peer might be
		// for that torrent
		if (m_non_filtered_torrents == 0
			&& (m_ip_filter.access(endp.address()) & ip_filter::blocked))
		{
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			(*m_logger) << "filtered blocked ip\n";
#endif
			if (m_alerts.should_post<peer_blocked_alert>())
				m_alerts.post_alert(peer_blocked_alert(torrent_handle(), endp.address()));
			return;
		}
/*
#error instead of using ignore_local_limits_on_local_network. add another field to the peer_class saying it is allowed to exceed the connection limit (by some factor maybe) and change this code to resolve which peer classes the peer would belong to and go through those for this flag
*/
		// don't allow more connections than the max setting
		bool reject = false;
		if (
#ifndef TORRENT_NO_DEPRECATE
			m_settings.get_bool(settings_pack::ignore_limits_on_local_network) &&
#endif
			is_local(endp.address()))
			reject = m_settings.get_int(settings_pack::connections_limit) < INT_MAX / 12
				&& num_connections() >= m_settings.get_int(settings_pack::connections_limit) * 12 / 10;
		else
			reject = num_connections() >= m_settings.get_int(settings_pack::connections_limit);

		if (reject)
		{
			if (m_alerts.should_post<peer_disconnected_alert>())
			{
				m_alerts.post_alert(
					peer_disconnected_alert(torrent_handle(), endp, peer_id()
						, error_code(errors::too_many_connections, get_libtorrent_category())));
			}
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			(*m_logger) << "number of connections limit exceeded (conns: "
				<< num_connections() << ", limit: " << m_settings.get_int(settings_pack::connections_limit)
				<< "), connection rejected\n";
#endif
			return;
		}

		// check if we have any active torrents
		// if we don't reject the connection
		if (m_torrents.empty())
		{
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
			(*m_logger) << " There are no torrents, disconnect\n";
#endif
		  	return;
		}

		// if we don't have any active torrents, there's no
		// point in accepting this connection. If, however,
		// the setting to start up queued torrents when they
		// get an incoming connection is enabled, we cannot
		// perform this check.
		if (!m_settings.get_bool(settings_pack::incoming_starts_queued_torrents))
		{
			bool has_active_torrent = false;
			for (torrent_map::iterator i = m_torrents.begin()
				, end(m_torrents.end()); i != end; ++i)
			{
				if (i->second->allows_peers())
				{
					has_active_torrent = true;
					break;
				}
			}
			if (!has_active_torrent)
			{
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
				(*m_logger) << " There are no _active_ torrents, disconnect\n";
#endif
			  	return;
			}
		}

		setup_socket_buffers(*s);

		boost::intrusive_ptr<peer_connection> c(
			new bt_peer_connection(*this, m_settings
				, *this, m_disk_thread, m_io_service, s, endp, 0));
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		c->m_in_constructor = false;
#endif

		if (!c->is_disconnecting())
		{
			m_connections.insert(c);
			c->start();
			// update the next disk peer round-robin cursor
			if (m_next_disk_peer == m_connections.end()) m_next_disk_peer = m_connections.begin();
		}
	}

	void session_impl::setup_socket_buffers(socket_type& s)
	{
		error_code ec;
		if (m_settings.get_int(settings_pack::send_socket_buffer_size))
		{
			stream_socket::send_buffer_size option(
				m_settings.get_int(settings_pack::send_socket_buffer_size));
			s.set_option(option, ec);
		}
		if (m_settings.get_int(settings_pack::recv_socket_buffer_size))
		{
			stream_socket::receive_buffer_size option(
				m_settings.get_int(settings_pack::recv_socket_buffer_size));
			s.set_option(option, ec);
		}
	}

	void session_impl::on_socks_accept(boost::shared_ptr<socket_type> const& s
		, error_code const& e)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("session_impl::on_socks_accept");
#endif
		m_socks_listen_socket.reset();
		if (e == asio::error::operation_aborted) return;
		if (e)
		{
			if (m_alerts.should_post<listen_failed_alert>())
				m_alerts.post_alert(listen_failed_alert(tcp::endpoint(
					address_v4::any(), m_listen_interface.port()), e));
			return;
		}
		open_new_incoming_socks_connection();
		incoming_connection(s);
	}

	// if cancel_with_cq is set, the peer connection is
	// currently expected to be scheduled for a connection
	// with the connection queue, and should be cancelled
	void session_impl::close_connection(peer_connection* p
		, error_code const& ec, bool cancel_with_cq)
	{
		TORRENT_ASSERT(is_single_thread());

		if (cancel_with_cq) m_half_open.cancel(p);

		// someone else is holding a reference, it's important that
		// it's destructed from the network thread. Make sure the
		// last reference is held by the network thread.
		if (p->refcount() > 1)
			m_undead_peers.push_back(boost::intrusive_ptr<peer_connection>(p));

// too expensive
//		INVARIANT_CHECK;

#ifdef TORRENT_DEBUG
//		for (aux::session_impl::torrent_map::const_iterator i = m_torrents.begin()
//			, end(m_torrents.end()); i != end; ++i)
//			TORRENT_ASSERT(!i->second->has_peer((peer_connection*)p));
#endif

#if defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " CLOSING CONNECTION "
			<< p->remote() << " : " << ec.message() << "\n";
#endif

		TORRENT_ASSERT(p->is_disconnecting());

		if (!p->is_choked() && !p->ignore_unchoke_slots()) --m_num_unchoked;
		TORRENT_ASSERT(p->refcount() > 0);

		boost::intrusive_ptr<peer_connection> sp((peer_connection*)p);
		connection_map::iterator i = m_connections.find(sp);
		// make sure the next disk peer round-robin cursor stays valid
		if (m_next_disk_peer == i) ++m_next_disk_peer;
		if (i != m_connections.end()) m_connections.erase(i);
		if (m_next_disk_peer == m_connections.end()) m_next_disk_peer = m_connections.begin();
	}

	// implements alert_dispatcher
	bool session_impl::post_alert(alert* a)
	{
		if (!m_alerts.should_post(a)) return false;
		m_alerts.post_alert_ptr(a);
		return true;
	}

	void session_impl::set_peer_id(peer_id const& id)
	{
		m_peer_id = id;
	}

	void session_impl::set_key(int key)
	{
		m_key = key;
	}

	void session_impl::unchoke_peer(peer_connection& c)
	{
		TORRENT_ASSERT(!c.ignore_unchoke_slots());
		torrent* t = c.associated_torrent().lock().get();
		TORRENT_ASSERT(t);
		if (t->unchoke_peer(c))
			++m_num_unchoked;
	}

	void session_impl::choke_peer(peer_connection& c)
	{
		TORRENT_ASSERT(!c.ignore_unchoke_slots());
		torrent* t = c.associated_torrent().lock().get();
		TORRENT_ASSERT(t);
		if (t->choke_peer(c))
			--m_num_unchoked;
	}

	int session_impl::next_port()
	{
		int start = m_settings.get_int(settings_pack::outgoing_port);
		int num = m_settings.get_int(settings_pack::num_outgoing_ports);
		std::pair<int, int> out_ports(start, start + num);
		if (m_next_port < out_ports.first || m_next_port > out_ports.second)
			m_next_port = out_ports.first;
	
		int port = m_next_port;
		++m_next_port;
		if (m_next_port > out_ports.second) m_next_port = out_ports.first;
#if defined TORRENT_LOGGING
		(*m_logger) << time_now_string() << " *** BINDING OUTGOING CONNECTION [ "
			"port: " << port << " ]\n";
#endif
		return port;
	}

	// used to cache the current time
	// every 100 ms. This is cheaper
	// than a system call and can be
	// used where more accurate time
	// is not necessary
	extern ptime g_current_time;

	initialize_timer::initialize_timer()
	{
		g_current_time = time_now_hires();
	}

	int session_impl::rate_limit(peer_class_t c, int channel) const
	{
		TORRENT_ASSERT(channel >= 0 && channel <= 1);
		if (channel < 0 || channel > 1) return 0;

		peer_class const* pc = m_classes.at(c);
		if (pc == 0) return 0;
		return pc->channel[channel].throttle();
	}

	int session_impl::upload_rate_limit(peer_class_t c) const
	{
		return rate_limit(c, peer_connection::upload_channel);
	}

	int session_impl::download_rate_limit(peer_class_t c) const
	{
		return rate_limit(c, peer_connection::download_channel);
	}

	void session_impl::set_rate_limit(peer_class_t c, int channel, int limit)
	{
		TORRENT_ASSERT(is_single_thread());
		TORRENT_ASSERT(limit >= -1);
		TORRENT_ASSERT(channel >= 0 && channel <= 1);

		if (channel < 0 || channel > 1) return;

		peer_class* pc = m_classes.at(c);
		if (pc == 0) return;
		if (limit <= 0) limit = 0;
		pc->channel[channel].throttle(limit);
	}

	void session_impl::set_upload_rate_limit(peer_class_t c, int limit)
	{
		set_rate_limit(c, peer_connection::upload_channel, limit);
	}

	void session_impl::set_download_rate_limit(peer_class_t c, int limit)
	{
		set_rate_limit(c, peer_connection::download_channel, limit);
	}

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	bool session_impl::has_peer(peer_connection const* p) const
	{
		TORRENT_ASSERT(is_single_thread());
		return std::find_if(m_connections.begin(), m_connections.end()
			, boost::bind(&boost::intrusive_ptr<peer_connection>::get, _1) == p)
			!= m_connections.end();
	}

	bool session_impl::any_torrent_has_peer(peer_connection const* p) const
	{
		for (aux::session_impl::torrent_map::const_iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
			if (i->second->has_peer(p)) return true;
		return false;
	}
#endif

	void session_impl::sent_bytes(int bytes_payload, int bytes_protocol)
	{
		m_stat.sent_bytes(bytes_payload, bytes_protocol);
	}

	void session_impl::received_bytes(int bytes_payload, int bytes_protocol)
	{
		m_stat.received_bytes(bytes_payload, bytes_protocol);
	}

	void session_impl::trancieve_ip_packet(int bytes, bool ipv6)
	{
		m_stat.trancieve_ip_packet(bytes, ipv6);
	}

	void session_impl::sent_syn(bool ipv6)
	{
		m_stat.sent_syn(ipv6);
	}

	void session_impl::received_synack(bool ipv6)
	{
		m_stat.received_synack(ipv6);
	}

	void session_impl::on_tick(error_code const& e)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("session_impl::on_tick");
#endif
#ifdef TORRENT_STATS
		inc_stats_counter(on_tick_counter);
#endif

		TORRENT_ASSERT(is_single_thread());

		// submit all disk jobs when we leave this function
		deferred_submit_jobs();

		ptime now = time_now_hires();
		aux::g_current_time = now;
// too expensive
//		INVARIANT_CHECK;

#if defined TORRENT_VERBOSE_LOGGING
//		(*m_logger) << time_now_string() << " session_impl::on_tick\n";
#endif

		// we have to keep ticking the utp socket manager
		// until they're all closed
		if (m_abort)
		{
			if (m_utp_socket_manager.num_sockets() == 0)
				return;
#if defined TORRENT_ASIO_DEBUGGING
			fprintf(stderr, "uTP sockets left: %d\n", m_utp_socket_manager.num_sockets());
#endif
		}

		if (e == asio::error::operation_aborted) return;

		if (e)
		{
#if defined TORRENT_LOGGING || defined TORRENT_VERBOSE_LOGGING
			(*m_logger) << "*** TICK TIMER FAILED " << e.message() << "\n";
#endif
			::abort();
			return;
		}

#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_tick");
#endif
		error_code ec;
		m_timer.expires_at(now + milliseconds(m_settings.get_int(settings_pack::tick_interval)), ec);
		m_timer.async_wait(bind(&session_impl::on_tick, this, _1));

		m_download_rate.update_quotas(now - m_last_tick);
		m_upload_rate.update_quotas(now - m_last_tick);

		m_last_tick = now;

		m_utp_socket_manager.tick(now);

		// only tick the following once per second
		if (now - m_last_second_tick < seconds(1)) return;

		// remove undead peers that only have this list as their reference keeping them alive
		std::vector<intrusive_ptr<peer_connection> >::iterator i = std::remove_if(
			m_undead_peers.begin(), m_undead_peers.end(), boost::bind(&peer_connection::refcount, _1) < 2);
		m_undead_peers.erase(i, m_undead_peers.end());

		int tick_interval_ms = total_milliseconds(now - m_last_second_tick);
		m_last_second_tick = now;
		m_tick_residual += tick_interval_ms - 1000;

		int session_time = total_seconds(now - m_created);
		if (session_time > 65000)
		{
			// we're getting close to the point where our timestamps
			// in policy::peer are wrapping. We need to step all counters back
			// four hours. This means that any timestamp that refers to a time
			// more than 18.2 - 4 = 14.2 hours ago, will be incremented to refer to
			// 14.2 hours ago.

			m_created += hours(4);

			const int four_hours = 60 * 60 * 4;
			for (torrent_map::iterator i = m_torrents.begin()
				, end(m_torrents.end()); i != end; ++i)
			{
				policy& p = i->second->get_policy();
				for (policy::iterator j = p.begin_peer()
					, end(p.end_peer()); j != end; ++j)
				{
					policy::peer* pe = *j;

					if (pe->last_optimistically_unchoked < four_hours)
						pe->last_optimistically_unchoked = 0;
					else
						pe->last_optimistically_unchoked -= four_hours;

					if (pe->last_connected < four_hours)
						pe->last_connected = 0;
					else
						pe->last_connected -= four_hours;
				}
			}
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (ses_extension_list_t::const_iterator i = m_ses_extensions.begin()
			, end(m_ses_extensions.end()); i != end; ++i)
		{
			TORRENT_TRY {
				(*i)->on_tick();
			} TORRENT_CATCH(std::exception&) {}
		}
#endif

		// don't do any of the following while we're shutting down
		if (m_abort) return;

		// --------------------------------------------------------------
		// RSS feeds
		// --------------------------------------------------------------
		if (now > m_next_rss_update)
			update_rss_feeds();

		switch (m_settings.get_int(settings_pack::mixed_mode_algorithm))
		{
			case settings_pack::prefer_tcp:
				set_upload_rate_limit(m_tcp_peer_class, 0);
				set_download_rate_limit(m_tcp_peer_class, 0);
				break;
			case settings_pack::peer_proportional:
				{
					int num_peers[2][2] = {{0, 0}, {0, 0}};
					for (connection_map::iterator i = m_connections.begin()
						, end(m_connections.end());i != end; ++i)
					{
						peer_connection& p = *(*i);
						if (p.in_handshake()) continue;
						int protocol = 0;
						if (p.get_socket()->get<utp_stream>()) protocol = 1;

						if (p.download_queue().size() + p.request_queue().size() > 0)
							++num_peers[protocol][peer_connection::download_channel];
						if (p.upload_queue().size() > 0)
							++num_peers[protocol][peer_connection::upload_channel];
					}

					peer_class* pc = m_classes.at(m_tcp_peer_class);
					bandwidth_channel* tcp_channel = pc->channel;
					int stat_rate[] = {m_stat.upload_rate(), m_stat.download_rate() };
					// never throttle below this
					int lower_limit[] = {5000, 30000};

					for (int i = 0; i < 2; ++i)
					{
						// if there are no uploading uTP peers, don't throttle TCP up
						if (num_peers[1][i] == 0)
						{
							tcp_channel[i].throttle(0);
						}
						else
						{
							if (num_peers[0][i] == 0) num_peers[0][i] = 1;
							int total_peers = num_peers[0][i] + num_peers[1][i];
							// this are 64 bits since it's multiplied by the number
							// of peers, which otherwise might overflow an int
							boost::uint64_t rate = (std::max)(stat_rate[i], lower_limit[i]);
							tcp_channel[i].throttle(int(rate * num_peers[0][i] / total_peers));
						}
					}
				}
				break;
		}

		// --------------------------------------------------------------
		// auto managed torrent
		// --------------------------------------------------------------
		if (!m_paused) m_auto_manage_time_scaler--;
		if (m_auto_manage_time_scaler < 0)
		{
			INVARIANT_CHECK;
			m_auto_manage_time_scaler = settings().get_int(settings_pack::auto_manage_interval);
			recalculate_auto_managed_torrents();
		}

		// --------------------------------------------------------------
		// check for incoming connections that might have timed out
		// --------------------------------------------------------------

		for (connection_map::iterator i = m_connections.begin();
			i != m_connections.end();)
		{
			peer_connection* p = (*i).get();
			++i;
			// ignore connections that already have a torrent, since they
			// are ticked through the torrents' second_tick
			if (!p->associated_torrent().expired()) continue;
			if (m_last_tick - p->connected_time()
				> seconds(m_settings.get_int(settings_pack::handshake_timeout)))
				p->disconnect(errors::timed_out);
		}

		// --------------------------------------------------------------
		// second_tick every torrent (that wants it)
		// --------------------------------------------------------------

		std::vector<torrent*>& want_tick = m_torrent_lists[torrent_want_tick];
		for (int i = 0; i < int(want_tick.size()); ++i)
		{
			torrent& t = *want_tick[i];
			TORRENT_ASSERT(t.want_tick());
			TORRENT_ASSERT(!t.is_aborted());

			size_t size = want_tick.size();
			t.second_tick(tick_interval_ms);

			// if the call to second_tick caused the torrent
			// to no longer want to be ticked (i.e. it was
			// removed from the list) we need to back up the counter
			// to not miss the torrent after it
			if (!t.want_tick()) --i;
		}

#ifndef TORRENT_DISABLE_DHT
		if (m_dht)
		{
			int dht_down;
			int dht_up;
			m_dht->network_stats(dht_up, dht_down);
			m_stat.sent_dht_bytes(dht_up);
			m_stat.received_dht_bytes(dht_down);
		}
#endif

		// #error this should apply to all bandwidth channels
		if (m_settings.get_bool(settings_pack::rate_limit_ip_overhead))
		{
			peer_class* gpc = m_classes.at(m_global_class);

			gpc->channel[peer_connection::download_channel].use_quota(
#ifndef TORRENT_DISABLE_DHT
				m_stat.download_dht() +
#endif
				m_stat.download_tracker());

			gpc->channel[peer_connection::upload_channel].use_quota(
#ifndef TORRENT_DISABLE_DHT
				m_stat.upload_dht() +
#endif
				m_stat.upload_tracker());

			int up_limit = upload_rate_limit(m_global_class);
			int down_limit = download_rate_limit(m_global_class);

			if (down_limit > 0
				&& m_stat.download_ip_overhead() >= down_limit
				&& m_alerts.should_post<performance_alert>())
			{
				m_alerts.post_alert(performance_alert(torrent_handle()
					, performance_alert::download_limit_too_low));
			}

			if (up_limit > 0
				&& m_stat.upload_ip_overhead() >= up_limit
				&& m_alerts.should_post<performance_alert>())
			{
				m_alerts.post_alert(performance_alert(torrent_handle()
					, performance_alert::upload_limit_too_low));
			}
		}

		m_peak_up_rate = (std::max)(m_stat.upload_rate(), m_peak_up_rate);
		m_peak_down_rate = (std::max)(m_stat.download_rate(), m_peak_down_rate);
	
		m_stat.second_tick(tick_interval_ms);

#ifdef TORRENT_STATS
		if (m_stats_logging_enabled)
		{
			print_log_line(tick_interval_ms, now);
		}
#endif

		// --------------------------------------------------------------
		// scrape paused torrents that are auto managed
		// (unless the session is paused)
		// --------------------------------------------------------------
		if (!is_paused())
		{
			INVARIANT_CHECK;
			--m_auto_scrape_time_scaler;
			if (m_auto_scrape_time_scaler <= 0)
			{
				std::vector<torrent*>& want_scrape = m_torrent_lists[torrent_want_scrape];
				m_auto_scrape_time_scaler = m_settings.get_int(settings_pack::auto_scrape_interval)
					/ (std::max)(1, int(want_scrape.size()));
				if (m_auto_scrape_time_scaler < m_settings.get_int(settings_pack::auto_scrape_min_interval))
					m_auto_scrape_time_scaler = m_settings.get_int(settings_pack::auto_scrape_min_interval);

				if (!want_scrape.empty() && !m_abort)
				{
					if (m_next_scrape_torrent >= int(want_scrape.size()))
						m_next_scrape_torrent = 0;

					torrent& t = *want_scrape[m_next_scrape_torrent];
					TORRENT_ASSERT(t.is_paused() && t.is_auto_managed());

					t.scrape_tracker();

					++m_next_scrape_torrent;
					if (m_next_scrape_torrent >= int(want_scrape.size()))
						m_next_scrape_torrent = 0;

				}
			}
		}

		// --------------------------------------------------------------
		// refresh torrent suggestions
		// --------------------------------------------------------------
		--m_suggest_timer;
		if (m_settings.get_int(settings_pack::suggest_mode) != settings_pack::no_piece_suggestions
			&& m_suggest_timer <= 0)
		{
			INVARIANT_CHECK;
			m_suggest_timer = 10;

			torrent_map::iterator least_recently_refreshed = m_torrents.begin();
			if (m_next_suggest_torrent >= int(m_torrents.size()))
				m_next_suggest_torrent = 0;

			std::advance(least_recently_refreshed, m_next_suggest_torrent);

			if (least_recently_refreshed != m_torrents.end())
				least_recently_refreshed->second->refresh_suggest_pieces();
			++m_next_suggest_torrent;
		}

		// --------------------------------------------------------------
		// refresh explicit disk read cache
		// --------------------------------------------------------------
		--m_cache_rotation_timer;
		if (m_settings.get_bool(settings_pack::explicit_read_cache)
			&& m_cache_rotation_timer <= 0)
		{
			INVARIANT_CHECK;
			m_cache_rotation_timer = m_settings.get_int(settings_pack::explicit_cache_interval);

			torrent_map::iterator least_recently_refreshed = m_torrents.begin();
			if (m_next_explicit_cache_torrent >= int(m_torrents.size()))
				m_next_explicit_cache_torrent = 0;

			std::advance(least_recently_refreshed, m_next_explicit_cache_torrent);

			// how many blocks does this torrent get?
			int cache_size = (std::max)(0, m_settings.get_int(settings_pack::cache_size) * 9 / 10);

			if (m_connections.empty())
			{
				// if we don't have any connections at all, split the
				// cache evenly across all torrents
				cache_size = cache_size / (std::max)(int(m_torrents.size()), 1);
			}
			else
			{
				cache_size = cache_size * least_recently_refreshed->second->num_peers()
					/ m_connections.size();
			}

			if (least_recently_refreshed != m_torrents.end())
				least_recently_refreshed->second->refresh_explicit_cache(cache_size);
			++m_next_explicit_cache_torrent;
		}

		// --------------------------------------------------------------
		// connect new peers
		// --------------------------------------------------------------

		try_connect_more_peers();

		// --------------------------------------------------------------
		// unchoke set calculations
		// --------------------------------------------------------------
		m_unchoke_time_scaler--;
		if (m_unchoke_time_scaler <= 0 && !m_connections.empty())
		{
			m_unchoke_time_scaler = settings().get_int(settings_pack::unchoke_interval);
			recalculate_unchoke_slots();
		}

		// --------------------------------------------------------------
		// optimistic unchoke calculation
		// --------------------------------------------------------------
		m_optimistic_unchoke_time_scaler--;
		if (m_optimistic_unchoke_time_scaler <= 0)
		{
			m_optimistic_unchoke_time_scaler
				= settings().get_int(settings_pack::optimistic_unchoke_interval);
			recalculate_optimistic_unchoke_slots();
		}

		// --------------------------------------------------------------
		// disconnect peers when we have too many
		// --------------------------------------------------------------
		--m_disconnect_time_scaler;
		if (m_disconnect_time_scaler <= 0)
		{
			m_disconnect_time_scaler = m_settings.get_int(settings_pack::peer_turnover_interval);

			if (num_connections() >= m_settings.get_int(settings_pack::connections_limit)
				* m_settings.get_int(settings_pack::peer_turnover_cutoff) / 100
				&& !m_torrents.empty())
			{
				// every 90 seconds, disconnect the worst peers
				// if we have reached the connection limit
				torrent_map::iterator i = std::max_element(m_torrents.begin(), m_torrents.end()
					, boost::bind(&torrent::num_peers, boost::bind(&torrent_map::value_type::second, _1))
					< boost::bind(&torrent::num_peers, boost::bind(&torrent_map::value_type::second, _2)));
			
				TORRENT_ASSERT(i != m_torrents.end());
				int peers_to_disconnect = (std::min)((std::max)(
					int(i->second->num_peers() * m_settings.get_int(settings_pack::peer_turnover) / 100), 1)
					, i->second->get_policy().num_connect_candidates());
				i->second->disconnect_peers(peers_to_disconnect
					, error_code(errors::optimistic_disconnect, get_libtorrent_category()));
			}
			else
			{
				// if we haven't reached the global max. see if any torrent
				// has reached its local limit
				for (torrent_map::iterator i = m_torrents.begin()
					, end(m_torrents.end()); i != end; ++i)
				{
					boost::shared_ptr<torrent> t = i->second;
					if (t->num_peers() < t->max_connections() * m_settings.get_int(settings_pack::peer_turnover_cutoff) / 100)
						continue;

					int peers_to_disconnect = (std::min)((std::max)(int(i->second->num_peers()
						* m_settings.get_int(settings_pack::peer_turnover) / 100), 1)
						, i->second->get_policy().num_connect_candidates());
					t->disconnect_peers(peers_to_disconnect
						, error_code(errors::optimistic_disconnect, get_libtorrent_category()));
				}
			}
		}

		while (m_tick_residual >= 1000) m_tick_residual -= 1000;
//		m_peer_pool.release_memory();
	}

#ifdef TORRENT_STATS

	void session_impl::enable_stats_logging(bool s)
	{
		if (m_stats_logging_enabled == s) return;

		m_stats_logging_enabled = s;

		reset_stat_counters();
		if (!s)
		{
			if (m_stats_logger) fclose(m_stats_logger);
			m_stats_logger = 0;
		}
		else
		{
			rotate_stats_log();
			get_thread_cpu_usage(&m_network_thread_cpu_usage);
		}
	}

	void session_impl::received_buffer(int s)
	{
		int size = 8;
		int index = 0;
		while (s > size) { size <<= 1; ++index; }
		int num_max = sizeof(m_recv_buffer_sizes)/sizeof(m_recv_buffer_sizes[0]);
		if (index >= num_max) index = num_max - 1;
		++m_recv_buffer_sizes[index];
	}

	void session_impl::sent_buffer(int s)
	{
		int size = 8;
		int index = 0;
		while (s > size + 13) { size <<= 1; ++index; }
		int num_max = sizeof(m_send_buffer_sizes)/sizeof(m_send_buffer_sizes[0]);
		if (index >= num_max) index = num_max - 1;
		++m_send_buffer_sizes[index];
	}

	void session_impl::reset_stat_counters()
	{
		memset(m_stats_counter, 0, sizeof(m_stats_counter));
		memset(m_send_buffer_sizes, 0, sizeof(m_send_buffer_sizes));
		memset(m_recv_buffer_sizes, 0, sizeof(m_recv_buffer_sizes));
	}

	void session_impl::print_log_line(int tick_interval_ms, ptime now)
	{
		int connect_candidates = 0;

		int checking_torrents = 0;
		int stopped_torrents = 0;
		int upload_only_torrents = 0;
		int downloading_torrents = 0;
		int seeding_torrents = 0;
		int queued_seed_torrents = 0;
		int queued_download_torrents = 0;
		int error_torrents = 0;

		int num_peers = 0;
		int peer_dl_rate_buckets[7];
		int peer_ul_rate_buckets[7];
		memset(peer_dl_rate_buckets, 0, sizeof(peer_dl_rate_buckets));
		memset(peer_ul_rate_buckets, 0, sizeof(peer_ul_rate_buckets));
		int outstanding_requests = 0;
		int outstanding_end_game_requests = 0;
		int outstanding_write_blocks = 0;

		int peers_up_interested = 0;
		int peers_down_interesting = 0;
		int peers_up_requests = 0;
		int peers_down_requests = 0;
		int peers_up_send_buffer = 0;

		int partial_pieces = 0;
		int partial_downloading_pieces = 0;
		int partial_full_pieces = 0;
		int partial_finished_pieces = 0;

		// number of torrents that want more peers
		int num_want_more_peers = int(m_torrent_lists[torrent_want_peers_download].size()
			+ m_torrent_lists[torrent_want_peers_finished].size());

		// number of peers among torrents with a peer limit
		int num_limited_peers = 0;
		// sum of limits of all torrents with a peer limit
		int total_peers_limit = 0;

		int total_pieces = 0;
		int total_have_pieces = 0;
		int total_passed_pieces = 0;

		std::vector<partial_piece_info> dq;
		for (torrent_map::iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			torrent* t = i->second.get();

			int connection_slots = (std::max)(t->max_connections() - t->num_peers(), 0);
			int candidates = t->get_policy().num_connect_candidates();
			connect_candidates += (std::min)(candidates, connection_slots);
			num_peers += t->get_policy().num_peers();

			if (t->max_connections() > 0)
			{
				num_limited_peers += t->num_peers();
				num_limited_peers += t->max_connections();
			}

			if (t->has_error())
				++error_torrents;
			else
			{
				if (t->is_paused())
				{
					if (!t->is_auto_managed())
						++stopped_torrents;
					else
					{
						if (t->is_seed())
							++queued_seed_torrents;
						else
							++queued_download_torrents;
					}
				}
				else
				{
					if (i->second->state() == torrent_status::checking_files
						|| i->second->state() == torrent_status::queued_for_checking)
						++checking_torrents;
					else if (i->second->is_seed())
						++seeding_torrents;
					else if (i->second->is_upload_only())
						++upload_only_torrents;
					else
						++downloading_torrents;
				}
			}

			if (t->has_picker())
			{
				piece_picker& p = t->picker();
				partial_pieces += p.get_download_queue_size();
				int a, b, c;
				p.get_download_queue_sizes(&a, &b, &c);
				partial_downloading_pieces += a;
				partial_full_pieces += b;
				partial_finished_pieces += c;
			}

			if (t->valid_metadata())
			{
				total_pieces += t->torrent_file().num_pieces();
				total_have_pieces += t->num_have();
				total_passed_pieces += t->num_passed();
			}

			dq.clear();
			t->get_download_queue(&dq);
			for (std::vector<partial_piece_info>::iterator j = dq.begin()
				, end(dq.end()); j != end; ++j)
			{
				for (int k = 0; k < j->blocks_in_piece; ++k)
				{
					block_info& bi = j->blocks[k];
					if (bi.state == block_info::requested)
					{
						++outstanding_requests;
						if (bi.num_peers > 1) ++outstanding_end_game_requests;
					}
					else if (bi.state == block_info::writing)
						++outstanding_write_blocks;
				}
			}
		}
		int tcp_up_rate = 0;
		int tcp_down_rate = 0;
		int utp_up_rate = 0;
		int utp_down_rate = 0;
		int utp_peak_send_delay = 0;
		int utp_peak_recv_delay = 0;
		boost::uint64_t utp_send_delay_sum = 0;
		boost::uint64_t utp_recv_delay_sum = 0;
		int num_utp_peers = 0;
		int num_tcp_peers = 0;
		int utp_num_delay_sockets = 0;
		int utp_num_recv_delay_sockets = 0;
		int num_complete_connections = 0;
		int num_half_open = 0;
		int peers_down_unchoked = 0;
		int peers_up_unchoked = 0;
		int num_end_game_peers = 0;
		int reading_bytes = 0;
		int pending_incoming_reqs = 0;

		for (connection_map::iterator i = m_connections.begin()
			, end(m_connections.end()); i != end; ++i)
		{
			peer_connection* p = i->get();
			if (p->is_connecting())
			{
				++num_half_open;
				continue;
			}

			++num_complete_connections;
			if (!p->is_choked()) ++peers_up_unchoked;
			if (!p->has_peer_choked()) ++peers_down_unchoked;
			if (!p->download_queue().empty()) ++peers_down_requests;
			if (p->is_peer_interested()) ++peers_up_interested;
			if (p->is_interesting()) ++peers_down_interesting;
			if (p->send_buffer_size() > 100 || !p->upload_queue().empty() || p->num_reading_bytes() > 0)
				++peers_up_requests;
			if (p->endgame()) ++num_end_game_peers;
			reading_bytes += p->num_reading_bytes();
		
			pending_incoming_reqs += int(p->upload_queue().size());

			int dl_bucket = 0;
			int dl_rate = p->statistics().download_payload_rate();
			if (dl_rate == 0) dl_bucket = 0;
			else if (dl_rate < 2000) dl_bucket = 1;
			else if (dl_rate < 5000) dl_bucket = 2;
			else if (dl_rate < 10000) dl_bucket = 3;
			else if (dl_rate < 50000) dl_bucket = 4;
			else if (dl_rate < 100000) dl_bucket = 5;
			else dl_bucket = 6;

			int ul_rate = p->statistics().upload_payload_rate();
			int ul_bucket = 0;
			if (ul_rate == 0) ul_bucket = 0;
			else if (ul_rate < 2000) ul_bucket = 1;
			else if (ul_rate < 5000) ul_bucket = 2;
			else if (ul_rate < 10000) ul_bucket = 3;
			else if (ul_rate < 50000) ul_bucket = 4;
			else if (ul_rate < 100000) ul_bucket = 5;
			else ul_bucket = 6;

			++peer_dl_rate_buckets[dl_bucket];
			++peer_ul_rate_buckets[ul_bucket];

			boost::uint64_t upload_rate = int(p->statistics().upload_rate());
			int buffer_size_watermark = upload_rate
				* m_settings.get_int(settings_pack::send_buffer_watermark_factor) / 100;
			if (buffer_size_watermark < m_settings.get_int(settings_pack::send_buffer_low_watermark))
				buffer_size_watermark = m_settings.get_int(settings_pack::send_buffer_low_watermark);
			else if (buffer_size_watermark > m_settings.get_int(settings_pack::send_buffer_watermark))
				buffer_size_watermark = m_settings.get_int(settings_pack::send_buffer_watermark);
			if (p->send_buffer_size() + p->num_reading_bytes() >= buffer_size_watermark)
				++peers_up_send_buffer;

			utp_stream* utp_socket = p->get_socket()->get<utp_stream>();
			if (utp_socket)
			{
				utp_up_rate += ul_rate;
				utp_down_rate += dl_rate;
				int send_delay = utp_socket->send_delay();
				int recv_delay = utp_socket->recv_delay();
				utp_peak_send_delay = (std::max)(utp_peak_send_delay, send_delay);
				utp_peak_recv_delay = (std::max)(utp_peak_recv_delay, recv_delay);
				if (send_delay > 0)
				{
					utp_send_delay_sum += send_delay;
					++utp_num_delay_sockets;
				}
				if (recv_delay > 0)
				{
					utp_recv_delay_sum += recv_delay;
					++utp_num_recv_delay_sockets;
				}
				++num_utp_peers;
			}
			else
			{
				tcp_up_rate += ul_rate;
				tcp_down_rate += dl_rate;
				++num_tcp_peers;
			}

		}

		if (now - m_last_log_rotation > hours(1))
			rotate_stats_log();

		// system memory stats
		error_code vm_ec;
		vm_statistics_data_t vm_stat;
		get_vm_stats(&vm_stat, vm_ec);
		thread_cpu_usage cur_cpu_usage;
		get_thread_cpu_usage(&cur_cpu_usage);

		if (m_stats_logger)
		{
			cache_status cs;
			m_disk_thread.get_cache_info(&cs);
			session_status sst = status();

			m_read_ops.add_sample((cs.reads - m_last_cache_status.reads) * 1000.0 / float(tick_interval_ms));
			m_write_ops.add_sample((cs.writes - m_last_cache_status.writes) * 1000.0 / float(tick_interval_ms));

			int total_job_time = cs.cumulative_job_time == 0 ? 1 : cs.cumulative_job_time;

#define STAT_LOG(type, val) fprintf(m_stats_logger, "%" #type "\t", val)

			STAT_LOG(f, total_milliseconds(now - m_last_log_rotation) / 1000.f);
			size_type uploaded = m_stat.total_upload() - m_last_uploaded;
			STAT_LOG(d, int(uploaded));
			size_type downloaded = m_stat.total_download() - m_last_downloaded;
			STAT_LOG(d, int(downloaded));
			STAT_LOG(d, downloading_torrents);
			STAT_LOG(d, seeding_torrents);
			STAT_LOG(d, num_complete_connections);
			STAT_LOG(d, num_half_open);
			STAT_LOG(d, cs.total_used_buffers);
			STAT_LOG(d, num_peers);
			STAT_LOG(d, logging_allocator::allocations);
			STAT_LOG(d, logging_allocator::allocated_bytes);
			STAT_LOG(d, checking_torrents);
			STAT_LOG(d, stopped_torrents);
			STAT_LOG(d, upload_only_torrents);
			STAT_LOG(d, queued_seed_torrents);
			STAT_LOG(d, queued_download_torrents);
			STAT_LOG(d, m_upload_rate.queue_size());
			STAT_LOG(d, m_download_rate.queue_size());
			STAT_LOG(d, m_disk_queues[peer_connection::upload_channel]);
			STAT_LOG(d, m_disk_queues[peer_connection::download_channel]);
			STAT_LOG(d, m_stat.upload_rate());
			STAT_LOG(d, m_stat.download_rate());
			STAT_LOG(d, int(m_writing_bytes));
			STAT_LOG(d, peer_dl_rate_buckets[0]);
			STAT_LOG(d, peer_dl_rate_buckets[1]);
			STAT_LOG(d, peer_dl_rate_buckets[2]);
			STAT_LOG(d, peer_dl_rate_buckets[3]);
			STAT_LOG(d, peer_dl_rate_buckets[4]);
			STAT_LOG(d, peer_dl_rate_buckets[5]);
			STAT_LOG(d, peer_dl_rate_buckets[6]);
			STAT_LOG(d, peer_ul_rate_buckets[0]);
			STAT_LOG(d, peer_ul_rate_buckets[1]);
			STAT_LOG(d, peer_ul_rate_buckets[2]);
			STAT_LOG(d, peer_ul_rate_buckets[3]);
			STAT_LOG(d, peer_ul_rate_buckets[4]);
			STAT_LOG(d, peer_ul_rate_buckets[5]);
			STAT_LOG(d, peer_ul_rate_buckets[6]);
			STAT_LOG(d, m_stats_counter[session_interface::error_peers]);
			STAT_LOG(d, peers_down_interesting);
			STAT_LOG(d, peers_down_unchoked);
			STAT_LOG(d, peers_down_requests);
			STAT_LOG(d, peers_up_interested);
			STAT_LOG(d, peers_up_unchoked);
			STAT_LOG(d, peers_up_requests);
			STAT_LOG(d, m_stats_counter[session_interface::disconnected_peers]);
			STAT_LOG(d, m_stats_counter[session_interface::eof_peers]);
			STAT_LOG(d, m_stats_counter[session_interface::connreset_peers]);
			STAT_LOG(d, outstanding_requests);
			STAT_LOG(d, outstanding_end_game_requests);
			STAT_LOG(d, outstanding_write_blocks);
			STAT_LOG(d, m_stats_counter[end_game_piece_picker_blocks]);
			STAT_LOG(d, m_stats_counter[piece_picker_blocks]);
			STAT_LOG(d, m_stats_counter[piece_picker_loops]);
			STAT_LOG(d, m_stats_counter[piece_picks]);
			STAT_LOG(d, m_stats_counter[reject_piece_picks]);
			STAT_LOG(d, m_stats_counter[unchoke_piece_picks]);
			STAT_LOG(d, m_stats_counter[incoming_redundant_piece_picks]);
			STAT_LOG(d, m_stats_counter[incoming_piece_picks]);
			STAT_LOG(d, m_stats_counter[end_game_piece_picks]);
			STAT_LOG(d, m_stats_counter[snubbed_piece_picks]);
			STAT_LOG(d, m_stats_counter[connect_timeouts]);
			STAT_LOG(d, m_stats_counter[uninteresting_peers]);
			STAT_LOG(d, m_stats_counter[timeout_peers]);
			STAT_LOG(f, (float(m_total_failed_bytes) * 100.f / (m_stat.total_payload_download() == 0 ? 1 : m_stat.total_payload_download())));
			STAT_LOG(f, (float(m_total_redundant_bytes) * 100.f / (m_stat.total_payload_download() == 0 ? 1 : m_stat.total_payload_download())));
			STAT_LOG(f, (float(m_stat.total_protocol_download()) * 100.f / (m_stat.total_download() == 0 ? 1 : m_stat.total_download())));
			STAT_LOG(f, float(cs.average_read_time) / 1000000.f);
			STAT_LOG(f, float(cs.average_write_time) / 1000000.f);
			STAT_LOG(d, int(cs.pending_jobs + cs.queued_jobs));
			STAT_LOG(d, int(cs.queued_bytes));
			STAT_LOG(d, int(cs.blocks_read_hit - m_last_cache_status.blocks_read_hit));
			STAT_LOG(d, int(cs.blocks_read - m_last_cache_status.blocks_read));
			STAT_LOG(d, int(cs.blocks_written - m_last_cache_status.blocks_written));
			STAT_LOG(d, int(m_total_failed_bytes - m_last_failed));
			STAT_LOG(d, int(m_total_redundant_bytes - m_last_redundant));
			STAT_LOG(d, error_torrents);
			STAT_LOG(d, cs.read_cache_size);
			STAT_LOG(d, cs.write_cache_size + cs.read_cache_size);
			STAT_LOG(d, cs.total_used_buffers);
			STAT_LOG(f, float(cs.average_hash_time) / 1000000.f);
			STAT_LOG(d, m_stats_counter[connection_attempts]);
			STAT_LOG(d, m_stats_counter[num_banned_peers]);
			STAT_LOG(d, m_stats_counter[banned_for_hash_failure]);
			STAT_LOG(d, m_settings.get_int(settings_pack::cache_size));
			STAT_LOG(d, m_settings.get_int(settings_pack::connections_limit));
			STAT_LOG(d, connect_candidates);
			STAT_LOG(d, int(m_settings.get_int(settings_pack::cache_size)
				- m_settings.get_int(settings_pack::max_queued_disk_bytes) / 0x4000));
			STAT_LOG(f, float(cs.cumulative_read_time * 100.f / total_job_time));
			STAT_LOG(f, float(cs.cumulative_write_time * 100.f / total_job_time));
			STAT_LOG(f, float(cs.cumulative_hash_time * 100.f / total_job_time));
			STAT_LOG(d, int(cs.total_read_back - m_last_cache_status.total_read_back));
			STAT_LOG(f, float(cs.total_read_back * 100.f / (cs.blocks_written == 0 ? 1: cs.blocks_written)));
			STAT_LOG(d, cs.read_queue_size);
			STAT_LOG(f, float(tick_interval_ms) / 1000.f);
			STAT_LOG(f, float(m_tick_residual) / 1000.f);
			STAT_LOG(d, m_allowed_upload_slots);
			STAT_LOG(d, m_stat.low_pass_upload_rate());
			STAT_LOG(d, m_stat.low_pass_download_rate());
			STAT_LOG(d, num_end_game_peers);
			STAT_LOG(d, tcp_up_rate);
			STAT_LOG(d, tcp_down_rate);
			STAT_LOG(d, int(rate_limit(m_tcp_peer_class, peer_connection::upload_channel)));
			STAT_LOG(d, int(rate_limit(m_tcp_peer_class, peer_connection::download_channel)));
			STAT_LOG(d, utp_up_rate);
			STAT_LOG(d, utp_down_rate);
			STAT_LOG(f, float(utp_peak_send_delay) / 1000000.f);
			STAT_LOG(f, float(utp_num_delay_sockets ? float(utp_send_delay_sum) / float(utp_num_delay_sockets) : 0) / 1000000.f);
			STAT_LOG(f, float(utp_peak_recv_delay) / 1000000.f);
			STAT_LOG(f, float(utp_num_recv_delay_sockets ? float(utp_recv_delay_sum) / float(utp_num_recv_delay_sockets) : 0) / 1000000.f);
			STAT_LOG(f, float(cs.reads - m_last_cache_status.reads) * 1000.0 / float(tick_interval_ms));
			STAT_LOG(f, float(cs.writes - m_last_cache_status.writes) * 1000.0 / float(tick_interval_ms));

			STAT_LOG(d, int(vm_stat.active_count));
			STAT_LOG(d, int(vm_stat.inactive_count));
			STAT_LOG(d, int(vm_stat.wire_count));
			STAT_LOG(d, int(vm_stat.free_count));
			STAT_LOG(d, int(vm_stat.pageins - m_last_vm_stat.pageins));
			STAT_LOG(d, int(vm_stat.pageouts - m_last_vm_stat.pageouts));
			STAT_LOG(d, int(vm_stat.faults - m_last_vm_stat.faults));

			STAT_LOG(d, m_read_ops.mean());
			STAT_LOG(d, m_write_ops.mean());
			STAT_LOG(d, cs.pinned_blocks);

			STAT_LOG(d, partial_pieces);
			STAT_LOG(d, partial_downloading_pieces);
			STAT_LOG(d, partial_full_pieces);
			STAT_LOG(d, partial_finished_pieces);

			STAT_LOG(d, cs.num_jobs);
			STAT_LOG(d, cs.num_read_jobs);
			STAT_LOG(d, cs.num_write_jobs);

			STAT_LOG(d, reading_bytes);

			for (int i = on_read_counter; i <= on_disk_counter; ++i)
				STAT_LOG(d, m_stats_counter[i]);

			int num_max = sizeof(m_send_buffer_sizes)/sizeof(m_send_buffer_sizes[0]);
			for (int i = 0; i < num_max; ++i)
				STAT_LOG(d, m_send_buffer_sizes[i]);
			num_max = sizeof(m_recv_buffer_sizes)/sizeof(m_recv_buffer_sizes[0]);
			for (int i = 0; i < num_max; ++i)
				STAT_LOG(d, m_recv_buffer_sizes[i]);

			STAT_LOG(f, total_microseconds(cur_cpu_usage.user_time
				- m_network_thread_cpu_usage.user_time) / double(tick_interval_ms * 10));
			STAT_LOG(f, (total_microseconds(cur_cpu_usage.system_time
					- m_network_thread_cpu_usage.system_time)
				+ total_microseconds(cur_cpu_usage.user_time
					- m_network_thread_cpu_usage.user_time))
				/ double(tick_interval_ms * 10));

			for (int i = 0; i < torrent::waste_reason_max; ++i)
				STAT_LOG(f, (m_redundant_bytes[i] * 100.) / double(m_total_redundant_bytes == 0 ? 1 : m_total_redundant_bytes));

			STAT_LOG(d, m_stats_counter[no_memory_peers]);
			STAT_LOG(d, m_stats_counter[too_many_peers]);
			STAT_LOG(d, m_stats_counter[transport_timeout_peers]);

			STAT_LOG(d, cs.arc_write_size);
			STAT_LOG(d, cs.arc_volatile_size);
			STAT_LOG(d, cs.arc_volatile_size + cs.arc_mru_size);
			STAT_LOG(d, cs.arc_volatile_size + cs.arc_mru_size + cs.arc_mru_ghost_size);
			STAT_LOG(d, -cs.arc_mfu_size);
			STAT_LOG(d, -cs.arc_mfu_size - cs.arc_mfu_ghost_size);

			STAT_LOG(d, sst.utp_stats.num_idle);
			STAT_LOG(d, sst.utp_stats.num_syn_sent);
			STAT_LOG(d, sst.utp_stats.num_connected);
			STAT_LOG(d, sst.utp_stats.num_fin_sent);
			STAT_LOG(d, sst.utp_stats.num_close_wait);

			STAT_LOG(d, num_tcp_peers);
			STAT_LOG(d, num_utp_peers);

			STAT_LOG(d, m_stats_counter[connrefused_peers]);
			STAT_LOG(d, m_stats_counter[connaborted_peers]);
			STAT_LOG(d, m_stats_counter[perm_peers]);
			STAT_LOG(d, m_stats_counter[buffer_peers]);
			STAT_LOG(d, m_stats_counter[unreachable_peers]);
			STAT_LOG(d, m_stats_counter[broken_pipe_peers]);
			STAT_LOG(d, m_stats_counter[addrinuse_peers]);
			STAT_LOG(d, m_stats_counter[no_access_peers]);
			STAT_LOG(d, m_stats_counter[invalid_arg_peers]);
			STAT_LOG(d, m_stats_counter[aborted_peers]);

			STAT_LOG(d, m_stats_counter[error_incoming_peers]);
			STAT_LOG(d, m_stats_counter[error_outgoing_peers]);
			STAT_LOG(d, m_stats_counter[error_rc4_peers]);
			STAT_LOG(d, m_stats_counter[error_encrypted_peers]);
			STAT_LOG(d, m_stats_counter[error_tcp_peers]);
			STAT_LOG(d, m_stats_counter[error_utp_peers]);

			STAT_LOG(d, int(m_connections.size()));
			STAT_LOG(d, pending_incoming_reqs);
			STAT_LOG(f, num_complete_connections == 0 ? 0.f : (float(pending_incoming_reqs) / num_complete_connections));

			STAT_LOG(d, num_want_more_peers);
			STAT_LOG(f, total_peers_limit == 0 ? 0 : float(num_limited_peers) / total_peers_limit);

			STAT_LOG(d, m_stats_counter[piece_requests]);
			STAT_LOG(d, m_stats_counter[max_piece_requests]);
			STAT_LOG(d, m_stats_counter[invalid_piece_requests]);
			STAT_LOG(d, m_stats_counter[choked_piece_requests]);
			STAT_LOG(d, m_stats_counter[cancelled_piece_requests]);
			STAT_LOG(d, m_stats_counter[piece_rejects]);

			STAT_LOG(d, total_pieces);
			STAT_LOG(d, total_have_pieces);
			STAT_LOG(d, total_passed_pieces);

			STAT_LOG(d, peers_up_send_buffer);

			// loaded torrents
			STAT_LOG(d, m_torrent_lru.size());
			STAT_LOG(d, m_stats_counter[torrent_evicted_counter]);

			fprintf(m_stats_logger, "\n");

#undef STAT_LOG

			m_last_cache_status = cs;
			if (!vm_ec) m_last_vm_stat = vm_stat;
			m_network_thread_cpu_usage = cur_cpu_usage;
			m_last_failed = m_total_failed_bytes;
			m_last_redundant = m_total_redundant_bytes;
			m_last_uploaded = m_stat.total_upload();
			m_last_downloaded = m_stat.total_download();
		}

		reset_stat_counters();
	}
#endif // TORRENT_STATS

	void session_impl::update_rss_feeds()
	{
		time_t now_posix = time(0);
		ptime min_update = max_time();
		ptime now = time_now();
		for (std::vector<boost::shared_ptr<feed> >::iterator i
			= m_feeds.begin(), end(m_feeds.end()); i != end; ++i)
		{
			feed& f = **i;
			int delta = f.next_update(now_posix);
			if (delta <= 0)
				delta = f.update_feed();
			TORRENT_ASSERT(delta >= 0);
			ptime next_update = now + seconds(delta);
			if (next_update < min_update) min_update = next_update;
		}
		m_next_rss_update = min_update;
	}

#ifndef TORRENT_DISABLE_DHT

	void session_impl::on_dht_announce(error_code const& e)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("session_impl::on_dht_announce");
#endif
		TORRENT_ASSERT(is_single_thread());
		if (e) return;

		if (m_abort) return;

#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_dht_announce");
#endif
		// announce to DHT every 15 minutes
		int delay = (std::max)(m_settings.get_int(settings_pack::dht_announce_interval)
			/ (std::max)(int(m_torrents.size()), 1), 1);
		error_code ec;
		m_dht_announce_timer.expires_from_now(seconds(delay), ec);
		m_dht_announce_timer.async_wait(
			bind(&session_impl::on_dht_announce, this, _1));

		if (m_torrents.empty()) return;

		if (m_next_dht_torrent == m_torrents.end())
			m_next_dht_torrent = m_torrents.begin();
		m_next_dht_torrent->second->dht_announce();
		++m_next_dht_torrent;
		if (m_next_dht_torrent == m_torrents.end())
			m_next_dht_torrent = m_torrents.begin();
  	}
#endif

	void session_impl::on_lsd_announce(error_code const& e)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("session_impl::on_lsd_announce");
#endif
#ifdef TORRENT_STATS
		inc_stats_counter(on_lsd_counter);
#endif
		TORRENT_ASSERT(is_single_thread());
		if (e) return;

		if (m_abort) return;

#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_lsd_announce");
#endif
		// announce on local network every 5 minutes
		int delay = (std::max)(m_settings.get_int(settings_pack::local_service_announce_interval)
			/ (std::max)(int(m_torrents.size()), 1), 1);
		error_code ec;
		m_lsd_announce_timer.expires_from_now(seconds(delay), ec);
		m_lsd_announce_timer.async_wait(
			bind(&session_impl::on_lsd_announce, this, _1));

		if (m_torrents.empty()) return;

		if (m_next_lsd_torrent == m_torrents.end())
			m_next_lsd_torrent = m_torrents.begin();
		m_next_lsd_torrent->second->lsd_announce();
		++m_next_lsd_torrent;
		if (m_next_lsd_torrent == m_torrents.end())
			m_next_lsd_torrent = m_torrents.begin();
	}

	namespace
	{
		bool is_active(torrent* t, aux::session_settings const& s)
		{
			// if we count slow torrents, every torrent
			// is considered active
			if (!s.get_bool(settings_pack::dont_count_slow_torrents)) return true;
			
			// if the torrent started less than 2 minutes
			// ago (default), let it count as active since
			// the rates are probably not accurate yet
			if (time_now() - t->started()
				< seconds(s.get_int(settings_pack::auto_manage_startup))) return true;

			return t->statistics().upload_payload_rate() != 0.f
				|| t->statistics().download_payload_rate() != 0.f;
		}
	}
	
	void session_impl::auto_manage_torrents(std::vector<torrent*>& list
		, int& checking_limit, int& dht_limit, int& tracker_limit
		, int& lsd_limit, int& hard_limit, int type_limit)
	{
		for (std::vector<torrent*>::iterator i = list.begin()
			, end(list.end()); i != end; ++i)
		{
			torrent* t = *i;

			if (t->state() == torrent_status::checking_files)
			{
				if (checking_limit <= 0) t->pause();
				else
				{
					t->resume();
					t->start_checking();
					--checking_limit;
				}
				continue;
			}

			--dht_limit;
			--lsd_limit;
			--tracker_limit;
			t->set_announce_to_dht(dht_limit >= 0);
			t->set_announce_to_trackers(tracker_limit >= 0);
			t->set_announce_to_lsd(lsd_limit >= 0);

			if (!t->is_paused() && !is_active(t, settings())
				&& hard_limit > 0)
			{
				--hard_limit;
				continue;
			}

			if (type_limit > 0 && hard_limit > 0)
			{
				--hard_limit;
				--type_limit;
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING
				t->log_to_all_peers(("AUTO MANAGER STARTING TORRENT: " + t->torrent_file().name()).c_str());
#endif
				t->set_allow_peers(true);
			}
			else
			{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING
				t->log_to_all_peers(("AUTO MANAGER PAUSING TORRENT: " + t->torrent_file().name()).c_str());
#endif
				// use graceful pause for auto-managed torrents
				t->set_allow_peers(false, true);
			}
		}
	}

	void session_impl::recalculate_auto_managed_torrents()
	{
		INVARIANT_CHECK;

		// these vectors are filled with auto managed torrents

		// TODO: these vectors could be copied from m_torrent_lists,
		// if we would maintain them. That way the first pass over
		// all torrents could be avoided. It would be especially
		// efficient if most torrents are not auto-managed
		// whenever we receive a scrape response (or anything
		// that may change the rank of a torrent) that one torrent
		// could re-sort itself in a list that's kept sorted at all
		// times. That way, this pass over all torrents could be
		// avoided alltogether.
		std::vector<torrent*> checking;
		std::vector<torrent*> downloaders;
		downloaders.reserve(m_torrents.size());
		std::vector<torrent*> seeds;
		seeds.reserve(m_torrents.size());

		// these counters are set to the number of torrents
		// of each kind we're allowed to have active
		int num_downloaders = settings().get_int(settings_pack::active_downloads);
		int num_seeds = settings().get_int(settings_pack::active_seeds);
		int checking_limit = 1;
		int dht_limit = settings().get_int(settings_pack::active_dht_limit);
		int tracker_limit = settings().get_int(settings_pack::active_tracker_limit);
		int lsd_limit = settings().get_int(settings_pack::active_lsd_limit);
		int hard_limit = settings().get_int(settings_pack::active_limit);

		if (num_downloaders == -1)
			num_downloaders = (std::numeric_limits<int>::max)();
		if (num_seeds == -1)
			num_seeds = (std::numeric_limits<int>::max)();
		if (hard_limit == -1)
			hard_limit = (std::numeric_limits<int>::max)();
            
		for (torrent_map::iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			torrent* t = i->second.get();
			TORRENT_ASSERT(t);

			if (t->is_auto_managed() && !t->has_error())
			{
				if (t->state() == torrent_status::checking_files)
				{
					checking.push_back(t);
					continue;
				}

				TORRENT_ASSERT(t->m_resume_data_loaded || !t->valid_metadata());
				// this torrent is auto managed, add it to
				// the list (depending on if it's a seed or not)
				if (t->is_finished())
					seeds.push_back(t);
				else
					downloaders.push_back(t);
			}
			else if (!t->is_paused())
			{
				if (t->state() == torrent_status::checking_files)
				{
					if (checking_limit > 0) --checking_limit;
					continue;
				}
				TORRENT_ASSERT(t->m_resume_data_loaded || !t->valid_metadata());
				--hard_limit;
			  	if (is_active(t, settings()))
				{
					// this is not an auto managed torrent,
					// if it's running and active, decrease the
					// counters.
					if (t->is_finished())
						--num_seeds;
					else
						--num_downloaders;
				}
			}
		}

		bool handled_by_extension = false;

#ifndef TORRENT_DISABLE_EXTENSIONS
		// TODO: allow extensions to sort torrents for queuing
#endif

		if (!handled_by_extension)
		{
			std::sort(downloaders.begin(), downloaders.end()
				, boost::bind(&torrent::sequence_number, _1) < boost::bind(&torrent::sequence_number, _2));

			std::sort(seeds.begin(), seeds.end()
				, boost::bind(&torrent::seed_rank, _1, boost::ref(m_settings))
				> boost::bind(&torrent::seed_rank, _2, boost::ref(m_settings)));
		}

		auto_manage_torrents(checking, checking_limit, dht_limit, tracker_limit, lsd_limit
			, hard_limit, num_downloaders);

		if (settings().get_bool(settings_pack::auto_manage_prefer_seeds))
		{
			auto_manage_torrents(seeds, checking_limit, dht_limit, tracker_limit, lsd_limit
				, hard_limit, num_seeds);
			auto_manage_torrents(downloaders, checking_limit, dht_limit, tracker_limit, lsd_limit
				, hard_limit, num_downloaders);
		}
		else
		{
			auto_manage_torrents(downloaders, checking_limit, dht_limit, tracker_limit, lsd_limit
				, hard_limit, num_downloaders);
			auto_manage_torrents(seeds, checking_limit, dht_limit, tracker_limit, lsd_limit
				, hard_limit, num_seeds);
		}
	}

	void session_impl::recalculate_optimistic_unchoke_slots()
	{
		TORRENT_ASSERT(is_single_thread());
		if (m_allowed_upload_slots == 0) return;
	
		std::vector<policy::peer*> opt_unchoke;

		for (connection_map::iterator i = m_connections.begin()
			, end(m_connections.end()); i != end; ++i)
		{
			peer_connection* p = i->get();
			TORRENT_ASSERT(p);
			policy::peer* pi = p->peer_info_struct();
			if (!pi) continue;
			if (pi->web_seed) continue;
			torrent* t = p->associated_torrent().lock().get();
			if (!t) continue;
			if (t->is_paused()) continue;

			if (pi->optimistically_unchoked)
			{
				TORRENT_ASSERT(!p->is_choked());
				opt_unchoke.push_back(pi);
			}

			if (!p->is_connecting()
				&& !p->is_disconnecting()
				&& p->is_peer_interested()
				&& t->free_upload_slots()
				&& p->is_choked()
				&& !p->ignore_unchoke_slots()
				&& t->valid_metadata())
			{
				opt_unchoke.push_back(pi);
			}
		}

		// find the peers that has been waiting the longest to be optimistically
		// unchoked

		// avoid having a bias towards peers that happen to be sorted first
		std::random_shuffle(opt_unchoke.begin(), opt_unchoke.end());

		// sort all candidates based on when they were last optimistically
		// unchoked.
		std::sort(opt_unchoke.begin(), opt_unchoke.end()
			, boost::bind(&policy::peer::last_optimistically_unchoked, _1)
			< boost::bind(&policy::peer::last_optimistically_unchoked, _2));

		int num_opt_unchoke = m_settings.get_int(settings_pack::num_optimistic_unchoke_slots);
		if (num_opt_unchoke == 0) num_opt_unchoke = (std::max)(1, m_allowed_upload_slots / 5);

		// unchoke the first num_opt_unchoke peers in the candidate set
		// and make sure that the others are choked
		for (std::vector<policy::peer*>::iterator i = opt_unchoke.begin()
			, end(opt_unchoke.end()); i != end; ++i)
		{
			policy::peer* pi = *i;
			if (num_opt_unchoke > 0)
			{
				--num_opt_unchoke;
				if (!pi->optimistically_unchoked)
				{
					torrent* t = pi->connection->associated_torrent().lock().get();
					bool ret = t->unchoke_peer(*pi->connection, true);
					if (ret)
					{
						pi->optimistically_unchoked = true;
						++m_num_unchoked;
						pi->last_optimistically_unchoked = session_time();
					}
					else
					{
						// we failed to unchoke it, increment the count again
						++num_opt_unchoke;
					}
				}
			}
			else
			{
				if (pi->optimistically_unchoked)
				{
					torrent* t = pi->connection->associated_torrent().lock().get();
					pi->optimistically_unchoked = false;
					t->choke_peer(*pi->connection);
					--m_num_unchoked;
				}	
			}
		}
	}

	void session_impl::try_connect_more_peers()
	{
		if (m_abort) return;

		if (num_connections() >= m_settings.get_int(settings_pack::connections_limit))
			return;

		// this is the maximum number of connections we will
		// attempt this tick
		int max_connections = m_settings.get_int(settings_pack::connection_speed);

		// zero connections speeds are allowed, we just won't make any connections
		if (max_connections <= 0) return;

		// this loop will "hand out" max(connection_speed
		// , half_open.free_slots()) to the torrents, in a
		// round robin fashion, so that every torrent is
		// equally likely to connect to a peer

		int free_slots = m_half_open.free_slots();

		// if we don't have any free slots, return
		if (free_slots <= -m_half_open.limit()) return;

		// boost connections are connections made by torrent connection
		// boost, which are done immediately on a tracker response. These
		// connections needs to be deducted from this second
		if (m_boost_connections > 0)
		{
			if (m_boost_connections > max_connections)
			{
				m_boost_connections -= max_connections;
				max_connections = 0;
			}
			else
			{
				max_connections -= m_boost_connections;
				m_boost_connections = 0;
			}
		}

		// this logic is here to smooth out the number of new connection
		// attempts over time, to prevent connecting a large number of
		// sockets, wait 10 seconds, and then try again
		int limit = (std::min)(m_settings.get_int(settings_pack::connections_limit)
			- num_connections(), free_slots);
		if (m_settings.get_bool(settings_pack::smooth_connects) && max_connections > (limit+1) / 2)
			max_connections = (limit+1) / 2;

		std::vector<torrent*>& want_peers_download = m_torrent_lists[torrent_want_peers_download];
		std::vector<torrent*>& want_peers_finished = m_torrent_lists[torrent_want_peers_finished];

		// if no torrent want any peers, just return
		if (want_peers_download.empty() && want_peers_finished.empty()) return;

		// if we don't have any connection attempt quota, return
		if (max_connections <= 0) return;

		INVARIANT_CHECK;

		int steps_since_last_connect = 0;
		int num_torrents = int(want_peers_finished.size() + want_peers_download.size());
		for (;;)
		{
			if (m_next_downloading_connect_torrent >= int(want_peers_download.size()))
				m_next_downloading_connect_torrent = 0;

			if (m_next_finished_connect_torrent >= int(want_peers_finished.size()))
				m_next_finished_connect_torrent = 0;

			torrent* t;
			if ((m_download_connect_attempts >= m_settings.get_int(
					settings_pack::connect_seed_every_n_download)
				&& want_peers_finished.size())
					|| want_peers_download.empty())
			{
				// pick a finished torrent to give a peer to
				t = want_peers_finished[m_next_finished_connect_torrent];
				TORRENT_ASSERT(t->want_peers_finished());
				m_download_connect_attempts = 0;
				++m_next_finished_connect_torrent;
			}
			else
			{
				// pick a downloading torrent to give a peer to
				t = want_peers_download[m_next_downloading_connect_torrent];
				TORRENT_ASSERT(t->want_peers_download());
				++m_download_connect_attempts;
				++m_next_downloading_connect_torrent;
			}

			TORRENT_ASSERT(t->want_peers());
			TORRENT_ASSERT(t->allows_peers());

			TORRENT_TRY
			{
				if (t->try_connect_peer())
				{
					--max_connections;
					--free_slots;
					steps_since_last_connect = 0;
#ifdef TORRENT_STATS
					inc_stats_counter(connection_attempts);
#endif
				}
			}
			TORRENT_CATCH(std::bad_alloc&)
			{
				// we ran out of memory trying to connect to a peer
				// lower the global limit to the number of peers
				// we already have
				m_settings.set_int(settings_pack::connections_limit, num_connections());
				if (m_settings.get_int(settings_pack::connections_limit) < 2)
					m_settings.set_int(settings_pack::connections_limit, 2);
			}

			++steps_since_last_connect;

			// if there are no more free connection slots, abort
			if (free_slots <= -m_half_open.limit()) break;
			if (max_connections == 0) return;
			// there are no more torrents that want peers
			if (want_peers_download.empty() && want_peers_finished.empty()) break;
			// if we have gone a whole loop without
			// handing out a single connection, break
			if (steps_since_last_connect > num_torrents + 1) break;
			// maintain the global limit on number of connections
			if (num_connections() >= m_settings.get_int(settings_pack::connections_limit)) break;
		}
	}

	void session_impl::recalculate_unchoke_slots()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

		ptime now = time_now();
		time_duration unchoke_interval = now - m_last_choke;
		m_last_choke = now;

		// build list of all peers that are
		// unchokable.
		std::vector<peer_connection*> peers;
		for (connection_map::iterator i = m_connections.begin();
			i != m_connections.end();)
		{
			boost::intrusive_ptr<peer_connection> p = *i;
			TORRENT_ASSERT(p);
			++i;
			torrent* t = p->associated_torrent().lock().get();
			policy::peer* pi = p->peer_info_struct();

			if (p->ignore_unchoke_slots() || t == 0 || pi == 0 || pi->web_seed || t->is_paused())
				continue;

			if (m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::bittyrant_choker)
			{
				if (!p->is_choked() && p->is_interesting())
				{
					if (!p->has_peer_choked())
					{
						// we're unchoked, we may want to lower our estimated
						// reciprocation rate
						p->decrease_est_reciprocation_rate();
					}
					else
					{
						// we've unchoked this peer, and it hasn't reciprocated
						// we may want to increase our estimated reciprocation rate
						p->increase_est_reciprocation_rate();
					}
				}
			}

			if (!p->is_peer_interested()
				|| p->is_disconnecting()
				|| p->is_connecting())
			{
				// this peer is not unchokable. So, if it's unchoked
				// already, make sure to choke it.
				if (p->is_choked()) continue;
				if (pi && pi->optimistically_unchoked)
				{
					pi->optimistically_unchoked = false;
					// force a new optimistic unchoke
					m_optimistic_unchoke_time_scaler = 0;
				}
				t->choke_peer(*p);
				continue;
			}
			peers.push_back(p.get());
		}

		if (m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::rate_based_choker)
		{
			m_allowed_upload_slots = 0;
			std::sort(peers.begin(), peers.end()
				, boost::bind(&peer_connection::upload_rate_compare, _1, _2));

#ifdef TORRENT_DEBUG
			for (std::vector<peer_connection*>::const_iterator i = peers.begin()
				, end(peers.end()), prev(peers.end()); i != end; ++i)
			{
				if (prev != end)
				{
					boost::shared_ptr<torrent> t1 = (*prev)->associated_torrent().lock();
					TORRENT_ASSERT(t1);
					boost::shared_ptr<torrent> t2 = (*i)->associated_torrent().lock();
					TORRENT_ASSERT(t2);
					TORRENT_ASSERT((*prev)->uploaded_since_unchoke() * 1000
						* (1 + t1->priority()) / total_milliseconds(unchoke_interval)
						>= (*i)->uploaded_since_unchoke() * 1000
						* (1 + t2->priority()) / total_milliseconds(unchoke_interval));
				}
				prev = i;
			}
#endif

			// TODO: make configurable
			int rate_threshold = 1024;

			for (std::vector<peer_connection*>::const_iterator i = peers.begin()
				, end(peers.end()); i != end; ++i)
			{
				peer_connection const& p = **i;
				int rate = int(p.uploaded_since_unchoke()
					* 1000 / total_milliseconds(unchoke_interval));

				if (rate < rate_threshold) break;

				++m_allowed_upload_slots;

				// TODO: make configurable
				rate_threshold += 1024;
			}
			// allow one optimistic unchoke
			++m_allowed_upload_slots;
		}

		if (m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::bittyrant_choker)
		{
			// if we're using the bittyrant choker, sort peers by their return
			// on investment. i.e. download rate / upload rate
			std::sort(peers.begin(), peers.end()
				, boost::bind(&peer_connection::bittyrant_unchoke_compare, _1, _2));
		}
		else
		{
			// sorts the peers that are eligible for unchoke by download rate and secondary
			// by total upload. The reason for this is, if all torrents are being seeded,
			// the download rate will be 0, and the peers we have sent the least to should
			// be unchoked
			std::sort(peers.begin(), peers.end()
				, boost::bind(&peer_connection::unchoke_compare, _1, _2));
		}

		// auto unchoke
		peer_class* gpc = m_classes.at(m_global_class);
		int upload_limit = gpc->channel[peer_connection::upload_channel].throttle();
		if (m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::auto_expand_choker
			&& upload_limit > 0)
		{
			// if our current upload rate is less than 90% of our 
			// limit
			if (m_stat.upload_rate() < upload_limit * 0.9f
				&& m_allowed_upload_slots <= m_num_unchoked + 1
				&& m_upload_rate.queue_size() < 2)
			{
				++m_allowed_upload_slots;
			}
			else if (m_upload_rate.queue_size() > 1
				&& m_allowed_upload_slots > m_settings.get_int(settings_pack::unchoke_slots_limit)
				&& m_settings.get_int(settings_pack::unchoke_slots_limit) >= 0)
			{
				--m_allowed_upload_slots;
			}
		}

		int num_opt_unchoke = m_settings.get_int(settings_pack::num_optimistic_unchoke_slots);
		if (num_opt_unchoke == 0) num_opt_unchoke = (std::max)(1, m_allowed_upload_slots / 5);

		// reserve some upload slots for optimistic unchokes
		int unchoke_set_size = m_allowed_upload_slots - num_opt_unchoke;

		int upload_capacity_left = 0;
		if (m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::bittyrant_choker)
		{
			upload_capacity_left = upload_rate_limit(m_global_class);
			if (upload_capacity_left == 0)
			{
				// we don't know at what rate we can upload. If we have a
				// measurement of the peak, use that + 10kB/s, otherwise
				// assume 20 kB/s
				upload_capacity_left = (std::max)(20000, m_peak_up_rate + 10000);
				if (m_alerts.should_post<performance_alert>())
					m_alerts.post_alert(performance_alert(torrent_handle()
						, performance_alert::bittyrant_with_no_uplimit));
			}
		}

		m_num_unchoked = 0;
		// go through all the peers and unchoke the first ones and choke
		// all the other ones.
		for (std::vector<peer_connection*>::iterator i = peers.begin()
			, end(peers.end()); i != end; ++i)
		{
			peer_connection* p = *i;
			TORRENT_ASSERT(p);
			TORRENT_ASSERT(!p->ignore_unchoke_slots());

			// this will update the m_uploaded_at_last_unchoke
			// #error this should be called for all peers!
			p->reset_choke_counters();

			torrent* t = p->associated_torrent().lock().get();
			TORRENT_ASSERT(t);

			// if this peer should be unchoked depends on different things
			// in different unchoked schemes
			bool unchoke = false;
			if (m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::bittyrant_choker)
			{
				unchoke = p->est_reciprocation_rate() <= upload_capacity_left;
			}
			else
			{
				unchoke = unchoke_set_size > 0;
			}

			if (unchoke)
			{
				upload_capacity_left -= p->est_reciprocation_rate();

				// yes, this peer should be unchoked
				if (p->is_choked())
				{
					if (!t->unchoke_peer(*p))
						continue;
				}

				--unchoke_set_size;
				++m_num_unchoked;

				TORRENT_ASSERT(p->peer_info_struct());
				if (p->peer_info_struct()->optimistically_unchoked)
				{
					// force a new optimistic unchoke
					// since this one just got promoted into the
					// proper unchoke set
					m_optimistic_unchoke_time_scaler = 0;
					p->peer_info_struct()->optimistically_unchoked = false;
				}
			}
			else
			{
				// no, this peer should be choked
				TORRENT_ASSERT(p->peer_info_struct());
				if (!p->is_choked() && !p->peer_info_struct()->optimistically_unchoked)
					t->choke_peer(*p);
				if (!p->is_choked())
					++m_num_unchoked;
			}
		}
	}

	void session_impl::cork_burst(peer_connection* p)
	{
		TORRENT_ASSERT(is_single_thread());
		if (p->is_corked()) return;
		p->cork_socket();
		m_delayed_uncorks.push_back(p);
	}

	void session_impl::do_delayed_uncork()
	{
#ifdef TORRENT_STATS
		inc_stats_counter(aux::session_impl::on_disk_counter);
#endif
		TORRENT_ASSERT(is_single_thread());
		for (std::vector<peer_connection*>::iterator i = m_delayed_uncorks.begin()
			, end(m_delayed_uncorks.end()); i != end; ++i)
		{
			(*i)->uncork_socket();
		}
		m_delayed_uncorks.clear();
	}

	void session_impl::main_thread()
	{
		// this is a debug facility
		// see single_threaded in debug.hpp
		thread_started();

		TORRENT_ASSERT(is_single_thread());
		eh_initializer();

		// initialize async operations
		init();

		bool stop_loop = false;
		while (!stop_loop)
		{
			error_code ec;
			m_io_service.run(ec);
			if (ec)
			{
#ifdef TORRENT_DEBUG
				fprintf(stderr, "%s\n", ec.message().c_str());
				std::string err = ec.message();
#endif
				TORRENT_ASSERT(false);
			}
			m_io_service.reset();

			stop_loop = m_abort;
		}

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " locking mutex\n";
#endif

/*
#ifdef TORRENT_DEBUG
		for (torrent_map::iterator i = m_torrents.begin();
			i != m_torrents.end(); ++i)
		{
			TORRENT_ASSERT(i->second->num_peers() == 0);
		}
#endif
*/
#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string() << " cleaning up torrents\n";
#endif

		// clear the torrent LRU (probably not strictly necessary)
		list_node* i = m_torrent_lru.get_all();
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		// clear the prev and next pointers in all torrents
		// to avoid the assert when destructing them
		while (i)
		{
			list_node* tmp = i;
			i = i->next;
			tmp->next = NULL;
			tmp->prev= NULL;
		}
#endif
		m_torrents.clear();

		TORRENT_ASSERT(m_torrents.empty());
		TORRENT_ASSERT(m_connections.empty());

#if (defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS) && defined BOOST_HAS_PTHREADS
		m_network_thread = 0;
#endif
	}


	// the return value from this function is valid only as long as the
	// session is locked!
	boost::weak_ptr<torrent> session_impl::find_torrent(sha1_hash const& info_hash)
	{
		TORRENT_ASSERT(is_single_thread());

		torrent_map::iterator i = m_torrents.find(info_hash);
#ifdef TORRENT_DEBUG
		for (torrent_map::iterator j
			= m_torrents.begin(); j != m_torrents.end(); ++j)
		{
			torrent* p = boost::get_pointer(j->second);
			TORRENT_ASSERT(p);
		}
#endif
		if (i != m_torrents.end()) return i->second;
		return boost::weak_ptr<torrent>();
	}

	torrent const* session_impl::find_encrypted_torrent(sha1_hash const& info_hash
		, sha1_hash const& xor_mask)
	{
		aux::session_impl::torrent_map::const_iterator i;

		for (i = m_torrents.begin(); i != m_torrents.end(); ++i)
		{
			torrent const& ti = *i->second;

			sha1_hash const& skey_hash = ti.obfuscated_hash();
			sha1_hash obfs_hash = xor_mask;
			obfs_hash ^= skey_hash;

			if (info_hash == obfs_hash)
				return &ti;
		}
		return NULL;
	}

	boost::weak_ptr<torrent> session_impl::find_torrent(std::string const& uuid)
	{
		TORRENT_ASSERT(is_single_thread());

		std::map<std::string, boost::shared_ptr<torrent> >::iterator i
			= m_uuids.find(uuid);
		if (i != m_uuids.end()) return i->second;
		return boost::weak_ptr<torrent>();
	}

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
	boost::shared_ptr<logger> session_impl::create_log(std::string const& name
		, int instance, bool append)
	{
		// current options are file_logger, cout_logger and null_logger
		return boost::shared_ptr<logger>(new logger(m_logpath, name + ".log", instance, append));
	}

	void session_impl::session_log(char const* fmt, ...) const
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

	void session_impl::log_all_torrents(peer_connection* p)
	{
		for (session_impl::torrent_map::const_iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			p->peer_log("   %s", to_hex(i->second->torrent_file().info_hash().to_string()).c_str());
		}
	}
#endif

	void session_impl::get_torrent_status(std::vector<torrent_status>* ret
		, boost::function<bool(torrent_status const&)> const& pred
		, boost::uint32_t flags) const
	{
		for (torrent_map::const_iterator i
			= m_torrents.begin(), end(m_torrents.end());
			i != end; ++i)
		{
			if (i->second->is_aborted()) continue;
			torrent_status st;
			i->second->status(&st, flags);
			if (!pred(st)) continue;
			ret->push_back(st);
		}
	}

	void session_impl::refresh_torrent_status(std::vector<torrent_status>* ret
		, boost::uint32_t flags) const
	{
		for (std::vector<torrent_status>::iterator i
			= ret->begin(), end(ret->end()); i != end; ++i)
		{
			boost::shared_ptr<torrent> t = i->handle.m_torrent.lock();
			if (!t) continue;
			t->status(&*i, flags);
		}
	}
	
	void session_impl::post_torrent_updates()
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(is_single_thread());

		std::auto_ptr<state_update_alert> alert(new state_update_alert());
		std::vector<torrent*>& state_updates
			= m_torrent_lists[aux::session_impl::torrent_state_updates];

		alert->status.reserve(state_updates.size());

		// TODO: it might be a nice feature here to limit the number of torrents
		// to send in a single update. By just posting the first n torrents, they
		// would nicely be round-robined because the torrent lists are always
		// pushed back
		for (std::vector<torrent*>::iterator i = state_updates.begin()
			, end(state_updates.end()); i != end; ++i)
		{
			torrent* t = *i;
			TORRENT_ASSERT(t->m_links[aux::session_impl::torrent_state_updates].in_list());
			alert->status.push_back(torrent_status());
			t->status(&alert->status.back(), 0xffffffff);
			t->clear_in_state_update();
		}
		state_updates.clear();

		m_alerts.post_alert_ptr(alert.release());
	}

	std::vector<torrent_handle> session_impl::get_torrents() const
	{
		std::vector<torrent_handle> ret;

		for (torrent_map::const_iterator i
			= m_torrents.begin(), end(m_torrents.end());
			i != end; ++i)
		{
			if (i->second->is_aborted()) continue;
			ret.push_back(torrent_handle(i->second));
		}
		return ret;
	}

	torrent_handle session_impl::find_torrent_handle(sha1_hash const& info_hash)
	{
		return torrent_handle(find_torrent(info_hash));
	}

	void session_impl::async_add_torrent(add_torrent_params* params)
	{
		error_code ec;
		torrent_handle handle = add_torrent(*params, ec);
		m_alerts.post_alert(add_torrent_alert(handle, *params, ec));
		delete params->resume_data;
		delete params;
	}

	torrent_handle session_impl::add_torrent(add_torrent_params const& p
		, error_code& ec)
	{
		TORRENT_ASSERT(!p.save_path.empty());

#ifndef TORRENT_NO_DEPRECATE
		p.update_flags();
#endif

		add_torrent_params params = p;
		if (string_begins_no_case("magnet:", params.url.c_str()))
		{
			parse_magnet_uri(params.url, params, ec);
			if (ec) return torrent_handle();
			params.url.clear();
		}

		if (params.ti && params.ti->is_valid() && params.ti->num_files() == 0)
		{
			ec = errors::no_files_in_torrent;
			return torrent_handle();
		}

#ifndef TORRENT_DISABLE_DHT	
		// add p.dht_nodes to the DHT, if enabled
		if (m_dht && !p.dht_nodes.empty())
		{
			for (std::vector<std::pair<std::string, int> >::const_iterator i = p.dht_nodes.begin()
				, end(p.dht_nodes.end()); i != end; ++i)
				m_dht->add_node(*i);
		}
#endif

		INVARIANT_CHECK;

		if (is_aborted())
		{
			ec = errors::session_is_closing;
			return torrent_handle();
		}
		
		// figure out the info hash of the torrent
		sha1_hash const* ih = 0;
		sha1_hash tmp;
		if (params.ti) ih = &params.ti->info_hash();
		else if (!params.url.empty())
		{
			// in order to avoid info-hash collisions, for
			// torrents where we don't have an info-hash, but
			// just a URL, set the temporary info-hash to the
			// hash of the URL. This will be changed once we
			// have the actual .torrent file
			tmp = hasher(&params.url[0], params.url.size()).final();
			ih = &tmp;
		}
		else ih = &params.info_hash;

		// is the torrent already active?
		boost::shared_ptr<torrent> torrent_ptr = find_torrent(*ih).lock();
		if (!torrent_ptr && !params.uuid.empty()) torrent_ptr = find_torrent(params.uuid).lock();
		// TODO: find by url?

		if (torrent_ptr)
		{
			if ((params.flags & add_torrent_params::flag_duplicate_is_error) == 0)
			{
				if (!params.uuid.empty() && torrent_ptr->uuid().empty())
					torrent_ptr->set_uuid(params.uuid);
				if (!params.url.empty() && torrent_ptr->url().empty())
					torrent_ptr->set_url(params.url);
				if (!params.source_feed_url.empty() && torrent_ptr->source_feed_url().empty())
					torrent_ptr->set_source_feed_url(params.source_feed_url);
				return torrent_handle(torrent_ptr);
			}

			ec = errors::duplicate_torrent;
			return torrent_handle();
		}

		int queue_pos = ++m_max_queue_pos;

		torrent_ptr.reset(new torrent(*this, m_listen_interface
			, 16 * 1024, queue_pos, params, *ih));
		torrent_ptr->start();

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			boost::shared_ptr<torrent_plugin> tp((*i)(torrent_ptr.get(), params.userdata));
			if (tp) torrent_ptr->add_extension(tp);
		}

		for (ses_extension_list_t::iterator i = m_ses_extensions.begin()
			, end(m_ses_extensions.end()); i != end; ++i)
		{
			boost::shared_ptr<torrent_plugin> tp((*i)->new_torrent(torrent_ptr.get(), params.userdata));
			if (tp) torrent_ptr->add_extension(tp);
		}
#endif

#ifndef TORRENT_DISABLE_DHT
		if (m_dht && params.ti)
		{
			torrent_info::nodes_t const& nodes = params.ti->nodes();
			std::for_each(nodes.begin(), nodes.end(), boost::bind(
				(void(dht::dht_tracker::*)(std::pair<std::string, int> const&))
				&dht::dht_tracker::add_node
				, boost::ref(m_dht), _1));
		}
#endif

#if TORRENT_HAS_BOOST_UNORDERED
		sha1_hash next_lsd(0);
		sha1_hash next_dht(0);
		if (m_next_lsd_torrent != m_torrents.end())
			next_lsd = m_next_lsd_torrent->first;
		if (m_next_dht_torrent != m_torrents.end())
			next_dht = m_next_dht_torrent->first;
		float load_factor = m_torrents.load_factor();
#endif

		m_torrents.insert(std::make_pair(*ih, torrent_ptr));

		if (torrent_ptr->is_pinned() == false)
		{
			bump_torrent(torrent_ptr.get());
			evict_torrent(torrent_ptr.get());
		}

#if TORRENT_HAS_BOOST_UNORDERED
		// if this insert made the hash grow, the iterators became invalid
		// we need to reset them
		if (m_torrents.load_factor() < load_factor)
		{
			// this indicates the hash table re-hashed
			if (!next_lsd.is_all_zeros())
				m_next_lsd_torrent = m_torrents.find(next_lsd);
			if (!next_dht.is_all_zeros())
				m_next_dht_torrent = m_torrents.find(next_dht);
		}
#endif
		if (!params.uuid.empty() || !params.url.empty())
			m_uuids.insert(std::make_pair(params.uuid.empty()
				? params.url : params.uuid, torrent_ptr));

		if (m_alerts.should_post<torrent_added_alert>())
			m_alerts.post_alert(torrent_added_alert(torrent_ptr->get_handle()));

		// recalculate auto-managed torrents sooner (or put it off)
		// if another torrent will be added within one second from now
		// we want to put it off again anyway. So that while we're adding
		// a boat load of torrents, we postpone the recalculation until
		// we're done adding them all (since it's kind of an expensive operation)
		if (params.flags & add_torrent_params::flag_auto_managed)
			m_auto_manage_time_scaler = 2;

		return torrent_handle(torrent_ptr);
	}

	void session_impl::remove_torrent(const torrent_handle& h, int options)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> tptr = h.m_torrent.lock();
		if (!tptr) return;

		remove_torrent_impl(tptr, options);

		if (m_alerts.should_post<torrent_removed_alert>())
			m_alerts.post_alert(torrent_removed_alert(tptr->get_handle(), tptr->info_hash()));

		tptr->abort();
		tptr->set_queue_position(-1);
	}

	void session_impl::remove_torrent_impl(boost::shared_ptr<torrent> tptr, int options)
	{
		// remove from uuid list
		if (!tptr->uuid().empty())
		{
			std::map<std::string, boost::shared_ptr<torrent> >::iterator j
				= m_uuids.find(tptr->uuid());
			if (j != m_uuids.end()) m_uuids.erase(j);
		}

		torrent_map::iterator i =
			m_torrents.find(tptr->torrent_file().info_hash());

		// this torrent might be filed under the URL-hash
		if (i == m_torrents.end() && !tptr->url().empty())
		{
			std::string const& url = tptr->url();
			sha1_hash urlhash = hasher(&url[0], url.size()).final();
			i = m_torrents.find(urlhash);
		}

		if (i == m_torrents.end()) return;

		torrent& t = *i->second;
		if (options & session::delete_files)
			t.delete_files();

		if ((t.prev != NULL || t.next != NULL) && !t.is_pinned())
			m_torrent_lru.erase(&t);

		TORRENT_ASSERT(t.prev == NULL && t.next == NULL);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		sha1_hash i_hash = t.torrent_file().info_hash();
#endif
#ifndef TORRENT_DISABLE_DHT
		if (i == m_next_dht_torrent)
			++m_next_dht_torrent;
#endif
		if (i == m_next_lsd_torrent)
			++m_next_lsd_torrent;

		m_torrents.erase(i);

#ifndef TORRENT_DISABLE_DHT
		if (m_next_dht_torrent == m_torrents.end())
			m_next_dht_torrent = m_torrents.begin();
#endif
		if (m_next_lsd_torrent == m_torrents.end())
			m_next_lsd_torrent = m_torrents.begin();

		TORRENT_ASSERT(m_torrents.find(i_hash) == m_torrents.end());
	}

	void session_impl::listen_on(
		std::pair<int, int> const& port_range
		, error_code& ec
		, const char* net_interface, int flags)
	{
		INVARIANT_CHECK;

		tcp::endpoint new_interface;
		if (net_interface && std::strlen(net_interface) > 0)
		{
			new_interface = tcp::endpoint(address::from_string(net_interface, ec), port_range.first);
			if (ec)
			{
#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
				(*m_logger) << time_now_string() << "listen_on: " << net_interface
					<< " failed: " << ec.message() << "\n";
#endif
				return;
			}
		}
		else
		{
			new_interface = tcp::endpoint(address_v4::any(), port_range.first);
		}

		m_listen_port_retries = port_range.second - port_range.first;

		// if the interface is the same and the socket is open
		// don't do anything
		if (new_interface == m_listen_interface
			&& !m_listen_sockets.empty())
			return;

		m_listen_interface = new_interface;

		open_listen_port(flags, ec);

#if defined TORRENT_VERBOSE_LOGGING || defined TORRENT_LOGGING || defined TORRENT_ERROR_LOGGING
		m_logger = create_log("main_session", listen_port(), false);
		(*m_logger) << time_now_string() << "\n";
#endif
	}

	address session_impl::listen_address() const
	{
		for (std::list<listen_socket_t>::const_iterator i = m_listen_sockets.begin()
			, end(m_listen_sockets.end()); i != end; ++i)
		{
			if (i->external_address != address()) return i->external_address;
		}
		return address();
	}

	boost::uint16_t session_impl::listen_port() const
	{
		// if peer connections are set up to be received over a socks
		// proxy, and it's the same one as we're using for the tracker
		// just tell the tracker the socks5 port we're listening on
		if (m_socks_listen_socket && m_socks_listen_socket->is_open())
			return m_socks_listen_port;

		// if not, don't tell the tracker anything if we're in anonymous
		// mode. We don't want to leak our listen port since it can
		// potentially identify us if it is leaked elsewere
		if (m_settings.get_bool(settings_pack::anonymous_mode)) return 0;
		if (m_listen_sockets.empty()) return 0;
		return m_listen_sockets.front().external_port;
	}

	boost::uint16_t session_impl::ssl_listen_port() const
	{
#ifdef TORRENT_USE_OPENSSL
		// if peer connections are set up to be received over a socks
		// proxy, and it's the same one as we're using for the tracker
		// just tell the tracker the socks5 port we're listening on
		if (m_socks_listen_socket && m_socks_listen_socket->is_open()
			&& m_proxy.hostname == m_proxy.hostname)
			return m_socks_listen_port;

		// if not, don't tell the tracker anything if we're in anonymous
		// mode. We don't want to leak our listen port since it can
		// potentially identify us if it is leaked elsewere
		if (m_settings.get_bool(settings_pack::anonymous_mode)) return 0;
		if (m_listen_sockets.empty()) return 0;
		for (std::list<listen_socket_t>::const_iterator i = m_listen_sockets.begin()
			, end(m_listen_sockets.end()); i != end; ++i)
		{
			if (i->ssl) return i->external_port;
		}
#endif
		return 0;
	}

	void session_impl::announce_lsd(sha1_hash const& ih, int port, bool broadcast)
	{
		// use internal listen port for local peers
		if (m_lsd.get())
			m_lsd->announce(ih, port, broadcast);
	}

	void session_impl::on_lsd_peer(tcp::endpoint peer, sha1_hash const& ih)
	{
#ifdef TORRENT_STATS
		inc_stats_counter(on_lsd_peer_counter);
#endif
		TORRENT_ASSERT(is_single_thread());

		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = find_torrent(ih).lock();
		if (!t) return;
		// don't add peers from lsd to private torrents
		if (t->torrent_file().priv() || (t->torrent_file().is_i2p()
			&& !m_settings.get_bool(settings_pack::allow_i2p_mixed))) return;

#if defined(TORRENT_VERBOSE_LOGGING) || defined(TORRENT_LOGGING)
		(*m_logger) << time_now_string()
			<< ": added peer from local discovery: " << peer << "\n";
#endif
		t->get_policy().add_peer(peer, peer_id(0), peer_info::lsd, 0);
		t->update_want_peers();
		if (m_alerts.should_post<lsd_peer_alert>())
			m_alerts.post_alert(lsd_peer_alert(t->get_handle(), peer));
	}

	void session_impl::on_port_map_log(
		char const* msg, int map_transport)
	{
		TORRENT_ASSERT(map_transport >= 0 && map_transport <= 1);
		// log message
#ifdef TORRENT_UPNP_LOGGING
		char const* transport_names[] = {"NAT-PMP", "UPnP"};
		m_upnp_log << time_now_string() << " "
			<< transport_names[map_transport] << ": " << msg;
#endif
		if (m_alerts.should_post<portmap_log_alert>())
			m_alerts.post_alert(portmap_log_alert(map_transport, msg));
	}

	void session_impl::on_port_mapping(int mapping, address const& ip, int port
		, error_code const& ec, int map_transport)
	{
		TORRENT_ASSERT(is_single_thread());

		TORRENT_ASSERT(map_transport >= 0 && map_transport <= 1);

		if (mapping == m_udp_mapping[map_transport] && port != 0)
		{
			m_external_udp_port = port;
			if (m_alerts.should_post<portmap_alert>())
				m_alerts.post_alert(portmap_alert(mapping, port
					, map_transport));
			return;
		}

		if (mapping == m_tcp_mapping[map_transport] && port != 0)
		{
			// TODO: report the proper address of the router
			if (ip != address()) set_external_address(ip, source_router
				, address());

			if (!m_listen_sockets.empty()) {
				m_listen_sockets.front().external_address = ip;
				m_listen_sockets.front().external_port = port;
			}
			if (m_alerts.should_post<portmap_alert>())
				m_alerts.post_alert(portmap_alert(mapping, port
					, map_transport));
			return;
		}

		if (ec)
		{
			if (m_alerts.should_post<portmap_error_alert>())
				m_alerts.post_alert(portmap_error_alert(mapping
					, map_transport, ec));
		}
		else
		{
			if (m_alerts.should_post<portmap_alert>())
				m_alerts.post_alert(portmap_alert(mapping, port
					, map_transport));
		}
	}

	session_status session_impl::status() const
	{
//		INVARIANT_CHECK;
		TORRENT_ASSERT(is_single_thread());

		session_status s;

		s.optimistic_unchoke_counter = m_optimistic_unchoke_time_scaler;
		s.unchoke_counter = m_unchoke_time_scaler;

		s.num_peers = int(m_connections.size());
		s.num_dead_peers = int(m_undead_peers.size());
		s.num_unchoked = m_num_unchoked;
		s.allowed_upload_slots = m_allowed_upload_slots;

		s.total_redundant_bytes = m_total_redundant_bytes;
		s.total_failed_bytes = m_total_failed_bytes;

		s.up_bandwidth_queue = m_upload_rate.queue_size();
		s.down_bandwidth_queue = m_download_rate.queue_size();

		s.up_bandwidth_bytes_queue = m_upload_rate.queued_bytes();
		s.down_bandwidth_bytes_queue = m_download_rate.queued_bytes();

		s.disk_write_queue = m_disk_queues[peer_connection::download_channel];
		s.disk_read_queue = m_disk_queues[peer_connection::upload_channel];

		s.has_incoming_connections = m_incoming_connection;

		// total
		s.download_rate = m_stat.download_rate();
		s.total_upload = m_stat.total_upload();
		s.upload_rate = m_stat.upload_rate();
		s.total_download = m_stat.total_download();

		// payload
		s.payload_download_rate = m_stat.transfer_rate(stat::download_payload);
		s.total_payload_download = m_stat.total_transfer(stat::download_payload);
		s.payload_upload_rate = m_stat.transfer_rate(stat::upload_payload);
		s.total_payload_upload = m_stat.total_transfer(stat::upload_payload);

#ifndef TORRENT_DISABLE_FULL_STATS
		// IP-overhead
		s.ip_overhead_download_rate = m_stat.transfer_rate(stat::download_ip_protocol);
		s.total_ip_overhead_download = m_stat.total_transfer(stat::download_ip_protocol);
		s.ip_overhead_upload_rate = m_stat.transfer_rate(stat::upload_ip_protocol);
		s.total_ip_overhead_upload = m_stat.total_transfer(stat::upload_ip_protocol);

#ifndef TORRENT_DISABLE_DHT
		// DHT protocol
		s.dht_download_rate = m_stat.transfer_rate(stat::download_dht_protocol);
		s.total_dht_download = m_stat.total_transfer(stat::download_dht_protocol);
		s.dht_upload_rate = m_stat.transfer_rate(stat::upload_dht_protocol);
		s.total_dht_upload = m_stat.total_transfer(stat::upload_dht_protocol);
#endif

		// tracker
		s.tracker_download_rate = m_stat.transfer_rate(stat::download_tracker_protocol);
		s.total_tracker_download = m_stat.total_transfer(stat::download_tracker_protocol);
		s.tracker_upload_rate = m_stat.transfer_rate(stat::upload_tracker_protocol);
		s.total_tracker_upload = m_stat.total_transfer(stat::upload_tracker_protocol);
#else
		// IP-overhead
		s.ip_overhead_download_rate = 0;
		s.total_ip_overhead_download = 0;
		s.ip_overhead_upload_rate = 0;
		s.total_ip_overhead_upload = 0;

		// DHT protocol
		s.dht_download_rate = 0;
		s.total_dht_download = 0;
		s.dht_upload_rate = 0;
		s.total_dht_upload = 0;

		// tracker
		s.tracker_download_rate = 0;
		s.total_tracker_download = 0;
		s.tracker_upload_rate = 0;
		s.total_tracker_upload = 0;
#endif

#ifndef TORRENT_DISABLE_DHT
		if (m_dht)
		{
			m_dht->dht_status(s);
		}
		else
		{
			s.dht_nodes = 0;
			s.dht_node_cache = 0;
			s.dht_torrents = 0;
			s.dht_global_nodes = 0;
			s.dht_total_allocations = 0;
		}
#endif

		m_utp_socket_manager.get_status(s.utp_stats);

		// this loop is potentially expensive. It could be optimized by
		// simply keeping a global counter
		int peerlist_size = 0;
		for (torrent_map::const_iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			peerlist_size += i->second->get_policy().num_peers();
		}

		s.peerlist_size = peerlist_size;

		return s;
	}

#ifndef TORRENT_DISABLE_DHT

	void session_impl::start_dht()
	{ start_dht(m_dht_state); }

	void session_impl::start_dht(entry const& startup_state)
	{
		INVARIANT_CHECK;

		stop_dht();
		m_dht = new dht::dht_tracker(*this, m_udp_socket, m_dht_settings, &startup_state);

		for (std::list<udp::endpoint>::iterator i = m_dht_router_nodes.begin()
			, end(m_dht_router_nodes.end()); i != end; ++i)
		{
			m_dht->add_router_node(*i);
		}

		m_dht->start(startup_state);

		m_udp_socket.subscribe(m_dht.get());

		// announce all torrents we have to the DHT
		for (torrent_map::const_iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			i->second->dht_announce();
		}
	}

	void session_impl::stop_dht()
	{
		if (!m_dht) return;
		m_udp_socket.unsubscribe(m_dht.get());
		m_dht->stop();
		m_dht = 0;
	}

	void session_impl::set_dht_settings(dht_settings const& settings)
	{
		m_dht_settings = settings;
	}

#ifndef TORRENT_NO_DEPRECATE
	entry session_impl::dht_state() const
	{
		if (!m_dht) return entry();
		return m_dht->state();
	}
#endif

	void session_impl::add_dht_node_name(std::pair<std::string, int> const& node)
	{
		if (m_dht) m_dht->add_node(node);
	}

	void session_impl::add_dht_router(std::pair<std::string, int> const& node)
	{
#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_dht_router_name_lookup");
#endif
		char port[7];
		snprintf(port, sizeof(port), "%d", node.second);
		tcp::resolver::query q(node.first, port);
		m_host_resolver.async_resolve(q,
			boost::bind(&session_impl::on_dht_router_name_lookup, this, _1, _2));
	}

	void session_impl::on_dht_router_name_lookup(error_code const& e
		, tcp::resolver::iterator host)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("session_impl::on_dht_router_name_lookup");
#endif
		// TODO: report errors as alerts
		if (e) return;
		while (host != tcp::resolver::iterator())
		{
			// router nodes should be added before the DHT is started (and bootstrapped)
			udp::endpoint ep(host->endpoint().address(), host->endpoint().port());
			if (m_dht) m_dht->add_router_node(ep);
			m_dht_router_nodes.push_back(ep);
			++host;
		}
	}
#endif

	void session_impl::maybe_update_udp_mapping(int nat, int local_port, int external_port)
	{
		int local, external, protocol;
		if (nat == 0 && m_natpmp.get())
		{
			if (m_udp_mapping[nat] != -1)
			{
				if (m_natpmp->get_mapping(m_udp_mapping[nat], local, external, protocol))
				{
					// we already have a mapping. If it's the same, don't do anything
					if (local == local_port && external == external_port && protocol == natpmp::udp)
						return;
				}
				m_natpmp->delete_mapping(m_udp_mapping[nat]);
			}
			m_udp_mapping[nat] = m_natpmp->add_mapping(natpmp::udp
				, local_port, external_port);
			return;
		}
		else if (nat == 1 && m_upnp.get())
		{
			if (m_udp_mapping[nat] != -1)
			{
				if (m_upnp->get_mapping(m_udp_mapping[nat], local, external, protocol))
				{
					// we already have a mapping. If it's the same, don't do anything
					if (local == local_port && external == external_port && protocol == natpmp::udp)
						return;
				}
				m_upnp->delete_mapping(m_udp_mapping[nat]);
			}
			m_udp_mapping[nat] = m_upnp->add_mapping(upnp::udp
				, local_port, external_port);
			return;
		}
	}

#ifndef TORRENT_DISABLE_ENCRYPTION
	void session_impl::set_pe_settings(pe_settings const& settings)
	{
		m_pe_settings = settings;
	}
#endif

	bool session_impl::is_listening() const
	{
		return !m_listen_sockets.empty();
	}

	session_impl::~session_impl()
	{
#if defined TORRENT_REQUEST_LOGGING
		fclose(m_request_logger);
#endif
		m_io_service.post(boost::bind(&session_impl::abort, this));

		// now it's OK for the network thread to exit
		m_work.reset();

#if defined TORRENT_ASIO_DEBUGGING
		int counter = 0;
		while (log_async())
		{
			sleep(1000);
			++counter;
			printf("\n==== Waiting to shut down: %d ==== conn-queue: %d connecting: %d timeout (next: %f max: %f)\n\n"
				, counter, m_half_open.size(), m_half_open.num_connecting(), m_half_open.next_timeout()
				, m_half_open.max_timeout());
		}
		async_dec_threads();
#endif

		if (m_thread) m_thread->join();

		m_udp_socket.unsubscribe(this);
		m_udp_socket.unsubscribe(&m_utp_socket_manager);
		m_udp_socket.unsubscribe(&m_tracker_manager);

		TORRENT_ASSERT(m_torrents.empty());
		TORRENT_ASSERT(m_connections.empty());
		TORRENT_ASSERT(m_connections.empty());

#ifdef TORRENT_REQUEST_LOGGING
		if (m_request_log) fclose(m_request_log);
#endif

#ifdef TORRENT_STATS
		if (m_stats_logger) fclose(m_stats_logger);
#endif
	}

#ifndef TORRENT_NO_DEPRECATE
	int session_impl::max_connections() const
	{
		return m_settings.get_int(settings_pack::connections_limit);
	}

	int session_impl::max_uploads() const
	{
		return m_settings.get_int(settings_pack::unchoke_slots_limit);
	}

	int session_impl::max_half_open_connections() const
	{
		return m_settings.get_int(settings_pack::half_open_limit);
	}

	void session_impl::set_local_download_rate_limit(int bytes_per_second)
	{
		settings_pack* p = new settings_pack;
		p->set_int(settings_pack::local_download_rate_limit, bytes_per_second);
		apply_settings_pack(p);
	}

	void session_impl::set_local_upload_rate_limit(int bytes_per_second)
	{
		settings_pack* p = new settings_pack;
		p->set_int(settings_pack::local_upload_rate_limit, bytes_per_second);
		apply_settings_pack(p);
	}

	void session_impl::set_download_rate_limit(int bytes_per_second)
	{
		settings_pack* p = new settings_pack;
		p->set_int(settings_pack::download_rate_limit, bytes_per_second);
		apply_settings_pack(p);
	}

	void session_impl::set_upload_rate_limit(int bytes_per_second)
	{
		settings_pack* p = new settings_pack;
		p->set_int(settings_pack::upload_rate_limit, bytes_per_second);
		apply_settings_pack(p);
	}

	void session_impl::set_max_half_open_connections(int limit)
	{
		settings_pack* p = new settings_pack;
		p->set_int(settings_pack::half_open_limit, limit);
		apply_settings_pack(p);
	}

	void session_impl::set_max_connections(int limit)
	{
		settings_pack* p = new settings_pack;
		p->set_int(settings_pack::connections_limit, limit);
		apply_settings_pack(p);
	}

	void session_impl::set_max_uploads(int limit)
	{
		settings_pack* p = new settings_pack;
		p->set_int(settings_pack::unchoke_slots_limit, limit);
		apply_settings_pack(p);
	}

	int session_impl::local_upload_rate_limit() const
	{
		return upload_rate_limit(m_local_peer_class);
	}

	int session_impl::local_download_rate_limit() const
	{
		return download_rate_limit(m_local_peer_class);
	}

	int session_impl::upload_rate_limit() const
	{
		return upload_rate_limit(m_global_class);
	}

	int session_impl::download_rate_limit() const
	{
		return download_rate_limit(m_global_class);
	}
#endif

	void session_impl::update_peer_tos()
	{
		error_code ec;
		m_udp_socket.set_option(type_of_service(m_settings.get_int(settings_pack::peer_tos)), ec);
#if defined TORRENT_VERBOSE_LOGGING
		(*m_logger) << ">>> SET_TOS[ udp_socket tos: " << m_settings.get_int(settings_pack::peer_tos)
			<< " e: " << ec.message() << " ]\n";
#endif
	}

	void session_impl::update_user_agent()
	{
		// replace all occurances of '\n' with ' '.
		std::string agent = m_settings.get_str(settings_pack::user_agent);
		std::string::iterator i = agent.begin();
		while ((i = std::find(i, agent.end(), '\n'))
			!= agent.end())
			*i = ' ';
		m_settings.set_str(settings_pack::user_agent, agent);
	}

	void session_impl::update_choking_algorithm()
	{
		int algo = m_settings.get_int(settings_pack::choking_algorithm);
		int unchoke_limit = m_settings.get_int(settings_pack::unchoke_slots_limit);

		if (algo == settings_pack::fixed_slots_choker)
			m_allowed_upload_slots = unchoke_limit;
		else if (algo == settings_pack::auto_expand_choker
			&& m_allowed_upload_slots < unchoke_limit)
			m_allowed_upload_slots = unchoke_limit;

		if (m_allowed_upload_slots < 0)
			m_allowed_upload_slots = (std::numeric_limits<int>::max)();

		if (m_settings.get_int(settings_pack::num_optimistic_unchoke_slots) >= m_allowed_upload_slots / 2)
		{
			if (m_alerts.should_post<performance_alert>())
				m_alerts.post_alert(performance_alert(torrent_handle()
					, performance_alert::too_many_optimistic_unchoke_slots));
		}
	}

	void session_impl::update_connection_speed()
	{
		if (m_settings.get_int(settings_pack::connection_speed) < 0)
			m_settings.set_int(settings_pack::connection_speed, 200);
	}

	void session_impl::update_alert_queue_size()
	{
		m_alerts.set_alert_queue_size_limit(m_settings.get_int(settings_pack::alert_queue_size));
	}

	void session_impl::upate_dht_upload_rate_limit()
	{
		m_udp_socket.set_rate_limit(m_settings.get_int(settings_pack::dht_upload_rate_limit));
	}

	void session_impl::update_disk_threads()
	{
		if (m_settings.get_int(settings_pack::aio_threads) < 1)
			m_settings.set_int(settings_pack::aio_threads, 1);

#if !TORRENT_USE_PREAD && !TORRENT_USE_PREADV
		// if we don't have pread() nor preadv() there's no way
		// to perform concurrent file operations on the same file
		// handle, so we must limit the disk thread to a single one

		if (m_settings.get_int(settings_pack::aio_threads) > 1)
			m_settings.set_int(settings_pack::aio_threads, 1);
#endif

		m_disk_thread.set_num_threads(m_settings.get_int(settings_pack::aio_threads));
	}

	void session_impl::update_network_threads()
	{
		m_net_thread_pool.set_num_threads(m_settings.get_int(settings_pack::network_threads));
	}

	void session_impl::post_socket_write_job(write_some_job& j)
	{
		m_net_thread_pool.post_job(j);
	}


	void session_impl::update_cache_buffer_chunk_size()
	{
		if (m_settings.get_int(settings_pack::cache_buffer_chunk_size) <= 0)
			m_settings.set_int(settings_pack::cache_buffer_chunk_size, 1);
	}

	void session_impl::update_report_web_seed_downloads()
	{
		// if this flag changed, update all web seed connections
		bool report = m_settings.get_bool(settings_pack::report_web_seed_downloads);
		for (connection_map::iterator i = m_connections.begin()
			, end(m_connections.end()); i != end; ++i)
		{
			int type = (*i)->type();
			if (type == peer_connection::url_seed_connection
				|| type == peer_connection::http_seed_connection)
				(*i)->ignore_stats(!report);
		}
	}

	void session_impl::reset_auto_manage_timer()
	{
		m_auto_manage_time_scaler = 2;
	}
 
	void session_impl::update_dht_announce_interval()
	{
#ifndef TORRENT_DISABLE_DHT
#if defined TORRENT_ASIO_DEBUGGING
		add_outstanding_async("session_impl::on_dht_announce");
#endif
		error_code ec;
		int delay = (std::max)(m_settings.get_int(settings_pack::dht_announce_interval)
			/ (std::max)(int(m_torrents.size()), 1), 1);
		m_dht_announce_timer.expires_from_now(seconds(delay), ec);
		m_dht_announce_timer.async_wait(
			boost::bind(&session_impl::on_dht_announce, this, _1));
#endif
	}

	void session_impl::update_anonymous_mode()
	{
		if (!m_settings.get_bool(settings_pack::anonymous_mode)) return;

		// enable anonymous mode. We don't want to accept any incoming
		// connections, except through a proxy.
		m_settings.set_str(settings_pack::user_agent, "");
		url_random((char*)&m_peer_id[0], (char*)&m_peer_id[0] + 20);
		stop_lsd();
		stop_upnp();
		stop_natpmp();
#ifndef TORRENT_DISABLE_DHT
		stop_dht();
#endif
		// close the listen sockets
		error_code ec;
		for (std::list<listen_socket_t>::iterator i = m_listen_sockets.begin()
			, end(m_listen_sockets.end()); i != end; ++i)
			i->sock->close(ec);
		m_listen_sockets.clear();
	}

	void session_impl::update_half_open()
	{
		if (m_settings.get_int(settings_pack::half_open_limit) <= 0)
			m_settings.set_int(settings_pack::half_open_limit, (std::numeric_limits<int>::max)());
		m_half_open.limit(m_settings.get_int(settings_pack::half_open_limit));
	}

#ifndef TORRENT_NO_DEPRECATE
	void session_impl::update_local_download_rate()
	{
		if (m_settings.get_int(settings_pack::local_download_rate_limit) < 0)
			m_settings.set_int(settings_pack::local_download_rate_limit, 0);
		set_download_rate_limit(m_local_peer_class
			, m_settings.get_int(settings_pack::local_download_rate_limit));
	}

	void session_impl::update_local_upload_rate()
	{
		if (m_settings.get_int(settings_pack::local_upload_rate_limit) < 0)
			m_settings.set_int(settings_pack::local_upload_rate_limit, 0);
		set_upload_rate_limit(m_local_peer_class
			, m_settings.get_int(settings_pack::local_upload_rate_limit));
	}
#endif

	void session_impl::update_download_rate()
	{
		if (m_settings.get_int(settings_pack::download_rate_limit) < 0)
			m_settings.set_int(settings_pack::download_rate_limit, 0);
		set_download_rate_limit(m_global_class
			, m_settings.get_int(settings_pack::download_rate_limit));
	}

	void session_impl::update_upload_rate()
	{
		if (m_settings.get_int(settings_pack::upload_rate_limit) < 0)
			m_settings.set_int(settings_pack::upload_rate_limit, 0);
		set_upload_rate_limit(m_global_class
			, m_settings.get_int(settings_pack::upload_rate_limit));
	}

	void session_impl::update_connections_limit()
	{
		if (m_settings.get_int(settings_pack::connections_limit) <= 0)
		{
			m_settings.set_int(settings_pack::connections_limit, (std::numeric_limits<int>::max)());
#if TORRENT_USE_RLIMIT
			rlimit l;
			if (getrlimit(RLIMIT_NOFILE, &l) == 0
				&& l.rlim_cur != RLIM_INFINITY)
			{
				m_settings.set_int(settings_pack::connections_limit
					, l.rlim_cur - m_settings.get_int(settings_pack::file_pool_size));
				if (m_settings.get_int(settings_pack::connections_limit) < 5)
					m_settings.set_int(settings_pack::connections_limit, 5);
			}
#endif
		}

		if (num_connections() > m_settings.get_int(settings_pack::connections_limit)
			&& !m_torrents.empty())
		{
			// if we have more connections that we're allowed, disconnect
			// peers from the torrents so that they are all as even as possible

			int to_disconnect = num_connections() - m_settings.get_int(settings_pack::connections_limit);

			int last_average = 0;
			int average = m_settings.get_int(settings_pack::connections_limit) / m_torrents.size();
	
			// the number of slots that are unused by torrents
			int extra = m_settings.get_int(settings_pack::connections_limit) % m_torrents.size();
	
			// run 3 iterations of this, then we're probably close enough
			for (int iter = 0; iter < 4; ++iter)
			{
				// the number of torrents that are above average
				int num_above = 0;
				for (torrent_map::iterator i = m_torrents.begin()
					, end(m_torrents.end()); i != end; ++i)
				{
					int num = i->second->num_peers();
					if (num <= last_average) continue;
					if (num > average) ++num_above;
					if (num < average) extra += average - num;
				}

				// distribute extra among the torrents that are above average
				if (num_above == 0) num_above = 1;
				last_average = average;
				average += extra / num_above;
				if (extra == 0) break;
				// save the remainder for the next iteration
				extra = extra % num_above;
			}

			for (torrent_map::iterator i = m_torrents.begin()
				, end(m_torrents.end()); i != end; ++i)
			{
				int num = i->second->num_peers();
				if (num <= average) continue;

				// distribute the remainder
				int my_average = average;
				if (extra > 0)
				{
					++my_average;
					--extra;
				}

				int disconnect = (std::min)(to_disconnect, num - my_average);
				to_disconnect -= disconnect;
				i->second->disconnect_peers(disconnect
					, error_code(errors::too_many_connections, get_libtorrent_category()));
			}
		}
	}

	void session_impl::update_dht_upload_rate_limit()
	{
		m_udp_socket.set_rate_limit(m_settings.get_int(settings_pack::dht_upload_rate_limit));	
	}

#ifndef TORRENT_NO_DEPRECATE
	void session_impl::update_rate_limit_utp()
	{
		if (m_settings.get_bool(settings_pack::rate_limit_utp))
		{
			// allow the global or local peer class to limit uTP peers
			m_peer_class_type_filter.add(peer_class_type_filter::utp_socket
				, m_local_peer_class);
			m_peer_class_type_filter.add(peer_class_type_filter::utp_socket
				, m_global_class);
			m_peer_class_type_filter.add(peer_class_type_filter::ssl_utp_socket
				, m_local_peer_class);
			m_peer_class_type_filter.add(peer_class_type_filter::ssl_utp_socket
				, m_global_class);
		}
		else
		{
			// don't add the global or local peer class to limit uTP peers
			m_peer_class_type_filter.remove(peer_class_type_filter::utp_socket
				, m_local_peer_class);
			m_peer_class_type_filter.remove(peer_class_type_filter::utp_socket
				, m_global_class);
			m_peer_class_type_filter.remove(peer_class_type_filter::ssl_utp_socket
				, m_local_peer_class);
			m_peer_class_type_filter.remove(peer_class_type_filter::ssl_utp_socket
				, m_global_class);
		}
	}

	void session_impl::update_ignore_rate_limits_on_local_network()
	{
		init_peer_class_filter(m_settings.get_bool(settings_pack::ignore_limits_on_local_network));
	}
#endif

	void session_impl::set_alert_dispatch(boost::function<void(std::auto_ptr<alert>)> const& fun)
	{
		m_alerts.set_dispatch_function(fun);
	}

	std::auto_ptr<alert> session_impl::pop_alert()
	{
		std::auto_ptr<alert> ret = m_alerts.get();
		if (alert_cast<save_resume_data_failed_alert>(ret.get())
			|| alert_cast<save_resume_data_alert>(ret.get()))
		{
			async_resume_dispatched(false);
		}
		return ret;
	}
	
	void session_impl::pop_alerts(std::deque<alert*>* alerts)
	{
		m_alerts.get_all(alerts);
		async_resume_dispatched(true);
	}

	alert const* session_impl::wait_for_alert(time_duration max_wait)
	{
		return m_alerts.wait_for_alert(max_wait);
	}

	void session_impl::set_alert_mask(boost::uint32_t m)
	{
		m_alerts.set_alert_mask(m);
	}

#ifndef TORRENT_NO_DEPRECATE
	size_t session_impl::set_alert_queue_size_limit(size_t queue_size_limit_)
	{
		m_settings.set_int(settings_pack::alert_queue_size, queue_size_limit_);
		return m_alerts.set_alert_queue_size_limit(queue_size_limit_);
	}
#endif

	void session_impl::start_lsd()
	{
		INVARIANT_CHECK;

		if (m_lsd) return;

		m_lsd = new lsd(m_io_service
			, m_listen_interface.address()
			, boost::bind(&session_impl::on_lsd_peer, this, _1, _2));
	}
	
	natpmp* session_impl::start_natpmp()
	{
		INVARIANT_CHECK;

		if (m_natpmp) return m_natpmp.get();

		// the natpmp constructor may fail and call the callbacks
		// into the session_impl.
		natpmp* n = new (std::nothrow) natpmp(m_io_service
			, m_listen_interface.address()
			, boost::bind(&session_impl::on_port_mapping
				, this, _1, _2, _3, _4, 0)
			, boost::bind(&session_impl::on_port_map_log
				, this, _1, 0));
		if (n == 0) return 0;

		m_natpmp = n;

		if (m_listen_interface.port() > 0)
		{
			remap_tcp_ports(1, m_listen_interface.port(), ssl_listen_port());
		}
		if (m_udp_socket.is_open())
		{
			m_udp_mapping[0] = m_natpmp->add_mapping(natpmp::udp
				, m_listen_interface.port(), m_listen_interface.port());
		}
		return n;
	}

	upnp* session_impl::start_upnp()
	{
		INVARIANT_CHECK;

		if (m_upnp) return m_upnp.get();

		// the upnp constructor may fail and call the callbacks
		upnp* u = new (std::nothrow) upnp(m_io_service
			, m_half_open
			, m_listen_interface.address()
			, m_settings.get_str(settings_pack::user_agent)
			, boost::bind(&session_impl::on_port_mapping
				, this, _1, _2, _3, _4, 1)
			, boost::bind(&session_impl::on_port_map_log
				, this, _1, 1)
			, m_settings.get_bool(settings_pack::upnp_ignore_nonrouters));

		if (u == 0) return 0;

		m_upnp = u;

		m_upnp->discover_device();
		if (m_listen_interface.port() > 0 || ssl_listen_port() > 0)
		{
			remap_tcp_ports(2, m_listen_interface.port(), ssl_listen_port());
		}
		if (m_udp_socket.is_open())
		{
			m_udp_mapping[1] = m_upnp->add_mapping(upnp::udp
				, m_listen_interface.port(), m_listen_interface.port());
		}
		return u;
	}

	void session_impl::stop_lsd()
	{
		if (m_lsd.get())
			m_lsd->close();
		m_lsd = 0;
	}
	
	void session_impl::stop_natpmp()
	{
		if (m_natpmp.get())
			m_natpmp->close();
		m_natpmp = 0;
	}
	
	void session_impl::stop_upnp()
	{
		if (m_upnp.get())
		{
			m_upnp->close();
			m_udp_mapping[1] = -1;
			m_tcp_mapping[1] = -1;
#ifdef TORRENT_USE_OPENSSL
			m_ssl_mapping[1] = -1;
#endif
		}
		m_upnp = 0;
	}
	
	bool session_impl::external_ip_t::add_vote(sha1_hash const& k, int type)
	{
		sources |= type;
		if (voters.find(k)) return false;
		voters.set(k);
		++num_votes;
		return true;
	}

	void session_impl::set_external_address(address const& ip
		, int source_type, address const& source)
	{
		if (is_any(ip)) return;
		if (is_local(ip)) return;
		if (is_loopback(ip)) return;

#if defined TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << ": set_external_address(" << print_address(ip)
			<< ", " << source_type << ", " << print_address(source) << ")\n";
#endif
		// this is the key to use for the bloom filters
		// it represents the identity of the voter
		sha1_hash k;
		hash_address(source, k);

		// do we already have an entry for this external IP?
		std::vector<external_ip_t>::iterator i = std::find_if(m_external_addresses.begin()
			, m_external_addresses.end(), boost::bind(&external_ip_t::addr, _1) == ip);

		if (i == m_external_addresses.end())
		{
			// each IP only gets to add a new IP once
			if (m_external_address_voters.find(k)) return;
		
			if (m_external_addresses.size() > 20)
			{
				if (random() < UINT_MAX / 2)
				{
#if defined TORRENT_VERBOSE_LOGGING
					(*m_logger) << time_now_string() << ": More than 20 slots, dopped\n";
#endif
					return;
				}
				// use stable sort here to maintain the fifo-order
				// of the entries with the same number of votes
				// this will sort in ascending order, i.e. the lowest
				// votes first. Also, the oldest are first, so this
				// is a sort of weighted LRU.
				std::stable_sort(m_external_addresses.begin(), m_external_addresses.end());
				// erase the first element, since this is the
				// oldest entry and the one with lowst number
				// of votes. This makes sense because the oldest
				// entry has had the longest time to receive more
				// votes to be bumped up
#if defined TORRENT_VERBOSE_LOGGING
				(*m_logger) << "  More than 20 slots, dopping "
					<< print_address(m_external_addresses.front().addr)
					<< " (" << m_external_addresses.front().num_votes << ")\n";
#endif
				m_external_addresses.erase(m_external_addresses.begin());
			}
			m_external_addresses.push_back(external_ip_t());
			i = m_external_addresses.end() - 1;
			i->addr = ip;
		}
		// add one more vote to this external IP
		if (!i->add_vote(k, source_type)) return;
		
		i = std::max_element(m_external_addresses.begin(), m_external_addresses.end());
		TORRENT_ASSERT(i != m_external_addresses.end());

#if defined TORRENT_VERBOSE_LOGGING
		for (std::vector<external_ip_t>::iterator j = m_external_addresses.begin()
			, end(m_external_addresses.end()); j != end; ++j)
		{
			(*m_logger) << ((j == i)?"-->":"   ")
				<< print_address(j->addr) << " votes: "
				<< j->num_votes << "\n";
		}
#endif
		if (i->addr == m_external_address) return;

#if defined TORRENT_VERBOSE_LOGGING
		(*m_logger) << "  external IP updated\n";
#endif
		m_external_address = i->addr;
		m_external_address_voters.clear();

		if (m_alerts.should_post<external_ip_alert>())
			m_alerts.post_alert(external_ip_alert(ip));

		// since we have a new external IP now, we need to
		// restart the DHT with a new node ID
#ifndef TORRENT_DISABLE_DHT
		if (m_dht)
		{
			entry s = m_dht->state();
			int cur_state = 0;
			int prev_state = 0;
			entry* nodes1 = s.find_key("nodes");
			if (nodes1 && nodes1->type() == entry::list_t) cur_state = nodes1->list().size();
			entry* nodes2 = m_dht_state.find_key("nodes");
			if (nodes2 && nodes2->type() == entry::list_t) prev_state = nodes2->list().size();
			if (cur_state > prev_state) m_dht_state = s;
			start_dht(m_dht_state);
		}
#endif
	}

	// decrement the refcount of the block in the disk cache
	// since the network thread doesn't need it anymore
	void session_impl::reclaim_block(block_cache_reference ref)
	{
		m_disk_thread.reclaim_block(ref);
	}

	char* session_impl::allocate_disk_buffer(char const* category)
	{
		return m_disk_thread.allocate_disk_buffer(category);
	}

	void session_impl::free_disk_buffer(char* buf)
	{
		m_disk_thread.free_disk_buffer(buf);
	}
	
	char* session_impl::allocate_disk_buffer(bool& exceeded
		, disk_observer* o
		, char const* category)
	{
		return m_disk_thread.allocate_disk_buffer(exceeded, o, category);
	}
	
	char* session_impl::allocate_buffer()
	{
		TORRENT_ASSERT(is_single_thread());

#ifdef TORRENT_BUFFER_STATS
		TORRENT_ASSERT(m_buffer_allocations >= 0);
		m_buffer_allocations++;
		m_buffer_usage_logger << log_time() << " protocol_buffer: "
			<< (m_buffer_allocations * send_buffer_size()) << std::endl;
#endif
#ifdef TORRENT_DISABLE_POOL_ALLOCATOR
		int num_bytes = send_buffer_size();
		return (char*)malloc(num_bytes);
#else
		return (char*)m_send_buffers.malloc();
#endif
	}

#ifdef TORRENT_BUFFER_STATS
	void session_impl::log_buffer_usage()
	{
		TORRENT_ASSERT(is_single_thread());

		int send_buffer_capacity = 0;
		int used_send_buffer = 0;
		for (connection_map::const_iterator i = m_connections.begin()
			, end(m_connections.end()); i != end; ++i)
		{
			send_buffer_capacity += (*i)->send_buffer_capacity();
			used_send_buffer += (*i)->send_buffer_size();
		}
		TORRENT_ASSERT(send_buffer_capacity >= used_send_buffer);
		m_buffer_usage_logger << log_time() << " send_buffer_size: " << send_buffer_capacity << std::endl;
		m_buffer_usage_logger << log_time() << " used_send_buffer: " << used_send_buffer << std::endl;
		m_buffer_usage_logger << log_time() << " send_buffer_utilization: "
			<< (used_send_buffer * 100.f / send_buffer_capacity) << std::endl;
	}
#endif

	void session_impl::free_buffer(char* buf)
	{
		TORRENT_ASSERT(is_single_thread());

#ifdef TORRENT_BUFFER_STATS
		m_buffer_allocations--;
		TORRENT_ASSERT(m_buffer_allocations >= 0);
		m_buffer_usage_logger << log_time() << " protocol_buffer: "
			<< (m_buffer_allocations * send_buffer_size()) << std::endl;
#endif
#ifdef TORRENT_DISABLE_POOL_ALLOCATOR
		free(buf);
#else
		m_send_buffers.free(buf);
#endif
	}	

#ifdef TORRENT_DEBUG
	void session_impl::check_invariant() const
	{
		TORRENT_ASSERT(is_single_thread());

		for (int l = 0; l < num_torrent_lists; ++l)
		{
			std::vector<torrent*> const& list = m_torrent_lists[l];
			for (std::vector<torrent*>::const_iterator i = list.begin()
				, end(list.end()); i != end; ++i)
			{
				TORRENT_ASSERT((*i)->m_links[l].in_list());
			}
		}
	
		std::set<int> unique;
		int num_active_downloading = 0;
		int num_active_finished = 0;
		int total_downloaders = 0;
		for (torrent_map::const_iterator i = m_torrents.begin()
			, end(m_torrents.end()); i != end; ++i)
		{
			boost::shared_ptr<torrent> t = i->second;
			if (t->want_peers_download()) ++num_active_downloading;
			if (t->want_peers_finished()) ++num_active_finished;
			TORRENT_ASSERT(!(t->want_peers_download() && t->want_peers_finished()));

			int pos = t->queue_position();
			if (pos < 0)
			{
				TORRENT_ASSERT(pos == -1);
				continue;
			}
			++total_downloaders;

			unique.insert(t->queue_position());
		}
		TORRENT_ASSERT(int(unique.size()) == total_downloaders);
		TORRENT_ASSERT(num_active_downloading == m_torrent_lists[torrent_want_peers_download].size());
		TORRENT_ASSERT(num_active_finished == m_torrent_lists[torrent_want_peers_finished].size());

		std::set<peer_connection*> unique_peers;
		TORRENT_ASSERT(m_settings.get_int(settings_pack::connections_limit) > 0);
		if (m_settings.get_int(settings_pack::choking_algorithm) == settings_pack::auto_expand_choker)
			TORRENT_ASSERT(m_allowed_upload_slots >= m_settings.get_int(settings_pack::unchoke_slots_limit));
		int unchokes = 0;
		int num_optimistic = 0;
		int disk_queue[2] = {0, 0};
		for (connection_map::const_iterator i = m_connections.begin();
			i != m_connections.end(); ++i)
		{
			TORRENT_ASSERT(*i);
			boost::shared_ptr<torrent> t = (*i)->associated_torrent().lock();
			TORRENT_ASSERT(unique_peers.find(i->get()) == unique_peers.end());
			unique_peers.insert(i->get());

			if ((*i)->m_channel_state[0] & peer_info::bw_disk) ++disk_queue[0];
			if ((*i)->m_channel_state[1] & peer_info::bw_disk) ++disk_queue[1];

			peer_connection* p = i->get();
			TORRENT_ASSERT(!p->is_disconnecting());
			if (p->ignore_unchoke_slots()) continue;
			if (!p->is_choked()) ++unchokes;
			if (p->peer_info_struct()
				&& p->peer_info_struct()->optimistically_unchoked)
			{
				++num_optimistic;
				TORRENT_ASSERT(!p->is_choked());
			}
			if (t && p->peer_info_struct() && !p->peer_info_struct()->web_seed)
			{
				TORRENT_ASSERT(t->get_policy().has_connection(p));
			}
		}

		TORRENT_ASSERT(disk_queue[0] == m_disk_queues[0]);
		TORRENT_ASSERT(disk_queue[1] == m_disk_queues[1]);

		if (m_settings.get_int(settings_pack::num_optimistic_unchoke_slots))
		{
			TORRENT_ASSERT(num_optimistic <= m_settings.get_int(settings_pack::num_optimistic_unchoke_slots));
		}

		if (m_num_unchoked != unchokes)
		{
			TORRENT_ASSERT(false);
		}
		for (torrent_map::const_iterator j
			= m_torrents.begin(); j != m_torrents.end(); ++j)
		{
			TORRENT_ASSERT(boost::get_pointer(j->second));
		}
	}
#endif

}}

