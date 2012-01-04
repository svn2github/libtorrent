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

#include "libtorrent/block_cache.hpp"
#include "libtorrent/disk_buffer_pool.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/disk_io_job.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/io_service_fwd.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/disk_io_thread.hpp" // disk_operation_failed
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/alloca.hpp"

#define DEBUG_CACHE 0

#define DLOG if (DEBUG_CACHE) fprintf

namespace libtorrent {

#if DEBUG_CACHE
void log_refcounts(cached_piece_entry const* pe)
{
	char out[4096];
	char* ptr = out;
	char* end = ptr + sizeof(out);
	ptr += snprintf(ptr, end - ptr, "piece: %d [ ", int(pe->piece));
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		ptr += snprintf(ptr, end - ptr, "%d ", int(pe->blocks[i].refcount));
	}
	strncpy(ptr, "]\n", end - ptr);
	DLOG(stderr, out);
}
#endif

cached_piece_entry::cached_piece_entry()
	: storage()
	, hash(0)
	, blocks()
	, jobs()
	, expire(min_time())
	, piece(0)
	, num_dirty(0)
	, num_blocks(0)
	, blocks_in_piece(0)
	, hashing(not_hashing)
	, marked_for_deletion(false)
	, need_readback(false)
	, cache_state(read_lru)
	, refcount(0)
{}

cached_piece_entry::~cached_piece_entry()
{
	TORRENT_ASSERT(refcount == 0);
#ifdef TORRENT_DEBUG
	for (int i = 0; i < blocks_in_piece; ++i)
	{
		TORRENT_ASSERT(blocks[i].buf == 0);
		TORRENT_ASSERT(!blocks[i].pending);
		TORRENT_ASSERT(blocks[i].refcount == 0);
		TORRENT_ASSERT(blocks[i].hashing == 0);
	}
#endif
	delete hash;
}

block_cache::block_cache(int block_size, hash_thread& h
	, io_service& ios
	, boost::function<void(alert*)> const& post_alert)
	: disk_buffer_pool(block_size, ios, post_alert)
	, m_read_cache_size(0)
	, m_write_cache_size(0)
	, m_send_buffer_blocks(0)
	, m_blocks_read(0)
	, m_blocks_read_hit(0)
	, m_cumulative_hash_time(0)
	, m_pinned_blocks(0)
	, m_hash_thread(h)
{}

// returns:
// -1: not in cache
// -2: no memory
int block_cache::try_read(disk_io_job* j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(j->buffer == 0);

	cached_piece_entry* p = find_piece(j);

	int ret = 0;

	// if the piece cannot be found in the cache,
	// it's a cache miss
	if (p == 0) return -1;

	bump_lru(p);

	ret = copy_from_piece(p, j);
	if (ret < 0) return ret;

	ret = j->d.io.buffer_size;
	++m_blocks_read;
	++m_blocks_read_hit;
	return ret;
}

void block_cache::bump_lru(cached_piece_entry* p)
{
	// move to the top of the LRU list
	TORRENT_ASSERT(p->cache_state < 2);
	linked_list* lru_list = &m_lru[p->cache_state];

	// move to the back (MRU) of the list
	lru_list->erase(p);
	lru_list->push_back(p);
	p->expire = time_now();
}

void block_cache::remove_lru(cached_piece_entry* p)
{
	TORRENT_ASSERT(p->cache_state < 2);
	linked_list* lru_list = &m_lru[p->cache_state];

	lru_list->erase(p);
}

void block_cache::update_cache_state(cached_piece_entry* p)
{
	int state = p->cache_state;
	int desired_state = cached_piece_entry::read_lru;
	if (p->num_dirty > 0 || p->hash != 0) desired_state = cached_piece_entry::write_lru;

	if (desired_state == state) return;

	TORRENT_ASSERT(state < 2);
	TORRENT_ASSERT(desired_state < 2);
	linked_list* src = &m_lru[state];
	linked_list* dst = &m_lru[desired_state];

	src->erase(p);
	dst->push_back(p);
	p->expire = time_now();
	p->cache_state = desired_state;
}

cached_piece_entry* block_cache::allocate_piece(disk_io_job const* j)
{
	INVARIANT_CHECK;

	cached_piece_entry* p = find_piece(j);
	if (p == 0)
	{
		int piece_size = j->storage->files()->piece_size(j->piece);
		int blocks_in_piece = (piece_size + block_size() - 1) / block_size();

		cached_piece_entry pe;
		pe.piece = j->piece;
		pe.storage = j->storage;
		pe.expire = time_now();
		pe.blocks_in_piece = blocks_in_piece;
		pe.blocks.reset(new (std::nothrow) cached_block_entry[blocks_in_piece]);
		pe.cache_state = cached_piece_entry::read_lru;
		TORRENT_ASSERT(pe.blocks);
		if (!pe.blocks) return 0;
		p = const_cast<cached_piece_entry*>(&*m_pieces.insert(pe).first);

		j->storage->add_piece(p);

		TORRENT_ASSERT(p->cache_state < 2);
		linked_list* lru_list = &m_lru[p->cache_state];
		lru_list->push_back(p);
	}
	return p;
}

cached_piece_entry* block_cache::add_dirty_block(disk_io_job* j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(j->buffer);

	cached_piece_entry* pe = allocate_piece(j);
	TORRENT_ASSERT(pe);
	if (pe == 0) return pe;

	int block = j->d.io.offset / block_size();
	TORRENT_ASSERT((j->d.io.offset % block_size()) == 0);

	// this only evicts read blocks

	int evict = num_to_evict(1);
	if (evict > 0) try_evict_blocks(evict, 1, pe);

	TORRENT_ASSERT(block < pe->blocks_in_piece);
	TORRENT_ASSERT(j->piece == pe->piece);
	TORRENT_ASSERT(!pe->marked_for_deletion);

	TORRENT_ASSERT(pe->blocks[block].refcount == 0);

	// we might have a left-over read block from
	// hash checking
	if (pe->blocks[block].buf != 0)
	{
		free_buffer(pe->blocks[block].buf);
		pe->blocks[block].buf = 0;
		TORRENT_ASSERT(pe->blocks[block].dirty == 0);
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		TORRENT_ASSERT(m_read_cache_size > 0);
		--m_read_cache_size;
	}

	pe->blocks[block].buf = j->buffer;

	pe->blocks[block].dirty = true;
	++pe->num_blocks;
	++pe->num_dirty;
	++m_write_cache_size;
	j->buffer = 0;
	TORRENT_ASSERT(j->piece == pe->piece);
	pe->jobs.push_back(j);

	update_cache_state(pe);

	bump_lru(pe);

	int hash_start = 0;
	int hash_end = 0;
	kick_hasher(pe, hash_start, hash_end);

	return pe;
}

std::pair<block_cache::iterator, block_cache::iterator> block_cache::all_pieces()
{
	return std::make_pair(m_pieces.begin(), m_pieces.end());
}

void block_cache::clear()
{
	std::vector<char*> buffers;
	for (iterator i = m_pieces.begin(); i != m_pieces.end(); ++i)
	{
		TORRENT_ASSERT(i->jobs.empty());
		cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*i);
		drain_piece_bufs(*pe, buffers);
	}
	if (!buffers.empty()) free_multiple_buffers(&buffers[0], buffers.size());
	m_pieces.clear();
	m_lru[0].get_all();
	m_lru[1].get_all();
}

bool block_cache::evict_piece(cached_piece_entry* pe)
{
	char** to_delete = TORRENT_ALLOCA(char*, pe->blocks_in_piece);
	int num_to_delete = 0;
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (pe->blocks[i].buf == 0 || pe->blocks[i].refcount > 0) continue;
		TORRENT_ASSERT(!pe->blocks[i].pending);
		TORRENT_ASSERT(pe->blocks[i].buf != 0);
		TORRENT_ASSERT(num_to_delete < pe->blocks_in_piece);
		to_delete[num_to_delete++] = pe->blocks[i].buf;
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		if (!pe->blocks[i].dirty)
		{
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
		}
		else
		{
			TORRENT_ASSERT(pe->num_dirty > 0);
			--pe->num_dirty;
			TORRENT_ASSERT(m_write_cache_size > 0);
			--m_write_cache_size;
		}
	}
	if (num_to_delete) free_multiple_buffers(to_delete, num_to_delete);

	if (pe->refcount == 0)
	{
		TORRENT_ASSERT(pe->jobs.empty());

		remove_lru(pe);
		pe->storage->remove_piece(pe);
		m_pieces.erase(*pe);
		return true;
	}

	update_cache_state(pe);

	return false;
}

void block_cache::mark_for_deletion(cached_piece_entry* p)
{
	INVARIANT_CHECK;

	DLOG(stderr, "[%p] block_cache mark-for-deletion "
		"piece: %d\n", this, int(p->piece));

	if (!evict_piece(p))
	{
		p->marked_for_deletion = true;
	}
}

// this only evicts read blocks. For write blocks, see
// try_flush_write_blocks in disk_io_thread.cpp
int block_cache::try_evict_blocks(int num, int prio, cached_piece_entry* ignore)
{
	INVARIANT_CHECK;

	if (num <= 0) return 0;

	DLOG(stderr, "[%p] try_evict_blocks: %d\n", this, num);

	char** to_delete = TORRENT_ALLOCA(char*, num);
	int num_to_delete = 0;

	// iterate over all blocks in order of last being used (oldest first) and as
	// long as we still have blocks to evict
	for (list_iterator i = m_lru[cached_piece_entry::read_lru].iterate(); i.get() && num > 0;)
	{
		cached_piece_entry* pe = reinterpret_cast<cached_piece_entry*>(i.get());

		if (pe == ignore)
		{
			i.next();
			continue;
		}

		if (pe->num_blocks == 0 && !pe->hash)
		{
#ifdef TORRENT_DEBUG
			for (int j = 0; j < pe->blocks_in_piece; ++j)
				TORRENT_ASSERT(pe->blocks[j].buf == 0);
#endif
			TORRENT_ASSERT(pe->refcount == 0);
			i.next();
			pe->storage->remove_piece(pe);
			remove_lru(pe);
			m_pieces.erase(*pe);
			continue;
		}

		// all blocks in this piece are dirty
		if (pe->num_dirty == pe->num_blocks)
		{
			i.next();
			continue;
		}

		// go through the blocks and evict the ones
		// that are not dirty and not referenced
		for (int j = 0; j < pe->blocks_in_piece && num > 0; ++j)
		{
			cached_block_entry& b = pe->blocks[j];
			if (b.buf == 0 || b.refcount > 0 || b.dirty || b.uninitialized || b.pending) continue;
			
			to_delete[num_to_delete++] = b.buf;
			b.buf = 0;
			TORRENT_ASSERT(pe->num_blocks > 0);
			--pe->num_blocks;
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
			--num;
		}

		if (pe->num_blocks == 0 && !pe->hash)
		{
#ifdef TORRENT_DEBUG
			for (int j = 0; j < pe->blocks_in_piece; ++j)
				TORRENT_ASSERT(pe->blocks[j].buf == 0);
#endif
			TORRENT_ASSERT(pe->refcount == 0);
			i.next();
			pe->storage->remove_piece(pe);
			remove_lru(pe);
			m_pieces.erase(*pe);
		}
		else i.next();
	}

	if (num_to_delete == 0) return num;

	DLOG(stderr, "[%p]    removed %d blocks\n", this, num_to_delete);

	free_multiple_buffers(to_delete, num_to_delete);

	return num;
}

// the priority controls which other blocks these new blocks
// are allowed to evict from the cache.
// 0 = regular read job
// 1 = write jobs
// 2 = required read jobs (like for read and hash)

// returns the number of blocks in the given range that are pending
// if this is > 0, it's safe to append the disk_io_job to the piece
// and it will be invoked once the pending blocks complete
// negative return values indicate different errors
// -1 = out of memory
// -2 = out of cache space

int block_cache::allocate_pending(cached_piece_entry* pe
	, int begin, int end, disk_io_job* j, int prio, bool force)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(begin >= 0);
	TORRENT_ASSERT(end <= pe->blocks_in_piece);
	TORRENT_ASSERT(begin < end);
	TORRENT_ASSERT(pe->piece == j->piece);
	TORRENT_ASSERT(pe->storage == j->storage);

	int ret = 0;

	int blocks_to_allocate = 0;

	for (int i = begin; i < end; ++i)
	{
		if (pe->blocks[i].buf) continue;
		if (pe->blocks[i].pending) continue;
		++blocks_to_allocate;
	}

	int evict = num_to_evict(blocks_to_allocate);
	if (evict > 0)
	{
		if (try_evict_blocks(evict, prio, pe) > 0
			&& prio < 1)
		{
			// we couldn't evict enough blocks to make room for this piece
			// we cannot return -1 here, since that means we're out of
			// memory. We're just out of cache space. -2 will tell the caller
			// to read the piece directly instead of going through the cache
			if (force) end = (std::min)(begin + 1, end);
			else return -2;
		}
	}

	for (int i = begin; i < end; ++i)
	{
		if (pe->blocks[i].buf) continue;
		if (pe->blocks[i].pending) continue;
		pe->blocks[i].buf = allocate_buffer("pending read");
		if (pe->blocks[i].buf == 0)
		{
			char** to_delete = TORRENT_ALLOCA(char*, end - begin);
			int num_to_delete = 0;
			for (int j = begin; j < end; ++j)
			{
				cached_block_entry& bl = pe->blocks[j];
				if (!bl.uninitialized) continue;
				TORRENT_ASSERT(bl.buf != 0);
				TORRENT_ASSERT(num_to_delete < end - begin);
				to_delete[num_to_delete++] = bl.buf;
				bl.buf = 0;
				bl.uninitialized = false;
				bl.dirty = false;
				TORRENT_ASSERT(m_read_cache_size > 0);
				--m_read_cache_size;
				TORRENT_ASSERT(pe->num_blocks > 0);
				--pe->num_blocks;
			}
			if (num_to_delete) free_multiple_buffers(to_delete, num_to_delete);

			return -1;
		}
		++pe->num_blocks;
		// this signals the disk_io_thread that this buffer should
		// be read in io_range()
		pe->blocks[i].uninitialized = true;
		++m_read_cache_size;
		++ret;
	}
	
	TORRENT_ASSERT(j->piece == pe->piece);
	if (ret >= 0)
	{
		// in case this was marked for deletion
		// don't do that anymore
		if (pe->num_dirty == 0)
		{
			DLOG(stderr, "[%p] block_cache allocate-pending unmark-for-deletion "
				"piece: %d\n", this, int(pe->piece));
			pe->marked_for_deletion = false;
		}
		TORRENT_ASSERT(j->piece == pe->piece);
		pe->jobs.push_back(j);
	}

	return ret;
}

void block_cache::mark_as_done(cached_piece_entry* pe, int begin, int end
		, tailqueue& jobs, storage_error const& ec)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(begin >= 0);
	TORRENT_ASSERT(end <= pe->blocks_in_piece);
	TORRENT_ASSERT(begin < end);

	DLOG(stderr, "[%p] block_cache mark_as_done error: %s\n"
		, this, ec.ec.message().c_str());

#if DEBUG_CACHE
	log_refcounts(pe);
#endif

	char** to_delete = TORRENT_ALLOCA(char*, pe->blocks_in_piece);
	int num_to_delete = 0;
	if (ec)
	{
		// fail all jobs for this piece with this error
		// and clear blocks

		for (int i = begin; i < end; ++i)
		{
			cached_block_entry& bl = pe->blocks[i];
			TORRENT_ASSERT(bl.refcount > 0);
			--bl.refcount;
			TORRENT_ASSERT(pe->refcount > 0);
			--pe->refcount;

			// TODO: if we have a hash job in the queue, that job
			// might hold references to the blocks as well. This
			// needs to be taken into account

			if (bl.refcount == 0)
			{
				TORRENT_ASSERT(m_pinned_blocks > 0);
				--m_pinned_blocks;
			}

			TORRENT_ASSERT(pe->blocks[i].pending);

			// if this block isn't pending, it was here before
			// this operation failed
			if (!bl.pending) continue;

			if (bl.dirty)
			{
				TORRENT_ASSERT(pe->num_dirty > 0);
				--pe->num_dirty;
				bl.dirty = false;
				TORRENT_ASSERT(m_write_cache_size > 0);
				--m_write_cache_size;
				++m_read_cache_size;
			}
			TORRENT_ASSERT(bl.buf != 0);

			bl.pending = false;

			// we can't free blocks that are in use by some 
			// async. operation
			if (bl.refcount > 0) continue;

			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;

			TORRENT_ASSERT(num_to_delete < pe->blocks_in_piece);
			to_delete[num_to_delete++] = bl.buf;
			bl.buf = 0;
			TORRENT_ASSERT(pe->num_blocks > 0);
			--pe->num_blocks;
		}
	}
	else
	{
		for (int i = begin; i < end; ++i)
		{
			TORRENT_ASSERT(pe->blocks[i].pending);
			TORRENT_ASSERT(pe->blocks[i].refcount > 0);
			--pe->blocks[i].refcount;
			TORRENT_ASSERT(pe->refcount > 0);
			--pe->refcount;
			pe->blocks[i].pending = false;
			if (pe->blocks[i].refcount == 0)
			{
				TORRENT_ASSERT(m_pinned_blocks > 0);
				--m_pinned_blocks;
			}

#if TORRENT_BUFFER_STATS
			rename_buffer(pe->blocks[i].buf, "read cache");
#endif

			if (!pe->blocks[i].dirty) continue;
			// turn this block into a read cache in case
			// it was a write cache
			TORRENT_ASSERT(pe->num_dirty > 0);
			--pe->num_dirty;
			pe->blocks[i].dirty = false;
			pe->blocks[i].written = true;
			TORRENT_ASSERT(m_write_cache_size > 0);
			--m_write_cache_size;
			++m_read_cache_size;
		}
	}

	if (num_to_delete) free_multiple_buffers(to_delete, num_to_delete);

	update_cache_state(pe);

	int hash_start = 0;
	int hash_end = 0;

	// if hash is set, we're trying to calculate the hash of this piece
	// if the jobs were submitted to another thread to be hashed,
	// hash_start and hash_end are both set to 0
	kick_hasher(pe, hash_start, hash_end);

	bool include_hash_jobs = hash_start != 0 || hash_end != 0;
	reap_piece_jobs(pe, ec, hash_start, hash_end, jobs, include_hash_jobs);

#if DEBUG_CACHE
	log_refcounts(pe);
#endif

	bool lower_fence = false;
	boost::intrusive_ptr<piece_manager> storage = pe->storage;

	if (pe->jobs.empty() && pe->storage->has_fence())
	{
		DLOG(stderr, "[%p] piece out of jobs. Count total jobs\n", this);
		// this piece doesn't have any outstanding jobs anymore
		// and we have a fence on the storage. Are all outstanding
		// jobs complete for this storage?

		int has_jobs = false;
		for (boost::unordered_set<cached_piece_entry*>::iterator i
			= pe->storage->cached_pieces().begin()
			, end(pe->storage->cached_pieces().end()); i != end; ++i)
		{
			cached_piece_entry* pe = *i;
			if (pe->jobs.empty()) continue;
			DLOG(stderr, "[%p] Found %d jobs on piece %d\n", this
				, int(pe->jobs.size()), int(pe->piece));
			has_jobs = true;
			break;
		}

		if (!has_jobs)
		{
			DLOG(stderr, "[%p] no more jobs. lower fence\n", this);
			// yes, all outstanding jobs are done, lower the fence
			lower_fence = true;
		}
	}

	DLOG(stderr, "[%p] block_cache mark_done mark-for-deletion: %d "
		"piece: %d refcount: %d\n", this, int(pe->marked_for_deletion)
		, int(pe->piece), int(pe->refcount));

	maybe_free_piece(pe, jobs);

	// lower the fence after we deleted the piece from the cache
	// to avoid inconsistent states when new jobs are issued
	if (lower_fence)
		storage->lower_fence();
}

void block_cache::kick_hasher(cached_piece_entry* pe, int& hash_start, int& hash_end)
{
	if (!pe->hash) return;
	if (pe->hashing != cached_piece_entry::not_hashing) return;

	int piece_size = pe->storage.get()->files()->piece_size(pe->piece);
	partial_hash& ph = *pe->hash;
	if (ph.offset < piece_size)
	{
		int cursor = ph.offset / block_size();
		int num_blocks = 0;

		int end = cursor;
		bool submitted = false;
		for (int i = cursor; i < pe->blocks_in_piece; ++i)
		{
			cached_block_entry& bl = pe->blocks[i];
			if ((bl.pending && !bl.dirty) || bl.buf == 0) break;
			++num_blocks;
			++end;
		}
		// once the hashing is done, a disk io job will be posted
		// to the disk io thread which will call hashing_done
		if (end > cursor)
		{
			ptime start_hash = time_now_hires();

			submitted = m_hash_thread.async_hash(pe, cursor, end);

			if (num_blocks > 0)
			{
				ptime done = time_now_hires();
				add_hash_time(done - start_hash, num_blocks);
			}

			DLOG(stderr, "[%p] block_cache async_hash "
				"piece: %d begin: %d end: %d submitted: %d\n", this
				, int(pe->piece), cursor, end, submitted);
		}
		if (!submitted)
		{
			hash_start = cursor;
			hash_end = end;
		}
		else
		{
			hash_start = 0;
			hash_end = 0;
		}
	}
}

void block_cache::reap_piece_jobs(cached_piece_entry* pe, storage_error const& ec
	, int hash_start, int hash_end, tailqueue& jobs
	, bool reap_hash_jobs)
{
#if DEBUG_CACHE
	log_refcounts(pe);
#endif

	tailqueue sync_jobs;

	disk_io_job* i = (disk_io_job*)pe->jobs.get_all();
	while (i)
	{
		disk_io_job* j = i;
		i = (disk_io_job*)i->next;
		j->next = 0;

		DLOG(stderr, "[%p] block_cache reap_piece_jobs j: %d\n"
			, this, j->action);
		TORRENT_ASSERT(j->piece == pe->piece);
		j->error = ec;
		int ret = 0;
		if (j->action == disk_io_job::read || j->action == disk_io_job::write)
		{
			ret = j->d.io.buffer_size;
		}
		if (ec)
		{
			// there was a read error, regardless of which blocks
			// this job is waiting for just return the failure
			if (j->action == disk_io_job::hash)
			{
				hash_start = j->d.io.offset;
				hash_end = pe->blocks_in_piece;

				// every hash job increases the refcount of all
				// blocks that it needs to complete when it's
				// issued to make sure they're not evicted before
				// they're hashed. As soon as they are hashed, the
				// refcount is decreased
				for (int b = hash_start; b < hash_end; ++b)
				{
					cached_block_entry& bl = pe->blocks[b];
					// obviously we need a buffer
					TORRENT_ASSERT(bl.buf != 0);
					TORRENT_ASSERT(bl.refcount >= bl.pending);
					--bl.refcount;
					TORRENT_ASSERT(pe->refcount >= bl.pending);
					--pe->refcount;
#ifdef TORRENT_DEBUG
					TORRENT_ASSERT(bl.check_count > 0);
					--bl.check_count;
#endif
					if (bl.refcount == 0)
					{
						TORRENT_ASSERT(m_pinned_blocks > 0);
						--m_pinned_blocks;
					}
				}
				j->d.io.offset = hash_end;
				DLOG(stderr, "[%p] block_cache reap_piece_jobs hash decrementing refcounts "
					"piece: %d begin: %d end: %d error: %s\n"
					, this, int(pe->piece), hash_start, hash_end, ec.ec.message().c_str());
			}

			ret = -1;
			goto post_job;
		}

		if (reap_hash_jobs && j->action == disk_io_job::hash)
		{
			TORRENT_ASSERT(pe->hash);

			partial_hash& ph = *pe->hash;

			// every hash job increases the refcount of all
			// blocks that it needs to complete when it's
			// issued to make sure they're not evicted before
			// they're hashed. As soon as they are hashed, the
			// refcount is decreased
			for (int b = j->d.io.offset; b < hash_end; ++b)
			{
				cached_block_entry& bl = pe->blocks[b];
				TORRENT_ASSERT(!bl.pending || bl.dirty);
				// obviously we need a buffer
				TORRENT_ASSERT(bl.buf != 0);
				TORRENT_ASSERT(bl.refcount >= bl.pending);
				--bl.refcount;
				TORRENT_ASSERT(pe->refcount >= bl.pending);
				--pe->refcount;
#ifdef TORRENT_DEBUG
				TORRENT_ASSERT(bl.check_count > 0);
				--bl.check_count;
#endif
				if (bl.refcount == 0)
				{
					TORRENT_ASSERT(m_pinned_blocks > 0);
					--m_pinned_blocks;
				}
			}
			j->d.io.offset = hash_end;
			DLOG(stderr, "[%p] block_cache reap_piece_jobs hash decrementing refcounts "
				"piece: %d begin: %d end: %d\n"
				, this, int(pe->piece), hash_start, hash_end);

			if (ph.offset < j->storage->files()->piece_size(j->piece))
			{
				DLOG(stderr, "[%p] block_cache reap_piece_jobs leaving job (incomplete hash) "
						"piece: %d offset: %d begin: %d end: %d piece_size: %d\n"
						, this, int(pe->piece)
						, ph.offset, hash_start, hash_end, j->storage->files()->piece_size(j->piece));
				TORRENT_ASSERT(j->piece == pe->piece);
				pe->jobs.push_back(j);
				continue;
			}
		}

		if (j->action == disk_io_job::hash)
		{
			TORRENT_ASSERT(j->piece == pe->piece);
			TORRENT_ASSERT(pe->hash);

			if (pe->hashing != cached_piece_entry::not_hashing
				|| pe->hash->offset < j->storage->files()->piece_size(pe->piece))
			{
				DLOG(stderr, "[%p] block_cache reap_piece_jobs leaving job (still hashing)"
					"piece: %d begin: %d end: %d\n", this, int(pe->piece)
					, hash_start, hash_end);
				TORRENT_ASSERT(j->piece == pe->piece);
				pe->jobs.push_back(j);
				continue;
			}
			TORRENT_ASSERT(pe->hash->offset == j->storage->files()->piece_size(pe->piece));
			partial_hash& ph = *pe->hash;

			memcpy(j->d.piece_hash, &ph.h.final()[0], 20);
			ret = 0;
			if (j->flags & disk_io_job::volatile_read)
			{
				pe->marked_for_deletion = true;
				DLOG(stderr, "[%p] block_cache reap_piece_jobs volatile read. "
					"piece: %d begin: %d end: %d\n", this, int(pe->piece)
					, hash_start, hash_end);
			}
			delete pe->hash;
			pe->hash = 0;

			update_cache_state(pe);
		}

		if (j->action == disk_io_job::read || j->action == disk_io_job::write)
		{
			// if the job overlaps any blocks that are still pending,
			// leave it in the list
			int first_block = j->d.io.offset / block_size();
			int last_block = (j->d.io.offset + j->d.io.buffer_size - 1) / block_size();
			TORRENT_ASSERT(first_block >= 0);
			TORRENT_ASSERT(last_block < pe->blocks_in_piece);
			TORRENT_ASSERT(first_block <= last_block);
			if (pe->blocks[first_block].pending || pe->blocks[last_block].pending
				|| pe->blocks[first_block].dirty || pe->blocks[last_block].dirty)
			{
				DLOG(stderr, "[%p] block_cache reap_piece_jobs leaving job (overlap) "
					"piece: %d begin: %d end: %d\n", this, int(pe->piece)
					, hash_start, hash_end);
				TORRENT_ASSERT(j->piece == pe->piece);
				pe->jobs.push_back(j);
				continue;
			}
		}

		if (j->action == disk_io_job::read)
		{
			ret = copy_from_piece(pe, j);
			if (ret == -1)
			{
				// this job is waiting for some other
				// blocks from this piece, we have to
				// leave it in here. It's not clear if this
				// would ever happen and in that case why
				TORRENT_ASSERT(j->piece == pe->piece);
				pe->jobs.push_back(j);
				continue;
			}
			else if (ret == -2)
			{
				ret = disk_io_thread::disk_operation_failed;
				j->error.ec = error::no_memory;
			}
			else
			{
				ret = j->d.io.buffer_size;
			}
		}

		if (j->action == disk_io_job::sync_piece)
		{
			sync_jobs.push_back(j);
			continue;
		}

post_job:
		TORRENT_ASSERT(j->piece == pe->piece);
		DLOG(stderr, "[%p] block_cache reap_piece_jobs post job "
			"piece: %d  jobtype: %d\n", this, int(j->piece), j->action);
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(j->callback_called == false);
		j->callback_called = true;
#endif
		j->ret = ret;
		jobs.push_back(j);
	}

	// handle the sync jobs last, to make sure all references are
	// released first
	i = (disk_io_job*)sync_jobs.get_all();
	if (pe->refcount == 0)
	{
		// post all the sync jobs
		while (i)
		{
			disk_io_job* j = i;
			i = (disk_io_job*)i->next;
			j->next = 0;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT(j->callback_called == false);
			j->callback_called = true;
#endif
			jobs.push_back(j);
		}
	}
	else
	{
		// save the jobs back again
		while (i)
		{
			disk_io_job* j = i;
			i = (disk_io_job*)i->next;
			j->next = 0;
			pe->jobs.push_back(j);
		}
	}
}

void block_cache::hashing_done(cached_piece_entry* pe, int begin, int end
	, tailqueue& jobs)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(begin == pe->hashing);
	TORRENT_ASSERT(pe->hashing != cached_piece_entry::not_hashing);
	TORRENT_ASSERT(pe->hash);
	pe->hashing = cached_piece_entry::not_hashing;

	DLOG(stderr, "[%p] block_cache hashing_done "
		"piece: %d begin: %d end: %d\n", this
		, int(pe->piece), begin, end);

#if DEBUG_CACHE
	log_refcounts(pe);
#endif

	for (int i = begin; i < end; ++i)
	{
		TORRENT_ASSERT(pe->blocks[i].refcount > 0);
		--pe->blocks[i].refcount;
		TORRENT_ASSERT(pe->refcount > 0);
		--pe->refcount;
#ifdef TORRENT_DEBUG
		TORRENT_ASSERT(pe->blocks[i].hashing);
		pe->blocks[i].hashing = false;
#endif
		if (pe->blocks[i].refcount == 0)
		{
			TORRENT_ASSERT(m_pinned_blocks > 0);
			--m_pinned_blocks;
		}
	}

#if DEBUG_CACHE
	log_refcounts(pe);
#endif

	DLOG(stderr, "[%p] block_cache hashing_done reap_piece_jobs "
		"piece: %d begin: %d end: %d\n", this, int(pe->piece), begin, end);

	reap_piece_jobs(pe, storage_error(), begin, end, jobs, true);

#if DEBUG_CACHE
	log_refcounts(pe);
#endif

	DLOG(stderr, "[%p] block_cache hashing_done kick_hasher "
		"piece: %d\n", this, int(pe->piece));

	int hash_start = 0;
	int hash_end = 0;
	kick_hasher(pe, hash_start, hash_end);

#if DEBUG_CACHE
	log_refcounts(pe);
#endif

	DLOG(stderr, "[%p] block_cache hashing_done delete? "
		"piece: %d refcount: %d marked_for_deletion: %d\n", this
		, int(pe->piece), int(pe->refcount), int(pe->marked_for_deletion));

	maybe_free_piece(pe, jobs);
}

void block_cache::abort_dirty(cached_piece_entry* pe, tailqueue& jobs)
{
	INVARIANT_CHECK;

	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (!pe->blocks[i].dirty || pe->blocks[i].refcount > 0) continue;
		TORRENT_ASSERT(!pe->blocks[i].pending);
		free_buffer(pe->blocks[i].buf);
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		TORRENT_ASSERT(m_write_cache_size > 0);
		--m_write_cache_size;
		TORRENT_ASSERT(pe->num_dirty > 0);
		--pe->num_dirty;
	}

	update_cache_state(pe);

	disk_io_job* i = (disk_io_job*)pe->jobs.get_all();
	while (i)
	{
		disk_io_job* j = (disk_io_job*)i;
		i = (disk_io_job*)i->next;
		j->next = 0;
		if (j->action != disk_io_job::write)
		{
			TORRENT_ASSERT(j->piece == pe->piece);
			pe->jobs.push_back(j);
			continue;
		}
		j->error.ec.assign(libtorrent::error::operation_aborted, get_system_category());
		TORRENT_ASSERT(j->callback);
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(j->callback_called == false);
		j->callback_called = true;
#endif
		j->ret = -1;
		jobs.push_back(j);
	}
}

// frees all buffers associated with this piece. May only
// be called for pieces with a refcount of 0
void block_cache::free_piece(cached_piece_entry* pe)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(pe->refcount == 0);
	// build a vector of all the buffers we need to free
	// and free them all in one go
	char** to_delete = TORRENT_ALLOCA(char*, pe->blocks_in_piece);
	int num_to_delete = 0;
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (pe->blocks[i].buf == 0) continue;
		TORRENT_ASSERT(pe->blocks[i].pending == false);
		TORRENT_ASSERT(pe->blocks[i].refcount == 0);
		TORRENT_ASSERT(num_to_delete < pe->blocks_in_piece);
		to_delete[num_to_delete++] = pe->blocks[i].buf;
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		if (pe->blocks[i].dirty)
		{
			TORRENT_ASSERT(m_write_cache_size > 0);
			--m_write_cache_size;
			TORRENT_ASSERT(pe->num_dirty > 0);
			--pe->num_dirty;
		}
		else
		{
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
		}
	}
	if (num_to_delete) free_multiple_buffers(to_delete, num_to_delete);
	update_cache_state(pe);
}

int block_cache::drain_piece_bufs(cached_piece_entry& p, std::vector<char*>& buf)
{
	int piece_size = p.storage->files()->piece_size(p.piece);
	int blocks_in_piece = (piece_size + block_size() - 1) / block_size();
	int ret = 0;

	for (int i = 0; i < blocks_in_piece; ++i)
	{
		if (p.blocks[i].buf == 0) continue;
		buf.push_back(p.blocks[i].buf);
		++ret;
		p.blocks[i].buf = 0;
		TORRENT_ASSERT(p.num_blocks > 0);
		--p.num_blocks;
		TORRENT_ASSERT(m_read_cache_size > 0);
		--m_read_cache_size;
	}
	update_cache_state(&p);
	return ret;
}

void block_cache::get_stats(cache_status* ret) const
{
	ret->blocks_read_hit = m_blocks_read_hit;
	ret->write_cache_size = m_write_cache_size;
	ret->read_cache_size = m_read_cache_size;
	ret->average_hash_time = m_hash_time.mean();
	ret->cumulative_hash_time = m_cumulative_hash_time;
	ret->pinned_blocks = m_pinned_blocks;
#ifndef TORRENT_NO_DEPRECATE
	ret->cache_size = m_read_cache_size + m_write_cache_size;
#endif
}

#ifdef TORRENT_DEBUG
void block_cache::check_invariant() const
{
	TORRENT_ASSERT(m_write_cache_size + m_read_cache_size <= in_use());

	int cached_write_blocks = 0;
	int cached_read_blocks = 0;
	int num_pinned = 0;

	for (int i = 0; i < 2; ++i)
	{
		ptime timeout = min_time();

		for (list_iterator p = m_lru[i].iterate(); p.get(); p.next())
		{
			cached_piece_entry* pe = (cached_piece_entry*)p.get();
			TORRENT_ASSERT(pe->cache_state == i);
			int desired_state = cached_piece_entry::read_lru;
			if (pe->num_dirty > 0 || pe->hash != 0)
				desired_state = cached_piece_entry::write_lru;
			TORRENT_ASSERT(desired_state == i);

			TORRENT_ASSERT(pe->expire >= timeout);
			timeout = pe->expire;
		}
	}

	for (iterator i = m_pieces.begin(), end(m_pieces.end()); i != end; ++i)
	{
		cached_piece_entry const& p = *i;
		TORRENT_ASSERT(p.blocks);
		
		TORRENT_ASSERT(p.storage);
		int piece_size = p.storage->files()->piece_size(p.piece);
		int blocks_in_piece = (piece_size + block_size() - 1) / block_size();
		int num_blocks = 0;
		int num_dirty = 0;
		int num_pending = 0;
		int num_refcount = 0;
		TORRENT_ASSERT(blocks_in_piece == p.blocks_in_piece);
		for (int k = 0; k < blocks_in_piece; ++k)
		{
			if (p.blocks[k].buf)
			{
#if !defined TORRENT_DISABLE_POOL_ALLOCATOR && defined TORRENT_EXPENSIVE_INVARIANT_CHECKS
				TORRENT_ASSERT(is_disk_buffer(p.blocks[k].buf));
#endif
				++num_blocks;
				if (p.blocks[k].dirty)
				{
					++num_dirty;
					++cached_write_blocks;
				}
				else
				{
					++cached_read_blocks;
				}
				if (p.blocks[k].pending) ++num_pending;
				if (p.blocks[k].refcount > 0) ++num_pinned;
			}
			else
			{
				TORRENT_ASSERT(!p.blocks[k].dirty);
				TORRENT_ASSERT(!p.blocks[k].pending);
				TORRENT_ASSERT(p.blocks[k].refcount == 0);
			}
			TORRENT_ASSERT(p.blocks[k].refcount >= 0);
			num_refcount += p.blocks[k].refcount;
		}
		TORRENT_ASSERT(num_blocks == p.num_blocks);
		TORRENT_ASSERT(num_pending <= p.refcount);
		TORRENT_ASSERT(num_refcount == p.refcount);
	}
	TORRENT_ASSERT(m_read_cache_size == cached_read_blocks);
	TORRENT_ASSERT(m_write_cache_size == cached_write_blocks);
	TORRENT_ASSERT(m_pinned_blocks == num_pinned);

#ifdef TORRENT_BUFFER_STATS
	int read_allocs = m_categories.find(std::string("read cache"))->second;
	int write_allocs = m_categories.find(std::string("write cache"))->second;
	TORRENT_ASSERT(cached_read_blocks == read_allocs);
	TORRENT_ASSERT(cached_write_blocks == write_allocs);
#endif
}
#endif

// returns
// -1: block not in cache
// -2: out of memory

int block_cache::copy_from_piece(cached_piece_entry* pe, disk_io_job* j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(j->buffer == 0);

	// copy from the cache and update the last use timestamp
	int block = j->d.io.offset / block_size();
	int block_offset = j->d.io.offset & (block_size()-1);
	int buffer_offset = 0;
	int size = j->d.io.buffer_size;
	int min_blocks_to_read = block_offset > 0 && (size > block_size() - block_offset) ? 2 : 1;
	TORRENT_ASSERT(size <= block_size());
	int start_block = block;
	if (pe->blocks[start_block].buf != 0
		&& !pe->blocks[start_block].pending
		&& min_blocks_to_read > 1)
		++start_block;

#ifdef TORRENT_DEBUG	
		int piece_size = j->storage->files()->piece_size(j->piece);
		int blocks_in_piece = (piece_size + block_size() - 1) / block_size();
		TORRENT_ASSERT(start_block < blocks_in_piece);
#endif

	// if block_offset > 0, we need to read two blocks, and then
	// copy parts of both, because it's not aligned to the block
	// boundaries
	if (pe->blocks[start_block].buf == 0
		|| pe->blocks[start_block].pending) return -1;

	if (min_blocks_to_read == 1 && (j->flags & disk_io_job::force_copy) == 0)
	{
		// special case for block aligned request
		// don't actually copy the buffer, just reference
		// the existing block
		if (pe->blocks[start_block].refcount == 0) ++m_pinned_blocks;
		++pe->blocks[start_block].refcount;
		TORRENT_ASSERT(pe->blocks[start_block].refcount > 0); // make sure it didn't wrap
		++pe->refcount;
		TORRENT_ASSERT(pe->refcount > 0); // make sure it didn't wrap
		j->d.io.ref.storage = j->storage.get();
		j->d.io.ref.piece = pe->piece;
		j->d.io.ref.block = start_block;
		j->buffer = pe->blocks[start_block].buf + (j->d.io.offset & (block_size()-1));
		++m_send_buffer_blocks;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		++pe->blocks[start_block].reading_count;
#endif
		return j->d.io.buffer_size;
	}

	j->buffer = allocate_buffer("send buffer");
	if (j->buffer == 0) return -2;

	// build a vector of all the buffers we need to free
	// and free them all in one go
	std::vector<char*> buffers;

	while (size > 0)
	{
		TORRENT_ASSERT(pe->blocks[block].buf);
		int to_copy = (std::min)(block_size()
			- block_offset, size);
		std::memcpy(j->buffer + buffer_offset
			, pe->blocks[block].buf + block_offset
			, to_copy);
		++pe->blocks[block].hitcount;
		size -= to_copy;
		block_offset = 0;
		buffer_offset += to_copy;
		// #error disabled because it breaks if there are multiple requests to the same block
		// the first request will go through, but the second one will read a NULL pointer
/*
		if (j->flags & disk_io_job::volatile_read)
		{
			// if volatile read cache is set, the assumption is
			// that no other peer is likely to request the same
			// piece. Therefore, for each request out of the cache
			// we clear the block that was requested and any blocks
			// the peer skipped
			for (int i = block; i >= 0 && pe->blocks[i].buf; --i)
			{
				if (pe->blocks[i].refcount > 0) continue;

				buffers.push_back(pe->blocks[i].buf);
				pe->blocks[i].buf = 0;
				TORRENT_ASSERT(pe->num_blocks > 0);
				--pe->num_blocks;
				TORRENT_ASSERT(m_read_cache_size > 0);
				--m_read_cache_size;
			}
		}
*/
		++block;
	}
	if (!buffers.empty()) free_multiple_buffers(&buffers[0], buffers.size());
	return j->d.io.buffer_size;
}

void block_cache::reclaim_block(block_cache_reference const& ref, tailqueue& jobs)
{
	cached_piece_entry* pe = find_piece(ref);
	TORRENT_ASSERT(pe->blocks[ref.block].refcount > 0);
	TORRENT_ASSERT(pe->blocks[ref.block].buf);
	--pe->blocks[ref.block].refcount;
	if (pe->blocks[ref.block].refcount == 0)
	{
		TORRENT_ASSERT(m_pinned_blocks > 0);
		--m_pinned_blocks;
	}
	TORRENT_ASSERT(pe->refcount > 0);
	--pe->refcount;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	TORRENT_ASSERT(pe->blocks[ref.block].reading_count > 0);
	--pe->blocks[ref.block].reading_count;
#endif

	TORRENT_ASSERT(m_send_buffer_blocks > 0);
	--m_send_buffer_blocks;

	maybe_free_piece(pe, jobs);
}

bool block_cache::maybe_free_piece(cached_piece_entry* pe, tailqueue& jobs)
{
	if (pe->refcount > 0 || !pe->marked_for_deletion) return false;

	boost::intrusive_ptr<piece_manager> s = pe->storage;

	DLOG(stderr, "[%p] block_cache maybe_free_piece "
		"piece: %d refcount: %d marked_for_deletion: %d\n", this
		, int(pe->piece), int(pe->refcount), int(pe->marked_for_deletion));

	// the refcount just reached 0, are there any sync-jobs to post?
	// post all the sync jobs
	disk_io_job* i = (disk_io_job*)pe->jobs.get_all();
	while (i)
	{
		disk_io_job* j = i;
		i = (disk_io_job*)i->next;
		j->next = 0;
		if (j->action == disk_io_job::sync_piece)
		{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT(j->callback_called == false);
			j->callback_called = true;
#endif
			jobs.push_back(j);
		}
		else
			pe->jobs.push_back(j);
	}

	TORRENT_ASSERT(pe->jobs.size() == 0);
	bool removed = evict_piece(pe);
	TORRENT_ASSERT(removed);
	if (!removed) return true;
	if (s->num_pieces() > 0) return true;

	disk_io_job* j = s->pop_abort_job();
	if (!j) return true;

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	TORRENT_ASSERT(j->callback_called == false);
	j->callback_called = true;
#endif
	jobs.push_back(j);

	return true;
}

cached_piece_entry* block_cache::find_piece(block_cache_reference const& ref)
{
	cached_piece_entry model;
	model.storage = (piece_manager*)ref.storage;
	model.piece = ref.piece;
	iterator i = m_pieces.find(model);
	TORRENT_ASSERT(i == m_pieces.end() || (i->storage == ref.storage && i->piece == ref.piece));
	if (i == m_pieces.end()) return 0;
	return const_cast<cached_piece_entry*>(&*i);
}

cached_piece_entry* block_cache::find_piece(cached_piece_entry const* pe)
{
	iterator i = m_pieces.find(*pe);
	TORRENT_ASSERT(i == m_pieces.end() || (i->storage == pe->storage && i->piece == pe->piece));
	if (i == m_pieces.end()) return 0;
	return const_cast<cached_piece_entry*>(&*i);
}

cached_piece_entry* block_cache::find_piece(disk_io_job const* j)
{
	cached_piece_entry model;
	model.storage = j->storage.get();
	model.piece = j->piece;
	iterator i = m_pieces.find(model);
	TORRENT_ASSERT(i == m_pieces.end() || (i->storage == j->storage && i->piece == j->piece));
	if (i == m_pieces.end()) return 0;
	return const_cast<cached_piece_entry*>(&*i);
}

}	
