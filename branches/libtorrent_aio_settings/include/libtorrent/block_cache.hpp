/*

Copyright (c) 2010, Arvid Norberg
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

#ifndef TORRENT_BLOCK_CACHE
#define TORRENT_BLOCK_CACHE

#include <boost/unordered_set.hpp>
#include <boost/cstdint.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/shared_array.hpp>
#include <list>
#include <vector>

#include "libtorrent/ptime.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/io_service_fwd.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/sliding_average.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/tailqueue.hpp"
#include "libtorrent/linked_list.hpp"
#include "libtorrent/disk_buffer_pool.hpp"

namespace libtorrent
{
	struct disk_io_job;
	class piece_manager;
	struct disk_buffer_pool;
	struct cache_status;
	struct hash_thread_interface;
	struct block_cache_reference;
	namespace aux { struct session_settings; }
	struct alert_dispatcher;

	struct partial_hash
	{
		partial_hash(): offset(0) {}
		// the number of bytes in the piece that has been hashed
		int offset;
		// the sha-1 context
		hasher h;
	};

	struct cached_block_entry
	{
		cached_block_entry(): buf(0), refcount(0), written(0), hitcount(0)
			, dirty(false), pending(false), uninitialized(false)
		{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			hashing = false;
			reading_count = 0;
			check_count = 0;
#endif
		}

		char* buf;

		// the number of references to this buffer. These references
		// might be in outstanding asyncronous requests or in peer
		// connection send buffers. We can't free the buffer until
		// all references are gone and refcount reaches 0. The buf
		// pointer in this struct doesn't count as a reference and
		// is always the last to be cleared
		boost::uint32_t refcount:15;

		// this block has been written to disk
		bool written:1;

		// the number of times this block has been copied out of
		// the cache, serving a request.
		boost::uint32_t hitcount:13;

		// if this is true, this block needs to be written to
		// disk before it's freed. Typically all blocks in a piece
		// would either be dirty (write coalesce cache) or not dirty
		// (read-ahead cache). Once blocks are written to disk, the
		// dirty flag is cleared and effectively turns the block
		// into a read cache block
		bool dirty:1;

		// pending means that this buffer has not yet been filled in
		// with valid data. There's an outstanding read job for this.
		// If the dirty flag is set, it means there's an outstanding
		// write job to write this block.
		bool pending:1;

		// this is used for freshly allocated read buffers. For read
		// operations, the disk-I/O thread will look for this flag
		// when issueing read jobs.
		// it is not valid for this flag to be set for blocks where
		// the dirty flag is set.
		bool uninitialized:1;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		// this block is part of an outstanding hash job
		bool hashing:1;
		// this block is being used in this many peer's send buffers currently
		int reading_count;
		// the number of check_piece disk jobs that have a reference to this block
		int check_count;
#endif
	};

	// list_node is here to be able to link this cache entry
	// into one of the LRU lists
	struct cached_piece_entry : list_node
	{
		cached_piece_entry();
		~cached_piece_entry();

		// storage this piece belongs to
		boost::intrusive_ptr<piece_manager> storage;

		int get_piece() const { return piece; }
		void* get_storage() const { return storage.get(); }

		bool operator==(cached_piece_entry const& rhs) const
		{ return storage.get() == rhs.storage.get() && piece == rhs.piece; }

		// if this is set, we'll be calculating the hash
		// for this piece. This member stores the interim
		// state while we're calulcating the hash.
		partial_hash* hash;

		// set to a unique identifier of a peer that last
		// requested from this piece.
		void* last_requester;

		// the pointers to the block data. If this is a ghost
		// cache entry, there won't be any data here
		// TODO: could this be a scoped_array instead? does cached_piece_entry need to be copyable?
		boost::shared_array<cached_block_entry> blocks;

		// jobs that cannot be performed right now are put on
		// this queue and retried whenever something completes on this piece
		tailqueue deferred_jobs;

		// these are outstanding jobs, waiting to be
		// handled for this piece. For read pieces, these
		// are the write jobs that will be dispatched back
		// to the writing peer once their data hits disk.
		// for read jobs, these are outstanding read jobs
		// for this piece that are waiting for data to become
		// avaialable. Read jobs may be overlapping.
		tailqueue jobs;

		// the last time a block was written to this piece
		// plus the minimum amount of time the block is guaranteed
		// to stay in the cache
		// TODO: now that there's a proper ARC cache, is this still necessary?
		ptime expire;

		boost::uint64_t piece:22;

		// the number of dirty blocks in this piece
		boost::uint64_t num_dirty:14;
		
		// the number of blocks in the cache for this piece
		boost::uint64_t num_blocks:14;

		// the total number of blocks in this piece (and the number
		// of elements in the blocks array)
		boost::uint64_t blocks_in_piece:14;

		// ---- 64 bit boundary ----

		// while we have an outstanding async hash operation
		// working on this piece, 'hashing' is set to the first block
		// in the range that is being hashed. When the operation
		// returns, this is set to -1. -1 means there's no
		// outstanding hash operation running
		enum { not_hashing = 0x3fff };
		boost::uint32_t hashing:14;

		// if this is true, whenever refcount hits 0, 
		// this piece should be deleted
		boost::uint32_t marked_for_deletion:1;

		// this is set to true once we flush blocks past
		// the hash cursor. Once this happens, there's
		// no point in keeping cache blocks around for
		// it in avoid_readback mode
		boost::uint32_t need_readback:1;

		// indicates which LRU list this piece is chained into
		enum cache_state_t
		{
			write_lru,
			read_lru1,
			read_lru1_ghost,
			read_lru2,
			read_lru2_ghost,
			num_lrus
		};

		boost::uint32_t cache_state:5;

		// unused
		boost::uint32_t padding:11;

		//	---- 32 bit boundary ---

		// the sum of all refcounts in all blocks
		boost::uint32_t refcount;	
	};

	inline std::size_t hash_value(cached_piece_entry const& p)
	{
		return std::size_t(p.storage.get()) + p.piece;
	}

	struct block_cache : disk_buffer_pool
	{
		block_cache(int block_size, hash_thread_interface& h, io_service& ios
			, alert_dispatcher* alert_disp);

	private:

		typedef boost::unordered_set<cached_piece_entry> cache_t;

	public:

		typedef cache_t::iterator iterator;

		void reclaim_block(block_cache_reference const& ref, tailqueue& jobs);

		// returns a range of all pieces. This migh be a very
		// long list, use carefully
		std::pair<iterator, iterator> all_pieces();
		int num_pieces() const { return m_pieces.size(); }

		list_iterator write_lru_pieces() const
		{ return m_lru[cached_piece_entry::write_lru].iterate(); }

		// deletes all pieces in the cache. asserts that there
		// are no outstanding jobs
		void clear(tailqueue& jobs);

		// mark this piece for deletion. If there are no outstanding
		// requests to this piece, it's removed immediately, and the
		// passed in iterator will be invalidated
		void mark_for_deletion(cached_piece_entry* p, tailqueue& jobs);

		// similar to mark_for_deletion, except for actually marking the
		// piece for deletion. If the piece was actually deleted,
		// the function returns true
		bool evict_piece(cached_piece_entry* p, tailqueue* jobs = 0);

		// if this piece is in L1 or L2 proper, move it to
		// its respective ghost list
		void move_to_ghost(cached_piece_entry* p);

		// returns the number of bytes read on success (cache hit)
		// -1 on cache miss
		int try_read(disk_io_job* j);

		// called when we're reading and we found the piece we're
		// reading from in the hash table (not necessarily that we
		// hit the block we needed)
		void cache_hit(cached_piece_entry* p, void* requester);

		// free block from piece entry
		void free_block(cached_piece_entry* pe, int block);

		// erase a piece (typically from the ghost list). Reclaim all
		// its blocks and unlink it and free it.
		void erase_piece(cached_piece_entry* p);

		// bump the piece 'p' to the back of the LRU list it's
		// in (back == MRU)
		// this is only used for the write cache
		void bump_lru(cached_piece_entry* p);

		// move p into the correct lru queue
		void update_cache_state(cached_piece_entry* p);

		// if the piece is marked for deletion and has a refcount
		// of 0, this function will post any sync jobs and
		// delete the piece from the cache
		bool maybe_free_piece(cached_piece_entry* p, tailqueue& jobs);

		// either returns the piece in the cache, or allocates
		// a new empty piece and returns it.
		// cache_state is one of cache_state_t enum
		cached_piece_entry* allocate_piece(disk_io_job const* j, int cache_state);

		// looks for this piece in the cache. If it's there, returns a pointer
		// to it, otherwise 0.
		cached_piece_entry* find_piece(block_cache_reference const& ref);
		cached_piece_entry* find_piece(disk_io_job const* j);
		cached_piece_entry* find_piece(cached_piece_entry const* pe);

		// allocates and marks the covered blocks as pending to
		// be filled in. This should be followed by issuing an
		// async read operation to read in the bytes. The function
		// returns the number of blocks that were allocated. If
		// it's less than the requested, it means the cache is
		// full and there's no space left
		int allocate_pending(cached_piece_entry* p
			, int start, int end, disk_io_job* j
			, int prio = 0, bool force = false);

		// clear the pending flags of the specified block range.
		// these blocks must be completely filled with valid
		// data from the disk before this call is made, unless
		// the disk call failed. If the disk read call failed,
		// the error code is passed in as 'ec'. In this case
		// all jobs for this piece are dispatched with the error
		// code. The io_service passed in is where the jobs are
		// dispatched
		void mark_as_done(cached_piece_entry* p, int begin, int end
			, tailqueue& jobs, tailqueue& restart_jobs, storage_error const& ec);

		// this is called by the hasher thread when hashing of
		// a range of block is complete.
		void hashing_done(cached_piece_entry* p, int begin, int end
			, tailqueue& jobs);

		// clear free all buffers marked as dirty with
		// refcount of 0.
		void abort_dirty(cached_piece_entry* p, tailqueue& jobs);

		// adds a block to the cache, marks it as dirty and
		// associates the job with it. When the block is
		// flushed, the callback is posted
		cached_piece_entry* add_dirty_block(disk_io_job* j);
	
#ifdef TORRENT_DEBUG
		void check_invariant() const;
#endif
		
		// try to remove num number of read cache blocks from the cache
		// pick the least recently used ones first
		// return the number of blocks that was requested to be evicted
		// that couldn't be
		int try_evict_blocks(int num, int prio, cached_piece_entry* ignore = 0);

		void get_stats(cache_status* ret) const;

		void add_hash_time(time_duration dt, int num_blocks)
		{
			TORRENT_ASSERT(num_blocks > 0);
			m_hash_time.add_sample(int(total_microseconds(dt / num_blocks)));
			m_cumulative_hash_time += total_microseconds(dt);
		}

		void pinned_change(int diff)
		{
			TORRENT_ASSERT(diff > 0 || m_pinned_blocks >= -diff);
			m_pinned_blocks += diff;
		}

		int pinned_blocks() const { return m_pinned_blocks; }

		void set_settings(aux::session_settings const& sett);

	private:

		// post operation-aborted errors for all jobs associated
		// with this piece
		void drain_jobs(cached_piece_entry* pe, tailqueue& jobs);

		void kick_hasher(cached_piece_entry* pe, int& hash_start, int& hash_end);

		void reap_piece_jobs(cached_piece_entry* pe, storage_error const& ec
			, int hash_start, int hash_end, tailqueue& jobs
			, bool reap_hash_jobs);

		// returns number of bytes read on success, -1 on cache miss
		// (just because the piece is in the cache, doesn't mean all
		// the blocks are there)
		int copy_from_piece(cached_piece_entry* p, disk_io_job* j);

		void free_piece(cached_piece_entry* p);
		int drain_piece_bufs(cached_piece_entry& p, std::vector<char*>& buf);

		// block container
		cache_t m_pieces;

		// linked list of all elements in m_pieces, in usage order
		// the most recently used are in the tail. iterating from head
		// to tail gives the least recently used entries first
		// the read-list is for read blocks and the write-list is for
		// dirty blocks that needs flushing before being evicted
		// [0] = write-LRU
		// [1] = read-LRU1
		// [2] = read-LRU1-ghost
		// [3] = read-LRU2
		// [4] = read-LRU2-ghost
		linked_list m_lru[cached_piece_entry::num_lrus];

		// this is used to determine whether to evict blocks from
		// L1 or L2.
		enum cache_op_t
		{
			cache_miss,
			ghost_hit_lru1,
			ghost_hit_lru2
		};
		int m_last_cache_op;

		// the number of pieces to keep in the ARC ghost lists
		// this is determined by being a fraction of the cache size
		int m_ghost_size;

		// the number of blocks in the cache
		// that are in the read cache
		boost::uint32_t m_read_cache_size;
		// the number of blocks in the cache
		// that are in the write cache
		boost::uint32_t m_write_cache_size;

		// the number of blocks that are currently sitting
		// in peer's send buffers. If two peers are sending
		// the same block, it counts as 2, even though there're
		// no buffer duplication
		boost::uint32_t m_send_buffer_blocks;

		boost::uint32_t m_blocks_read;
		boost::uint32_t m_blocks_read_hit;

		// the sum of all reference counts in all blocks
		boost::uint32_t m_refcount;

		// average hash time (in microseconds)
		sliding_average<512> m_hash_time;

		// microseconds
		size_type m_cumulative_hash_time;

		// the number of blocks with a refcount > 0, i.e.
		// they may not be evicted
		int m_pinned_blocks;

		// this is the object hash jobs are posted to
		hash_thread_interface& m_hash_thread;
	};

}

#endif // TORRENT_BLOCK_CACHE

