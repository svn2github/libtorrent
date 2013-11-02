/*

Copyright (c) 2008, Arvid Norberg
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

#include <fstream>
#include <deque>

#include "setup_transfer.hpp"

#include "libtorrent/session.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/http_parser.hpp"
#include "libtorrent/thread.hpp"

#include "libtorrent/thread.hpp"
#include <boost/tuple/tuple.hpp>
#include <boost/bind.hpp>
#include <boost/make_shared.hpp>

#include "test.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/create_torrent.hpp"
#include "libtorrent/socket_io.hpp" // print_endpoint
#include "libtorrent/socket_type.hpp"
#include "libtorrent/instantiate_connection.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/atomic.hpp"
#include "setup_transfer.hpp"

#ifdef TORRENT_USE_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/context.hpp>
#endif

#ifndef _WIN32
#include <spawn.h>
#include <signal.h>
#endif

#define DEBUG_WEB_SERVER 0

#define DLOG if (DEBUG_WEB_SERVER) fprintf

using namespace libtorrent;

static int tests_failure = 0;
static std::vector<std::string> failure_strings;

#if defined TORRENT_WINDOWS
#include <conio.h>
#endif

address rand_v4()
{
	return address_v4((rand() << 16 | rand()) & 0xffffffff);
}

#if TORRENT_USE_IPV6
address rand_v6()
{
	address_v6::bytes_type bytes;
	for (int i = 0; i < bytes.size(); ++i) bytes[i] = rand();
	return address_v6(bytes);
}
#endif

tcp::endpoint rand_tcp_ep()
{
	return tcp::endpoint(rand_v4(), rand() + 1024);
}

udp::endpoint rand_udp_ep()
{
	return udp::endpoint(rand_v4(), rand() + 1024);
}

void report_failure(char const* err, char const* file, int line)
{
	char buf[500];
	snprintf(buf, sizeof(buf), "\x1b[41m***** %s:%d \"%s\" *****\x1b[0m\n", file, line, err);
	fprintf(stderr, "\n%s\n", buf);
	failure_strings.push_back(buf);
	++tests_failure;
}

int print_failures()
{
	for (std::vector<std::string>::iterator i = failure_strings.begin()
		, end(failure_strings.end()); i != end; ++i)
	{
		fputs(i->c_str(), stderr);
	}
	fprintf(stderr, "\n\n\x1b[41m   == %d TEST(S) FAILED ==\x1b[0m\n\n\n", tests_failure);
	return tests_failure;
}

std::auto_ptr<alert> wait_for_alert(session& ses, int type, char const* name)
{
	std::auto_ptr<alert> ret;
	while (!ret.get())
	{
		ses.wait_for_alert(milliseconds(5000));
		std::deque<alert*> alerts;
		ses.pop_alerts(&alerts);
		for (std::deque<alert*>::iterator i = alerts.begin()
			, end(alerts.end()); i != end; ++i)
		{
			fprintf(stderr, "%s: %s: [%s] %s\n", time_now_string(), name, (*i)->what(), (*i)->message().c_str());
			if (!ret.get() && (*i)->type() == type)
			{
				ret = std::auto_ptr<alert>(*i);
			}
			else
				delete *i;
		}
	}
	return ret;
}

int load_file(std::string const& filename, std::vector<char>& v, libtorrent::error_code& ec, int limit)
{
	ec.clear();
	FILE* f = fopen(filename.c_str(), "rb");
	if (f == NULL)
	{
		ec.assign(errno, boost::system::get_generic_category());
		return -1;
	}

	int r = fseek(f, 0, SEEK_END);
	if (r != 0)
	{
		ec.assign(errno, boost::system::get_generic_category());
		fclose(f);
		return -1;
	}
	long s = ftell(f);
	if (s < 0)
	{
		ec.assign(errno, boost::system::get_generic_category());
		fclose(f);
		return -1;
	}

	if (s > limit)
	{
		fclose(f);
		return -2;
	}

	r = fseek(f, 0, SEEK_SET);
	if (r != 0)
	{
		ec.assign(errno, boost::system::get_generic_category());
		fclose(f);
		return -1;
	}

	v.resize(s);
	if (s == 0)
	{
		fclose(f);
		return 0;
	}

	r = fread(&v[0], 1, v.size(), f);
	if (r < 0)
	{
		ec.assign(errno, boost::system::get_generic_category());
		fclose(f);
		return -1;
	}

	fclose(f);

	if (r != s) return -3;

	return 0;
}

void save_file(char const* filename, char const* data, int size)
{
	error_code ec;
	file out(filename, file::write_only, ec);
	TEST_CHECK(!ec);
	if (ec)
	{
		fprintf(stderr, "ERROR opening file '%s': %s\n", filename, ec.message().c_str());
		return;
	}
	file::iovec_t b = { (void*)data, size_t(size) };
	out.writev(0, &b, 1, ec);
	TEST_CHECK(!ec);
	if (ec)
	{
		fprintf(stderr, "ERROR writing file '%s': %s\n", filename, ec.message().c_str());
		return;
	}

}

bool print_alerts(libtorrent::session& ses, char const* name
	, bool allow_disconnects, bool allow_no_torrents, bool allow_failed_fastresume
	, bool (*predicate)(libtorrent::alert*), bool no_output)
{
	bool ret = false;
	std::vector<torrent_handle> handles = ses.get_torrents();
	TEST_CHECK(!handles.empty() || allow_no_torrents);
	torrent_handle h;
	if (!handles.empty()) h = handles[0];
	std::deque<alert*> alerts;
	ses.pop_alerts(&alerts);
	for (std::deque<alert*>::iterator i = alerts.begin(); i != alerts.end(); ++i)
	{
		if (predicate && predicate(*i)) ret = true;
		if (peer_disconnected_alert* p = alert_cast<peer_disconnected_alert>(*i))
		{
			fprintf(stderr, "%s: %s: [%s] (%s): %s\n", time_now_string(), name, (*i)->what(), print_endpoint(p->ip).c_str(), p->message().c_str());
		}
		else if ((*i)->message() != "block downloading"
			&& (*i)->message() != "block finished"
			&& (*i)->message() != "piece finished"
			&& !no_output)
		{
			fprintf(stderr, "%s: %s: [%s] %s\n", time_now_string(), name, (*i)->what(), (*i)->message().c_str());
		}

		TEST_CHECK(alert_cast<fastresume_rejected_alert>(*i) == 0 || allow_failed_fastresume);
/*
		peer_error_alert* pea = alert_cast<peer_error_alert>(*i);
		if (pea)
		{
			fprintf(stderr, "%s: peer error: %s\n", time_now_string(), pea->error.message().c_str());
			TEST_CHECK((!handles.empty() && h.status().is_seeding)
				|| pea->error.message() == "connecting to peer"
				|| pea->error.message() == "closing connection to ourself"
				|| pea->error.message() == "duplicate connection"
				|| pea->error.message() == "duplicate peer-id"
				|| pea->error.message() == "upload to upload connection"
				|| pea->error.message() == "stopping torrent"
				|| (allow_disconnects && pea->error.message() == "Broken pipe")
				|| (allow_disconnects && pea->error.message() == "Connection reset by peer")
				|| (allow_disconnects && pea->error.message() == "no shared cipher")
				|| (allow_disconnects && pea->error.message() == "End of file."));
		}
*/

		invalid_request_alert* ira = alert_cast<invalid_request_alert>(*i);
		if (ira)
		{
			fprintf(stderr, "peer error: %s\n", ira->message().c_str());
			TEST_CHECK(false);
		}
		delete *i;
	}
	return ret;
}

bool listen_done = false;
bool listen_alert(libtorrent::alert* a)
{
	if (alert_cast<listen_failed_alert>(a)
		|| alert_cast<listen_succeeded_alert>(a))
		listen_done = true;
	return true;
}

void wait_for_listen(libtorrent::session& ses, char const* name)
{
	listen_done = false;
	alert const* a = 0;
	do
	{
		print_alerts(ses, name, true, true, true, &listen_alert, false);
		if (listen_done) break;
		a = ses.wait_for_alert(milliseconds(500));
	} while (a);
	// we din't receive a listen alert!
	TEST_CHECK(listen_done);
}

bool downloading_done = false;
bool downloading_alert(libtorrent::alert* a)
{
	state_changed_alert* sc = alert_cast<state_changed_alert>(a);
	if (sc && sc->state == torrent_status::downloading) 
		downloading_done = true;
	return true;
}

void wait_for_downloading(libtorrent::session& ses, char const* name)
{
	downloading_done = false;
	alert const* a = 0;
	do
	{
		print_alerts(ses, name, true, true, true, &downloading_alert, false);
		if (downloading_done) break;
		a = ses.wait_for_alert(milliseconds(500));
	} while (a);
}

void print_ses_rate(float time
	, libtorrent::torrent_status const* st1
	, libtorrent::torrent_status const* st2
	, libtorrent::torrent_status const* st3)
{
	fprintf(stderr, "%3.1fs | %dkB/s %dkB/s %d%% %d", time
		, int(st1->download_payload_rate / 1000)
		, int(st1->upload_payload_rate / 1000)
		, int(st1->progress * 100)
		, st1->num_peers);
	if (st2)
		std::cerr << " : "
			<< int(st2->download_payload_rate / 1000.f) << "kB/s "
			<< int(st2->upload_payload_rate / 1000.f) << "kB/s "
			<< int(st2->progress * 100) << "% "
			<< st2->num_peers
			<< " cc: " << st2->connect_candidates;
	if (st3)
		std::cerr << " : "
			<< int(st3->download_payload_rate / 1000.f) << "kB/s "
			<< int(st3->upload_payload_rate / 1000.f) << "kB/s "
			<< int(st3->progress * 100) << "% "
			<< st3->num_peers
			<< " cc: " << st3->connect_candidates;

	fprintf(stderr, "\n");
}

void test_sleep(int millisec)
{
	libtorrent::sleep(millisec);
}

#ifdef _WIN32
typedef DWORD pid_type;
#else
typedef pid_t pid_type;
#endif

struct proxy_t
{
	pid_type pid;
	int type;
};

// maps port to proxy type
static std::map<int, proxy_t> running_proxies;

void stop_proxy(int port)
{
	// don't shut down proxies until the test is
	// completely done. This saves a lot of time.
	// they're closed at the end of main() by
	// calling stop_all_proxies().
}

// returns 0 on failure, otherwise pid
pid_type async_run(char const* cmdline)
{
#ifdef _WIN32
	char buf[2048];
	snprintf(buf, sizeof(buf), "%s", cmdline);

	PROCESS_INFORMATION pi;
	STARTUPINFOA startup;
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput= GetStdHandle(STD_OUTPUT_HANDLE);
	startup.hStdError = GetStdHandle(STD_INPUT_HANDLE);
	int ret = CreateProcessA(NULL, buf, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &startup, &pi);

	if (ret == 0)
	{
		int error = GetLastError();
		fprintf(stderr, "failed (%d) %s\n", error, error_code(error, get_system_category()).message().c_str());
		return 0;
	}
	return pi.dwProcessId;
#else
	pid_type p;
	char arg_storage[4096];
	char* argp = arg_storage;
	std::vector<char*> argv;
	argv.push_back(argp);
	for (char const* in = cmdline; *in != '\0'; ++in)
	{
		if (*in != ' ')
		{
			*argp++ = *in;
			continue;
		}
		*argp++ = '\0';
		argv.push_back(argp);
	}
	*argp = '\0';
	argv.push_back(NULL);

	int ret = posix_spawnp(&p, argv[0], NULL, NULL, &argv[0], NULL);
	if (ret != 0)
	{
		fprintf(stderr, "failed (%d) %s\n", errno, strerror(errno));
		return 0;
	}
	return p;
#endif
}

void stop_all_proxies()
{
	std::map<int, proxy_t> proxies = running_proxies;
	for (std::map<int, proxy_t>::iterator i = proxies.begin()
		, end(proxies.end()); i != end; ++i)
	{
#ifdef _WIN32
		HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, i->second.pid);
		TerminateProcess(proc, 138);
		CloseHandle(proc);
#else
		printf("killing pid: %d\n", i->second.pid);
		kill(i->second.pid, SIGKILL);
#endif
		running_proxies.erase(i->second.pid);
	}
}

// returns a port on success and -1 on failure
int start_proxy(int proxy_type)
{
	using namespace libtorrent;

	for (std::map<int, proxy_t>::iterator i = running_proxies.begin()
		, end(running_proxies.end()); i != end; ++i)
	{
		if (i->second.type == proxy_type) return i->first;
	}

	unsigned int seed = total_microseconds(time_now_hires() - min_time());
	printf("random seed: %u\n", seed);
	std::srand(seed);
	int port = 5000 + (rand() % 55000);

	char const* type = "";
	char const* auth = "";
	char const* cmd = "";

	switch (proxy_type)
	{
		case proxy_settings::socks4:
			type = "socks4";
			auth = " --allow-v4";
			cmd = "python ../socks.py";
			break;
		case proxy_settings::socks5:
			type = "socks5";
			cmd = "python ../socks.py";
			break;
		case proxy_settings::socks5_pw:
			type = "socks5";
			auth = " --username testuser --password testpass";
			cmd = "python ../socks.py";
			break;
		case proxy_settings::http:
			type = "http";
			cmd = "python ../http.py";
			break;
		case proxy_settings::http_pw:
			type = "http";
			auth = " --username testuser --password testpass";
			cmd = "python ../http.py";
			break;
	}

	char buf[512];
	snprintf(buf, sizeof(buf), "%s --port %d%s", cmd, port, auth);

	fprintf(stderr, "%s starting proxy on port %d (%s %s)...\n", time_now_string(), port, type, auth);
	fprintf(stderr, "%s\n", buf);
	int r = async_run(buf);
	if (r == 0) exit(1);
	proxy_t t = { r, proxy_type };
	running_proxies.insert(std::make_pair(port, t));
	fprintf(stderr, "%s launched\n", time_now_string());
	test_sleep(500);
	return port;
}

using namespace libtorrent;

template <class T>
boost::shared_ptr<T> clone_ptr(boost::shared_ptr<T> const& ptr)
{
	return boost::make_shared<T>(*ptr);
}

void create_random_files(std::string const& path, const int file_sizes[], int num_files)
{
	error_code ec;
	char* random_data = (char*)malloc(300000);
	for (int i = 0; i != num_files; ++i)
	{
		std::generate(random_data, random_data + 300000, &std::rand);
		char filename[200];
		snprintf(filename, sizeof(filename), "test%d", i);
		std::string full_path = combine_path(path, filename);
		int to_write = file_sizes[i];
		file f(full_path, file::write_only, ec);
		if (ec) fprintf(stderr, "failed to create file \"%s\": (%d) %s\n"
			, full_path.c_str(), ec.value(), ec.message().c_str());
		size_type offset = 0;
		while (to_write > 0)
		{
			int s = (std::min)(to_write, 300000);
			file::iovec_t b = { random_data, size_t(s)};
			f.writev(offset, &b, 1, ec);
			if (ec) fprintf(stderr, "failed to write file \"%s\": (%d) %s\n"
				, full_path.c_str(), ec.value(), ec.message().c_str());
			offset += s;
			to_write -= s;
		}
	}
	free(random_data);
}

boost::shared_ptr<torrent_info> create_torrent(std::ostream* file, int piece_size
	, int num_pieces, bool add_tracker, std::string ssl_certificate)
{
	char const* tracker_url = "http://non-existent-name.com/announce";
	// excercise the path when encountering invalid urls
	char const* invalid_tracker_url = "http:";
	char const* invalid_tracker_protocol = "foo://non/existent-name.com/announce";
	
	file_storage fs;
	int total_size = piece_size * num_pieces;
	fs.add_file("temporary", total_size);
	libtorrent::create_torrent t(fs, piece_size);
	if (add_tracker)
	{
		t.add_tracker(tracker_url);
		t.add_tracker(invalid_tracker_url);
		t.add_tracker(invalid_tracker_protocol);
	}

	if (!ssl_certificate.empty())
	{
		std::vector<char> file_buf;
		error_code ec;
		int res = load_file(ssl_certificate, file_buf, ec);
		if (ec || res < 0)
		{
			fprintf(stderr, "failed to load SSL certificate: %s\n", ec.message().c_str());
		}
		else
		{
			std::string pem;
			std::copy(file_buf.begin(), file_buf.end(), std::back_inserter(pem));
			t.set_root_cert(pem);
		}
	}

	std::vector<char> piece(piece_size);
	for (int i = 0; i < int(piece.size()); ++i)
		piece[i] = (i % 26) + 'A';
	
	// calculate the hash for all pieces
	int num = t.num_pieces();
	sha1_hash ph = hasher(&piece[0], piece.size()).final();
	for (int i = 0; i < num; ++i)
		t.set_hash(i, ph);

	if (file)
	{
		while (total_size > 0)
		{
			file->write(&piece[0], (std::min)(int(piece.size()), total_size));
			total_size -= piece.size();
		}
	}
	
	std::vector<char> tmp;
	std::back_insert_iterator<std::vector<char> > out(tmp);

	entry tor = t.generate();

	bencode(out, tor);
	error_code ec;
	return boost::make_shared<torrent_info>(
		&tmp[0], tmp.size(), boost::ref(ec), 0);
}

boost::tuple<torrent_handle, torrent_handle, torrent_handle>
setup_transfer(session* ses1, session* ses2, session* ses3
	, bool clear_files, bool use_metadata_transfer, bool connect_peers
	, std::string suffix, int piece_size
	, boost::shared_ptr<torrent_info>* torrent, bool super_seeding
	, add_torrent_params const* p, bool stop_lsd, bool use_ssl_ports)
{
	assert(ses1);
	assert(ses2);

	if (stop_lsd)
	{
		ses1->stop_lsd();
		ses2->stop_lsd();
		if (ses3) ses3->stop_lsd();
	}

	// This has the effect of applying the global
	// rule to all peers, regardless of if they're local or not
	ip_filter f;
	f.add_rule(address_v4::from_string("0.0.0.0")
		, address_v4::from_string("255.255.255.255")
		, session::global_peer_class_id);
	ses1->set_peer_class_filter(f);
	ses2->set_peer_class_filter(f);
	if (ses3) ses3->set_peer_class_filter(f);

	settings_pack pack;
	pack.set_int(settings_pack::alert_mask, ~(alert::progress_notification | alert::stats_notification));
	if (ses3) pack.set_bool(settings_pack::allow_multiple_connections_per_ip, true);
	pack.set_int(settings_pack::mixed_mode_algorithm, settings_pack::prefer_tcp);
	pack.set_int(settings_pack::max_failcount, 1);
	ses1->apply_settings(pack);
	ses2->apply_settings(pack);
	if (ses3) ses3->apply_settings(pack);

	peer_id pid;
	std::generate(&pid[0], &pid[0] + 20, std::rand);
	ses1->set_peer_id(pid);
	std::generate(&pid[0], &pid[0] + 20, std::rand);
	ses2->set_peer_id(pid);
	assert(ses1->id() != ses2->id());
	if (ses3)
	{
		std::generate(&pid[0], &pid[0] + 20, std::rand);
		ses3->set_peer_id(pid);
		assert(ses3->id() != ses2->id());
	}

	boost::shared_ptr<torrent_info> t;
	if (torrent == 0)
	{
		error_code ec;
		create_directory("tmp1" + suffix, ec);
		std::ofstream file(combine_path("tmp1" + suffix, "temporary").c_str());
		t = ::create_torrent(&file, piece_size, 9, false);
		file.close();
		if (clear_files)
		{
			remove_all(combine_path("tmp2" + suffix, "temporary"), ec);
			remove_all(combine_path("tmp3" + suffix, "temporary"), ec);
		}
		char ih_hex[41];
		to_hex((char const*)&t->info_hash()[0], 20, ih_hex);
		fprintf(stderr, "generated torrent: %s tmp1%s/temporary\n", ih_hex, suffix.c_str());
	}
	else
	{
		t = *torrent;
	}

	// they should not use the same save dir, because the
	// file pool will complain if two torrents are trying to
	// use the same files
	add_torrent_params param;
	param.flags &= ~add_torrent_params::flag_paused;
	param.flags &= ~add_torrent_params::flag_auto_managed;
	if (p) param = *p;
	param.ti = clone_ptr(t);
	param.save_path = "tmp1" + suffix;
	param.flags |= add_torrent_params::flag_seed_mode;
	error_code ec;
	torrent_handle tor1 = ses1->add_torrent(param, ec);
	if (ec)
	{
		fprintf(stderr, "ses1.add_torrent: %s\n", ec.message().c_str());
		return boost::make_tuple(torrent_handle(), torrent_handle(), torrent_handle());
	}
	tor1.super_seeding(super_seeding);

	// the downloader cannot use seed_mode
	param.flags &= ~add_torrent_params::flag_seed_mode;

	TEST_CHECK(!ses1->get_torrents().empty());

	torrent_handle tor2;
	torrent_handle tor3;

	if (ses3)
	{
		param.ti = clone_ptr(t);
		param.save_path = "tmp3" + suffix;
		tor3 = ses3->add_torrent(param, ec);
		TEST_CHECK(!ses3->get_torrents().empty());
	}

  	if (use_metadata_transfer)
	{
		param.ti.reset();
		param.info_hash = t->info_hash();
	}
	else
	{
		param.ti = clone_ptr(t);
	}
	param.save_path = "tmp2" + suffix;

	tor2 = ses2->add_torrent(param, ec);
	TEST_CHECK(!ses2->get_torrents().empty());

	assert(ses1->get_torrents().size() == 1);
	assert(ses2->get_torrents().size() == 1);

//	test_sleep(100);

	if (connect_peers)
	{
		std::auto_ptr<alert> a;
/*		do
		{
			a = wait_for_alert(*ses2, state_changed_alert::alert_type, "ses2");
		} while (static_cast<state_changed_alert*>(a.get())->state != torrent_status::downloading);
*/
//		wait_for_alert(*ses1, torrent_finished_alert::alert_type, "ses1");

		error_code ec;
		if (use_ssl_ports)
		{
			fprintf(stderr, "%s: ses1: connecting peer port: %d\n", time_now_string(), int(ses2->ssl_listen_port()));
			tor1.connect_peer(tcp::endpoint(address::from_string("127.0.0.1", ec)
				, ses2->ssl_listen_port()));
		}
		else
		{
			fprintf(stderr, "%s: ses1: connecting peer port: %d\n", time_now_string(), int(ses2->listen_port()));
			tor1.connect_peer(tcp::endpoint(address::from_string("127.0.0.1", ec)
				, ses2->listen_port()));
		}

		if (ses3)
		{
			// give the other peers some time to get an initial
			// set of pieces before they start sharing with each-other

			if (use_ssl_ports)
			{
				fprintf(stderr, "ses3: connecting peer port: %d\n", int(ses2->ssl_listen_port()));
				tor3.connect_peer(tcp::endpoint(
					address::from_string("127.0.0.1", ec)
					, ses2->ssl_listen_port()));
				fprintf(stderr, "ses3: connecting peer port: %d\n", int(ses1->ssl_listen_port()));
				tor3.connect_peer(tcp::endpoint(
					address::from_string("127.0.0.1", ec)
					, ses1->ssl_listen_port()));
			}
			else
			{
				fprintf(stderr, "ses3: connecting peer port: %d\n", int(ses2->listen_port()));
				tor3.connect_peer(tcp::endpoint(
					address::from_string("127.0.0.1", ec)
					, ses2->listen_port()));
				fprintf(stderr, "ses3: connecting peer port: %d\n", int(ses1->listen_port()));
				tor3.connect_peer(tcp::endpoint(
					address::from_string("127.0.0.1", ec)
					, ses1->listen_port()));
			}
		}
	}

	return boost::make_tuple(tor1, tor2, tor3);
}

boost::asio::io_service* tracker_ios = 0;
boost::shared_ptr<libtorrent::thread> tracker_server;
libtorrent::mutex tracker_lock;
libtorrent::event tracker_initialized;

bool udp_failed = false;

void stop_tracker()
{
	fprintf(stderr, "%s: stop_tracker()\n", time_now_string());
	if (tracker_server && tracker_ios)
	{
		tracker_ios->stop();
		tracker_server->join();
		tracker_server.reset();
		delete tracker_ios;
		tracker_ios = 0;
	}
	fprintf(stderr, "%s: stop_tracker() done\n", time_now_string());
}

void udp_tracker_thread(int* port);

int start_tracker()
{
	stop_tracker();

	{
		libtorrent::mutex::scoped_lock l(tracker_lock);
		tracker_initialized.clear(l);
	}

	int port = 0;

	tracker_server.reset(new libtorrent::thread(boost::bind(&udp_tracker_thread, &port)));

	{
		libtorrent::mutex::scoped_lock l(tracker_lock);
		tracker_initialized.wait(l);
	}
//	test_sleep(100);
	return port;
}

atomic_count g_udp_tracker_requests(0);
atomic_count g_http_tracker_requests(0);

void on_udp_receive(error_code const& ec, size_t bytes_transferred, udp::endpoint const* from, char* buffer, udp::socket* sock)
{
	if (ec)
	{
		fprintf(stderr, "%s: UDP tracker, read failed: %s\n", time_now_string(), ec.message().c_str());
		return;
	}

	udp_failed = false;

	if (bytes_transferred < 16)
	{
		fprintf(stderr, "%s: UDP message too short (from: %s)\n", time_now_string(), print_endpoint(*from).c_str());
		return;
	}

	fprintf(stderr, "%s: UDP message %d bytes\n", time_now_string(), int(bytes_transferred));

	char* ptr = buffer;
	detail::read_uint64(ptr);
	boost::uint32_t action = detail::read_uint32(ptr);
	boost::uint32_t transaction_id = detail::read_uint32(ptr);

	error_code e;

	switch (action)
	{
		case 0: // connect

			fprintf(stderr, "%s: UDP connect from %s\n", time_now_string(), print_endpoint(*from).c_str());
			ptr = buffer;
			detail::write_uint32(0, ptr); // action = connect
			detail::write_uint32(transaction_id, ptr); // transaction_id
			detail::write_uint64(10, ptr); // connection_id
			sock->send_to(asio::buffer(buffer, 16), *from, 0, e);
			if (e) fprintf(stderr, "%s: send_to failed. ERROR: %s\n", time_now_string(), e.message().c_str());
			break;

		case 1: // announce

			++g_udp_tracker_requests;
			fprintf(stderr, "%s: UDP announce [%d]\n", time_now_string(), int(g_udp_tracker_requests));
			ptr = buffer;
			detail::write_uint32(1, ptr); // action = announce
			detail::write_uint32(transaction_id, ptr); // transaction_id
			detail::write_uint32(1800, ptr); // interval
			detail::write_uint32(1, ptr); // incomplete
			detail::write_uint32(1, ptr); // complete
			// 0 peers
			sock->send_to(asio::buffer(buffer, 20), *from, 0, e);
			if (e) fprintf(stderr, "%s: send_to failed. ERROR: %s\n", time_now_string(), e.message().c_str());
			break;
		case 2:
			// ignore scrapes
			fprintf(stderr, "%s: UDP scrape\n", time_now_string());
			break;
		default:
			fprintf(stderr, "%s: UDP unknown message: %d\n", time_now_string(), action);
			break;
	}
}

void udp_tracker_thread(int* port)
{
	tracker_ios = new io_service;

	udp::socket acceptor(*tracker_ios);
	error_code ec;
	acceptor.open(udp::v4(), ec);
	if (ec)
	{
		fprintf(stderr, "Error opening listen UDP socket: %s\n", ec.message().c_str());
		libtorrent::mutex::scoped_lock l(tracker_lock);
		tracker_initialized.signal(l);
		return;
	}
	acceptor.bind(udp::endpoint(address_v4::any(), 0), ec);
	if (ec)
	{
		fprintf(stderr, "Error binding UDP socket to port 0: %s\n", ec.message().c_str());
		libtorrent::mutex::scoped_lock l(tracker_lock);
		tracker_initialized.signal(l);
		return;
	}
	*port = acceptor.local_endpoint().port();

	fprintf(stderr, "%s: UDP tracker initialized on port %d\n", time_now_string(), *port);

	{
		libtorrent::mutex::scoped_lock l(tracker_lock);
		tracker_initialized.signal(l);
	}

	char buffer[2000];

	for (;;)
	{
		error_code ec;
		udp::endpoint from;
		udp_failed = true;
		acceptor.async_receive_from(
			asio::buffer(buffer, sizeof(buffer)), from, boost::bind(
				&on_udp_receive, _1, _2, &from, &buffer[0], &acceptor));
		tracker_ios->run_one(ec);
		if (udp_failed) return;

		if (ec)
		{
			fprintf(stderr, "%s: Error receiving on UDP socket: %s\n", time_now_string(), ec.message().c_str());
			libtorrent::mutex::scoped_lock l(tracker_lock);
			tracker_initialized.signal(l);
			return;
		}
		tracker_ios->reset();
	}
}

boost::asio::io_service* web_ios = 0;
boost::shared_ptr<libtorrent::thread> web_server;
libtorrent::mutex web_lock;
libtorrent::event web_initialized;
bool stop_thread = false;

static void terminate_web_thread()
{
	stop_thread = true;
	web_ios->stop();
	web_ios = 0;
}

void stop_web_server()
{
	fprintf(stderr, "%s: stop_web_server()\n", time_now_string());
	if (web_server && web_ios)
	{
		fprintf(stderr, "%s: stopping web server thread\n", time_now_string());
		web_ios->post(&terminate_web_thread);
		web_server->join();
		web_server.reset();
	}
	remove("server.pem");
	fprintf(stderr, "%s: stop_web_server() done\n", time_now_string());
}

void web_server_thread(int* port, bool ssl, bool chunked);

int start_web_server(bool ssl, bool chunked_encoding)
{
	stop_web_server();

	stop_thread = false;

	{
		libtorrent::mutex::scoped_lock l(web_lock);
		web_initialized.clear(l);
	}

	int port = 0;

	web_server.reset(new libtorrent::thread(boost::bind(
		&web_server_thread, &port, ssl, chunked_encoding)));

	{
		libtorrent::mutex::scoped_lock l(web_lock);
		web_initialized.wait(l);
	}

	// create this directory so that the path
	// "relative/../test_file" can resolve
	error_code ec;
	create_directory("relative", ec);
	if (ec)
		fprintf(stderr, "failed to create directory 'relative': %s\n", ec.message().c_str());
//	test_sleep(100);
	return port;
}

void send_response(socket_type& s, error_code& ec
	, int code, char const* status_message, char const** extra_header
	, int len)
{
	char msg[600];
	int pkt_len = snprintf(msg, sizeof(msg), "HTTP/1.1 %d %s\r\n"
		"content-length: %d\r\n"
		"%s"
		"%s"
		"%s"
		"%s"
		"\r\n"
		, code, status_message, len
		, extra_header[0]
		, extra_header[1]
		, extra_header[2]
		, extra_header[3]);
	DLOG(stderr, "[HTTP] >> %s\n", msg);
	write(s, boost::asio::buffer(msg, pkt_len), boost::asio::transfer_all(), ec);
	if (ec) fprintf(stderr, "[HTTP] *** send failed: %s\n", ec.message().c_str());
}

void on_accept(error_code& accept_ec, error_code const& ec, bool* done)
{
	accept_ec = ec;
	*done = true;
}

void send_content(socket_type& s, char const* file, int size, bool chunked)
{
	error_code ec;
	if (chunked)
	{
		int chunk_size = 13;
		char head[20];
		std::vector<boost::asio::const_buffer> bufs(3);
		bufs[2] = asio::const_buffer("\r\n", 2);
		while (chunk_size > 0)
		{
			chunk_size = std::min(chunk_size, size);
			int len = snprintf(head, sizeof(head), "%x\r\n", chunk_size);
			bufs[0] = asio::const_buffer(head, len);
			if (chunk_size == 0)
			{
				// terminate
				bufs.erase(bufs.begin()+1);
			}
			else
			{
				bufs[1] = asio::const_buffer(file, chunk_size);
			}
			write(s, bufs, boost::asio::transfer_all(), ec);
			if (ec) fprintf(stderr, "*** send failed: %s\n", ec.message().c_str());
			size -= chunk_size;
			file += chunk_size;
			chunk_size *= 2;
		}
	}
	else
	{
		write(s, boost::asio::buffer(file, size), boost::asio::transfer_all(), ec);
//		DLOG(stderr, " >> %s\n", std::string(file, size).c_str());
		if (ec) fprintf(stderr, "*** send failed: %s\n", ec.message().c_str());
	}
}

void on_read(error_code const& ec, size_t bytes_transferred, size_t* bt, error_code* e, bool* done)
{
	DLOG(stderr, "on_read %d [ ec: %s ]\n", int(bytes_transferred), ec.message().c_str());
	*bt = bytes_transferred;
	*e = ec;
	*done = true;
}

void on_read_timeout(error_code const& ec, bool* timed_out)
{
	if (ec) return;
	fprintf(stderr, "%s: [HTTP] read timed out\n", time_now_string());
	*timed_out = true;
}

void web_server_thread(int* port, bool ssl, bool chunked)
{
	io_service ios;
	socket_acceptor acceptor(ios);
	error_code ec;
	acceptor.open(tcp::v4(), ec);
	if (ec)
	{
		fprintf(stderr, "Error opening listen socket: %s\n", ec.message().c_str());
		libtorrent::mutex::scoped_lock l(web_lock);
		web_initialized.signal(l);
		return;
	}
	acceptor.set_option(socket_acceptor::reuse_address(true), ec);
	if (ec)
	{
		fprintf(stderr, "Error setting listen socket to reuse addr: %s\n", ec.message().c_str());
		libtorrent::mutex::scoped_lock l(web_lock);
		web_initialized.signal(l);
		return;
	}
	acceptor.bind(tcp::endpoint(address_v4::any(), 0), ec);
	if (ec)
	{
		fprintf(stderr, "Error binding listen socket to port 0: %s\n", ec.message().c_str());
		libtorrent::mutex::scoped_lock l(web_lock);
		web_initialized.signal(l);
		return;
	}
	*port = acceptor.local_endpoint().port();
	acceptor.listen(10, ec);
	if (ec)
	{
		fprintf(stderr, "Error listening on socket: %s\n", ec.message().c_str());
		libtorrent::mutex::scoped_lock l(web_lock);
		web_initialized.signal(l);
		return;
	}

	web_ios = &ios;

	char buf[10000];
	int len = 0;
	int offset = 0;
	bool connection_close = false;
	socket_type s(ios);
	void* ctx = 0;
#ifdef TORRENT_USE_OPENSSL
	boost::asio::ssl::context ssl_ctx(ios, boost::asio::ssl::context::sslv23_server);
	if (ssl)
	{
		ssl_ctx.use_certificate_chain_file("../ssl/server.pem");
		ssl_ctx.use_private_key_file("../ssl/server.pem", asio::ssl::context::pem);
		ssl_ctx.set_verify_mode(boost::asio::ssl::context::verify_none);

		ctx = &ssl_ctx;
	}
#endif

	proxy_settings p;
	instantiate_connection(ios, p, s, ctx);

	fprintf(stderr, "web server initialized on port %d%s\n", *port, ssl ? " [SSL]" : "");

	{
		libtorrent::mutex::scoped_lock l(web_lock);
		web_initialized.signal(l);
	}

	for (;;)
	{
		if (connection_close)
		{
			error_code ec;
#ifdef TORRENT_USE_OPENSSL
			if (ssl)
			{
				DLOG(stderr, "shutting down SSL connection\n");
				s.get<ssl_stream<stream_socket> >()->shutdown(ec);
				if (ec) fprintf(stderr, "SSL shutdown failed: %s\n", ec.message().c_str());
				ec.clear();
			}
#endif
			DLOG(stderr, "closing connection\n");
			s.close(ec);
			if (ec) fprintf(stderr, "close failed: %s\n", ec.message().c_str());
			connection_close = false;
		}
      ec.clear();

		if (!s.is_open())
		{
			len = 0;
			offset = 0;

			error_code ec;
			instantiate_connection(ios, p, s, ctx);
			stream_socket* sock;
#ifdef TORRENT_USE_OPENSSL
			if (ssl) sock = &s.get<ssl_stream<stream_socket> >()->next_layer();
			else
#endif
			sock = s.get<stream_socket>();

			bool accept_done = false;
			fprintf(stderr, "HTTP waiting for incoming connection\n");
			acceptor.async_accept(*sock, boost::bind(&on_accept, boost::ref(ec), _1, &accept_done));
			while (!accept_done)
			{
				error_code e;
				ios.reset();
				if (stop_thread || ios.run_one(e) == 0)
				{
					fprintf(stderr, "%s: io_service stopped: %s\n", time_now_string(), e.message().c_str());
					break;
				}
			}
			if (stop_thread) break;

			if (ec)
			{
				fprintf(stderr, "%s: accept failed: %s\n", time_now_string(), ec.message().c_str());
				return;
			}
			fprintf(stderr, "%s: accepting incoming connection\n", time_now_string());
			if (!s.is_open())
			{
				fprintf(stderr, "%s: incoming connection closed\n", time_now_string());
				continue;
			}

#ifdef TORRENT_USE_OPENSSL
			if (ssl)
			{
				DLOG(stderr, "%s: SSL handshake\n", time_now_string());
				s.get<ssl_stream<stream_socket> >()->accept_handshake(ec);
				if (ec)
				{
					fprintf(stderr, "SSL handshake failed: %s\n", ec.message().c_str());
					connection_close = true;
					continue;
				}
			}
#endif

		}

		http_parser p;
		bool failed = false;

		do
		{
			p.reset();
			bool error = false;

			p.incoming(buffer::const_interval(buf + offset, buf + len), error);

			char const* extra_header[4] = {"","","",""};

			TEST_CHECK(error == false);
			if (error)
			{
				fprintf(stderr, "HTTP parse failed\n");
				failed = true;
				break;
			}

			while (!p.finished())
			{
				TORRENT_ASSERT(len <= int(sizeof(buf)));
				size_t received = 0;
				bool done = false;
				bool timed_out = false;
				DLOG(stderr, "[HTTP] async_read_some %d bytes [ len: %d ]\n", int(sizeof(buf) - len), len);
				s.async_read_some(boost::asio::buffer(&buf[len]
					, sizeof(buf) - len), boost::bind(&on_read, _1, _2, &received, &ec, &done));
				deadline_timer timer(ios);
				timer.expires_from_now(milliseconds(500));
				timer.async_wait(boost::bind(&on_read_timeout, _1, &timed_out));

				while (!done && !timed_out)
				{
					error_code e;
					ios.reset();
					if (stop_thread || ios.run_one(e) == 0)
					{
						fprintf(stderr, "[HTTP] io_service stopped: %s\n", e.message().c_str());
						break;
					}
				}
				if (timed_out)
				{
					fprintf(stderr, "[HTTP] read timed out, closing connection\n");
					failed = true;
					break;
				}
//				fprintf(stderr, "read: %d\n", int(received));

				if (ec || received <= 0)
				{
					fprintf(stderr, "[HTTP] read failed: \"%s\" (%s) received: %d\n"
						, ec.message().c_str(), ec.category().name(), int(received));
               libtorrent::sleep(500);
					failed = true;
					break;
				}

				timer.cancel(ec);
				if (ec)
					fprintf(stderr, "[HTTP] timer.cancel failed: %s\n", ec.message().c_str());
				ec.clear();

				len += received;
		
				p.incoming(buffer::const_interval(buf + offset, buf + len), error);

				TEST_CHECK(error == false);
				if (error)
				{
					fprintf(stderr, "[HTTP] parse failed\n");
					failed = true;
					break;
				}
			}

			std::string connection = p.header("connection");
			std::string via = p.header("via");

			if (p.protocol() == "HTTP/1.0")
			{
				DLOG(stderr, "*** HTTP/1.0, closing connection when done\n");
				connection_close = true;
			}

			DLOG(stderr, "REQ: %s", std::string(buf + offset, p.body_start()).c_str());

			if (failed)
			{
				fprintf(stderr, "[HTTP] *** connection failed, closing connection\n");
				connection_close = true;
				break;
			}

			offset += int(p.body_start() + p.content_length());
//			fprintf(stderr, "offset: %d len: %d\n", offset, len);

			if (p.method() != "get" && p.method() != "post")
			{
				fprintf(stderr, "*** incorrect method: %s\n", p.method().c_str());
				connection_close = true;
				break;
			}

			std::string path = p.path();

			std::vector<char> file_buf;
			if (path.substr(0, 4) == "http")
			{
				// remove the http://hostname and the first / of the path
				path = path.substr(path.find("://")+3);
				path = path.substr(path.find_first_of('/')+1);
			}
			else
			{
				// remove the / from the path
				path = path.substr(1);
			}

			fprintf(stderr, "%s: [HTTP] %s\n", time_now_string(), path.c_str());

			if (path == "redirect")
			{
				extra_header[0] = "Location: /test_file\r\n";
				send_response(s, ec, 301, "Moved Permanently", extra_header, 0);
				break;
			}

			if (path == "infinite_redirect")
			{
				extra_header[0] = "Location: /infinite_redirect\r\n";
				send_response(s, ec, 301, "Moved Permanently", extra_header, 0);
				break;
			}

			if (path == "relative/redirect")
			{
				extra_header[0] = "Location: ../test_file\r\n";
				send_response(s, ec, 301, "Moved Permanently", extra_header, 0);
				break;
			}

			if (path.substr(0, 8) == "announce")
			{
				fprintf(stderr, "%s\n", path.c_str());
				entry announce;
				announce["interval"] = 1800;
				announce["complete"] = 1;
				announce["incomplete"] = 1;
				announce["peers"].string();
				std::vector<char> buf;
				bencode(std::back_inserter(buf), announce);
				++g_http_tracker_requests;
				fprintf(stderr, "[HTTP]: announce [%d]\n", int(g_http_tracker_requests));
			
				send_response(s, ec, 200, "OK", extra_header, buf.size());
				write(s, boost::asio::buffer(&buf[0], buf.size()), boost::asio::transfer_all(), ec);
				if (ec)
					fprintf(stderr, "[HTTP] *** send response failed: %s\n", ec.message().c_str());
				continue;
			}

			if (filename(path).substr(0, 5) == "seed?")
			{
				char const* piece = strstr(path.c_str(), "&piece=");
				if (piece == 0) piece = strstr(path.c_str(), "?piece=");
				if (piece == 0)
				{
					fprintf(stderr, "invalid web seed request: %s\n", path.c_str());
					break;
				}
				boost::uint64_t idx = atoi(piece + 7);
				char const* range = strstr(path.c_str(), "&ranges=");
				if (range == 0) range = strstr(path.c_str(), "?ranges=");
				int range_end = 0;
				int range_start = 0;
				if (range)
				{
					range_start = atoi(range + 8);
					range = strchr(range, '-');
					if (range == 0)
					{
						fprintf(stderr, "invalid web seed request: %s\n", path.c_str());
						break;
					}
					range_end = atoi(range + 1);
				}
				else
				{
					range_start = 0;
					// assume piece size of 64kiB
					range_end = 64*1024+1;
				}

				int size = range_end - range_start + 1;
				boost::uint64_t off = idx * 64 * 1024 + range_start;
				std::vector<char> file_buf;
				error_code ec;
				std::string file_path = path.substr(0, path.find_first_of('?'));
				int res = load_file(file_path.c_str(), file_buf, ec);

				if (res == -1 || file_buf.empty())
				{
					fprintf(stderr, "file not found: %s\n", file_path.c_str());
					send_response(s, ec, 404, "Not Found", extra_header, 0);
					continue;
				}
				send_response(s, ec, 200, "OK", extra_header, size);
				DLOG(stderr, "sending %d bytes of payload [%d, %d) piece: %d\n"
					, size, int(off), int(off + size), int(idx));
				write(s, boost::asio::buffer(&file_buf[0] + off, size)
					, boost::asio::transfer_all(), ec);
				if (ec)
					fprintf(stderr, "*** send failed: %s\n", ec.message().c_str());
				else
				{
					DLOG(stderr, "*** done\n");
				}

				memmove(buf, buf + offset, len - offset);
				len -= offset;
				offset = 0;
				continue;
			}

			DLOG(stderr, ">> serving file %s\n", path.c_str());
			error_code ec;
			int res = load_file(path, file_buf, ec, 8000000);
			if (res == -1)
			{
				fprintf(stderr, ">> file not found: %s\n", path.c_str());
				send_response(s, ec, 404, "Not Found", extra_header, 0);
				continue;
			}

			if (res != 0)
			{
				// this means the file was either too big or couldn't be read
				fprintf(stderr, ">> file too big: %s\n", path.c_str());
				send_response(s, ec, 503, "Internal Error", extra_header, 0);
				continue;
			}

			// serve file

			if (extension(path) == ".gz")
			{
				extra_header[0] = "Content-Encoding: gzip\r\n";
			}

			if (chunked)
			{
				extra_header[2] = "Transfer-Encoding: chunked\r\n";
			}

			if (!p.header("range").empty())
			{
				std::string range = p.header("range");
				int start, end;
				sscanf(range.c_str(), "bytes=%d-%d", &start, &end);
				char eh[400];
				snprintf(eh, sizeof(eh), "Content-Range: bytes %d-%d\r\n", start, end);
				extra_header[1] = eh;
				if (end - start + 1 >= 1000)
				{
					DLOG(stderr, "request size: %.2f kB\n", int(end - start + 1)/1000.f);
				}
				else
				{
					DLOG(stderr, "request size: %d Bytes\n", int(end - start + 1));
				}
				send_response(s, ec, 206, "Partial", extra_header, end - start + 1);
				if (!file_buf.empty())
				{
					send_content(s, &file_buf[0] + start, end - start + 1, chunked);
				}
				DLOG(stderr, "send %d bytes of payload\n", end - start + 1);
			}
			else
			{
				send_response(s, ec, 200, "OK", extra_header, file_buf.size());
				if (!file_buf.empty())
					send_content(s, &file_buf[0], file_buf.size(), chunked);
			}
			DLOG(stderr, "%d bytes left in receive buffer. offset: %d\n", len - offset, offset);
			memmove(buf, buf + offset, len - offset);
			len -= offset;
			offset = 0;
		} while (offset < len);
	}

	web_ios = 0;
	fprintf(stderr, "%s: [HTTP] exiting web server thread\n", time_now_string());
}


