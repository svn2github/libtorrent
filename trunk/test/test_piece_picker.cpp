#include "libtorrent/piece_picker.hpp"
#include "libtorrent/policy.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/bind.hpp>
#include <algorithm>
#include <vector>

#include "test.hpp"

using namespace libtorrent;

std::vector<bool> string2vec(char const* have_str)
{
	const int num_pieces = strlen(have_str);
	std::vector<bool> have(num_pieces, false);
	for (int i = 0; i < num_pieces; ++i)
		if (have_str[i] != ' ')  have[i] = true;
	return have;
}

// availability is a string where each character is the
// availability of that piece, '1', '2' etc.
// have_str is a string where each character represents a
// piece, ' ' means we don't have the piece and any other
// character means we have it
boost::shared_ptr<piece_picker> setup_picker(
	char const* availability
	, char const* have_str
	, char const* priority
	, char const* partial)
{
	const int blocks_per_piece = 4;
	const int num_pieces = strlen(availability);
	assert(int(strlen(have_str)) == num_pieces);

	boost::shared_ptr<piece_picker> p(new piece_picker(blocks_per_piece, num_pieces * blocks_per_piece));

	std::vector<bool> have = string2vec(have_str);

	std::vector<piece_picker::downloading_piece> unfinished;
	piece_picker::downloading_piece pp;
	std::vector<piece_picker::block_info> blocks(blocks_per_piece * num_pieces);

	for (int i = 0; i < num_pieces; ++i)
	{
		if (partial[i] == 0) break;

		if (partial[i] == ' ') continue;
		pp.index = i;
		pp.info = &blocks[i * blocks_per_piece];

		int blocks = 0;
		if (partial[i] >= '0' && partial[i] <= '9')
			blocks = partial[i] - '0';
		else
			blocks = partial[i] - 'a' + 10;

		if (blocks & 1)
			pp.info[0].state = piece_picker::block_info::state_finished;
		if (blocks & 2)
			pp.info[1].state = piece_picker::block_info::state_finished;
		if (blocks & 4)
			pp.info[2].state = piece_picker::block_info::state_finished;
		if (blocks & 8)
			pp.info[3].state = piece_picker::block_info::state_finished;
		unfinished.push_back(pp);
	}

	for (int i = 0; i < num_pieces; ++i)
	{
		if (priority[i] == 0) break;
		const int prio = priority[i] - '0';
		assert(prio >= 0);
		p->set_piece_priority(i, prio);

		TEST_CHECK(p->piece_priority(i) == prio);
	}

	std::vector<int> verify_pieces;
	p->files_checked(have, unfinished, verify_pieces);

	for (std::vector<piece_picker::downloading_piece>::iterator i = unfinished.begin()
		, end(unfinished.end()); i != end; ++i)
	{
		for (int j = 0; j < blocks_per_piece; ++j)
			TEST_CHECK(p->is_finished(piece_block(i->index, j)) == (i->info[j].state == piece_picker::block_info::state_finished));

		piece_picker::downloading_piece st;
		p->piece_info(i->index, st);
		TEST_CHECK(st.writing == 0);
		TEST_CHECK(st.requested == 0);
		TEST_CHECK(st.index == i->index);
		int counter = 0;
		for (int j = 0; j < blocks_per_piece; ++j)
			if (i->info[j].state == piece_picker::block_info::state_finished) ++counter;
			
		TEST_CHECK(st.finished == counter);
	}

	for (int i = 0; i < num_pieces; ++i)
	{
		if (!have[i]) continue;
		for (int j = 0; j < blocks_per_piece; ++j)
			TEST_CHECK(p->is_finished(piece_block(i, j)));
	}

	for (int i = 0; i < num_pieces; ++i)
	{
		const int avail = availability[i] - '0';
		assert(avail >= 0);
		
		for (int j = 0; j < avail; ++j) p->inc_refcount(i);
	}

	std::vector<int> availability_vec;
	p->get_availability(availability_vec);
	for (int i = 0; i < num_pieces; ++i)
	{
		const int avail = availability[i] - '0';
		assert(avail >= 0);
		TEST_CHECK(avail == availability_vec[i]);
	}

	return p;
}

bool verify_pick(boost::shared_ptr<piece_picker> p
	, std::vector<piece_block> const& picked)
{
	for (std::vector<piece_block>::const_iterator i = picked.begin()
		, end(picked.end()); i != end; ++i)
	{
		if (p->num_peers(*i) > 0) return false;
	}
	return true;
}

void print_pick(std::vector<piece_block> const& picked)
{
	for (int i = 0; i < int(picked.size()); ++i)
	{
		std::cout << "(" << picked[i].piece_index << ", " << picked[i].block_index << ") ";
	}
	std::cout << std::endl;
}

int test_main()
{

	tcp::endpoint endp;
	policy::peer peer_struct(endp, policy::peer::connectable, 0);
	std::vector<piece_block> picked;
	boost::shared_ptr<piece_picker> p;

	// make sure the block that is picked is from piece 1, since it
	// it is the piece with the lowest availability
	p = setup_picker("2223333", "* * *  ", "", "");
	picked.clear();
	p->pick_pieces(string2vec("*******"), picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() > 0);
	TEST_CHECK(picked.front().piece_index == 1);
	
// ========================================================

	// make sure the block that is picked is from piece 5, since it
	// has the highest priority among the available pieces
	p = setup_picker("1111111", "* * *  ", "1111122", "");
	picked.clear();
	p->pick_pieces(string2vec("****** "), picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() > 0);
	TEST_CHECK(picked.front().piece_index == 5);

// ========================================================

	// make sure the 4 blocks are picked from the same piece if
	// whole pieces are preferred. The only whole piece is 1.
	p = setup_picker("1111111", "       ", "1111111", "1023460");
	picked.clear();
	p->pick_pieces(string2vec("****** "), picked, 1, true, 0, piece_picker::fast, true);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() >= 4);
	for (int i = 0; i < 4 && i < int(picked.size()); ++i)
		TEST_CHECK(picked[i].piece_index == 1);

// ========================================================

	// test the distributed copies function. It should include ourself
	// in the availability. i.e. piece 0 has availability 2.
	// there are 2 pieces with availability 2 and 5 with availability 3
	p = setup_picker("1233333", "*      ", "", "");
	float dc = p->distributed_copies();
	TEST_CHECK(fabs(dc - (2.f + 5.f / 7.f)) < 0.01f);

// ========================================================
	
	// make sure filtered pieces are ignored
	p = setup_picker("1111111", "       ", "0010000", "");
	picked.clear();
	p->pick_pieces(string2vec("*** ** "), picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() > 0);
	TEST_CHECK(picked.front().piece_index == 2);

// ========================================================
	
	// make sure requested blocks aren't picked
	p = setup_picker("1234567", "       ", "", "");
	picked.clear();
	p->pick_pieces(string2vec("*******"), picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() > 0);
	TEST_CHECK(picked.front().piece_index == 0);
	piece_block first = picked.front();
	p->mark_as_downloading(picked.front(), &peer_struct, piece_picker::fast);
	TEST_CHECK(p->num_peers(picked.front()) == 1);
	picked.clear();
	p->pick_pieces(string2vec("*******"), picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() > 0);
	TEST_CHECK(picked.front() != first);
	TEST_CHECK(picked.front().piece_index == 0);

// ========================================================

	// test sequenced download
	p = setup_picker("1212211", "       ", "", "");
	picked.clear();
	p->set_sequenced_download_threshold(2);
	p->pick_pieces(string2vec(" * **  "), picked, 4 * 3, false, 0, piece_picker::fast, true);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() >= 4 * 3);
	print_pick(picked);
	for (int i = 0; i < 4 && i < int(picked.size()); ++i)
		TEST_CHECK(picked[i].piece_index == 1);
	for (int i = 4; i < 8 && i < int(picked.size()); ++i)
		TEST_CHECK(picked[i].piece_index == 3);
	for (int i = 8; i < 12 && i < int(picked.size()); ++i)
		TEST_CHECK(picked[i].piece_index == 4);

// ========================================================

	// test non-rarest-first mode
	p = setup_picker("1234567", "* * *  ", "1111122", "");
	picked.clear();
	p->pick_pieces(string2vec("****** "), picked, 5 * 4, false, 0, piece_picker::fast, false);
	TEST_CHECK(verify_pick(p, picked));
	print_pick(picked);
	TEST_CHECK(picked.size() == 4 * 4);

	for (int i = 0; i < 4 * 4 && i < int(picked.size()); ++i)
	{
		TEST_CHECK(picked[i].piece_index != 0);
		TEST_CHECK(picked[i].piece_index != 2);
		TEST_CHECK(picked[i].piece_index != 4);
	}

// ========================================================
	
	// test have_all and have_none
	p = setup_picker("1233333", "*      ", "", "");
	p->inc_refcount_all();
	dc = p->distributed_copies();
	TEST_CHECK(fabs(dc - (3.f + 5.f / 7.f)) < 0.01f);
	p->dec_refcount_all();
	dc = p->distributed_copies();
	TEST_CHECK(fabs(dc - (2.f + 5.f / 7.f)) < 0.01f);


// ========================================================
	
	// test have_all and have_none, with a sequenced download threshold
	p = setup_picker("1233333", "*      ", "", "");
	p->set_sequenced_download_threshold(3);
	p->inc_refcount_all();
	dc = p->distributed_copies();
	TEST_CHECK(fabs(dc - (3.f + 5.f / 7.f)) < 0.01f);
	p->dec_refcount_all();
	dc = p->distributed_copies();
	TEST_CHECK(fabs(dc - (2.f + 5.f / 7.f)) < 0.01f);
	p->dec_refcount(2);
	dc = p->distributed_copies();
	TEST_CHECK(fabs(dc - (2.f + 4.f / 7.f)) < 0.01f);
	p->mark_as_downloading(piece_block(1,0), &peer_struct, piece_picker::fast);
	p->mark_as_downloading(piece_block(1,1), &peer_struct, piece_picker::fast);
	p->we_have(1);
	dc = p->distributed_copies();
	TEST_CHECK(fabs(dc - (2.f + 5.f / 7.f)) < 0.01f);
	picked.clear();
	// make sure it won't pick the piece we just got
	p->pick_pieces(string2vec(" * ****"), picked, 1, false, 0, piece_picker::fast, false);
	TEST_CHECK(verify_pick(p, picked));
	TEST_CHECK(picked.size() >= 1);
	print_pick(picked);
	TEST_CHECK(picked.front().piece_index == 3);
	
// ========================================================
	
	// test unverified_blocks, marking blocks and get_downloader
	p = setup_picker("1111111", "       ", "", "0300700");
	TEST_CHECK(p->unverified_blocks() == 2 + 3);
	TEST_CHECK(p->get_downloader(piece_block(4, 0)) == 0);
	TEST_CHECK(p->get_downloader(piece_block(4, 1)) == 0);
	TEST_CHECK(p->get_downloader(piece_block(4, 2)) == 0);
	TEST_CHECK(p->get_downloader(piece_block(4, 3)) == 0);
	p->mark_as_downloading(piece_block(4, 3), &peer_struct, piece_picker::fast);
	TEST_CHECK(p->get_downloader(piece_block(4, 3)) == &peer_struct);
	piece_picker::downloading_piece st;
	p->piece_info(4, st);
	TEST_CHECK(st.requested == 1);
	TEST_CHECK(st.writing == 0);
	TEST_CHECK(st.finished == 3);
	TEST_CHECK(p->unverified_blocks() == 2 + 3);
	p->mark_as_writing(piece_block(4, 3), &peer_struct);
	TEST_CHECK(p->get_downloader(piece_block(4, 3)) == &peer_struct);
	p->piece_info(4, st);
	TEST_CHECK(st.requested == 0);
	TEST_CHECK(st.writing == 1);
	TEST_CHECK(st.finished == 3);
	TEST_CHECK(p->unverified_blocks() == 2 + 3);
	p->mark_as_finished(piece_block(4, 3), &peer_struct);
	TEST_CHECK(p->get_downloader(piece_block(4, 3)) == &peer_struct);
	p->piece_info(4, st);
	TEST_CHECK(st.requested == 0);
	TEST_CHECK(st.writing == 0);
	TEST_CHECK(st.finished == 4);
	TEST_CHECK(p->unverified_blocks() == 2 + 4);
	p->we_have(4);
	TEST_CHECK(p->get_downloader(piece_block(4, 3)) == 0);
	TEST_CHECK(p->unverified_blocks() == 2);



/*

	p.pick_pieces(peer1, picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(picked.size() == 1);
	TEST_CHECK(picked.front().piece_index == 2);

	// now pick a piece from peer2. The block is supposed to be
	// from piece 3, since it is the rarest piece that peer has.
	picked.clear();
	p.pick_pieces(peer2, picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(picked.size() == 1);
	TEST_CHECK(picked.front().piece_index == 3);

	// same thing for peer3.

	picked.clear();
	p.pick_pieces(peer3, picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(picked.size() == 1);
	TEST_CHECK(picked.front().piece_index == 5);

	// now, if all peers would have piece 1 (the piece we have partially)
	// it should be prioritized over picking a completely new piece.
	peer3[1] = true;
	p.inc_refcount(1);
	
	picked.clear();
	p.pick_pieces(peer3, picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(picked.size() == 1);
	TEST_CHECK(picked.front().piece_index == 1);
	// and the block picked should not be 0 or 2
	// since we already have those blocks

	TEST_CHECK(picked.front().block_index != 0);
	TEST_CHECK(picked.front().block_index != 2);

	// now, if we mark piece 1 and block 0 in piece 2
	// as being downloaded and picks a block from peer1,
	// it should pick a block from piece 2. But since
	// block 0 is marked as requested from another peer,
	// the piece_picker will continue to pick blocks
	// until it can return at least 1 block (since we
	// tell it we want one block) that is not being
	// downloaded from anyone else. This is to make it
	// possible for us to determine if we want to request
	// the block from more than one peer.
	// Both piece 1 and 2 are partial pieces, but pice
	// 2 is the rarest, so that's why it is picked.

	// we have block 0 and 2 already, so we can't mark
	// them as begin downloaded. 
	p.mark_as_downloading(piece_block(1, 1), &peer_struct, piece_picker::fast);
	p.mark_as_downloading(piece_block(1, 3), &peer_struct, piece_picker::fast);
	p.mark_as_downloading(piece_block(2, 0), &peer_struct, piece_picker::fast);

	std::vector<piece_picker::downloading_piece> const& downloads = p.get_download_queue();
	TEST_CHECK(downloads.size() == 2);
	TEST_CHECK(downloads[0].index == 1);
	TEST_CHECK(downloads[0].info[0].state == piece_picker::block_info::state_finished);
	TEST_CHECK(downloads[0].info[1].state == piece_picker::block_info::state_requested);
	TEST_CHECK(downloads[0].info[2].state == piece_picker::block_info::state_finished);
	TEST_CHECK(downloads[0].info[3].state == piece_picker::block_info::state_requested);

	TEST_CHECK(downloads[1].index == 2);
	TEST_CHECK(downloads[1].info[0].state == piece_picker::block_info::state_requested);
	TEST_CHECK(downloads[1].info[1].state == piece_picker::block_info::state_none);
	TEST_CHECK(downloads[1].info[2].state == piece_picker::block_info::state_none);
	TEST_CHECK(downloads[1].info[3].state == piece_picker::block_info::state_none);

	TEST_CHECK(p.is_requested(piece_block(1, 1)));
	TEST_CHECK(p.is_requested(piece_block(1, 3)));
	TEST_CHECK(p.is_requested(piece_block(2, 0)));
	TEST_CHECK(!p.is_requested(piece_block(2, 1)));

	picked.clear();
	p.pick_pieces(peer1, picked, 1, false, 0, piece_picker::fast, true);
	TEST_CHECK(picked.size() == 2);

	piece_block expected3[] = { piece_block(2, 0), piece_block(2, 1) };
	TEST_CHECK(std::equal(picked.begin()
		, picked.end(), expected3));

	// now, if we assume we're downloading at such a speed that
	// we might prefer to download whole pieces at a time from
	// this peer. It should not pick piece 1 or 2 (since those are
	// partially selected)

	picked.clear();
	p.pick_pieces(peer1, picked, 1, true, 0, piece_picker::fast, true);

	// it will pick 4 blocks, since we said we
	// wanted whole pieces.
	TEST_CHECK(picked.size() == 4);

	piece_block expected4[] =
	{
		piece_block(3, 0), piece_block(3, 1)
		, piece_block(3, 2), piece_block(3, 3)
	};

	TEST_CHECK(std::equal(picked.begin()
		, picked.end(), expected4));

	// now, try the same thing, but pick as many pieces as possible
	// to make sure it can still fall back on partial pieces

	picked.clear();
	p.pick_pieces(peer1, picked, 100, true, 0, piece_picker::fast, true);

	TEST_CHECK(picked.size() == 12);

	piece_block expected5[] =
	{
		piece_block(3, 0), piece_block(3, 1)
		, piece_block(3, 2), piece_block(3, 3)
		, piece_block(5, 0), piece_block(5, 1)
		, piece_block(5, 2), piece_block(5, 3)
		, piece_block(2, 0), piece_block(2, 1)
		, piece_block(2, 2), piece_block(2, 3)
	};

	TEST_CHECK(std::equal(picked.begin()
		, picked.end(), expected5));

	// now, try the same thing, but pick as many pieces as possible
	// to make sure it can still fall back on partial pieces

	picked.clear();
	p.pick_pieces(peer1, picked, 100, true, &peer_struct, piece_picker::fast, true);

	TEST_CHECK(picked.size() == 11);

	piece_block expected6[] =
	{
		piece_block(2, 1), piece_block(2, 2)
		, piece_block(2, 3)
		, piece_block(3, 0), piece_block(3, 1)
		, piece_block(3, 2), piece_block(3, 3)
		, piece_block(5, 0), piece_block(5, 1)
		, piece_block(5, 2), piece_block(5, 3)
	};

	TEST_CHECK(std::equal(picked.begin()
		, picked.end(), expected6));

	// make sure the piece picker allows filtered pieces
	// to become available
	p.mark_as_finished(piece_block(4, 2), 0);
*/
	return 0;
}

