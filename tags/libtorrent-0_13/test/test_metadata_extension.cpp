#include "libtorrent/session.hpp"
#include "libtorrent/hasher.hpp"
#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>

#include "test.hpp"
#include "setup_transfer.hpp"
#include "libtorrent/extensions/metadata_transfer.hpp"

using boost::filesystem::remove_all;
using boost::tuples::ignore;

void test_transfer(bool clear_files = true, bool disconnect = false)
{
	using namespace libtorrent;

	session ses1(fingerprint("LT", 0, 1, 0, 0), std::make_pair(48100, 49000));
	session ses2(fingerprint("LT", 0, 1, 0, 0), std::make_pair(49100, 50000));
	ses1.add_extension(&create_metadata_plugin);
	ses2.add_extension(&create_metadata_plugin);
	torrent_handle tor1;
	torrent_handle tor2;
#ifndef TORRENT_DISABLE_ENCRYPTION
	pe_settings pes;
	pes.out_enc_policy = pe_settings::forced;
	pes.in_enc_policy = pe_settings::forced;
	ses1.set_pe_settings(pes);
	ses2.set_pe_settings(pes);
#endif

 	boost::tie(tor1, tor2, ignore) = setup_transfer(&ses1, &ses2, 0, clear_files, true, true, "_meta");	

	for (int i = 0; i < 50; ++i)
	{
		// make sure this function can be called on
		// torrents without metadata
		if (!disconnect) tor2.status();
		print_alerts(ses1, "ses1", false, true);
		print_alerts(ses2, "ses2", false, true);

		if (disconnect && tor2.is_valid()) ses2.remove_torrent(tor2);
		if (!disconnect && tor2.has_metadata()) break;
		test_sleep(100);
	}

	if (disconnect) return;

	TEST_CHECK(tor2.has_metadata());
	std::cerr << "waiting for transfer to complete\n";

	for (int i = 0; i < 30; ++i)
	{
		torrent_status st1 = tor1.status();
		torrent_status st2 = tor2.status();

		std::cerr
			<< "\033[33m" << int(st1.upload_payload_rate / 1000.f) << "kB/s "
			<< st1.num_peers << ": "
			<< "\033[32m" << int(st2.download_payload_rate / 1000.f) << "kB/s "
			<< "\033[31m" << int(st2.upload_payload_rate / 1000.f) << "kB/s "
			<< "\033[0m" << int(st2.progress * 100) << "% "
			<< st2.num_peers
			<< std::endl;
		if (tor2.is_seed()) break;
		test_sleep(1000);
	}

	TEST_CHECK(tor2.is_seed());
	if (tor2.is_seed()) std::cerr << "done\n";

	using boost::filesystem::remove_all;
	remove_all("./tmp1_meta");
	remove_all("./tmp2_meta");
	remove_all("./tmp3_meta");
}

int test_main()
{
	using namespace libtorrent;
	using namespace boost::filesystem;

	// test to disconnect one client prematurely
	test_transfer(true, true);
	
	// test where one has data and one doesn't
	test_transfer(true);

	// test where both have data (to trigger the file check)
	test_transfer(false);

	remove_all("./tmp1");
	remove_all("./tmp2");

	return 0;
}

