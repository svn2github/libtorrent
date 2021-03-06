/*

Copyright (c) 2007, Arvid Norberg
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

#include "libtorrent/storage.hpp"
#include "libtorrent/disk_io_thread.hpp"
#include "libtorrent/disk_buffer_holder.hpp"
#include "libtorrent/alloca.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/file_pool.hpp"
#include <boost/scoped_array.hpp>
#include <boost/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include <set>
#include <vector>

#include "libtorrent/time.hpp"
#include "libtorrent/disk_buffer_pool.hpp"
#include "libtorrent/disk_io_job.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/alert_dispatcher.hpp"

#if TORRENT_USE_AIO_SIGNALFD
#include <sys/signalfd.h>
#endif

#if TORRENT_USE_AIO_PORTS
#include <port.h>
#endif

#if TORRENT_USE_AIO_KQUEUE
#include <sys/event.h>
#endif

#ifdef TORRENT_BSD
#include <sys/sysctl.h>
#endif

#if TORRENT_USE_RLIMIT
#include <sys/resource.h>
#endif

#ifdef TORRENT_LINUX
#include <linux/unistd.h>
#endif

#if TORRENT_USE_AIO
#include <signal.h>
#endif

#if TORRENT_USE_IOSUBMIT
#include <libaio.h>
#include <sys/eventfd.h>
#endif


#define DEBUG_STORAGE 0

#define DLOG if (DEBUG_STORAGE) fprintf

namespace libtorrent
{
	struct async_handler;

	bool same_sign(size_type a, size_type b) { return ((a < 0) == (b < 0)) || (a == 0) || (b == 0); }

	// this is posted to the network thread and run from there
	static void alert_callback(alert_dispatcher* disp, alert* a)
	{
		if (disp && disp->post_alert(a)) return;
		delete a;
	}

	bool between(size_type v, size_type b1, size_type b2)
	{
		return (b2 <= b1 && v <= b1 && v >= b2)
			|| (b2 >= b1 && v >= b1 && v <= b2);
	}

	bool is_ahead_of(size_type head, size_type v, int elevator)
	{
		return (v > head && elevator == 1) || (v < head && elevator == -1);
	}

	bool elevator_ordered(size_type v, size_type next, size_type prev, int elevator)
	{
		// if the point is in between prev and next, we should always sort it in
		// between them, i.e. we're in the right place.
		if (between(v, prev, next)) return true;

		// if the point is in the elevator direction from prev (and not
		// in between prev and next) and the next point is not in the
		// elevator direction, we've found the right spot as well
		if (same_sign(v - prev, elevator) && !same_sign(next - prev, elevator)) return true;

		// otherwise we need to keep iterating forward looking for the
		// right insertion point
		return false;
	}

	// free function to prepend a chain of aios to a list
	void prepend_aios(file::aiocb_t*& list, file::aiocb_t* aios)
	{
		if (aios == 0) return;
		if (list)
		{
			file::aiocb_t* last = aios;
			while (last->next)
			{
				TORRENT_ASSERT(last->next == 0 || last->next->prev == last);
				last = last->next;
			}
			last->next = list;
			list->prev = last;
		}
		list = aios;
		return;
	}

#ifdef TORRENT_DEBUG
	file::aiocb_t* find_aiocb(file::aiocb_t* haystack, file::aiocb_t const* needle)
	{
		while (haystack)
		{
			if (haystack->file_ptr == needle->file_ptr
				&& aio_offset(haystack) == aio_offset(needle))
			{
				TORRENT_ASSERT(aio_op(haystack) == aio_op(needle));
				return haystack;
			}
			haystack = haystack->next;
		}
		return 0;
	}
#endif

	// free function to append a chain of aios to a list
	// elevator direction determines how the new items are sorted
	// if it's 0, they are just prepended without any insertion sort
	// if it's -1, the direction from the first element is going down
	// towards lower offsets. If the element being inserted is higher,
	// it's inserted close to the end where the elevator has turned back.
	// if it's lower it's inserted early, as the offset would pass it.
	// a positive elevator direction has the same semantics but oposite order
	// returns the number of items in the aios chain
	TORRENT_EXPORT int append_aios(file::aiocb_t*& list_start, file::aiocb_t*& list_end
		, file::aiocb_t* aios, int elevator_direction, disk_io_thread* io)
	{
		int ret = 0;
		if (aios == 0) return 0;
		if (list_start == 0)
		{
			TORRENT_ASSERT(list_end == 0);
			list_start = aios;
			++ret;
			// find the last item in the list chain
			file::aiocb_t* last = list_start;
			while (last->next)
			{
				++ret;
				TORRENT_ASSERT(last->next == 0 || last->next->prev == last);
				TORRENT_ASSERT(last->prev == 0 || last->prev->next == last);
				last = last->next;
			}
			list_end = last;
			TORRENT_ASSERT(list_end->next == 0);
			return ret;
		}

		TORRENT_ASSERT(list_end->next == 0);

#if TORRENT_USE_SYNCIO
		// this is only optional when we have a phys_offset member
		// and we can sort by it, i.e. only when we're doing sync I/O
		if (elevator_direction == 0)
#endif
		{
			// append the aios chain at the end of the list
			list_end->next = aios;
			aios->prev = list_end;

			file::aiocb_t* last = list_end;
			while (last->next)
			{
				++ret;
				TORRENT_ASSERT(last->next == 0 || last->next->prev == last);
				TORRENT_ASSERT(last->prev == 0 || last->prev->next == last);
				last = last->next;
			}
			// update the end-of-list pointer
			list_end = last;
			TORRENT_ASSERT(list_end->next == 0);
			return ret;
		}

#if TORRENT_USE_SYNCIO
		// insert each aio ordered by phys_offset
		// according to elevator_direction

		ptime start_sort = time_now_hires();

		while (aios)
		{
			++ret;
			// pop the first element from aios into i
			file::aiocb_t* i = aios;
			aios = aios->next;
			i->next = 0;
			if (aios) aios->prev = 0;

			// find the right place in the current list to insert i
			// since the local elevator direction may change during
			// this scan, use a local copy
			// we want the ordering to look something like this:
			//
			//     /      or like this:      ^
			//    /     (depending on the   /
			// \         elevator          /
			//  \        direction)           \ 
			//   V                             \ 
			//

			/* for this to work, we need a tail pointer

			// first, if if i is "ahead of" list, we search from the
			// beginning of the right place for insertion. If i is "behind"
			// list, search from the end of the list
			size_type last_offset = j ? j->phys_offset : 0;
			if (is_ahead_of(last_offset, i->phys_offset, elevator_direction))
			{
				// scan from the beginning
			}
			else
			{
				// scan from the end
			}
			*/

			// the knee is where the elevator direction changes. We never
			// want to insert an element before the first one, since that
			// might make the drive head move backwards
			int elevator = elevator_direction;
			file::aiocb_t* last = 0;
			file::aiocb_t* j = list_start;
			size_type last_offset = j ? j->phys_offset : 0;
			// this will keep iterating as long as j->phys_offset < i->phys_offset
			// for negative elevator dir, and as long as j->phys_offset > i->phys_offset
			// for positive elevator dir.
			// never insert in front of the first element (j == list), since
			// that's the one that determines where the current head is
			while (j
				&& (!elevator_ordered(i->phys_offset, j->phys_offset, last_offset, elevator)
					|| j == list_start))
			{
				if (!same_sign(j->phys_offset - last_offset, elevator))
				{
					// the elevator direction changed
					elevator *= -1;
				}

				last_offset = j->phys_offset;
				last = j;
				j = j->next;
			}
			last->next = i;
			i->next = j;
			i->prev = last;
			if (j) j->prev = i;
			else list_end = i;
		}
		
		TORRENT_ASSERT(list_end->next == 0);

		if (io)
		{
			ptime done = time_now_hires();
			io->m_sort_time.add_sample(total_microseconds(done - start_sort));
			io->m_cache_stats.cumulative_sort_time += total_microseconds(done - start_sort);
		}

		return ret;
#endif // TORRENT_USE_SYNCIO
	}

#if (TORRENT_USE_AIO && !TORRENT_USE_AIO_SIGNALFD && !TORRENT_USE_AIO_PORTS && !TORRENT_USE_AIO_KQUEUE) \
	|| TORRENT_USE_SYNCIO
	// this semaphore is global so that the global signal
	// handler can access it. The side-effect of this is
	// that if there are more than one instances of libtorrent
	// they will all share a single semaphore, and 
	// all of them will wake up regardless of which one actually
	// was affected. This seems like a reasonable work-around
	// since it will most likely only affect unit-tests anyway

	// used to wake up the disk IO thread
	semaphore g_job_sem;

	// incremented in signal handler
	// for each job that's completed
	boost::detail::atomic_count g_completed_aios(0);
#endif

// ------- disk_io_thread ------

	disk_io_thread::disk_io_thread(io_service& ios
		, alert_dispatcher* alert_disp
		, void* userdata
		, int block_size)
		: m_abort(false)
		, m_userdata(userdata)
		, m_last_cache_expiry(min_time())
		, m_pending_buffer_size(0)
		, m_queue_buffer_size(0)
		, m_last_file_check(time_now_hires())
		, m_file_pool(40)
		, m_hash_thread(this)
		, m_disk_cache(block_size, m_hash_thread, ios, alert_disp)
		, m_last_stats_flip(time_now())
		, m_in_progress(0)
		, m_to_issue(0)
		, m_to_issue_end(0)
		, m_num_to_issue(0)
		, m_peak_num_to_issue(0)
		, m_outstanding_jobs(0)
		, m_peak_outstanding(0)
#if TORRENT_USE_SYNCIO
		, m_elevator_direction(1)
		, m_elevator_turns(0)
		, m_last_phys_off(0)
#endif
		, m_physical_ram(0)
		, m_ios(ios)
		, m_num_blocked_jobs(0)
		, m_work(io_service::work(m_ios))
		, m_last_disk_aio_performance_warning(min_time())
		, m_post_alert(alert_disp)
#if TORRENT_USE_SYNCIO
		, m_worker_thread(this)
#endif
#if TORRENT_USE_SUBMIT_THREADS
		, m_submit_queue(&m_aiocb_pool)
#endif
#if TORRENT_USE_OVERLAPPED
		, m_completion_port(CreateIoCompletionPort(INVALID_HANDLE_VALUE
			, NULL, 0, 1))
#endif
		, m_disk_io_thread(boost::bind(&disk_io_thread::thread_fun, this))
	{
		// Essentially all members
		// of this object are owned by the newly created thread.
		// initialize stuff in thread_fun().
#if TORRENT_USE_IOSUBMIT
		m_io_queue = 0;
		int ret = io_setup(4096, &m_io_queue);
		if (ret != 0)
		{
			// error handling!
			TORRENT_ASSERT(false);
		}
		m_disk_event_fd = eventfd(0, 0);
		if (m_disk_event_fd < 0)
		{
			TORRENT_ASSERT(false);
		}
		m_job_event_fd = eventfd(0, 0);
		if (m_job_event_fd < 0)
		{
			TORRENT_ASSERT(false);
		}
		m_aiocb_pool.io_queue = m_io_queue;
		m_aiocb_pool.event = m_disk_event_fd;
		
#endif

#if TORRENT_USE_SYNCIO
		m_aiocb_pool.worker_thread = &m_worker_thread;
#endif

#if TORRENT_USE_AIO

#if TORRENT_USE_AIO_PORTS 
		m_port = port_create();
		DLOG(stderr, "port_create() = %d\n", m_port);
		TORRENT_ASSERT(m_port >= 0);
		m_aiocb_pool.port = m_port;
#endif // TORRENT_USE_AIO_PORTS

#if TORRENT_USE_AIO_SIGNALFD
		m_job_event_fd = eventfd(0, 0);
		if (m_job_event_fd < 0)
		{
			TORRENT_ASSERT(false);
		}
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, TORRENT_AIO_SIGNAL);

		m_signal_fd[1] = signalfd(-1, &mask, SFD_NONBLOCK);
		if (pthread_sigmask(SIG_BLOCK, &mask, 0) == -1)
		{
			TORRENT_ASSERT(false);
		}
#endif // TORRENT_USE_AIO_SIGNALFD

#if TORRENT_USE_AIO_KQUEUE
		m_queue = kqueue();
		TORRENT_ASSERT(m_queue >= 0);
		m_aiocb_pool.queue = m_queue;
		pipe(m_job_pipe);
		// set up an event on m_job_pipe[1] being readable
		// this is how we communicate that a new job has been
		// posted
		struct kevent e;
		EV_SET(&e, m_job_pipe[1], EVFILT_READ, EV_ADD, 0, 0, 0);
		kevent(m_queue, &e, 1, 0, 0, 0);
#endif // TORRENT_USE_AIO_KQUEUE

#endif // TORRENT_USE_AIO

		// initialize default settings
		m_disk_cache.set_settings(m_settings);
	}

	disk_io_thread::~disk_io_thread()
	{
		DLOG(stderr, "destructing disk_io_thread [%p]\n", this);

		TORRENT_ASSERT(m_abort == true);
		TORRENT_ASSERT(m_in_progress == 0);
		TORRENT_ASSERT(m_to_issue == 0);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		// by now, all pieces should have been evicted
		std::pair<block_cache::iterator, block_cache::iterator> pieces
			= m_disk_cache.all_pieces();
		TORRENT_ASSERT(pieces.first == pieces.second);
#endif

#if TORRENT_USE_AIO

#if TORRENT_USE_AIO_PORTS
		close(m_port);
#elif TORRENT_USE_AIO_KQUEUE
		close(m_job_pipe[0]);
		close(m_job_pipe[1]);
		close(m_queue);
#else
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, TORRENT_AIO_SIGNAL);

		if (pthread_sigmask(SIG_BLOCK, &mask, 0) == -1)
		{
			TORRENT_ASSERT(false);
		}

#if TORRENT_USE_AIO_SIGNALFD
		close(m_signal_fd[0]);
		close(m_signal_fd[1]);
		close(m_event_fd);
#endif // TORRENT_USE_AIO_SIGNALFD
#endif // TORRENT_USE_AIO_PORTS

#elif TORRENT_USE_OVERLAPPED
		CloseHandle(m_completion_port);
#elif TORRENT_USE_IOSUBMIT
		io_destroy(m_io_queue);
		close(m_disk_event_fd);
		close(m_job_event_fd);
#endif
	}

	void disk_io_thread::reclaim_block(block_cache_reference ref)
	{
		TORRENT_ASSERT(ref.storage);
		// technically this isn't allowed, since these values are owned
		// and modified by the disk thread (and this call is made from the
		// network thread). However, it's just asserts (so it only affects
		// debug builds) and on the most popular systems, these read operations
		// will most likely be atomic anyway
		disk_io_job* j = m_aiocb_pool.allocate_job(disk_io_job::reclaim_block);
		TORRENT_ASSERT(ref.piece >= 0);
		TORRENT_ASSERT(ref.storage != 0);
		TORRENT_ASSERT(ref.block >= 0);
		TORRENT_ASSERT(ref.piece < ((piece_manager*)ref.storage)->files()->num_pieces());
		TORRENT_ASSERT(ref.block <= ((piece_manager*)ref.storage)->files()->piece_length() / 0x4000);
		j->d.io.ref = ref;
		add_job(j, true);
	}

	void disk_io_thread::set_settings(session_settings const& sett)
	{
		disk_io_job* j = m_aiocb_pool.allocate_job(disk_io_job::update_settings);
		j->buffer = (char*)new session_settings(sett);
		add_job(j);
	}

	void disk_io_thread::abort()
	{
		disk_io_job* j = m_aiocb_pool.allocate_job(disk_io_job::abort_thread);
		add_job(j);
	}

	void disk_io_thread::join()
	{
		DLOG(stderr, "[%p] waiting for disk_io_thread\n", this);
		m_disk_io_thread.join();
		TORRENT_ASSERT(m_abort == true);
	}

	// flush blocks of 'cont_block' contiguous blocks, and if at least 'num'
	// blocks are flushed, stop.
	int disk_io_thread::try_flush_contiguous(cached_piece_entry* p, int cont_block, int num)
	{
		int start_of_run = 0;
		int i = 0;
		cont_block = (std::min)(cont_block, int(p->blocks_in_piece));
		int ret = 0;
		DLOG(stderr, "[%p] try_flush_contiguous: %d blocks: %d cont_block: %d num: %d\n"
			, this, int(p->piece), int(p->blocks_in_piece), int(cont_block), num);

		int block_size = m_disk_cache.block_size();
		int hash_pos = p->hash == 0 ? 0 : (p->hash->offset + block_size - 1) / block_size;
		cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);

		for (; i < p->blocks_in_piece; ++i)
		{
			if (p->blocks[i].dirty && !p->blocks[i].pending) continue;

			if (start_of_run == i
				|| i - start_of_run < cont_block)
			{
				start_of_run = i + 1;
				continue;
			}

			// we should flush start_of_run - i.
			// we're flushing a block which we will need
			// to read back later, when we hash this piece
			if (start_of_run > hash_pos) pe->need_readback = true;
			ret += io_range(p, start_of_run, i, op_write, 0);
			start_of_run = i + 1;
			if (ret >= num) return ret;
		}

		if (i - start_of_run >= cont_block)
		{
			// we're flushing a block which we will need
			// to read back later, when we hash this piece
			if (start_of_run > hash_pos) pe->need_readback = true;
			// we should flush start_of_run - i.
			ret += io_range(p, start_of_run, i, op_write, 0);
			start_of_run = i + 1;
		}
		return ret;
	}

	// flush all blocks that are below p->hash.offset, since we've
	// already hashed those blocks, they won't cause any read-back
	int disk_io_thread::try_flush_hashed(cached_piece_entry* p, int cont_block, int num)
	{
		TORRENT_ASSERT(cont_block > 0);
		if (p->hash == 0)
		{
			DLOG(stderr, "[%p] no hash\n", this);
			return 0;
		}

		// end is one past the end
		// round offset up to include the last block, which might
		// have an odd size
		int block_size = m_disk_cache.block_size();
		int end = (p->hash->offset + block_size - 1) / block_size;

		// nothing has been hashed yet, don't flush anything
		if (end == 0 && !p->need_readback) return 0;

		// the number of contiguous blocks we need to be allowed to flush
		cont_block = (std::min)(cont_block, int(p->blocks_in_piece));

		// if everything has been hashed, we might as well flush everythin
		// regardless of the contiguous block restriction
		if (end == int(p->blocks_in_piece)) cont_block = 1;

		if (p->need_readback)
		{
			// if this piece needs a read-back already, don't
			// try to keep it from being flushed, since we'll
			// need to read it back regardless. Flushing will
			// save blocks that can be used to "save" other
			// pieces from being fllushed prematurely
			end = int(p->blocks_in_piece);
		}

		// count number of blocks that would be flushed
		int num_blocks = 0;
		for (int i = end-1; i >= 0; --i)
			num_blocks += (p->blocks[i].dirty && !p->blocks[i].pending);

		// we did not satisfy the cont_block requirement
		// i.e. too few blocks would be flushed at this point, put it off
		if (cont_block > num_blocks) return 0;

		DLOG(stderr, "[%p] try_flush_hashed: %d blocks: %d end: %d num: %d\n"
			, this, int(p->piece), int(p->blocks_in_piece), end, num);

		return io_range(p, 0, end, op_write, 0);
	}

	int count_aios(file::aiocb_t* a)
	{
		int ret = 0;
		while (a)
		{
			TORRENT_ASSERT(a->prev == 0 || a->prev->next == a);
			TORRENT_ASSERT(a->next == 0 || a->next->prev == a);
			++ret;
			a = a->next;
		}
		return ret;
	}

	// issues read or write operations for blocks in the given
	// range on the given piece. Returns the number of blocks operations
	// were actually issued for
	int disk_io_thread::io_range(cached_piece_entry* pe, int start, int end
		, int readwrite, int flags)
	{
		INVARIANT_CHECK;

		DLOG(stderr, "[%p] io_range: readwrite=%d piece=%d [%d, %d)\n"
			, this, readwrite, int(pe->piece), start, end);
		TORRENT_ASSERT(start >= 0);
		TORRENT_ASSERT(start < end);
		end = (std::min)(end, int(pe->blocks_in_piece));

		int piece_size = pe->storage->files()->piece_size(pe->piece);
		TORRENT_ASSERT(piece_size > 0);
		
		int buffer_size = 0;

		file::iovec_t* iov = TORRENT_ALLOCA(file::iovec_t, pe->blocks_in_piece);
		int iov_counter = 0;
		int ret = 0;

		end = (std::min)(end, int(pe->blocks_in_piece));

#ifdef DEBUG_STORAGE
		DLOG(stderr, "[%p] io_range: piece: %d [", this, int(pe->piece));
		for (int i = 0; i < start; ++i) DLOG(stderr, ".");
#endif

		// the termination condition is deliberately <= end here
		// so that we get one extra loop where we can issue the last
		// async operation
		for (int i = start; i <= end; ++i)
		{
			// don't flush blocks that are empty (buf == 0), not dirty
			// (read cache blocks), or pending (already being written)
			if (i == end
				|| pe->blocks[i].buf == 0
				// if we're writing and the block is already pending, it
				// means we're already writing it, skip it!
				|| pe->blocks[i].pending
				|| (!pe->blocks[i].dirty && readwrite == op_write)
				|| (!pe->blocks[i].uninitialized && readwrite == op_read))
			{
/*				if (i == end)
				{
					DLOG(stderr, "[%p] io_range: skipping block=%d end: %d\n", this, i, end);
				}
				else
				{
					DLOG(stderr, "[%p] io_range: skipping block=%d end: %d buf=%p pending=%d dirty=%d\n"
						, this, i, end, pe->blocks[i].buf, pe->blocks[i].pending, pe->blocks[i].dirty);
				}
*/				if (buffer_size == 0)
				{
					if (i != end) DLOG(stderr, ".");
					continue;
				}

				int elevator_direction = 0;
#if TORRENT_USE_SYNCIO
				elevator_direction = m_settings.allow_reordered_disk_operations ? m_elevator_direction : 0;
#endif

				int block_size = m_disk_cache.block_size();
				TORRENT_ASSERT(buffer_size <= i * block_size);
				int to_write = (std::min)(i * block_size, piece_size) - buffer_size;
				int range_start = i - (buffer_size + block_size - 1) / block_size;
				file::aiocb_t* aios = 0;
				async_handler* a = m_aiocb_pool.alloc_handler();
				if (a == 0)
				{
					// #error handle no mem
				}
				if (readwrite == op_write)
				{
//					DLOG(stderr, "[%p] io_range: write piece=%d start_block=%d end_block=%d\n"
//						, this, int(pe->piece), range_start, i);
					m_pending_buffer_size += to_write;
					a->handler = boost::bind(&disk_io_thread::on_disk_write, this, pe
						, range_start, i, to_write, _1);

					aios = pe->storage->get_storage_impl()->async_writev(iov, iov_counter
						, pe->piece, to_write, flags, a);
					m_cache_stats.blocks_written += i - range_start;
					++m_cache_stats.writes;
				}
				else
				{
//					DLOG(stderr, "[%p] io_range: read piece=%d start_block=%d end_block=%d\n"
//						, this, int(pe->piece), range_start, i);
					++m_outstanding_jobs;
					a->handler = boost::bind(&disk_io_thread::on_disk_read, this, pe
						, range_start, i, _1);
					aios = pe->storage->get_storage_impl()->async_readv(iov, iov_counter
						, pe->piece, range_start * block_size, flags, a);
					m_cache_stats.blocks_read += i - range_start;
					++m_cache_stats.reads;
				}

				if (a->references == 0)
				{
					// this is a special case for when the storage doesn't want to produce
					// any actual async. file operations, but just filled in the buffers
					if (!a->error.ec) a->transferred = bufs_size(iov, iov_counter);
					a->handler(a);
					m_aiocb_pool.free_handler(a);
					a = 0;
				}

#ifdef TORRENT_DEBUG
				// make sure we're not already requesting this same block
				file::aiocb_t* k = aios;
				while (k)
				{
					file::aiocb_t* found = find_aiocb(m_to_issue, k);
					TORRENT_ASSERT(found == 0);
					found = find_aiocb(m_in_progress, k);
					TORRENT_ASSERT(found == 0);
					k = k->next;
				}
#endif

				m_num_to_issue += append_aios(m_to_issue, m_to_issue_end, aios, elevator_direction, this);
				if (m_num_to_issue > m_peak_num_to_issue) m_peak_num_to_issue = m_num_to_issue;
				TORRENT_ASSERT(m_num_to_issue == count_aios(m_to_issue));

				iov_counter = 0;
				buffer_size = 0;
				continue;
			}
			DLOG(stderr, "x");

			int block_size = m_disk_cache.block_size();
			block_size = (std::min)(piece_size - i * block_size, block_size);
			TORRENT_ASSERT_VAL(i < end, i);
			iov[iov_counter].iov_base = pe->blocks[i].buf;
			iov[iov_counter].iov_len = block_size;
#ifdef TORRENT_DEBUG
			if (readwrite == op_write)
				TORRENT_ASSERT(pe->blocks[i].dirty == true);
			else
				TORRENT_ASSERT(pe->blocks[i].dirty == false);
#endif
			TORRENT_ASSERT(pe->blocks[i].pending == false);
			pe->blocks[i].uninitialized = false;
			if (!pe->blocks[i].pending)
			{
				TORRENT_ASSERT(pe->blocks[i].buf);
				pe->blocks[i].pending = true;
				if (pe->blocks[i].refcount == 0) m_disk_cache.pinned_change(1);
				++pe->blocks[i].refcount;
				TORRENT_ASSERT(pe->blocks[i].refcount > 0); // make sure it didn't wrap
				++pe->refcount;
				TORRENT_ASSERT(pe->refcount > 0); // make sure it didn't wrap
			}
			++iov_counter;
			++ret;
			buffer_size += block_size;
		}

		if (m_outstanding_jobs > m_peak_outstanding) m_peak_outstanding = m_outstanding_jobs;

#ifdef DEBUG_STORAGE
		for (int i = end; i < int(pe->blocks_in_piece); ++i) DLOG(stderr, ".");
		DLOG(stderr, "] ret = %d\n", ret);
#endif

		return ret;
	}

	void disk_io_thread::on_disk_write(cached_piece_entry* pe, int begin
		, int end, int to_write, async_handler* handler)
	{
		if (!handler->error.ec)
		{
			boost::uint32_t write_time = total_microseconds(time_now_hires() - handler->started);
			m_write_time.add_sample(write_time);
			m_cache_stats.cumulative_write_time += write_time;
		}

		TORRENT_ASSERT(m_pending_buffer_size >= to_write);
		m_pending_buffer_size -= to_write;

		DLOG(stderr, "[%p] on_disk_write piece: %d start: %d end: %d\n"
			, this, int(pe->piece), begin, end);

		tailqueue retry_jobs;
		m_disk_cache.mark_as_done(pe, begin, end, m_completed_jobs, retry_jobs, handler->error);
		if (!retry_jobs.empty())
		{
			mutex::scoped_lock l(m_job_mutex);
			m_queued_jobs.append(retry_jobs);
		}

		if (!handler->error)
		{
			boost::uint32_t job_time = total_microseconds(time_now_hires() - handler->started);
			m_job_time.add_sample(job_time);
			m_cache_stats.cumulative_job_time += job_time;
		}
	}

	void disk_io_thread::on_disk_read(cached_piece_entry* pe, int begin
		, int end, async_handler* handler)
	{
		if (!handler->error.ec)
		{
			boost::uint32_t read_time = total_microseconds(time_now_hires() - handler->started);
			m_read_time.add_sample(read_time);
			m_cache_stats.cumulative_read_time += read_time;
		}

		file::iovec_t* vec = TORRENT_ALLOCA(file::iovec_t, end - begin);
		int piece_size = pe->storage->files()->piece_size(pe->piece);
		int block_size = m_disk_cache.block_size();
		for (int i = begin, k = 0; i < end; ++i, ++k)
		{
			vec[k].iov_base = (file::iovec_base_t)pe->blocks[i].buf;
			vec[k].iov_len = (std::min)(piece_size - i * block_size, block_size);
		}

		pe->storage->get_storage_impl()->readv_done(vec, end - begin
			, pe->piece, begin * block_size);

		DLOG(stderr, "[%p] on_disk_read piece: %d start: %d end: %d\n"
			, this, int(pe->piece), begin, end);

		tailqueue retry_jobs;
		m_disk_cache.mark_as_done(pe, begin, end, m_completed_jobs
			, retry_jobs, handler->error);

		if (!retry_jobs.empty())
		{
			mutex::scoped_lock l(m_job_mutex);
			disk_io_job* i = (disk_io_job*)retry_jobs.get_all();
			while (i)
			{
				disk_io_job* j = i;
				i = (disk_io_job*)i->next;
				j->next = 0;
				m_queued_jobs.push_back(j);
			}
		}

		if (!handler->error)
		{
			boost::uint32_t job_time = total_microseconds(time_now_hires() - handler->started);
			m_job_time.add_sample(job_time);
			m_cache_stats.cumulative_job_time += job_time;
		}

		TORRENT_ASSERT(m_outstanding_jobs > 0);
		--m_outstanding_jobs;
	}

	void disk_io_thread::flush_piece(cached_piece_entry* pe, int flags, int& ret)
	{
		if (flags & flush_delete_cache)
		{
			// delete dirty blocks and post handlers with
			// operation_aborted error code
			m_disk_cache.abort_dirty(pe, m_completed_jobs);
		}
		else if ((flags & flush_write_cache) && pe->num_dirty > 0)
		{
			// issue write commands
			io_range(pe, 0, INT_MAX, op_write, 0);

			// if we're also flushing the read cache, this piece
			// should be removed as soon as all write jobs finishes
			// otherwise it will turn into a read piece
		}

		// we need to count read jobs as well
		// because we can't close files with
		// any outstanding jobs
		ret += pe->jobs.size();

		// mark_for_deletion may erase the piece from the cache, that's
		// why we don't have the 'i' iterator referencing it at this point
		if (flags & (flush_read_cache | flush_delete_cache))
			m_disk_cache.mark_for_deletion(pe, m_completed_jobs);
	}

	// returns the number of outstanding jobs on the pieces. If this is 0
	// it indicates that files can be closed without interrupting any operation
	int disk_io_thread::flush_cache(disk_io_job* j, boost::uint32_t flags)
	{
		int ret = 0;

		piece_manager* storage = j->storage.get();

		std::pair<block_cache::iterator, block_cache::iterator> range;

		if (storage)
		{
			// iterate over all blocks and issue writes for the ones
			// that have dirty blocks (i.e. needs to be written)
			for (boost::unordered_set<cached_piece_entry*>::iterator i
				= storage->cached_pieces().begin(), end(storage->cached_pieces().end()); i != end;)
			{
				cached_piece_entry* pe = *i;
				++i;
				TORRENT_ASSERT(pe->storage == j->storage);
				flush_piece(pe, flags, ret);
			}
		}
		else
		{
			for (block_cache::iterator i = range.first; i != range.second;)
			{
				cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*i);
				++i;
				flush_piece(pe, flags, ret);
			}
		}
		return ret;
	}

	// this is called if we're exceeding (or about to exceed) the cache
	// size limit. This means we should not restrict ourselves to contiguous
	// blocks of write cache line size, but try to flush all old blocks
	// this is why we pass in 1 as cont_block to the flushing functions
	void disk_io_thread::try_flush_write_blocks(int num)
	{
		DLOG(stderr, "[%p] try_flush_write_blocks: %d\n", this, num);

		list_iterator range = m_disk_cache.write_lru_pieces();

		TORRENT_ASSERT(m_settings.disk_cache_algorithm == session_settings::avoid_readback);

		if (m_settings.disk_cache_algorithm == session_settings::largest_contiguous)
		{
			for (list_iterator p = range; p.get() && num > 0; p.next())
			{
				cached_piece_entry* e = (cached_piece_entry*)p.get();
				if (e->num_dirty == 0) continue;

				// prefer contiguous blocks. If we won't find any, we'll
				// start over but actually flushing single blocks
				num -= try_flush_contiguous(e, m_settings.write_cache_line_size, num);
			}
		}
		else if (m_settings.disk_cache_algorithm == session_settings::avoid_readback)
		{
			for (list_iterator p = range; p.get() && num > 0; p.next())
			{
				cached_piece_entry* e = (cached_piece_entry*)p.get();
				if (e->num_dirty == 0) continue;

				num -= try_flush_hashed(e, 1, num);
			}
		}

		// if we still need to flush blocks, start over and flush
		// everything in LRU order (degrade to lru cache eviction)
		if (num > 0)
		{
			for (list_iterator p = range; p.get() && num > 0; p.next())
			{
				cached_piece_entry* e = (cached_piece_entry*)p.get();
				if (e->num_dirty == 0) continue;

				num -= try_flush_contiguous(e, 1, num);
			}
		}
	}

	void disk_io_thread::flush_expired_write_blocks()
	{
		DLOG(stderr, "[%p] flush_expired_write_blocks\n", this);

		TORRENT_ASSERT(m_settings.disk_cache_algorithm == session_settings::avoid_readback);

		ptime now = time_now();
		time_duration expiration_limit = seconds(m_settings.cache_expiry);

#ifdef TORRENT_DEBUG
		ptime timeout = min_time();
#endif

		for (list_iterator p = m_disk_cache.write_lru_pieces(); p.get(); p.next())
		{
			cached_piece_entry* e = (cached_piece_entry*)p.get();
#ifdef TORRENT_DEBUG
			TORRENT_ASSERT(e->expire >= timeout);
			timeout = e->expire;
#endif

			// since we're iterating in order of last use, if this piece
			// shouldn't be evicted, none of the following ones will either
			if (now - e->expire < expiration_limit) break;
			if (e->num_dirty == 0) continue;

			io_range(e, 0, INT_MAX, op_write, 0);
		}
	}

	typedef int (disk_io_thread::*disk_io_fun_t)(disk_io_job* j);

	// this is a jump-table for disk I/O jobs
	static const disk_io_fun_t job_functions[] =
	{
		&disk_io_thread::do_read,
		&disk_io_thread::do_write,
		&disk_io_thread::do_hash,
		&disk_io_thread::do_move_storage,
		&disk_io_thread::do_release_files,
		&disk_io_thread::do_delete_files,
		&disk_io_thread::do_check_fastresume,
		&disk_io_thread::do_save_resume_data,
		&disk_io_thread::do_rename_file,
		&disk_io_thread::do_abort_thread,
		&disk_io_thread::do_clear_read_cache,
		&disk_io_thread::do_abort_torrent,
		&disk_io_thread::do_update_settings,
		&disk_io_thread::do_cache_piece,
		&disk_io_thread::do_finalize_file,
		&disk_io_thread::do_get_cache_info,
		&disk_io_thread::do_hashing_done,
		&disk_io_thread::do_file_status,
		&disk_io_thread::do_reclaim_block,
		&disk_io_thread::do_clear_piece,
		&disk_io_thread::do_sync_piece,
		&disk_io_thread::do_flush_piece,
		&disk_io_thread::do_trim_cache,
		&disk_io_thread::do_aiocb_complete,
	};

	const char* job_action_name[] =
	{
		"read",
		"write",
		"hash",
		"move_storage",
		"release_files",
		"delete_files",
		"check_fastresume",
		"save_resume_data",
		"rename_file",
		"abort_thread",
		"clear_read_cache",
		"abort_torrent",
		"update_settings",
		"cache_piece",
		"finalize_file",
		"get_cache_info",
		"hashing_done",
		"file_status",
		"reclaim_block",
		"clear_piece",
		"sync_piece",
		"flush_piece",
		"trim_cache",
		"aiocb_complete"
	};

	void disk_io_thread::perform_async_job(disk_io_job* j)
	{
		INVARIANT_CHECK;
		TORRENT_ASSERT(j->next == 0);

		int evict = m_disk_cache.num_to_evict(0);
		if (evict > 0)
		{
			evict = m_disk_cache.try_evict_blocks(evict, 1);
			if (evict > 0) try_flush_write_blocks(evict);
		}

		DLOG(stderr, "[%p] perform_async_job job: %s piece: %d offset: %d\n"
			, this, job_action_name[j->action], j->piece, j->d.io.offset);
		if (j->storage && j->storage->get_storage_impl()->m_settings == 0)
			j->storage->get_storage_impl()->m_settings = &m_settings;

		TORRENT_ASSERT(j->action < sizeof(job_functions)/sizeof(job_functions[0]));

		// is the fence up for this storage?
		// jobs that are instantaneous are not affected by the fence, is_blocked()
		// will take ownership of the job and queue it up, in case the fence is up
		if (!is_job_immediate(j->action) && j->storage && j->storage->is_blocked(j))
		{
			++m_num_blocked_jobs;
			DLOG(stderr, "[%p] blocked job: %s (torrent: %d total: %d)\n"
				, this, job_action_name[j->action], j->storage->num_blocked()
				, m_num_blocked_jobs);
			return;
		}

		if (time_now() > m_last_stats_flip + seconds(1)) flip_stats();

		ptime now = time_now_hires();
		m_queue_time.add_sample(total_microseconds(now - j->start_time));
		j->start_time = now;

		// call disk function
		int ret = (this->*(job_functions[j->action]))(j);

		DLOG(stderr, "[%p]   return: %d error: %s\n"
			, this, ret, j->error ? j->error.ec.message().c_str() : "");

		if (ret != defer_handler)
		{
			TORRENT_ASSERT(j->next == 0);
			DLOG(stderr, "[%p]   posting callback j->buffer: %p\n", this, j->buffer);
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT(j->callback_called == false);
			j->callback_called = true;
#endif
			j->ret = ret;
			m_completed_jobs.push_back(j);
		}
	}

	int disk_io_thread::do_read(disk_io_job* j)
	{
		DLOG(stderr, "[%p] do_read\n", this);
		INVARIANT_CHECK;

		TORRENT_ASSERT(j->d.io.buffer_size <= m_disk_cache.block_size());
		j->d.io.ref.storage = 0;

		// there's no point in hinting that we will read something
		// when using async I/O anyway
#if TORRENT_USE_SYNCIO
		// TODO: when reading too much ahead, linux seems to freeze (posix_fadvise() blocks indefinitely)
		// this logic should be changed to apply to aiocb_t objects instead of disk_io_job, and once
		// sorted, the first X aiocb_t object should have the hint_read called on them. That way the
		// number of read-ahead-bytes is limited
		if (m_settings.use_disk_read_ahead)
		{
			j->storage->get_storage_impl()->hint_read(j->piece, j->d.io.offset, j->d.io.buffer_size);
		}
#endif

		int block_size = m_disk_cache.block_size();

		if (m_settings.use_read_cache)
		{
			int ret = m_disk_cache.try_read(j);
			if (ret >= 0)
			{
				DLOG(stderr, "[%p] do_read: cache hit\n", this);
				j->flags |= disk_io_job::cache_hit;
				return ret;
			}
			else if (ret == -2)
			{
				j->error.ec = error::no_memory;
				return disk_operation_failed;
			}

			// cache the piece, unless we're using an explicit cache
			if (!m_settings.explicit_read_cache)
			{
				cached_piece_entry* p = m_disk_cache.allocate_piece(j, cached_piece_entry::read_lru1);
				if (p)
				{
					int start_block = j->d.io.offset / block_size;
					int end_block = (std::min)(int(p->blocks_in_piece)
							, start_block + m_settings.read_cache_line_size);
					// this will also add the job to the pending job list in this piece
					// unless it fails and returns -1
					int ret = m_disk_cache.allocate_pending(p, start_block, end_block, j, 0, true);
					DLOG(stderr, "[%p] do_read: allocate_pending ret=%d start_block=%d end_block=%d\n"
							, this, ret, start_block, end_block);

					// a return value of 0 means these same blocks are already
					// scheduled to be read, and we just tacked on this new jobs
					// to be notified of the buffers being complete
					if (ret >= 0)
					{
						// some blocks were allocated
						if (ret > 0) io_range(p, start_block, end_block, op_read, j->flags);

						DLOG(stderr, "[%p] do_read: cache miss\n", this);
						return defer_handler;
					}
					else if (ret == -1)
					{
						// allocation failed
						m_disk_cache.mark_for_deletion(p, m_completed_jobs);
						j->buffer = 0;
						j->error.ec = error::no_memory;
						return disk_operation_failed;
					}

					// we get here if allocate_pending failed with
					// an error other than -1. This happens for instance
					// if the cache is full. Then fall through and issue the
					// read circumventing the cache

					m_disk_cache.mark_for_deletion(p, m_completed_jobs);
				}
			}
		}

		j->buffer = m_disk_cache.allocate_buffer("send buffer");
		if (j->buffer == 0)
		{
			j->error.ec = error::no_memory;
			return disk_operation_failed;
		}

		DLOG(stderr, "[%p] do_read: async\n", this);
		++m_outstanding_jobs;
		if (m_outstanding_jobs > m_peak_outstanding) m_peak_outstanding = m_outstanding_jobs;
		async_handler* a = m_aiocb_pool.alloc_handler();
		if (a == 0)
		{
			j->error.ec = error::no_memory;
			return disk_operation_failed;
		}
		j->storage->new_job(j);
		a->handler = boost::bind(&disk_io_thread::on_read_one_buffer, this, _1, j);
		file::iovec_t b = { j->buffer, j->d.io.buffer_size };
		file::aiocb_t* aios = j->storage->get_storage_impl()->async_readv(&b, 1
			, j->piece, j->d.io.offset, j->flags, a);

		if (a->references == 0)
		{
			// this is a special case for when the storage doesn't want to produce
			// any actual async. file operations, but just filled in the buffers
			if (!a->error.ec) a->transferred = j->d.io.buffer_size;
			a->handler(a);
			m_aiocb_pool.free_handler(a);
			a = 0;
		}

		DLOG(stderr, "prepending aios (%p) from read_async_impl to m_to_issue (%p)\n"
			, aios, m_to_issue);

#ifdef TORRENT_DEBUG
		// make sure we're not already requesting this same block
		file::aiocb_t* k = aios;
		while (k)
		{
			file::aiocb_t* found = find_aiocb(m_to_issue, k);
			TORRENT_ASSERT(found == 0);
			found = find_aiocb(m_in_progress, k);
			TORRENT_ASSERT(found == 0);
			k = k->next;
		}
#endif

		int elevator_direction = 0;
#if TORRENT_USE_SYNCIO
		elevator_direction = m_settings.allow_reordered_disk_operations ? m_elevator_direction : 0;
#endif
		m_num_to_issue += append_aios(m_to_issue, m_to_issue_end, aios, elevator_direction, this);
		if (m_num_to_issue > m_peak_num_to_issue) m_peak_num_to_issue = m_num_to_issue;
		TORRENT_ASSERT(m_num_to_issue == count_aios(m_to_issue));

//		for (file::aiocb_t* j = m_to_issue; j; j = j->next)
//			DLOG(stderr, "  %"PRId64, j->phys_offset);
//		DLOG(stderr, "\n");
		return defer_handler;
	}

	int disk_io_thread::do_write(disk_io_job* j)
	{
		INVARIANT_CHECK;
		TORRENT_ASSERT(j->buffer != 0);
		TORRENT_ASSERT(j->d.io.buffer_size <= m_disk_cache.block_size());
		int block_size = m_disk_cache.block_size();

		if (m_settings.cache_size > 0)
		{
			cached_piece_entry* pe = m_disk_cache.add_dirty_block(j);

			if (pe == 0)
			{
				m_disk_cache.free_buffer(j->buffer);
				j->buffer = 0;
				j->error.ec = error::no_memory;
				return disk_operation_failed;
			}

			if (pe->hash == 0 && !m_settings.disable_hash_checks)
			{
				pe->hash = new partial_hash;
				m_disk_cache.update_cache_state(pe);
			}

			// flushes the piece to disk in case
			// it satisfies the condition for a write
			// piece to be flushed
			if (m_settings.disk_cache_algorithm == session_settings::avoid_readback)
			{
				try_flush_hashed(pe, m_settings.write_cache_line_size);
			}
			else
			{
				try_flush_contiguous(pe, m_settings.write_cache_line_size);
			}

			// if we have more blocks in the cache than allowed by
			// the cache size limit, flush some dirty blocks
			// deduct the writing blocks from the cache size, otherwise we'll flush the
			// entire cache as soon as we exceed the limit, since all flush operations are
			// async.
			int num_pending_write_blocks = (m_pending_buffer_size + block_size - 1) / block_size;
			int current_size = m_disk_cache.in_use();
			if (m_settings.cache_size <= current_size - num_pending_write_blocks)
			{
				int left = current_size - m_settings.cache_size;
				left = m_disk_cache.try_evict_blocks(left, 1);
				if (left > 0 && !m_settings.dont_flush_write_cache)
					try_flush_write_blocks(left);
			}

			// the handler will be called when the block
			// is flushed to disk
			return defer_handler;
		}

		file::iovec_t b = { j->buffer, j->d.io.buffer_size };

		m_pending_buffer_size += j->d.io.buffer_size;

		async_handler* a = m_aiocb_pool.alloc_handler();
		if (a == 0)
		{
			j->error.ec = error::no_memory;
			return disk_operation_failed;
		}
		j->storage->new_job(j);
		a->handler = boost::bind(&disk_io_thread::on_write_one_buffer, this, _1, j);
		file::aiocb_t* aios = j->storage->get_storage_impl()->async_writev(&b, 1
			, j->piece, j->d.io.offset, j->flags, a);

		DLOG(stderr, "prepending aios (%p) from write_async_impl to m_to_issue (%p)\n"
			, aios, m_to_issue);

		if (a->references == 0)
		{
			// this is a special case for when the storage doesn't want to produce
			// any actual async. file operations, but just filled in the buffers
			if (!a->error.ec) a->transferred = j->d.io.buffer_size;
			a->handler(a);
			m_aiocb_pool.free_handler(a);
			a = 0;
		}

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		// make sure we're not already requesting this same block
		file::aiocb_t* i = aios;
		while (i)
		{
			file::aiocb_t* found = find_aiocb(m_to_issue, i);
			TORRENT_ASSERT(found == 0);
			found = find_aiocb(m_in_progress, i);
			TORRENT_ASSERT(found == 0);
			i = i->next;
		}
#endif

		int elevator_direction = 0;
#if TORRENT_USE_SYNCIO
		elevator_direction = m_settings.allow_reordered_disk_operations ? m_elevator_direction : 0;
#endif
		m_num_to_issue += append_aios(m_to_issue, m_to_issue_end, aios, elevator_direction, this);
		if (m_num_to_issue > m_peak_num_to_issue) m_peak_num_to_issue = m_num_to_issue;
		TORRENT_ASSERT(m_num_to_issue == count_aios(m_to_issue));

//		for (file::aiocb_t* j = m_to_issue; j; j = j->next)
//			DLOG(stderr, "  %"PRId64, j->phys_offset);
//		DLOG(stderr, "\n");
		return defer_handler;
	}

	int disk_io_thread::do_hash(disk_io_job* j)
	{
		INVARIANT_CHECK;

		cached_piece_entry* pe = m_disk_cache.find_piece(j);

		int ret = defer_handler;

		bool job_added = false;
		if (m_settings.disable_hash_checks)
		{
			DLOG(stderr, "[%p] do_hash: hash checking turned off, returning piece: %d\n"
				, this, int(pe ? pe->piece : -1));
			return 0;
		}

		int block_size = m_disk_cache.block_size();

		int start_block = 0;
		bool need_read = false;

		// potentially allocate and issue read commands for blocks we don't have, but
		// need in order to calculate the hash
		if (pe == 0)
		{
			DLOG(stderr, "[%p] do_hash: allocating a new piece: %d\n"
				, this, int(j->piece));

			pe = m_disk_cache.allocate_piece(j, cached_piece_entry::read_lru1);
			if (pe == 0)
			{
				TORRENT_ASSERT(j->buffer == 0);
				j->error.ec = error::no_memory;
				return disk_operation_failed;
			}

			// allocate_pending will add the job to the piece
			ret = m_disk_cache.allocate_pending(pe, 0, pe->blocks_in_piece, j, 2);
			DLOG(stderr, "[%p] do_hash: allocate_pending ret=%d\n", this, ret);
			job_added = true;

			if (ret >= 0)
			{
				// some blocks were allocated
				if (ret > 0) need_read = true;
				TORRENT_ASSERT(start_block == 0);
				ret = defer_handler;
			}
			else if (ret == -1)
			{
				// allocation failed
				m_disk_cache.mark_for_deletion(pe, m_completed_jobs);
				TORRENT_ASSERT(j->buffer == 0);
				j->error.ec = error::no_memory;
				return disk_operation_failed;
			}
			else if (ret < -1)
			{
				m_disk_cache.mark_for_deletion(pe, m_completed_jobs);
				// this shouldn't happen
				TORRENT_ASSERT(false);
			}
			ret = defer_handler;
		}
		else
		{
			// issue read commands to read those blocks in
			if (pe->hash)
			{
				if (pe->hashing != cached_piece_entry::not_hashing)
					start_block = pe->hashing;
				else start_block = (pe->hash->offset + block_size - 1) / block_size;
			}

			// find a (potential) range that we can start hashing, of blocks that we already have
			// it's OK to start hashing blocks that are dirty and being written right now
			// in fact, we want to do that to be able to serve them as soon as possible
			int end = start_block;
			while (end < pe->blocks_in_piece
				&& pe->blocks[end].buf
				&& (!pe->blocks[end].pending || pe->blocks[end].dirty)) ++end;

			if (end > start_block && pe->hashing == cached_piece_entry::not_hashing)
			{
				// do we need the partial hash object?
				if (pe->hash == 0)
				{
					DLOG(stderr, "[%p] do_hash: creating hash object piece: %d\n"
						, this, int(pe->piece));
					// TODO: maybe the partial_hash objects should be pool allocated
					pe->hash = new partial_hash;
					m_disk_cache.update_cache_state(pe);
				}

				m_hash_thread.async_hash(pe, start_block, end);
			}

			// deal with read-back. i.e. blocks that have already been flushed to disk
			// and are no longer in the cache, we need to read those back in order to hash
			// them
			if (end < pe->blocks_in_piece)
			{
				ret = m_disk_cache.allocate_pending(pe, end, pe->blocks_in_piece, j, 2);
				DLOG(stderr, "[%p] do_hash: allocate_pending() = %d piece: %d\n"
					, this, ret, int(pe->piece));
				if (ret >= 0)
				{
					// if allocate_pending succeeds, it adds the job as well
					job_added = true;
					// some blocks were allocated
					if (ret > 0) need_read = true;
					ret = defer_handler;
				}
				else if (ret == -1)
				{
					// allocation failed
					m_disk_cache.mark_for_deletion(pe, m_completed_jobs);
					TORRENT_ASSERT(j->buffer == 0);
					j->error.ec = error::no_memory;
					return disk_operation_failed;
				}
				ret = defer_handler;
			}
			else if (pe->hashing == cached_piece_entry::not_hashing)
			{
				// we get here if the hashing is already complete
				// in the pe->hash object. We just need to finalize
				// it and compare to the actual hash
				// This doesn't seem very likely to ever happen

				TORRENT_ASSERT(pe->hash->offset == j->storage->files()->piece_size(pe->piece));
				partial_hash& ph = *pe->hash;
				memcpy(j->d.piece_hash, &ph.h.final()[0], 20);
				ret = 0;
				// return value:
				// 0: success, piece passed hash check
				// -1: disk failure
				if (j->flags & disk_io_job::volatile_read)
				{
					pe->marked_for_deletion = true;
					DLOG(stderr, "[%p] do_hash: volatile, mark piece for deletion. "
						"ret: %d piece: %d\n", this, ret, int(pe->piece));
				}
				delete pe->hash;
				pe->hash = 0;
				m_disk_cache.update_cache_state(pe);
				return ret;
			}
		}

		// do we need the partial hash object?
		if (pe->hash == 0)
		{
			DLOG(stderr, "[%p] do_hash: creating hash object piece: %d\n"
				, this, int(pe->piece));
			// TODO: maybe the partial_hash objects should be pool allocated
			pe->hash = new partial_hash;
			m_disk_cache.update_cache_state(pe);
		}

		// increase the refcount for all blocks the hash job needs in
		// order to complete. These are decremented in block_cache::reap_piece_jobs
		// for hash jobs
		for (int i = start_block; i < pe->blocks_in_piece; ++i)
		{
			TORRENT_ASSERT(pe->blocks[i].buf);
			if (pe->blocks[i].refcount == 0) m_disk_cache.pinned_change(1);
			++pe->blocks[i].refcount;
			++pe->refcount;
			TORRENT_ASSERT(pe->blocks[i].refcount > 0); // make sure it didn't wrap
			TORRENT_ASSERT(pe->refcount > 0); // make sure it didn't wrap
#ifdef TORRENT_DEBUG
			++pe->blocks[i].check_count;
#endif
		}
		j->d.io.offset = start_block;

		if (!job_added)
		{
			DLOG(stderr, "[%p] do_hash: adding job piece: %d\n"
				, this, int(pe->piece));
			TORRENT_ASSERT(j->piece == pe->piece);
			pe->storage->new_job(j);
			pe->jobs.push_back(j);
		}

		if (need_read)
		{
			m_cache_stats.total_read_back += io_range(pe, start_block
				, pe->blocks_in_piece, op_read, j->flags);
		}
#if DEBUG_STORAGE
		DLOG(stderr, "[%p] do_hash: jobs [", this);
		for (tailqueue_iterator i = pe->jobs.iterate(); i.get(); i.next())
		{
			DLOG(stderr, " %s", job_action_name[((disk_io_job*)i.get())->action]);
		}
		DLOG(stderr, " ]\n");
#endif

		return defer_handler;
	}

	int disk_io_thread::do_move_storage(disk_io_job* j)
	{
		// if files have to be closed, that's the storage's responsibility
		j->storage->get_storage_impl()->move_storage(j->buffer, j->error);
		return j->error ? disk_operation_failed : 0;
	}

	int disk_io_thread::do_release_files(disk_io_job* j)
	{
		INVARIANT_CHECK;

		int ret = flush_cache(j, flush_write_cache);
		if (ret == 0)
		{
			// this means there are no outstanding requests
			// to this piece. We can go ahead and close the
			// files immediately without interfering with
			// any async operations
			j->storage->get_storage_impl()->release_files(j->error);
			return j->error ? disk_operation_failed : 0;
		}

		// this fence have to block both read and write operations and let read operations
		// When blocks are reference counted, even read operation would force cache pieces to linger
		// raise the fence to block new async. operations
		DLOG(stderr, "[%p] raising fence [%p] ret: %d\n", this, j->storage.get(), ret);
		j->storage->raise_fence(j);
		++m_num_blocked_jobs;
		return defer_handler;
	}

	int disk_io_thread::do_delete_files(disk_io_job* j)
	{
		TORRENT_ASSERT(j->buffer == 0);
		INVARIANT_CHECK;

		int ret = flush_cache(j, flush_delete_cache);
		if (ret == 0)
		{
			// this means there are no outstanding requests
			// to this piece. We can go ahead and delete the
			// files immediately without interfering with
			// any async operations
			j->storage->get_storage_impl()->delete_files(j->error);
			return j->error ? disk_operation_failed : 0;
		}

		// raise the fence to block new async. operations
		DLOG(stderr, "[%p] raising fence [%p] ret: %d\n", this, j->storage.get(), ret);
		j->storage->raise_fence(j);
		++m_num_blocked_jobs;
		return defer_handler;
	}

	int disk_io_thread::do_check_fastresume(disk_io_job* j)
	{
		lazy_entry const* rd = (lazy_entry const*)j->buffer;
		TORRENT_ASSERT(rd != 0);

		return j->storage->check_fastresume(*rd, j->error);
	}

	int disk_io_thread::do_save_resume_data(disk_io_job* j)
	{
		int ret = flush_cache(j, flush_write_cache);
		if (ret == 0)
		{
			// this means there are no outstanding requests
			// to this piece. We can go ahead and close the
			// files immediately without interfering with
			// any async operations
			entry* resume_data = new entry(entry::dictionary_t);
			j->storage->get_storage_impl()->write_resume_data(*resume_data, j->error);
			TORRENT_ASSERT(j->buffer == 0);
			j->buffer = (char*)resume_data;
			return j->error ? disk_operation_failed : 0;
		}

//#error this fence would only have to block write operations and could let read operations through

		// raise the fence to block new
		DLOG(stderr, "[%p] raising [%p] fence\n", this, j->storage.get());
		j->storage->raise_fence(j);
		++m_num_blocked_jobs;
		return defer_handler;
	}

	int disk_io_thread::do_rename_file(disk_io_job* j)
	{
		// if files need to be closed, that's the storage's responsibility
		j->storage->get_storage_impl()->rename_file(j->piece, j->buffer, j->error);
		return j->error ? disk_operation_failed : 0;
	}

	int disk_io_thread::do_abort_thread(disk_io_job* j)
	{
		// issue write commands for all dirty blocks
		// and clear all read jobs
		flush_cache(j, flush_read_cache | flush_write_cache);
		m_abort = true;
		return 0;
	}

	int disk_io_thread::do_clear_read_cache(disk_io_job* j)
	{
		flush_cache(j, flush_read_cache);
		return 0;
	}

	int disk_io_thread::do_abort_torrent(disk_io_job* j)
	{
		// issue write commands for all dirty blocks
		// and clear all read jobs
		flush_cache(j, flush_read_cache | flush_write_cache);

		m_disk_cache.release_memory();

		if (j->storage->num_pieces() == 0) return 0;

		// there are some blocks left, we cannot post the completion
		// for this job yet.

		j->storage->set_abort_job(j);

		return defer_handler;
	}

	int disk_io_thread::do_update_settings(disk_io_job* j)
	{
		TORRENT_ASSERT(j->buffer);
		session_settings* s = ((session_settings*)j->buffer);
		TORRENT_ASSERT(s->cache_size >= 0);
		TORRENT_ASSERT(s->cache_expiry > 0);
		int block_size = m_disk_cache.block_size();

#if defined TORRENT_WINDOWS
		if (m_settings.low_prio_disk != s->low_prio_disk)
		{
			m_file_pool.set_low_prio_io(s->low_prio_disk);
			// we need to close all files, since the prio
			// only takes affect when files are opened
			m_file_pool.release(0);
		}
#endif
		if (m_settings.hashing_threads != s->hashing_threads)
			m_hash_thread.set_num_threads(s->hashing_threads);

#if TORRENT_USE_AIOINIT
		if (m_settings.aio_threads != s->aio_threads
			|| m_settings.aio_max != s->aio_max)
		{
			aioinit a;
			memset(&a, 0, sizeof(a));
			a.aio_threads = s->aio_threads;
			a.aio_num = s->aio_max;
			aio_init(&a);
		}
#endif

#if TORRENT_USE_SYNCIO
		if (m_settings.aio_threads != s->aio_threads)
			m_worker_thread.set_num_threads(s->aio_threads);
#endif

		m_settings = *s;
		delete s;

		m_file_pool.resize(m_settings.file_pool_size);
#if defined __APPLE__ && defined __MACH__ && MAC_OS_X_VERSION_MIN_REQUIRED >= 1050
		setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD
			, m_settings.low_prio_disk ? IOPOL_THROTTLE : IOPOL_DEFAULT);
#elif defined IOPRIO_WHO_PROCESS
		syscall(ioprio_set, IOPRIO_WHO_PROCESS, getpid());
#endif
		if (m_settings.cache_size == -1)
		{
			// the cache size is set to automatic. Make it
			// depend on the amount of physical RAM
			// if we don't know how much RAM we have, just set the
			// cache size to 16 MiB (1024 blocks)
			if (m_physical_ram == 0)
				m_settings.cache_size = 1024;
			else
				m_settings.cache_size = m_physical_ram / 8 / block_size;
		}
		m_disk_cache.set_settings(m_settings);

		// deduct the writing blocks from the cache size, otherwise we'll flush the
		// entire cache as soon as we exceed the limit, since all flush operations are
		// async.
		int num_pending_write_blocks = (m_pending_buffer_size + block_size - 1) / block_size;
		int current_size = m_disk_cache.in_use();
		if (current_size - num_pending_write_blocks > m_settings.cache_size)
			m_disk_cache.try_evict_blocks(current_size - m_settings.cache_size, 0);

		return 0;
	}

	int disk_io_thread::do_cache_piece(disk_io_job* j)
	{
		INVARIANT_CHECK;
		TORRENT_ASSERT(j->buffer == 0);

		cached_piece_entry* pe = m_disk_cache.allocate_piece(j, cached_piece_entry::read_lru1);
		if (pe == 0)
		{
			j->error.ec = errors::no_memory;
			return disk_operation_failed;
		}
		int ret = m_disk_cache.allocate_pending(pe, 0, pe->blocks_in_piece, j);

		if (ret > 0)
		{
			io_range(pe, 0, INT_MAX, op_read, j->flags);
			return defer_handler;
		}
		else if (ret == -1)
		{
			TORRENT_ASSERT(j->buffer == 0);
			j->error.ec = error::no_memory;
			return disk_operation_failed;
		}
		// the piece is already in the cache
		return 0;
	}

	int disk_io_thread::do_finalize_file(disk_io_job* j)
	{
		j->storage->get_storage_impl()->finalize_file(j->piece, j->error);
		return j->error ? disk_operation_failed : 0;
	}

	void disk_io_thread::get_disk_metrics(cache_status& ret) const
	{
		ret = m_cache_stats;

		ret.total_used_buffers = m_disk_cache.in_use();
#if TORRENT_USE_SYNCIO
		ret.elevator_turns = m_elevator_turns;
#else
		ret.elevator_turns = 0;
#endif
		ret.queued_bytes = m_pending_buffer_size + m_queue_buffer_size;

		ret.blocked_jobs = m_num_blocked_jobs;
		ret.queued_jobs = m_num_to_issue;
		ret.peak_queued = m_peak_num_to_issue;
		ret.pending_jobs = m_outstanding_jobs;
		ret.peak_pending = m_peak_outstanding;
		ret.num_aiocb = m_aiocb_pool.in_use();
		ret.peak_aiocb = m_aiocb_pool.peak_in_use();
		ret.num_jobs = m_aiocb_pool.jobs_in_use();
		ret.num_read_jobs = m_aiocb_pool.read_jobs_in_use();
		ret.num_write_jobs = m_aiocb_pool.write_jobs_in_use();

		m_disk_cache.get_stats(&ret);
	}

	void disk_io_thread::flip_stats()
	{
		// calling mean() will actually reset the accumulators
		m_cache_stats.average_queue_time = m_queue_time.mean();
		m_cache_stats.average_read_time = m_read_time.mean();
		m_cache_stats.average_write_time = m_write_time.mean();
		m_cache_stats.average_hash_time = m_hash_time.mean();
		m_cache_stats.average_job_time = m_job_time.mean();
		m_cache_stats.average_sort_time = m_sort_time.mean();
		m_cache_stats.average_issue_time = m_issue_time.mean();
		m_last_stats_flip = time_now();
	}

	void get_cache_info(cached_piece_info& info, cached_piece_entry const* i, int block_size)
	{
		info.piece = i->piece;
		info.last_use = i->expire;
		info.need_readback = i->need_readback;
		info.next_to_hash = i->hash == 0 ? -1 : (i->hash->offset + block_size - 1) / block_size;
		info.kind = i->num_dirty ? cached_piece_info::write_cache : cached_piece_info::read_cache;
		int blocks_in_piece = i->blocks_in_piece;
		info.blocks.resize(blocks_in_piece);
		for (int b = 0; b < blocks_in_piece; ++b)
			info.blocks[b] = i->blocks[b].buf != 0;
		// count the number of jobs hanging off of this piece, keep
		// separate counts per type of job
		memset(info.num_jobs, 0, sizeof(info.num_jobs));
		for (tailqueue_iterator iter = i->jobs.iterate(); iter.get(); iter.next())
			++info.num_jobs[((disk_io_job*)iter.get())->action];
	}

	int disk_io_thread::do_get_cache_info(disk_io_job* j)
	{
		cache_status* ret = (cache_status*)j->buffer;
		get_disk_metrics(*ret);

		if (j->flags & disk_io_job::no_pieces) return 0;

		int block_size = m_disk_cache.block_size();

		// TODO: this is potentially pretty expensive, allow cache info jobs to not ask for this full list
		if (j->storage)
		{
			ret->pieces.resize(j->storage->num_pieces());
			int counter = 0;

			for (boost::unordered_set<cached_piece_entry*>::iterator i
				= j->storage->cached_pieces().begin(), end(j->storage->cached_pieces().end());
				i != end; ++i, ++counter)
				get_cache_info(ret->pieces[counter], *i, block_size);
		}
		else
		{
			ret->pieces.resize(m_disk_cache.num_pieces());
			int counter = 0;
			std::pair<block_cache::iterator, block_cache::iterator> range
				= m_disk_cache.all_pieces();

			for (block_cache::iterator i = range.first; i != range.second; ++i, ++counter)
				get_cache_info(ret->pieces[counter], &*i, block_size);
		}
		return 0;
	}

	int disk_io_thread::do_hashing_done(disk_io_job* j)
	{
		m_hash_thread.hash_job_done();
		m_disk_cache.hashing_done((cached_piece_entry*)j->buffer
			, j->piece, j->d.io.offset, m_completed_jobs);
		return 0;
	}

	int disk_io_thread::do_file_status(disk_io_job* j)
	{
		std::vector<pool_file_status>* files = (std::vector<pool_file_status>*)j->buffer;
		m_file_pool.get_status(files, (void*)j->storage->get_storage_impl());
		return 0;
	}

	int disk_io_thread::do_reclaim_block(disk_io_job* j)
	{
		TORRENT_ASSERT(j->d.io.ref.storage);
		if (j->d.io.ref.block < 0)
		{
			TORRENT_ASSERT(false);
			return 0;
		}

		m_disk_cache.reclaim_block(j->d.io.ref, m_completed_jobs);
		return 0;
	}

	// TODO: does this function belong to block_cache?
	int disk_io_thread::do_clear_piece(disk_io_job* j)
	{
		cached_piece_entry* pe = m_disk_cache.find_piece(j);
		if (pe == 0) return 0;

		// cancel all jobs (at least the ones that haven't
		// started yet).
		storage_error e;
		e.ec = error_code(boost::system::errc::operation_canceled, get_system_category());

		disk_io_job* k = (disk_io_job*)pe->jobs.get_all();
		while (k)
		{
			disk_io_job* j = k;
			k = (disk_io_job*)k->next;
			j->next = 0;

			if (j->action != disk_io_job::write)
			{
				pe->jobs.push_back(j);
				continue;
			}

			int job_start = j->d.io.offset / m_disk_cache.block_size();
			int job_last = (j->d.io.offset + j->d.io.buffer_size - 1) / m_disk_cache.block_size();
			if (pe->blocks[job_start].pending
				|| pe->blocks[job_last].pending)
			{
				pe->jobs.push_back(j);
				continue;
			}
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT(j->callback_called == false);
			j->callback_called = true;
#endif
			j->error = e;
			m_completed_jobs.push_back(j);
		}

		m_disk_cache.evict_piece(pe);
		return 0;
	}

	// if the piece doesn't have any outstanding operations
	// queued on it, complete immediately and return true.
	// if it has outstanding operations, add the job to it
	// and return false. The job will be completed when the
	// piece no longer have any outstanding operations
	int disk_io_thread::do_sync_piece(disk_io_job* j)
	{
		cached_piece_entry* pe = m_disk_cache.find_piece(j);
		if (pe == 0) return 0;
		if (pe->jobs.empty()) return 0;
		pe->storage->new_job(j);
		pe->jobs.push_back(j);
		return defer_handler;
	}

	int disk_io_thread::do_flush_piece(disk_io_job* j)
	{
		cached_piece_entry* pe = m_disk_cache.find_piece(j);

		// flush the write jobs for this piece
		if (pe != 0 && pe->num_dirty > 0)
		{
			DLOG(stderr, "[%p] do_flush_piece: flushing %d dirty blocks piece: %d\n"
				, this, int(pe->num_dirty), int(pe->piece));
			// issue write commands
			io_range(pe, 0, INT_MAX, op_write, j->flags);
		}
		return 0;
	}

	int disk_io_thread::do_trim_cache(disk_io_job* j)
	{
		// no need to do anything in here, since perform_async_job() always
		// trims the cache
		return 0;
	}

	int disk_io_thread::do_aiocb_complete(disk_io_job* j)
	{
		file::aiocb_t* aios = (file::aiocb_t*)j->buffer;
		aios->handler->done(j->error, j->ret, aios, &m_aiocb_pool);
		return 0;
	}

	void disk_io_thread::on_write_one_buffer(async_handler* handler, disk_io_job* j)
	{
		int ret = j->d.io.buffer_size;
		TORRENT_ASSERT(handler->error.ec || handler->transferred == j->d.io.buffer_size);

		TORRENT_ASSERT(m_pending_buffer_size >= j->d.io.buffer_size);
		m_pending_buffer_size -= j->d.io.buffer_size;

		m_disk_cache.free_buffer(j->buffer);
		j->buffer = 0;

		DLOG(stderr, "[%p] on_write_one_buffer piece=%d offset=%d error=%s\n"
			, this, j->piece, j->d.io.offset, handler->error.ec.message().c_str());
		if (handler->error.ec)
		{
			j->error = handler->error;
			ret = -1;
		}
		else
		{
			boost::uint32_t write_time = total_microseconds(time_now_hires() - handler->started);
			m_write_time.add_sample(write_time);
			m_job_time.add_sample(write_time);
			m_cache_stats.cumulative_write_time += write_time;
			m_cache_stats.cumulative_job_time += write_time;
		}

		++m_cache_stats.blocks_written;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(j->callback_called == false);
		j->callback_called = true;
#endif
		m_completed_jobs.push_back(j);
	}

	void disk_io_thread::on_read_one_buffer(async_handler* handler, disk_io_job* j)
	{
		TORRENT_ASSERT(m_outstanding_jobs > 0);
		--m_outstanding_jobs;
		DLOG(stderr, "[%p] on_read_one_buffer piece=%d offset=%d error=%s\n"
			, this, j->piece, j->d.io.offset, handler->error.ec.message().c_str());
		int ret = j->d.io.buffer_size;
		j->error = handler->error;
		if (!j->error && handler->transferred != j->d.io.buffer_size)
			j->error.ec = errors::file_too_short;

		if (j->error)
		{
			TORRENT_ASSERT(j->buffer == 0);
			ret = -1;
		}
		else
		{
			boost::uint32_t read_time = total_microseconds(time_now_hires() - handler->started);
			m_read_time.add_sample(read_time);
			m_job_time.add_sample(read_time);
			m_cache_stats.cumulative_read_time += read_time;
			m_cache_stats.cumulative_job_time += read_time;
		}

		file::iovec_t vec;
		vec.iov_base = (file::iovec_base_t)j->buffer;
		vec.iov_len = j->d.io.buffer_size;

		j->storage->get_storage_impl()->readv_done(&vec, 1, j->piece, j->d.io.offset);

		++m_cache_stats.blocks_read;

		// the only way the buffer is freed is by a callback
		TORRENT_ASSERT(j->callback);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		TORRENT_ASSERT(j->callback_called == false);
		j->callback_called = true;
#endif
		m_completed_jobs.push_back(j);
	}

	// This is sometimes called from an outside thread!
	void disk_io_thread::add_job(disk_io_job* j, bool high_priority)
	{
		j->start_time = time_now_hires();

		TORRENT_ASSERT(!m_abort
			|| j->action == disk_io_job::reclaim_block
			|| j->action == disk_io_job::hash_complete
			|| j->action == disk_io_job::flush_piece);

		mutex::scoped_lock l(m_job_mutex);

		if (high_priority)
			m_queued_jobs.push_front(j);
		else
			m_queued_jobs.push_back(j);

		if (j->action == disk_io_job::write)
		{
			j->flags |= disk_io_job::counts_towards_queue_size;
			m_queue_buffer_size += j->d.io.buffer_size;
		}
		else
		{
			// just in case it was set when the job was passed in
			TORRENT_ASSERT((j->flags & disk_io_job::counts_towards_queue_size) == 0);
			j->flags &= ~disk_io_job::counts_towards_queue_size;
		}

		l.unlock();

		DLOG(stderr, "[%p] add_job job: %s\n", this, job_action_name[j->action]);

		// high priority jobs tries to wake up the disk thread immediately
		if (high_priority) submit_jobs_impl();
	}

	void disk_io_thread::submit_jobs()
	{
		mutex::scoped_lock l (m_job_mutex);
		if (m_queued_jobs.empty()) return;
		l.unlock();

		submit_jobs_impl();
	}

	void disk_io_thread::submit_jobs_impl()
	{
		// wake up the disk thread to issue this new job
#if TORRENT_USE_OVERLAPPED
		PostQueuedCompletionStatus(m_completion_port, 1, 0, 0);
#elif TORRENT_USE_AIO_SIGNALFD || TORRENT_USE_IOSUBMIT
		boost::uint64_t dummy = 1;
		int len = write(m_job_event_fd, &dummy, sizeof(dummy));
		DLOG(stderr, "[%p] write(m_job_event_fd) = %d\n", this, len);
		TORRENT_ASSERT(len == sizeof(dummy));
#elif TORRENT_USE_AIO_PORTS
		port_send(m_port, 1, 0);
#elif TORRENT_USE_AIO_KQUEUE
		boost::uint8_t dummy = 0;
		int len = write(m_job_pipe[0], &dummy, sizeof(dummy));
		DLOG(stderr, "[%p] write(m_job_pipe) = %d\n", this, len);
		TORRENT_ASSERT(len == sizeof(dummy));
#else
		g_job_sem.signal_all();
#endif
	}

#if TORRENT_USE_AIO && !TORRENT_USE_AIO_SIGNALFD && !TORRENT_USE_AIO_PORTS && !TORRENT_USE_AIO_KQUEUE

	void disk_io_thread::signal_handler(int signal, siginfo_t* si, void*)
	{
		DLOG(stderr, "*** signal_handler: %d siginfo: %p\n", signal, si->si_value.sival_ptr);

		if (signal != TORRENT_AIO_SIGNAL) return;

		++g_completed_aios;
		// wake up the disk thread to
		// make it handle these completed jobs
		g_job_sem.signal_all();
	}
#endif

#ifdef TORRENT_DEBUG
#define TORRENT_ASSERT_VALID_AIOCB(x) \
	do { \
		TORRENT_ASSERT(m_aiocb_pool.is_from(x)); \
		bool found = false; \
		for (file::aiocb_t* i = m_in_progress; i; i = i->next) { \
			if (i != x) continue; \
			found = true; \
			break; \
		} \
		TORRENT_ASSERT(found); \
	} while(false)
#else
#define TORRENT_ASSERT_VALID_AIOCB(x) do {} while(false)
#endif // TORRENT_DEBUG

	void disk_io_thread::thread_fun()
	{
#if defined TORRENT_DEBUG && defined BOOST_HAS_PTHREADS
		m_file_pool.set_thread_owner();
#endif

#if TORRENT_USE_OVERLAPPED
		TORRENT_ASSERT(m_completion_port != INVALID_HANDLE_VALUE);
		m_file_pool.set_iocp(m_completion_port);
#endif

#ifdef TORRENT_DISK_STATS
		m_aiocb_pool.file_access_log = fopen("file_access.log", "w+");
#endif


#if TORRENT_USE_RLIMIT
		// ---- auto-cap open files ----

		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
		{
			// deduct some margin for epoll/kqueue, log files,
			// futexes, shared objects etc.
			rl.rlim_cur -= 20;

			// 80% of the available file descriptors should go to connections
			// 20% goes towards regular files
			m_file_pool.resize((std::min)(m_file_pool.size_limit(), int(rl.rlim_cur * 2 / 10)));
		}
#endif // TORRENT_USE_RLIMIT

		// figure out how much physical RAM there is in
		// this machine. This is used for automatically
		// sizing the disk cache size when it's set to
		// automatic.
#ifdef TORRENT_BSD
#ifdef HW_MEMSIZE
		int mib[2] = { CTL_HW, HW_MEMSIZE };
#else
		// not entirely sure this sysctl supports 64
		// bit return values, but it's probably better
		// than not building
		int mib[2] = { CTL_HW, HW_PHYSMEM };
#endif
		size_t len = sizeof(m_physical_ram);
		if (sysctl(mib, 2, &m_physical_ram, &len, NULL, 0) != 0)
			m_physical_ram = 0;
#elif defined TORRENT_WINDOWS
		MEMORYSTATUSEX ms;
		ms.dwLength = sizeof(MEMORYSTATUSEX);
		if (GlobalMemoryStatusEx(&ms))
			m_physical_ram = ms.ullTotalPhys;
		else
			m_physical_ram = 0;
#elif defined TORRENT_LINUX
		m_physical_ram = sysconf(_SC_PHYS_PAGES);
		m_physical_ram *= sysconf(_SC_PAGESIZE);
#elif defined TORRENT_AMIGA
		m_physical_ram = AvailMem(MEMF_PUBLIC);
#endif

#if TORRENT_USE_RLIMIT
		if (m_physical_ram > 0)
		{
			struct rlimit r;
			if (getrlimit(RLIMIT_AS, &r) == 0 && r.rlim_cur != RLIM_INFINITY)
			{
				if (m_physical_ram > r.rlim_cur)
					m_physical_ram = r.rlim_cur;
			}
		}
#endif

#if TORRENT_USE_AIO
#if defined BOOST_HAS_PTHREADS
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, TORRENT_AIO_SIGNAL);

// if we're using signalfd, we don't want a signal handler to catch our
// signal, but our file descriptor to swallow all of them
#if TORRENT_USE_AIO_SIGNALFD
		m_signal_fd[0] = signalfd(-1, &mask, SFD_NONBLOCK);
		// #error error handling needed

		if (pthread_sigmask(SIG_BLOCK, &mask, 0) == -1)
		{
			TORRENT_ASSERT(false);
		}
#else
		if (pthread_sigmask(SIG_UNBLOCK, &mask, 0) == -1)
		{
			TORRENT_ASSERT(false);
		}
#endif // TORRENT_USE_AIO_SIGNALFD
#endif // BOOST_HAS_PTHREADS
#endif // TORRENT_USE_AIO

#if TORRENT_USE_AIO && !TORRENT_USE_AIO_SIGNALFD && !TORRENT_USE_AIO_PORTS && !TORRENT_USE_AIO_KQUEUE
		struct sigaction sa;

		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sa.sa_sigaction = &disk_io_thread::signal_handler;
		sigemptyset(&sa.sa_mask);

		int ret = sigaction(TORRENT_AIO_SIGNAL, &sa, 0);
		DLOG(stderr, "[%p] sigaction() = %d\n", this, ret);
		if (ret == -1)
		{
			TORRENT_ASSERT(false);
		}
#endif

#if (TORRENT_USE_AIO && !TORRENT_USE_AIO_SIGNALFD && !TORRENT_USE_AIO_PORTS) \
	|| TORRENT_USE_SYNCIO
		int last_completed_aios = 0;
#endif

		do
		{
			bool new_job = false;
			bool iocbs_reaped = false;

			// TODO: this large portion of code that's platform and io-backend
			// dependent should really be factored out into separate functions
#if TORRENT_USE_OVERLAPPED
			TORRENT_ASSERT(m_completion_port != INVALID_HANDLE_VALUE);
			file::aiocb_t* aio = 0;
			DWORD bytes_transferred;
			ULONG_PTR key;
			OVERLAPPED* ol = 0;
			DLOG(stderr, "[%p] GetQueuedCompletionStatus()\n", this);
			bool ret = GetQueuedCompletionStatus(m_completion_port
				, &bytes_transferred, &key, &ol, INFINITE);
			if (ret == false)
			{
				error_code ec(GetLastError(), get_system_category());
				DLOG(stderr, "[%p] GetQueuedCompletionStatus() = FALSE %s\n"
					, this, ec.message().c_str());
				sleep(10);
			}
			if (key == NULL && ol != 0)
			{
				file::aiocb_t* aio = to_aiocb(ol);
				// since synchronous calls also use overlapped
				// we'll get some stack allocated overlapped structures
				// as well. Once everything is moved over to async.
				// operations, hopefully this won't be needed anymore
				if (!m_aiocb_pool.is_from(aio)) continue;
				TORRENT_ASSERT_VALID_AIOCB(aio);
				file::aiocb_t* next = aio->next;
				bool removed = reap_aio(aio, m_aiocb_pool);
				if (removed) ++m_cache_stats.cumulative_completed_aiocbs;
				iocbs_reaped = removed;
				if (removed && m_in_progress == aio) m_in_progress = next;
				DLOG(stderr, "[%p] overlapped = %p removed = %d\n", this, ol, removed);
			}
			else
			{
				// this should only happen for our own posted
				// events from add_job()
//				TORRENT_ASSERT(key == 1);
				new_job = true;
			}
#elif TORRENT_USE_IOSUBMIT
			fd_set set;
			FD_ZERO(&set);
			FD_SET(m_disk_event_fd, &set);
			FD_SET(m_job_event_fd, &set);
			DLOG(stderr, "[%p] select(m_disk_event_fd, m_job_event_fd)\n", this);
			int ret = select((std::max)(m_disk_event_fd, m_job_event_fd) + 1, &set, 0, 0, 0);
			DLOG(stderr, "[%p]  = %d\n", this, ret);

			if (FD_ISSET(m_job_event_fd, &set))
			{
				boost::uint64_t n = 0;
				int ret = read(m_job_event_fd, &n, sizeof(n));
				if (ret != sizeof(n)) DLOG(stderr, "[%p] read(m_job_event_fd) = %d %s\n"
					, this, ret, strerror(errno));
				new_job = true;
			}

			if (FD_ISSET(m_disk_event_fd, &set))
			{
				// at least one disk event finished, maybe more.
				// reading from the event fd will reset the event
				// and tell us how many times it was fired. i.e.
				// how many disk events are ready to be reaped
				const int max_events = 512;
				io_event events[max_events];
				boost::int64_t n = 0;
				int ret = read(m_disk_event_fd, &n, sizeof(n));
				if (ret != sizeof(n)) DLOG(stderr, "[%p] read(m_disk_event_fd) = %d %s\n"
					, this, ret, strerror(errno));

				DLOG(stderr, "[%p] %"PRId64" completed disk jobs\n", this, n);

				int num_events = 0;
				do
				{
					// if we allow reading more than n jobs here, there is a race condition
					// since there might have been more jobs completed since we read the
					// event fd, we could end up reaping more events than were signalled by the
					// event fd, resulting in trying to reap them again later, getting stuck
					num_events = io_getevents(m_io_queue, 1, (std::min)(max_events, int(n)), events, NULL);
					if (num_events < 0) DLOG(stderr, "[%p] io_getevents() = %d %s\n"
						, this, num_events, strerror(-num_events));

					for (int i = 0; i < num_events; ++i)
					{
						file::aiocb_t* aio = to_aiocb(events[i].obj);
						TORRENT_ASSERT(aio->in_use);
						TORRENT_ASSERT_VALID_AIOCB(aio);
						file::aiocb_t* next = aio->next;
						// copy the return codes from the io_event
						aio->ret = events[i].res;
						aio->error = events[i].res < 0 ? -events[i].res : 0;
						bool removed = reap_aio(aio, m_aiocb_pool);
						if (removed) ++m_cache_stats.cumulative_completed_aiocbs;
						iocbs_reaped = removed;
						if (removed && m_in_progress == aio) m_in_progress = next;
						DLOG(stderr, "[%p]  removed = %d\n", this, removed);
					}
					if (num_events > 0) n -= num_events;
				} while (num_events == max_events);
			}

#elif TORRENT_USE_AIO_SIGNALFD
			// wait either for a signal coming in through the 
			// signalfd or an add-job even coming in through
			// the eventfd
			fd_set set;
			FD_ZERO(&set);
			FD_SET(m_signal_fd[0], &set);
			FD_SET(m_signal_fd[1], &set);
			FD_SET(m_job_event_fd, &set);
			DLOG(stderr, "[%p] select(m_signal_fd, m_job_event_fd)\n", this);
			int ret = select((std::max)((std::max)(m_signal_fd[0], m_signal_fd[1]), m_job_event_fd) + 1, &set, 0, 0, 0);
			DLOG(stderr, "[%p]  = %d\n", this, ret);
			if (FD_ISSET(m_job_event_fd, &set))
			{
				// yes, there's a new job available
				boost::uint64_t dummy;
				int len = read(m_job_event_fd, &dummy, sizeof(dummy));
				TORRENT_ASSERT(len == sizeof(dummy));
				new_job = true;
			}
			for (int i = 0; i < 2; ++i) {
			if (FD_ISSET(m_signal_fd[i], &set))
			{
				int len = 0;
				signalfd_siginfo sigbuf[30];
				do
				{
					len = read(m_signal_fd[i], sigbuf, sizeof(sigbuf));
					if (len <= 0)
					{
						error_code ec(errno, get_system_category());
						DLOG(stderr, "[%p] read() = %d %s\n", this, len, ec.message().c_str());
						break;
					}
					DLOG(stderr, "[%p] read() = %d\n", this, len);
					TORRENT_ASSERT((len % sizeof(signalfd_siginfo)) == 0);
					for (int i = 0; i < len / sizeof(signalfd_siginfo); ++i)
					{
						signalfd_siginfo* siginfo = &sigbuf[i];
						// this is not an AIO signal.
						if (siginfo->ssi_signo != TORRENT_AIO_SIGNAL) continue;
						// the userdata pointer in our iocb requests is the pointer
						// to our aiocb_t link
						file::aiocb_t* aio = (file::aiocb_t*)siginfo->ssi_ptr;
						TORRENT_ASSERT_VALID_AIOCB(aio);
						file::aiocb_t* next = aio->next;
						bool removed = reap_aio(aio, m_aiocb_pool);
						if (removed) ++m_cache_stats.cumulative_completed_aiocbs;
						iocbs_reaped = removed;
						if (removed && m_in_progress == aio) m_in_progress = next;
						DLOG(stderr, "[%p]  removed = %d\n", this, removed);
					}
					// if we filled our signal buffer, read again
					// until we read less than our max
				} while (len == sizeof(sigbuf));
			}
			}
#elif TORRENT_USE_AIO_PORTS
			const int max_events = 300;
			uint_t num_events = 1;
			port_event_t events[max_events];
			// if there are no events in 5 seconds, return anyway in order to
			// flush write blocks
			timespec sp = { 5, 0 };
			DLOG(stderr, "[%p] port_getn()\n", this);
			int ret = port_getn(m_port, events, max_events, &num_events, &sp);
			DLOG(stderr, "[%p]  = %d nget: %d\n", this, ret, num_events);

			for (int i = 0; i < num_events; ++i)
			{
				if (events[i].portev_source == PORT_SOURCE_USER)
				{
					new_job = true;
					continue;
				}
				if (events[i].portev_source != PORT_SOURCE_AIO)
				{
					TORRENT_ASSERT(false);
					continue;
				}
				// at this point, event[i] refers to an AIO event
				// and the user-data pointer points to our aiocb_t

				file::aiocb_t* aio = (file::aiocb_t*)events[i].portev_user;

				TORRENT_ASSERT_VALID_AIOCB(aio);
				file::aiocb_t* next = aio->next;
				bool removed = reap_aio(aio, m_aiocb_pool);
				if (removed) ++m_cache_stats.cumulative_completed_aiocbs;
				iocbs_reaped = removed;
				if (removed && m_in_progress == aio) m_in_progress = next;
				DLOG(stderr, "[%p]  removed = %d\n", this, removed);
			}

#elif TORRENT_USE_AIO_KQUEUE
			const int max_events = 300;
			struct kevent events[max_events];
			// if there are no events in 5 seconds, return anyway in order to
			// flush write blocks
			timespec sp = { 5, 0 };
			DLOG(stderr, "[%p] kevent()\n", this);
			int num_events = kevent(m_queue, 0, 0, events, max_events, &sp);
			DLOG(stderr, "[%p]  = %d\n", this, num_events);

			for (int i = 0; i < num_events; ++i)
			{
				struct kevent& e = events[i];
				if (e.filter == EVFILT_READ && e.ident == m_job_pipe[1])
				{
					new_job = true;
					continue;
				}
				if (e.filter == EVFILT_AIO)
				{
					// at this point, event[i] refers to an AIO event
					// and the user-data pointer points to our aiocb_t

					file::aiocb_t* aio = (file::aiocb_t*)e.udata;
					TORRENT_ASSERT((void*)e.data == (void*)&aio->cb);

					TORRENT_ASSERT_VALID_AIOCB(aio);
					file::aiocb_t* next = aio->next;
					bool removed = reap_aio(aio, m_aiocb_pool);
					if (removed) ++m_cache_stats.cumulative_completed_aiocbs;
					iocbs_reaped = removed;
					if (removed && m_in_progress == aio) m_in_progress = next;
					DLOG(stderr, "[%p]  removed = %d\n", this, removed);
					continue;
				}
				DLOG(stderr, "[%p] unknown event [ filter: %d ident: %p flags: %d fflags: %d data: %p udata: %p ]\n"
					, this, int(e.filter), e.ident, int(e.flags), int(e.fflags), e.data, e.udata);
				TORRENT_ASSERT(false);
			}

#else
			// always time out after half a second, since the global nature of the semaphore
			// makes it unreliable when there are multiple instances of the disk_io_thread
			// object. There might also a potential race condition if the semaphore is signalled
			// right before we start waiting on it
			if (last_completed_aios == g_completed_aios)
			{
//				DLOG(stderr, "[%p] sem_wait()\n", this);
				g_job_sem.timed_wait(500);
//				DLOG(stderr, "[%p] sem_wait() returned\n", this);
			}

			// more jobs might complete as we go through
			// the list. In which case m_completed_aios
			// would have incremented again. It's incremented
			// in the aio signal handler
			int complete_aios = g_completed_aios;
			while (complete_aios != last_completed_aios)
			{
				DLOG(stderr, "[%p] m_completed_aios %d last_completed_aios: %d\n"
					, this, complete_aios, last_completed_aios);

				// this needs to be atomic for the signal handler
				int tmp = g_completed_aios;
				last_completed_aios = complete_aios;
				complete_aios = tmp;
				// go through all outstanding disk operations
				// and potentially dispatch ones that are complete
				DLOG(stderr, "[%p] reap in progress aios (%p)\n", this, m_in_progress);
				m_in_progress = reap_aios(m_in_progress, m_aiocb_pool);
				DLOG(stderr, "[%p] new in progress aios (%p)\n", this, m_in_progress);
				m_cache_stats.cumulative_completed_aiocbs = g_completed_aios;
			}
			new_job = true;
			iocbs_reaped = true;
#endif

			ptime now = time_now_hires();
			if (now > m_last_cache_expiry + seconds(5))
			{
				DLOG(stderr, "[%p] blocked_jobs: %d queued_jobs: %d abort: %d\n"
					, this, m_num_blocked_jobs, m_queued_jobs.size(), m_abort);
				m_last_cache_expiry = now;
				flush_expired_write_blocks();
			}

#if TORRENT_USE_SUBMIT_THREADS
			if (iocbs_reaped)
			{
				m_submit_queue.kick();
			}
#endif

			// if we have new jobs, pop them and run them
			if (new_job)
			{
				// keep the mutex locked for as short as possible
				// while we swap out all the jobs in the queue
				// we can then go through the queue without having
				// to block the mutex
				mutex::scoped_lock l(m_job_mutex);
				disk_io_job* j = (disk_io_job*)m_queued_jobs.get_all();
				l.unlock();
				if (j)
				{
					DLOG(stderr, "[%p] new jobs\n", this);
				}

				// go through list of newly submitted jobs
				// and perform the appropriate action
				while (j)
				{
					if (j->flags & disk_io_job::counts_towards_queue_size)
					{
						mutex::scoped_lock l(m_job_mutex);
						TORRENT_ASSERT(m_queue_buffer_size >= j->d.io.buffer_size);
						m_queue_buffer_size -= j->d.io.buffer_size;
						l.unlock();
						// clear the flag so we don't do this again
						j->flags &= ~disk_io_job::counts_towards_queue_size;
					}

					disk_io_job* job = j;
					j = (disk_io_job*)j->next;
					job->next = 0;
					perform_async_job(job);
				}

				int evict = m_disk_cache.num_to_evict(0);
				if (evict > 0)
				{
					evict = m_disk_cache.try_evict_blocks(evict, 1);
					if (evict > 0) try_flush_write_blocks(evict);
				}
			}

			if (!m_completed_jobs.empty())
			{
				DLOG(stderr, "[%p] posting %d completed jobs\n", this, m_completed_jobs.size());
				disk_io_job* j = (disk_io_job*)m_completed_jobs.get_all();
				for (disk_io_job* i = j; i != NULL; i = (disk_io_job*)i->next)
				{
					if ((i->flags & disk_io_job::async_operation) && i->storage)
					{
						tailqueue jobs;
						int ret = i->storage->job_complete(i, jobs);
						if (!jobs.empty())
						{
							mutex::scoped_lock l(m_job_mutex);
							m_queued_jobs.append(jobs);
							submit_jobs_impl();
						}
						if (ret) DLOG(stderr, "[%p] unblocked %d jobs (%d left)\n", this, ret, m_num_blocked_jobs - ret);
						TORRENT_ASSERT(m_num_blocked_jobs >= ret);
						m_num_blocked_jobs -= ret;
					}
				}
				m_ios.post(boost::bind(&complete_job, m_userdata, &m_aiocb_pool, j));
			}

			// tell the kernel about the async disk I/O jobs we want to perform

			// if we're on a system that doesn't do async. I/O, we should only perform
			// one at a time in case new jobs are issued that should take priority (such
			// as asking for stats)
			if (m_to_issue)
			{
				ptime start = time_now_hires();
#if TORRENT_USE_SYNCIO
				if (!same_sign(m_to_issue->phys_offset - m_last_phys_off, m_elevator_direction))
				{
					m_elevator_direction *= -1;
					++m_elevator_turns;
				}

				m_last_phys_off = m_to_issue->phys_offset;

				DLOG(stderr, "[%p] issue aios (%p) phys_offset=%"PRId64" elevator=%d\n"
					, this, m_to_issue, m_to_issue->phys_offset, m_elevator_direction);
#else
				DLOG(stderr, "[%p] issue aios (%p)\n", this, m_to_issue);
#endif


				file::aiocb_t* pending;
				int num_issued = 0;
#if TORRENT_USE_SUBMIT_THREADS
				// #error move this into issue_aios, or just get rid of it
				num_issued = m_submit_queue.submit(m_to_issue);
				pending = m_to_issue;
				m_to_issue = 0;
#else
				boost::tie(pending, m_to_issue) = issue_aios(m_to_issue, m_aiocb_pool
					, num_issued);
#endif
				if (m_to_issue == 0) m_to_issue_end = 0;
				TORRENT_ASSERT(m_num_to_issue >= num_issued);
				m_num_to_issue -= num_issued;
				TORRENT_ASSERT(m_num_to_issue == count_aios(m_to_issue));
				DLOG(stderr, "[%p] prepend aios (%p) to m_in_progress (%p)\n"
					, this, pending, m_in_progress);

				prepend_aios(m_in_progress, pending);

				int issue_time = total_microseconds(time_now_hires() - start);
				m_issue_time.add_sample(issue_time);
				m_cache_stats.cumulative_issue_time += issue_time;

#if !TORRENT_USE_SYNCIO
				if (m_to_issue)
				{
					ptime now = time_now();
					if (now - m_last_disk_aio_performance_warning > seconds(10))
					{
						// there were some jobs that couldn't be posted
						// the the kernel. This limits the performance of
						// the disk throughput, issue a performance warning
						m_ios.post(boost::bind(alert_callback, m_post_alert
							, new performance_alert(torrent_handle(), performance_alert::aio_limit_reached)));
						m_last_disk_aio_performance_warning = now;
					}
				}
#endif
				if (num_issued == 0)
				{
					// we did not issue a single job! avoid spinning
					// and pegging the CPU
					TORRENT_ASSERT(iocbs_reaped);
					sleep(10);
				}
			}

			// now, we may have received the abort thread
			// message, and m_abort may have been set to
			// true, but we still need to wait for the outstanding
			// jobs, that's why we'll keep looping while m_in_progress
			// is has jobs in it as well
		
		} while (!m_abort || m_in_progress || m_to_issue
			|| m_completed_jobs.size() > 0 || m_num_blocked_jobs > 0
			|| m_hash_thread.num_pending_jobs() || m_disk_cache.pinned_blocks() > 0);

		m_hash_thread.stop();

		m_disk_cache.clear(m_completed_jobs);

		if (!m_completed_jobs.empty())
		{
			DLOG(stderr, "[%p] posting %d completed jobs\n", this, m_completed_jobs.size());
			disk_io_job* j = (disk_io_job*)m_completed_jobs.get_all();
			for (disk_io_job* i = j; i != NULL; i = (disk_io_job*)i->next)
			{
				if ((i->flags & disk_io_job::async_operation) && i->storage)
				{
					tailqueue jobs;
					int ret = i->storage->job_complete(i, jobs);
					if (!jobs.empty())
					{
						mutex::scoped_lock l(m_job_mutex);
						m_queued_jobs.append(jobs);
						submit_jobs_impl();
					}
					if (ret) DLOG(stderr, "[%p] unblocked %d jobs (%d left)\n", this, ret, m_num_blocked_jobs - ret);
					TORRENT_ASSERT(m_num_blocked_jobs >= ret);
					m_num_blocked_jobs -= ret;
				}
			}
			m_ios.post(boost::bind(&complete_job, m_userdata, &m_aiocb_pool, j));
		}

		TORRENT_ASSERT(m_queued_jobs.empty());

		// release the io_service to allow the run() call to return
		// we do this once we stop posting new callbacks to it.
		m_work.reset();
		DLOG(stderr, "[%p] exiting disk thread\n", this);

#ifdef TORRENT_DISK_STATS
		fclose(m_aiocb_pool.file_access_log);
		m_aiocb_pool.file_access_log = 0;
#endif
#if defined TORRENT_DEBUG && defined BOOST_HAS_PTHREADS
		m_file_pool.clear_thread_owner();
#endif
	}

	char* disk_io_thread::allocate_buffer(bool& exceeded
		, boost::function<void()> const& cb
		, char const* category)
	{
		bool trigger_trim = false;
		char* ret = m_disk_cache.allocate_buffer(exceeded, trigger_trim, cb, category);
		if (trigger_trim)
		{
			// we just exceeded the cache size limit. Trigger a trim job
			disk_io_job* j = m_aiocb_pool.allocate_job(disk_io_job::trim_cache);
			add_job(j, true);
		}
		return ret;
	}

#ifdef TORRENT_DEBUG
	void disk_io_thread::check_invariant() const
	{
	}
#endif
		
}

