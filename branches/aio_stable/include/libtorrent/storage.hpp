/*

Copyright (c) 2003, Arvid Norberg
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

#ifndef TORRENT_STORAGE_HPP_INCLUDE
#define TORRENT_STORAGE_HPP_INCLUDE

#include <vector>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/function/function2.hpp>
#include <boost/function/function0.hpp>
#include <boost/limits.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/unordered_set.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif


#include "libtorrent/torrent_info.hpp"
#include "libtorrent/piece_picker.hpp"
#include "libtorrent/intrusive_ptr_base.hpp"
#include "libtorrent/peer_request.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/config.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/disk_buffer_holder.hpp"
#include "libtorrent/thread.hpp"
#include "libtorrent/storage_defs.hpp"
#include "libtorrent/allocator.hpp"
#include "libtorrent/file_pool.hpp" // pool_file_status

namespace libtorrent
{
	class session;
	struct file_pool;
	struct disk_io_job;
	struct disk_buffer_pool;
	struct cache_status;
	struct aiocb_pool;
	struct session_settings;
	struct cached_piece_entry;

	void complete_job(void* user, aiocb_pool* pool, disk_io_job* j);

	TORRENT_EXTRA_EXPORT std::vector<std::pair<size_type, std::time_t> > get_filesizes(
		file_storage const& t
		, std::string const& p);

	TORRENT_EXTRA_EXPORT bool match_filesizes(
		file_storage const& t
		, std::string const& p
		, std::vector<std::pair<size_type, std::time_t> > const& sizes
		, bool compact_mode
		, std::string* error = 0);

	TORRENT_EXTRA_EXPORT int bufs_size(file::iovec_t const* bufs, int num_bufs);

	struct TORRENT_EXPORT storage_interface
	{
		storage_interface(): m_disk_pool(0), m_aiocb_pool(0), m_settings(0) {}
		// create directories and set file sizes
		// if allocate_files is true. 
		// allocate_files is true if allocation mode
		// is set to full and sparse files are supported
		// false return value indicates an error
		virtual void initialize(bool allocate_files, storage_error& ec) = 0;

		virtual file::aiocb_t* async_readv(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a) = 0;
		virtual file::aiocb_t* async_writev(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a) = 0;

		virtual void readv_done(file::iovec_t const* bufs, int num_bufs, int piece, int offset) {}

		virtual bool has_any_file(storage_error& ec) = 0;
		virtual void hint_read(int slot, int offset, int len) {}
		virtual size_type physical_offset(int slot, int offset) = 0;

		// returns the end of the sparse region the slot 'start'
		// resides in i.e. the next slot with content. If start
		// is not in a sparse region, start itself is returned
		// #error replace this with a query to see if a whole piece is mapped to actual storage, and no sparse hole overlaps with it. If a sparse hole overlaps, the do_hash operation can take a short cut
		virtual int sparse_end(int start) const { return start; }

		// non-zero return value indicates an error
		virtual void move_storage(std::string const& save_path, storage_error& ec) = 0;

		// verify storage dependent fast resume entries
		virtual bool verify_resume_data(lazy_entry const& rd, storage_error& ec) = 0;

		// write storage dependent fast resume entries
		virtual void write_resume_data(entry& rd, storage_error& ec) const = 0;

		// this will close all open files that are opened for
		// writing. This is called when a torrent has finished
		// downloading.
		// non-zero return value indicates an error
		virtual void release_files(storage_error& ec) = 0;

		// this will rename the file specified by index.
		virtual void rename_file(int index, std::string const& new_filenamem, storage_error& ec) = 0;

		// this will close all open files and delete them
		// non-zero return value indicates an error
		virtual void delete_files(storage_error& ec) = 0;

		virtual void finalize_file(int file, storage_error& ec) {}

		disk_buffer_pool* disk_pool() { return m_disk_pool; }
		aiocb_pool* aiocbs() { return m_aiocb_pool; }
		session_settings const& settings() const { return *m_settings; }

		virtual ~storage_interface() {}

		// initialized in piece_manager::piece_manager
		disk_buffer_pool* m_disk_pool;
		aiocb_pool* m_aiocb_pool;
		// initialized in disk_io_thread::perform_async_job
		session_settings* m_settings;
	};

	class TORRENT_EXPORT default_storage : public storage_interface, boost::noncopyable
	{
	public:
		default_storage(file_storage const& fs, file_storage const* mapped, std::string const& path
			, file_pool& fp, std::vector<boost::uint8_t> const& file_prio);
		~default_storage();

		void finalize_file(int file, storage_error& ec);
		bool has_any_file(storage_error& ec);
		void rename_file(int index, std::string const& new_filename, storage_error& ec);
		void release_files(storage_error& ec);
		void delete_files(storage_error& ec);
		void initialize(bool allocate_files, storage_error& ec);
		void move_storage(std::string const& save_path, storage_error& ec);
		int sparse_end(int start) const;
		void hint_read(int slot, int offset, int len);
		size_type physical_offset(int slot, int offset);
		bool verify_resume_data(lazy_entry const& rd, storage_error& error);
		void write_resume_data(entry& rd, storage_error& ec) const;

		file::aiocb_t* async_readv(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a);
		file::aiocb_t* async_writev(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a);

		// this identifies a read or write operation
		// so that default_storage::readwritev() knows what to
		// do when it's actually touching the file
		struct fileop
		{
			// file operation
			file::aiocb_t* (file::*op)(size_type offset
				, file::iovec_t const* bufs, int num_bufs
				, aiocb_pool&, int);
			// for async operations, this is the handler that will be added
			// to every aiocb_t in the returned chain
			async_handler* handler;
			// for async operations, this is the returned aiocb_t chain
			file::aiocb_t* ret;
			int cache_setting;
			// file open mode (file::read_only, file::write_only etc.)
			int mode;
			int flags;
			// used for error reporting
			int operation_type;
		};

		void delete_one_file(std::string const& p, error_code& ec);
		int readwritev(file::iovec_t const* bufs, int slot, int offset
			, int num_bufs, fileop& op, storage_error& ec);

		file_storage const& files() const { return m_mapped_files?*m_mapped_files:m_files; }

		boost::scoped_ptr<file_storage> m_mapped_files;
		file_storage const& m_files;

		// in order to avoid calling stat() on each file multiple times
		// during startup, cache the results in here, and clear it all
		// out once the torrent starts (to avoid getting stale results)
		// each slot represents the size and timestamp of the file
		// a size of:
		// -1 means error
		// -2 means no data (i.e. if we want to stat the file, we should
		//    do it and fill in this slot)
		// -3 file doesn't exist
		struct stat_cache_t
		{
			stat_cache_t(size_type s, time_t t = 0): file_size(s), file_time(t) {}
			size_type file_size;
			time_t file_time;
		};
		mutable std::vector<stat_cache_t> m_stat_cache;

		// helper function to open a file in the file pool with the right mode
		boost::intrusive_ptr<file> open_file(file_storage::iterator fe, int mode
			, int flags, error_code& ec) const;

		std::vector<boost::uint8_t> m_file_priority;
		std::string m_save_path;
		// the file pool is typically stored in
		// the session, to make all storage
		// instances use the same pool
		file_pool& m_pool;

		int m_page_size;
		bool m_allocate_files;
	};

	// this storage implementation does not write anything to disk
	// and it pretends to read, and just leaves garbage in the buffers
	// this is useful when simulating many clients on the same machine
	// or when running stress tests and want to take the cost of the
	// disk I/O out of the picture. This cannot be used for any kind
	// of normal bittorrent operation, since it will just send garbage
	// to peers and throw away all the data it downloads. It would end
	// up being banned immediately
	class disabled_storage : public storage_interface, boost::noncopyable
	{
	public:
		disabled_storage(int piece_size) : m_piece_size(piece_size) {}
		bool has_any_file(storage_error& ec) { return false; }
		void rename_file(int index, std::string const& new_filename, storage_error& ec) {}
		void release_files(storage_error& ec) {}
		void delete_files(storage_error& ec) {}
		void initialize(bool allocate_files, storage_error& ec) {}
		void move_storage(std::string const& save_path, storage_error& ec) {}
		size_type physical_offset(int slot, int offset) { return 0; }

		file::aiocb_t* async_readv(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a);
		file::aiocb_t* async_writev(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a);

		bool verify_resume_data(lazy_entry const& rd, storage_error& error) { return false; }
		void write_resume_data(entry& rd, storage_error& ec) const {}

		int m_piece_size;
	};

	// this storage implementation always reads zeroes, and always discards
	// anything written to it
	struct zero_storage : storage_interface
	{
		virtual void initialize(bool allocate_files, storage_error& ec) {}

		virtual file::aiocb_t* async_readv(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a);
		virtual file::aiocb_t* async_writev(file::iovec_t const* bufs, int num_bufs
			, int piece, int offset, int flags, async_handler* a);

		virtual bool has_any_file(storage_error& ec) { return false; }
		virtual size_type physical_offset(int slot, int offset) { return slot; }

		virtual void move_storage(std::string const& save_path, storage_error& ec) {}
		virtual bool verify_resume_data(lazy_entry const& rd, storage_error& ec) { return false; }
		virtual void write_resume_data(entry& rd, storage_error& ec) const {}
		virtual void release_files(storage_error& ec) {}
		virtual void rename_file(int index, std::string const& new_filenamem, storage_error& ec) {}
		virtual void delete_files(storage_error& ec) {}
	};

	struct disk_io_thread;

	// implements the disk I/O job fence used by the piece_manager
	// to provide to the disk thread. Whenever a disk job needs
	// exclusive access to the storage for that torrent, it raises
	// the fence, blocking all new jobs, until there are no longer
	// any outstanding jobs on the torrent, then the fence is lowered
	// and it can be performed, along with the backlog of jobs that
	// accrued while the fence was up
	struct disk_job_fence
	{
		disk_job_fence();

		void raise_fence(disk_io_job* j);
		bool has_fence() const { return m_has_fence; }

		void new_job(disk_io_job* j);
		// called whenever a job completes and is posted back to the
		// main network thread. the tailqueue of jobs will have the
		// backed-up jobs prepended to it in case this resulted in the
		// fence being lowered.
		int job_complete(disk_io_job* j, tailqueue& job_queue);
		bool has_outstanding_jobs() const { return m_outstanding_jobs; }

		// if there is a fence up, returns true and adds the job
		// to the queue of blocked jobs
		bool is_blocked(disk_io_job* j);
		
		// the number of blocked jobs
		int num_blocked() const { return m_blocked_jobs.size(); }

	private:
		// when set, this storage is blocked for new async
		// operations until all outstanding jobs have completed.
		// at that point, the m_blocked_jobs are issued
		bool m_has_fence;

		// when there's a fence up, jobs are queued up in here
		// until the fence is lowered
		tailqueue m_blocked_jobs;

		// the number of disk_io_job objects there are, belonging
		// to this torrent, currently pending, hanging off of
		// cached_piece_entry objects. This is used to determine
		// when the fence can be lowered
		int m_outstanding_jobs;

	};

	class TORRENT_EXTRA_EXPORT piece_manager
		: public intrusive_ptr_base<piece_manager>
		, public disk_job_fence
		, boost::noncopyable
	{
	friend class invariant_access;
	friend struct disk_io_thread;
	public:

		piece_manager(
			boost::shared_ptr<void> const& torrent
			, file_storage* files
			, file_storage const* orig_files
			, std::string const& path
			, disk_io_thread& io
			, storage_constructor_type sc
			, storage_mode_t sm
			, std::vector<boost::uint8_t> const& file_prio);

		~piece_manager();

		void set_abort_job(disk_io_job* j)
		{
			TORRENT_ASSERT(m_abort_job == 0);
			m_abort_job = j;
		}
		disk_io_job* pop_abort_job()
		{
			disk_io_job* j = m_abort_job;
			m_abort_job = 0;
			return j;
		}

		file_storage const* files() const { return &m_files; }

		void async_finalize_file(int file);

		void async_get_cache_info(cache_status* ret
			, boost::function<void(int, disk_io_job const&)> const& handler);

		void async_file_status(std::vector<pool_file_status>* ret
			, boost::function<void(int, disk_io_job const&)> const& handler);

		void async_check_fastresume(lazy_entry const* resume_data
			, boost::function<void(int, disk_io_job const&)> const& handler);
		
		void async_rename_file(int index, std::string const& name
			, boost::function<void(int, disk_io_job const&)> const& handler);

		void async_read(
			peer_request const& r
			, boost::function<void(int, disk_io_job const&)> const& handler
			, void* requester
			, int flags = 0
			, int cache_line_size = 0);

		void async_cache(int piece
			, boost::function<void(int, disk_io_job const&)> const& handler);

		void  async_write(
			peer_request const& r
			, disk_buffer_holder& buffer
			, boost::function<void(int, disk_io_job const&)> const& f
			, int flags = 0);

		void async_hash(int piece, int flags
			, boost::function<void(int, disk_io_job const&)> const& f
			, void* requester);

		void async_release_files(
			boost::function<void(int, disk_io_job const&)> const& handler
			= boost::function<void(int, disk_io_job const&)>());

		void abort_disk_io(
			boost::function<void(int, disk_io_job const&)> const& handler
			= boost::function<void(int, disk_io_job const&)>());

		void async_clear_read_cache(
			boost::function<void(int, disk_io_job const&)> const& handler
			= boost::function<void(int, disk_io_job const&)>());

		void async_delete_files(
			boost::function<void(int, disk_io_job const&)> const& handler
			= boost::function<void(int, disk_io_job const&)>());

		void async_move_storage(std::string const& p
			, boost::function<void(int, disk_io_job const&)> const& handler);

		void async_save_resume_data(
			boost::function<void(int, disk_io_job const&)> const& handler);

		void async_clear_piece(int piece);

		void async_sync_piece(int piece
			, boost::function<void(int, disk_io_job const&)> const& handler);

		void async_flush_piece(int piece);

		enum return_t
		{
			// return values from check_fastresume
			no_error = 0,
			fatal_disk_error = -1,
			need_full_check = -2,
			disk_check_aborted = -3
		};

		storage_interface* get_storage_impl() { return m_storage.get(); }

		void write_resume_data(entry& rd, storage_error& ec) const;

		void add_piece(cached_piece_entry* p);
		void remove_piece(cached_piece_entry* p);
		bool has_piece(cached_piece_entry* p) const;
		int num_pieces() const { return m_cached_pieces.size(); }
		boost::unordered_set<cached_piece_entry*> const& cached_pieces()
		{ return m_cached_pieces; }

	private:

		// if error is set and return value is 'no_error' or 'need_full_check'
		// the error message indicates that the fast resume data was rejected
		// if 'fatal_disk_error' is returned, the error message indicates what
		// when wrong in the disk access
		int check_fastresume(lazy_entry const& rd, storage_error& error);

		// helper functions for check_fastresume	
		int check_no_fastresume(storage_error& error);
		int check_init_storage(storage_error& error);

#ifdef TORRENT_DEBUG
		std::string name() const { return m_files.name(); }

		void check_invariant() const;
#endif
		file_storage const& m_files;

		boost::scoped_ptr<storage_interface> m_storage;

		// abort jobs synchronize with all pieces being evicted
		// for a certain torrent. If some pieces cannot be evicted
		// we have to wait until those pieces are evicted. This
		// is the abort jobs, waiting for all pieces
		// for this torrent to be evicted
		disk_io_job* m_abort_job;

		storage_mode_t m_storage_mode;

		// this is saved in case we need to instantiate a new
		// storage (osed when remapping files)
		storage_constructor_type m_storage_constructor;

		disk_io_thread& m_io_thread;

		// the reason for this to be a void pointer
		// is to avoid creating a dependency on the
		// torrent. This shared_ptr is here only
		// to keep the torrent object alive until
		// the piece_manager destructs. This is because
		// the torrent_info object is owned by the torrent.
		boost::shared_ptr<void> m_torrent;

		// these are cached pieces belonging to this storage
		boost::unordered_set<cached_piece_entry*> m_cached_pieces;
	};

}

#endif // TORRENT_STORAGE_HPP_INCLUDED

