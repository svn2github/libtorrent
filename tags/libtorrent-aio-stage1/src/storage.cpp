/*

Copyright (c) 2003, Arvid Norberg, Daniel Wallin
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
#include <functional>

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/ref.hpp>
#include <boost/bind.hpp>
#include <boost/version.hpp>
#include <boost/scoped_array.hpp>
#if BOOST_VERSION >= 103500
#include <boost/system/system_error.hpp>
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "libtorrent/config.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/peer_id.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/file_pool.hpp"
#include "libtorrent/aux_/session_impl.hpp"
#include "libtorrent/disk_buffer_holder.hpp"
#include "libtorrent/alloca.hpp"
#include "libtorrent/allocator.hpp" // page_size

// #error get rid of this when regular piece checking is async as well
#include "libtorrent/block_cache.hpp" // for partial_hash

#include <cstdio>

//#define TORRENT_PARTIAL_HASH_LOG

#if TORRENT_USE_IOSTREAM
#include <ios>
#include <iostream>
#include <iomanip>
#endif

#if defined(__APPLE__)
// for getattrlist()
#include <sys/attr.h>
#include <unistd.h>
// for statfs()
#include <sys/param.h>
#include <sys/mount.h>
#endif

#if defined(__linux__)
#include <sys/statfs.h>
#endif

#if defined(__FreeBSD__)
// for statfs()
#include <sys/param.h>
#include <sys/mount.h>
#endif

// for convert_to_wstring and convert_to_native
#include "libtorrent/escape_string.hpp"

#if defined TORRENT_DEBUG && defined TORRENT_STORAGE_DEBUG && TORRENT_USE_IOSTREAM
namespace
{
	using namespace libtorrent;

	void print_to_log(std::string const& s)
	{
		static std::ofstream log("log.txt");
		log << s;
		log.flush();
	}
}
#endif

namespace libtorrent
{

	void recursive_copy(std::string const& old_path, std::string const& new_path, error_code& ec)
	{
		TORRENT_ASSERT(!ec);
		if (is_directory(old_path, ec))
		{
			create_directory(new_path, ec);
			if (ec) return;
			for (directory i(old_path, ec); !i.done(); i.next(ec))
			{
				std::string f = i.file();
				recursive_copy(f, combine_path(new_path, f), ec);
				if (ec) return;
			}
		}
		else if (!ec)
		{
			copy_file(old_path, new_path, ec);
		}
	}

	void recursive_remove(std::string const& old_path)
	{
		error_code ec;
		if (is_directory(old_path, ec))
		{
			for (directory i(old_path, ec); !i.done(); i.next(ec))
				recursive_remove(combine_path(old_path, i.file()));
			remove(old_path, ec);
		}
		else
		{
			remove(old_path, ec);
		}
	}

	std::vector<std::pair<size_type, std::time_t> > get_filesizes(
		file_storage const& storage, std::string const& p)
	{
		std::string save_path = complete(p);
		std::vector<std::pair<size_type, std::time_t> > sizes;
		for (file_storage::iterator i = storage.begin()
			, end(storage.end()); i != end; ++i)
		{
			size_type size = 0;
			std::time_t time = 0;

			if (!i->pad_file)
			{
				file_status s;
				error_code ec;
				stat_file(combine_path(save_path, storage.file_path(*i)), &s, ec);

				if (!ec)
				{
					size = s.file_size;
					time = s.mtime;
				}
			}
			sizes.push_back(std::make_pair(size, time));
		}
		return sizes;
	}

	// matches the sizes and timestamps of the files passed in
	// in non-compact mode, actual file sizes and timestamps
	// are allowed to be bigger and more recent than the fast
	// resume data. This is because full allocation will not move
	// pieces, so any older version of the resume data will
	// still be a correct subset of the actual data on disk.
	enum flags_t
	{
		compact_mode = 1,
		ignore_timestamps = 2
	};

	bool match_filesizes(
		file_storage const& fs
		, std::string p
		, std::vector<std::pair<size_type, std::time_t> > const& sizes
		, int flags
		, storage_error& ec)
	{
		if ((int)sizes.size() != fs.num_files())
		{
			ec.ec = errors::mismatching_number_of_files;
			ec.file.clear();
			ec.operation = 0;
			return false;
		}
		p = complete(p);

		std::vector<std::pair<size_type, std::time_t> >::const_iterator size_iter
			= sizes.begin();
		for (file_storage::iterator i = fs.begin()
			, end(fs.end());i != end; ++i, ++size_iter)
		{
			size_type size = 0;
			std::time_t time = 0;
			if (i->pad_file) continue;

			file_status s;
			error_code error;
			std::string file_path = combine_path(p, fs.file_path(*i));
			stat_file(file_path, &s, error);

			if (error)
			{
				if (error != boost::system::errc::no_such_file_or_directory)
				{
					ec.ec = error;
					ec.file = file_path;
					ec.operation = "stat";
					return false;
				}
			}
			else
			{
				size = s.file_size;
				time = s.mtime;
			}

			if (((flags & compact_mode) && size != size_iter->first)
				|| (!(flags & compact_mode) && size < size_iter->first))
			{
				ec.ec = errors::mismatching_file_size;
				ec.file = file_path;
				ec.operation = 0;
				return false;
			}

			if (flags & ignore_timestamps) continue;

			// allow one second 'slack', because of FAT volumes
			// in sparse mode, allow the files to be more recent
			// than the resume data, but only by 5 minutes
			if (((flags & compact_mode) && (time > size_iter->second + 1 || time < size_iter->second - 1)) ||
				(!(flags & compact_mode) && (time > size_iter->second + 5 * 60 || time < size_iter->second - 1)))
			{
				ec.ec = errors::mismatching_file_timestamp;
				ec.file = file_path;
				ec.operation = 0;
				return false;
			}
		}
		return true;
	}

	// for backwards compatibility, let the default readv and
	// writev implementations be implemented in terms of the
	// old read and write
	int storage_interface::readv(file::iovec_t const* bufs
		, int slot, int offset, int num_bufs, storage_error& ec)
	{
		int ret = 0;
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i < end; ++i)
		{
			int r = read((char*)i->iov_base, slot, offset, i->iov_len, ec);
			offset += i->iov_len;
			if (r == -1) return -1;
			ret += r;
		}
		return ret;
	}

	int storage_interface::writev(file::iovec_t const* bufs, int slot
		, int offset, int num_bufs, storage_error& ec)
	{
		int ret = 0;
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i < end; ++i)
		{
			int r = write((char const*)i->iov_base, slot, offset, i->iov_len, ec);
			offset += i->iov_len;
			if (r == -1) return -1;
			ret += r;
		}
		return ret;
	}

	int copy_bufs(file::iovec_t const* bufs, int bytes, file::iovec_t* target)
	{
		int size = 0;
		int ret = 1;
		for (;;)
		{
			*target = *bufs;
			size += bufs->iov_len;
			if (size >= bytes)
			{
				target->iov_len -= size - bytes;
				return ret;
			}
			++bufs;
			++target;
			++ret;
		}
	}

	void advance_bufs(file::iovec_t*& bufs, int bytes)
	{
		int size = 0;
		for (;;)
		{
			size += bufs->iov_len;
			if (size >= bytes)
			{
				((char*&)bufs->iov_base) += bufs->iov_len - (size - bytes);
				bufs->iov_len = size - bytes;
				return;
			}
			++bufs;
		}
	}

	TORRENT_EXPORT int bufs_size(file::iovec_t const* bufs, int num_bufs)
	{
		int size = 0;
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i < end; ++i)
			size += i->iov_len;
		return size;
	}
	
	void clear_bufs(file::iovec_t const* bufs, int num_bufs)
	{
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i < end; ++i)
			std::memset(i->iov_base, 0, i->iov_len);
	}

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	int count_bufs(file::iovec_t const* bufs, int bytes)
	{
		int size = 0;
		int count = 1;
		if (bytes == 0) return 0;
		for (file::iovec_t const* i = bufs;; ++i, ++count)
		{
			size += i->iov_len;
			TORRENT_ASSERT(size <= bytes);
			if (size >= bytes) return count;
		}
	}
#endif

	int piece_manager::hash_for_slot(int slot, partial_hash& ph, int piece_size, storage_error& ec
		, int small_piece_size, sha1_hash* small_hash)
	{
		int num_read = 0;
		int slot_size = piece_size - ph.offset;
		if (slot_size > 0)
		{
			int block_size = 16 * 1024;
			if (m_storage->disk_pool()) block_size = m_storage->disk_pool()->block_size();
			int size = slot_size;
			int num_blocks = (size + block_size - 1) / block_size;

			// when we optimize for speed we allocate all the buffers we
			// need for the rest of the piece, and read it all in one call
			// and then hash it. When optimizing for memory usage, we read
			// one block at a time and hash it. This ends up only using a
			// single buffer
			if (m_storage->settings().optimize_hashing_for_speed)
			{
				file::iovec_t* bufs = TORRENT_ALLOCA(file::iovec_t, num_blocks);
				for (int i = 0; i < num_blocks; ++i)
				{
					bufs[i].iov_base = m_storage->disk_pool()->allocate_buffer("hash temp");
					bufs[i].iov_len = (std::min)(block_size, size);
					size -= bufs[i].iov_len;
				}
				num_read = m_storage->readv(bufs, slot, ph.offset, num_blocks, ec);
				// TODO: if the read fails, set error and exit immediately

				for (int i = 0; i < num_blocks; ++i)
				{
					if (small_hash && small_piece_size <= block_size)
					{
						ph.h.update((char const*)bufs[i].iov_base, small_piece_size);
						*small_hash = hasher(ph.h).final();
						small_hash = 0; // avoid this case again
						if (int(bufs[i].iov_len) > small_piece_size)
							ph.h.update((char const*)bufs[i].iov_base + small_piece_size
								, bufs[i].iov_len - small_piece_size);
					}
					else
					{
						ph.h.update((char const*)bufs[i].iov_base, bufs[i].iov_len);
						small_piece_size -= bufs[i].iov_len;
					}
					ph.offset += bufs[i].iov_len;
					m_storage->disk_pool()->free_buffer((char*)bufs[i].iov_base);
				}
			}
			else
			{
				file::iovec_t buf;
				disk_buffer_holder holder(*m_storage->disk_pool()
					, m_storage->disk_pool()->allocate_buffer("hash temp"));
				buf.iov_base = holder.get();
				for (int i = 0; i < num_blocks; ++i)
				{
					buf.iov_len = (std::min)(block_size, size);
					int ret = m_storage->readv(&buf, slot, ph.offset, 1, ec);
					if (ret > 0) num_read += ret;
					// TODO: if the read fails, set error and exit immediately

					if (small_hash && small_piece_size <= block_size)
					{
						if (small_piece_size > 0) ph.h.update((char const*)buf.iov_base, small_piece_size);
						*small_hash = hasher(ph.h).final();
						small_hash = 0; // avoid this case again
						if (int(buf.iov_len) > small_piece_size)
							ph.h.update((char const*)buf.iov_base + small_piece_size
								, buf.iov_len - small_piece_size);
					}
					else
					{
						ph.h.update((char const*)buf.iov_base, buf.iov_len);
						small_piece_size -= buf.iov_len;
					}

					ph.offset += buf.iov_len;
					size -= buf.iov_len;
				}
			}
			if (ec) return 0;
		}
		return num_read;
	}

	default_storage::default_storage(file_storage const& fs, file_storage const* mapped
		, std::string const& path, file_pool& fp, std::vector<boost::uint8_t> const& file_prio)
		: m_files(fs)
		, m_file_priority(file_prio)
		, m_pool(fp)
		, m_page_size(page_size())
		, m_allocate_files(false)
	{
		if (mapped) m_mapped_files.reset(new file_storage(*mapped));

		TORRENT_ASSERT(m_files.begin() != m_files.end());
		m_save_path = complete(path);
	}

	default_storage::~default_storage()
	{
		// this may be called from a different
		// thread than the disk thread
//		m_pool.release(this);
	}

	void default_storage::initialize(bool allocate_files, storage_error& ec)
	{
		m_allocate_files = allocate_files;
		// first, create all missing directories
		std::string last_path;
		for (file_storage::iterator file_iter = files().begin(),
			end_iter = files().end(); file_iter != end_iter; ++file_iter)
		{
			std::string file_path = combine_path(m_save_path, files().file_path(*file_iter));
			std::string dir = parent_path(file_path);

			if (dir != last_path)
			{
				last_path = dir;

				error_code tmp;
				if (!is_directory(last_path, tmp))
					create_directories(last_path, tmp);
			}

			int file_index = files().file_index(*file_iter);

			// ignore files that have priority 0
			if (int(m_file_priority.size()) > file_index
				&& m_file_priority[file_index] == 0) continue;

			// ignore pad files
			if (file_iter->pad_file) continue;

			file_status s;
			stat_file(file_path, &s, ec.ec);
			if (ec && ec.ec != boost::system::errc::no_such_file_or_directory
				&& ec.ec != boost::system::errc::not_a_directory)
			{
				ec.file = file_path;
				ec.operation = "stat";
				break;
			}

			// ec is either ENOENT or the file existed and s is valid
			// allocate file only if it is not exist and (allocate_files == true)
			// if the file already exists, but is larger than what
			// it's supposed to be, also truncate it
			// if the file is empty, just create it either way.
			if ((ec && allocate_files)
				|| (!ec && s.file_size > file_iter->size)
				|| file_iter->size == 0)
			{
				ec.ec.clear();
				boost::intrusive_ptr<file> f = open_file(file_iter, file::read_write, ec.ec);
				if (!ec.ec && f) f->set_size(file_iter->size, ec.ec);
				if (ec)
				{
					ec.file = file_path;
					ec.operation = "open";
					break;
				}
			}
			ec.ec.clear();
		}

		std::vector<boost::uint8_t>().swap(m_file_priority);
		// close files that were opened in write mode
		m_pool.release(this);
	}

	void default_storage::finalize_file(int index, storage_error& ec)
	{
		TORRENT_ASSERT(index >= 0 && index < files().num_files());
		if (index < 0 || index >= files().num_files()) return;
	
		boost::intrusive_ptr<file> f = open_file(files().begin() + index, file::read_write, ec.ec);
		if (ec || !f)
		{
			ec.file = combine_path(m_save_path
				, files().file_path(*(files().begin() + index)));
			ec.operation = "open";
			return;
		}

		f->finalize();
	}

	bool default_storage::has_any_file(storage_error& ec)
	{
		file_storage::iterator i = files().begin();
		file_storage::iterator end = files().end();

		for (; i != end; ++i)
		{
			file_status s;
			std::string file_path = combine_path(m_save_path, files().file_path(*i));
			stat_file(file_path, &s, ec.ec);
	
			// if we didn't find the file, check the next one
			if (ec && ec.ec == boost::system::errc::no_such_file_or_directory)
			{
				ec.ec.clear();
				continue;
			}

			if (ec)
			{
				ec.file = file_path;
				ec.operation = "stat";
				return false;
			}
			if (s.mode & file_status::regular_file && i->size > 0)
				return true;
		}
		return false;
	}

	void default_storage::rename_file(int index, std::string const& new_filename, storage_error& ec)
	{
		if (index < 0 || index >= files().num_files()) return;
		std::string old_name = combine_path(m_save_path, files().file_path(files().at(index)));
		m_pool.release(this, index);

		rename(old_name, combine_path(m_save_path, new_filename), ec.ec);
		
		if (ec.ec == boost::system::errc::no_such_file_or_directory)
			ec.ec.clear();

		if (ec)
		{
			ec.file = old_name;
			ec.operation = "rename";
			return;
		}

		// if old path doesn't exist, just rename the file
		// in our file_storage, so that when it is created
		// it will get the new name
		if (!m_mapped_files)
		{ m_mapped_files.reset(new file_storage(m_files)); }
		m_mapped_files->rename_file(index, new_filename);
	}

	void default_storage::release_files(storage_error& ec)
	{
		m_pool.release(this);
	}

	void default_storage::delete_one_file(std::string const& p, error_code& ec)
	{
		remove(p, ec);
		
		if (ec == boost::system::errc::no_such_file_or_directory)
			ec.clear();
	}

	void default_storage::delete_files(storage_error& ec)
	{
		// make sure we don't have the files open
		m_pool.release(this);

		// delete the files from disk
		std::set<std::string> directories;
		typedef std::set<std::string>::iterator iter_t;
		for (file_storage::iterator i = files().begin()
			, end(files().end()); i != end; ++i)
		{
			std::string fp = files().file_path(*i);
			std::string p = combine_path(m_save_path, fp);
			std::string bp = parent_path(fp);
			std::pair<iter_t, bool> ret;
			ret.second = true;
			while (ret.second && !bp.empty())
			{
				ret = directories.insert(combine_path(m_save_path, bp));
				bp = parent_path(bp);
			}
			delete_one_file(p, ec.ec);
			if (ec) { ec.file = p; ec.operation = "remove"; }
		}

		// remove the directories. Reverse order to delete
		// subdirectories first

		for (std::set<std::string>::reverse_iterator i = directories.rbegin()
			, end(directories.rend()); i != end; ++i)
		{
			delete_one_file(*i, ec.ec);
			if (ec) { ec.file = *i; ec.operation = "remove"; }
		}
	}

	void default_storage::write_resume_data(entry& rd, storage_error& ec) const
	{
		TORRENT_ASSERT(rd.type() == entry::dictionary_t);

		std::vector<std::pair<size_type, std::time_t> > file_sizes
			= get_filesizes(files(), m_save_path);

		entry::list_type& fl = rd["file sizes"].list();
		for (std::vector<std::pair<size_type, std::time_t> >::iterator i
			= file_sizes.begin(), end(file_sizes.end()); i != end; ++i)
		{
			entry::list_type p;
			p.push_back(entry(i->first));
			p.push_back(entry(i->second));
			fl.push_back(entry(p));
		}
	}

	int default_storage::sparse_end(int slot) const
	{
		TORRENT_ASSERT(slot >= 0);
		TORRENT_ASSERT(slot < m_files.num_pieces());

		size_type file_offset = (size_type)slot * m_files.piece_length();
		file_storage::iterator file_iter;

		for (file_iter = files().begin();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
			TORRENT_ASSERT(file_iter != files().end());
		}
	
		error_code ec;
		boost::intrusive_ptr<file> file_handle = open_file(file_iter, file::read_only, ec);
		if (!file_handle || ec) return slot;

		size_type data_start = file_handle->sparse_end(file_offset);
		return int((data_start + m_files.piece_length() - 1) / m_files.piece_length());
	}

	bool default_storage::verify_resume_data(lazy_entry const& rd, storage_error& ec)
	{
		// TODO: make this more generic to not just work if files have been
		// renamed, but also if they have been merged into a single file for instance
		// maybe use the same format as .torrent files and reuse some code from torrent_info
		lazy_entry const* mapped_files = rd.dict_find_list("mapped_files");
		if (mapped_files && mapped_files->list_size() == m_files.num_files())
		{
			m_mapped_files.reset(new file_storage(m_files));
			for (int i = 0; i < m_files.num_files(); ++i)
			{
				std::string new_filename = mapped_files->list_string_value_at(i);
				if (new_filename.empty()) continue;
				m_mapped_files->rename_file(i, new_filename);
			}
		}
		
		lazy_entry const* file_priority = rd.dict_find_list("file_priority");
		if (file_priority && file_priority->list_size()
			== files().num_files())
		{
			m_file_priority.resize(file_priority->list_size());
			for (int i = 0; i < file_priority->list_size(); ++i)
				m_file_priority[i] = boost::uint8_t(file_priority->list_int_value_at(i, 1));
		}

		std::vector<std::pair<size_type, std::time_t> > file_sizes;
		lazy_entry const* file_sizes_ent = rd.dict_find_list("file sizes");
		if (file_sizes_ent == 0)
		{
			ec.ec = errors::missing_file_sizes;
			return false;
		}
		
		for (int i = 0; i < file_sizes_ent->list_size(); ++i)
		{
			lazy_entry const* e = file_sizes_ent->list_at(i);
			if (e->type() != lazy_entry::list_t
				|| e->list_size() != 2
				|| e->list_at(0)->type() != lazy_entry::int_t
				|| e->list_at(1)->type() != lazy_entry::int_t)
				continue;
			file_sizes.push_back(std::pair<size_type, std::time_t>(
				e->list_int_value_at(0), std::time_t(e->list_int_value_at(1))));
		}

		if (file_sizes.empty())
		{
			ec.ec = errors::no_files_in_resume_data;
			return false;
		}
		
		bool seed = false;
		
		lazy_entry const* slots = rd.dict_find_list("slots");
		if (slots)
		{
			if (int(slots->list_size()) == m_files.num_pieces())
			{
				seed = true;
				for (int i = 0; i < slots->list_size(); ++i)
				{
					if (slots->list_int_value_at(i, -1) >= 0) continue;
					seed = false;
					break;
				}
			}
		}
		else if (lazy_entry const* pieces = rd.dict_find_string("pieces"))
		{
			if (int(pieces->string_length()) == m_files.num_pieces())
			{
				seed = true;
				char const* p = pieces->string_ptr();
				for (int i = 0; i < pieces->string_length(); ++i)
				{
					if ((p[i] & 1) == 1) continue;
					seed = false;
					break;
				}
			}
		}
		else
		{
			ec.ec = errors::missing_pieces;
			return false;
		}

		bool full_allocation_mode = false;
		if (rd.dict_find_string_value("allocation") != "compact")
			full_allocation_mode = true;

		if (seed)
		{
			if (files().num_files() != (int)file_sizes.size())
			{
				ec.ec = errors::mismatching_number_of_files;
				return false;
			}

			std::vector<std::pair<size_type, std::time_t> >::iterator
				fs = file_sizes.begin();
			// the resume data says we have the entire torrent
			// make sure the file sizes are the right ones
			for (file_storage::iterator i = files().begin()
				, end(files().end()); i != end; ++i, ++fs)
			{
				if (!i->pad_file && i->size != fs->first)
				{
					ec.ec = errors::mismatching_file_size;
					return false;
				}
			}
		}
		int flags = (full_allocation_mode ? 0 : compact_mode)
			| (settings().ignore_resume_timestamps ? ignore_timestamps : 0);

		return match_filesizes(files(), m_save_path, file_sizes, flags, ec);

	}

	// returns true on success
	void default_storage::move_storage(std::string const& sp, storage_error& ec)
	{
		std::string save_path = complete(sp);

		file_status s;
		stat_file(save_path, &s, ec.ec);
		if (ec.ec == boost::system::errc::no_such_file_or_directory)
		{
			create_directories(save_path, ec.ec);
			if (ec)
			{
				ec.file = save_path;
				ec.operation = "mkdir";
				return;
			}
		}
		else if (ec)
		{
			ec.file = save_path;
			ec.operation = "stat";
			return;
		}
		ec.ec.clear();

		m_pool.release(this);

		std::set<std::string> to_move;
		file_storage const& f = files();

		for (file_storage::iterator i = f.begin()
			, end(f.end()); i != end; ++i)
		{
			std::string split = split_path(f.file_path(*i));
			to_move.insert(to_move.begin(), split);
		}

		for (std::set<std::string>::const_iterator i = to_move.begin()
			, end(to_move.end()); i != end; ++i)
		{
			std::string old_path = combine_path(m_save_path, *i);
			std::string new_path = combine_path(save_path, *i);

			rename(old_path, new_path, ec.ec);
			if (ec.ec == boost::system::errc::no_such_file_or_directory)
				ec.ec.clear();

			if (ec)
			{
				ec.ec.clear();
				recursive_copy(old_path, new_path, ec.ec);
				if (!ec) recursive_remove(old_path);
				else { ec.file = old_path; ec.operation = "copy"; }
				break;
			}
		}

		if (!ec) m_save_path = save_path;
	}

#ifdef TORRENT_DEBUG
/*
	void default_storage::shuffle()
	{
		int num_pieces = files().num_pieces();

		std::vector<int> pieces(num_pieces);
		for (std::vector<int>::iterator i = pieces.begin();
			i != pieces.end(); ++i)
		{
			*i = static_cast<int>(i - pieces.begin());
		}
		std::srand((unsigned int)std::time(0));
		std::vector<int> targets(pieces);
		std::random_shuffle(pieces.begin(), pieces.end());
		std::random_shuffle(targets.begin(), targets.end());

		for (int i = 0; i < (std::max)(num_pieces / 50, 1); ++i)
		{
			const int slot_index = targets[i];
			const int piece_index = pieces[i];
			const int slot_size =static_cast<int>(m_files.piece_size(slot_index));
			std::vector<char> buf(slot_size);
			read(&buf[0], piece_index, 0, slot_size);
			write(&buf[0], slot_index, 0, slot_size);
		}
	}
*/
#endif

#define TORRENT_ALLOCATE_BLOCKS(bufs, num_blocks, piece_size) \
	int num_blocks = (piece_size + disk_pool()->block_size() - 1) / disk_pool()->block_size(); \
	file::iovec_t* bufs = TORRENT_ALLOCA(file::iovec_t, num_blocks); \
	for (int i = 0, size = piece_size; i < num_blocks; ++i) \
	{ \
		bufs[i].iov_base = disk_pool()->allocate_buffer("move temp"); \
		bufs[i].iov_len = (std::min)(disk_pool()->block_size(), size); \
		size -= bufs[i].iov_len; \
	}

#define TORRENT_FREE_BLOCKS(bufs, num_blocks) \
	for (int i = 0; i < num_blocks; ++i) \
		disk_pool()->free_buffer((char*)bufs[i].iov_base);

#define TORRENT_SET_SIZE(bufs, size, num_bufs) \
	for (num_bufs = 0; size > 0; size -= disk_pool()->block_size(), ++num_bufs) \
		bufs[num_bufs].iov_len = (std::min)(disk_pool()->block_size(), size)
	

	void default_storage::move_slot(int src_slot, int dst_slot, storage_error& ec)
	{
		int piece_size = m_files.piece_size(dst_slot);

		TORRENT_ALLOCATE_BLOCKS(bufs, num_blocks, piece_size);

		readv(bufs, src_slot, 0, num_blocks, ec); if (ec) goto ret;
		writev(bufs, dst_slot, 0, num_blocks, ec); if (ec) goto ret;

ret:
		TORRENT_FREE_BLOCKS(bufs, num_blocks)
	}

	void default_storage::swap_slots(int slot1, int slot2, storage_error& ec)
	{
		// the size of the target slot is the size of the piece
		int piece1_size = m_files.piece_size(slot2);
		int piece2_size = m_files.piece_size(slot1);

		TORRENT_ALLOCATE_BLOCKS(bufs1, num_blocks1, piece1_size);
		TORRENT_ALLOCATE_BLOCKS(bufs2, num_blocks2, piece2_size);

		readv(bufs1, slot1, 0, num_blocks1, ec); if (ec) goto ret;
		readv(bufs2, slot2, 0, num_blocks2, ec); if (ec) goto ret;
		writev(bufs1, slot2, 0, num_blocks1, ec); if (ec) goto ret;
		writev(bufs2, slot1, 0, num_blocks2, ec); if (ec) goto ret;

ret:
		TORRENT_FREE_BLOCKS(bufs1, num_blocks1)
		TORRENT_FREE_BLOCKS(bufs2, num_blocks2)
	}

	void default_storage::swap_slots3(int slot1, int slot2, int slot3, storage_error& ec)
	{
		// the size of the target slot is the size of the piece
		int piece_size = m_files.piece_length();
		int piece1_size = m_files.piece_size(slot2);
		int piece2_size = m_files.piece_size(slot3);
		int piece3_size = m_files.piece_size(slot1);

		TORRENT_ALLOCATE_BLOCKS(bufs1, num_blocks1, piece_size);
		TORRENT_ALLOCATE_BLOCKS(bufs2, num_blocks2, piece_size);

		int tmp1 = 0;
		int tmp2 = 0;
		TORRENT_SET_SIZE(bufs1, piece1_size, tmp1);
		readv(bufs1, slot1, 0, tmp1, ec); if (ec) goto ret;
		TORRENT_SET_SIZE(bufs2, piece2_size, tmp2);
		readv(bufs2, slot2, 0, tmp2, ec); if (ec) goto ret;
		writev(bufs1, slot2, 0, tmp1, ec); if (ec) goto ret;
		TORRENT_SET_SIZE(bufs1, piece3_size, tmp1);
		readv(bufs1, slot3, 0, tmp1, ec); if (ec) goto ret;
		writev(bufs2, slot3, 0, tmp2, ec); if (ec) goto ret;
		writev(bufs1, slot1, 0, tmp1, ec); if (ec) goto ret;
ret:
		TORRENT_FREE_BLOCKS(bufs1, num_blocks1)
		TORRENT_FREE_BLOCKS(bufs2, num_blocks2)
	}

	int default_storage::writev(file::iovec_t const* bufs, int slot, int offset
		, int num_bufs, storage_error& ec)
	{
		int flags = 0;
		if (m_settings)
			flags |= settings().coalesce_writes ? file::coalesce_buffers : 0;

		fileop op = { &file::writev, &default_storage::write_unaligned
			, 0, 0, 0
			, m_settings ? settings().disk_io_write_mode : 0, file::read_write
			, flags, "writev" };
		return readwritev(bufs, slot, offset, num_bufs, op, ec);
	}

	size_type default_storage::physical_offset(int slot, int offset)
	{
		TORRENT_ASSERT(slot >= 0);
		TORRENT_ASSERT(slot < m_files.num_pieces());
		TORRENT_ASSERT(offset >= 0);

		// find the file and file
		size_type tor_off = size_type(slot)
			* files().piece_length() + offset;
		file_storage::iterator file_iter = files().file_at_offset(tor_off);
		while (file_iter->pad_file)
		{
			++file_iter;
			if (file_iter == files().end())
				return size_type(slot) * files().piece_length() + offset;
			// update offset as well, since we're moving it up ahead
			tor_off = file_iter->offset;
		}
		TORRENT_ASSERT(!file_iter->pad_file);

		size_type file_offset = tor_off - file_iter->offset;
		TORRENT_ASSERT(file_offset >= 0);

		// open the file read only to avoid re-opening
		// it in case it's already opened in read-only mode
		error_code ec;
		boost::intrusive_ptr<file> f = open_file(file_iter, file::read_only, ec);

		size_type ret = 0;
		if (f && !ec) ret = f->phys_offset(file_offset);

		if (ret == 0)
		{
			// this means we don't support true physical offset
			// just make something up
			return size_type(slot) * files().piece_length() + offset;
		}
		return ret;
	}

	void default_storage::hint_read(int slot, int offset, int size)
	{
		size_type start = slot * (size_type)m_files.piece_length() + offset;
		TORRENT_ASSERT(start + size <= m_files.total_size());

		size_type file_offset = start;
		file_storage::iterator file_iter;

		// TODO: use binary search!
		for (file_iter = files().begin();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
			TORRENT_ASSERT(file_iter != files().end());
		}

		boost::intrusive_ptr<file> file_handle;
		int bytes_left = size;
		int slot_size = static_cast<int>(m_files.piece_size(slot));

		if (offset + bytes_left > slot_size)
			bytes_left = slot_size - offset;

		TORRENT_ASSERT(bytes_left >= 0);

		int file_bytes_left;
		for (;bytes_left > 0; ++file_iter, bytes_left -= file_bytes_left)
		{
			TORRENT_ASSERT(file_iter != files().end());

			file_bytes_left = bytes_left;
			if (file_offset + file_bytes_left > file_iter->size)
				file_bytes_left = (std::max)(static_cast<int>(file_iter->size - file_offset), 0);

			if (file_bytes_left == 0) continue;

			if (file_iter->pad_file) continue;

			error_code ec;
			file_handle = open_file(file_iter, file::read_only, ec);

			// failing to hint that we want to read is not a big deal
			// just swollow the error and keep going
			if (!file_handle || ec) continue;

			file_handle->hint_read(file_offset, file_bytes_left);
			file_offset = 0;
		}
	}

	int default_storage::readv(file::iovec_t const* bufs, int slot, int offset
		, int num_bufs, storage_error& ec)
	{
		int flags = 0;
		if (m_settings)
			flags |= settings().coalesce_reads ? file::coalesce_buffers : 0;

		fileop op = { &file::readv, &default_storage::read_unaligned
			, 0, 0, 0
			, m_settings ? settings().disk_io_read_mode : 0, file::read_only
			, flags, "readv"};
#ifdef TORRENT_SIMULATE_SLOW_READ
		boost::thread::sleep(boost::get_system_time()
			+ boost::posix_time::milliseconds(1000));
#endif
		return readwritev(bufs, slot, offset, num_bufs, op, ec);
	}

	file::aiocb_t* default_storage::async_readv(file::iovec_t const* bufs, int slot, int offset, int num_bufs
		, boost::function<void(async_handler*)> const& handler)
	{
		async_handler* a = new async_handler(time_now_hires());
		a->handler = handler;

		int flags = 0;
		if (m_settings)
		{
			flags |= settings().coalesce_reads ? file::coalesce_buffers : 0;
			flags |= settings().allow_reordered_disk_operations ? file::resolve_phys_offset : 0;
		}

		fileop op = { &file::readv, &default_storage::read_unaligned, &file::async_readv
			, a, 0, m_settings ? settings().disk_io_read_mode : 0, file::read_only
			, flags, "async_readv"};
		storage_error ec;
		readwritev(bufs, slot, offset, num_bufs, op, ec);
		if (a->references == 0)
		{
			a->error = ec;
			handler(a);
			delete a;
		}
		return op.ret;
	}

	// TODO: error maybe the async_handler should be passed in here, so that a single job
	// could use a single handler
	file::aiocb_t* default_storage::async_writev(file::iovec_t const* bufs, int slot, int offset, int num_bufs
		, boost::function<void(async_handler*)> const& handler)
	{
		async_handler* a = new async_handler(time_now_hires());
		a->handler = handler;

		int flags = 0;
		if (m_settings)
			flags |= settings().coalesce_writes ? file::coalesce_buffers : 0;

		fileop op = { &file::writev, &default_storage::write_unaligned, &file::async_writev
			, a, 0, m_settings ? settings().disk_io_write_mode : 0, file::read_write
			, flags, "async_writev"};
		storage_error ec;
		readwritev(bufs, slot, offset, num_bufs, op, ec);
		if (a->references == 0)
		{
			a->error = ec;
			handler(a);
			delete a;
		}
		return op.ret;
	}

	// much of what needs to be done when reading and writing 
	// is buffer management and piece to file mapping. Most
	// of that is the same for reading and writing. This function
	// is a template, and the fileop decides what to do with the
	// file and the buffers.
	int default_storage::readwritev(file::iovec_t const* bufs, int slot, int offset
		, int num_bufs, fileop& op, storage_error& ec)
	{
		TORRENT_ASSERT(bufs != 0);
		TORRENT_ASSERT(slot >= 0);
		TORRENT_ASSERT(slot < m_files.num_pieces());
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(offset < m_files.piece_size(slot));
		TORRENT_ASSERT(num_bufs > 0);

		// this is the last element in the chain, that we hook new
		// aiocb's to
		file::aiocb_t* last = 0;
		op.ret = 0;

		int size = bufs_size(bufs, num_bufs);
		TORRENT_ASSERT(size > 0);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		std::vector<file_slice> slices
			= files().map_block(slot, offset, size);
		TORRENT_ASSERT(!slices.empty());
#endif

		size_type start = slot * (size_type)m_files.piece_length() + offset;
		TORRENT_ASSERT(start + size <= m_files.total_size());

		// find the file iterator and file offset
		size_type file_offset = start;
		file_storage::iterator file_iter;

		// TODO: use binary search!
		for (file_iter = files().begin();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
			TORRENT_ASSERT(file_iter != files().end());
		}

		int buf_pos = 0;

		boost::intrusive_ptr<file> file_handle;
		int bytes_left = size;
		int slot_size = static_cast<int>(m_files.piece_size(slot));

		if (offset + bytes_left > slot_size)
			bytes_left = slot_size - offset;

		TORRENT_ASSERT(bytes_left >= 0);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		int counter = 0;
#endif

		file::iovec_t* tmp_bufs = TORRENT_ALLOCA(file::iovec_t, num_bufs);
		file::iovec_t* current_buf = TORRENT_ALLOCA(file::iovec_t, num_bufs);
		copy_bufs(bufs, size, current_buf);
		TORRENT_ASSERT(count_bufs(current_buf, size) == num_bufs);
		int file_bytes_left;
		for (;bytes_left > 0; ++file_iter, bytes_left -= file_bytes_left
			, buf_pos += file_bytes_left)
		{
			TORRENT_ASSERT(file_iter != files().end());
			TORRENT_ASSERT(buf_pos >= 0);

			file_bytes_left = bytes_left;
			if (file_offset + file_bytes_left > file_iter->size)
				file_bytes_left = (std::max)(static_cast<int>(file_iter->size - file_offset), 0);

			if (file_bytes_left == 0) continue;

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT(int(slices.size()) > counter);
			size_type slice_size = slices[counter].size;
			TORRENT_ASSERT(slice_size == file_bytes_left);
			TORRENT_ASSERT((files().begin() + slices[counter].file_index)
				== file_iter);
			++counter;
#endif

			if (file_iter->pad_file)
			{
				if (op.mode == file::read_only)
				{
					int num_tmp_bufs = copy_bufs(current_buf, file_bytes_left, tmp_bufs);
					TORRENT_ASSERT(count_bufs(tmp_bufs, file_bytes_left) == num_tmp_bufs);
					TORRENT_ASSERT(num_tmp_bufs <= num_bufs);
					clear_bufs(tmp_bufs, num_tmp_bufs);
				}
				advance_bufs(current_buf, file_bytes_left);
				TORRENT_ASSERT(count_bufs(current_buf, bytes_left - file_bytes_left) <= num_bufs);
				file_offset = 0;
				continue;
			}

			file_handle = open_file(file_iter, op.mode, ec.ec);
			if ((op.mode == file::read_write) && ec.ec == boost::system::errc::no_such_file_or_directory)
			{
				// this means the directory the file is in doesn't exist.
				// so create it
				ec.ec.clear();
				std::string path = combine_path(m_save_path, files().file_path(*file_iter));
				create_directories(parent_path(path), ec.ec);
				// if the directory creation failed, don't try to open the file again
				// but actually just fail
				if (!ec) file_handle = open_file(file_iter, op.mode, ec.ec);
			}

			if (!file_handle || ec)
			{
				ec.file = combine_path(m_save_path, files().file_path(*file_iter));
				TORRENT_ASSERT(ec);
				return -1;
			}

			int num_tmp_bufs = copy_bufs(current_buf, file_bytes_left, tmp_bufs);
			TORRENT_ASSERT(count_bufs(tmp_bufs, file_bytes_left) == num_tmp_bufs);
			TORRENT_ASSERT(num_tmp_bufs <= num_bufs);
			int bytes_transferred = 0;
			// if the file is opened in no_buffer mode, and the
			// read is unaligned, we need to fall back on a slow
			// special read that reads aligned buffers and copies
			// it into the one supplied
			size_type adjusted_offset = files().file_base(*file_iter) + file_offset;
			if (op.async_op)
			{
				int flags = 0;
				TORRENT_ASSERT(op.handler);
				file::aiocb_t* aio = ((*file_handle).*op.async_op)(adjusted_offset
					, tmp_bufs, num_tmp_bufs, *aiocbs(), op.flags);
				if (op.ret == 0) op.ret = aio;
				// add this to the chain
				while (aio)
				{
					bytes_transferred += aio->nbytes();
					aio->handler = op.handler;
					++op.handler->references;
					if (last) last->next = aio;
					aio->prev = last;
					last = aio;
					aio = aio->next;
				}
			}
			else if ((file_handle->open_mode() & file::no_buffer)
				&& ((adjusted_offset & (file_handle->pos_alignment()-1)) != 0
				|| (uintptr_t(tmp_bufs->iov_base) & (file_handle->buf_alignment()-1)) != 0))
			{
				bytes_transferred = (int)(this->*op.unaligned_op)(file_handle, adjusted_offset
					, tmp_bufs, num_tmp_bufs, ec.ec);
				if (op.mode == file::read_write
					&& adjusted_offset + bytes_transferred == file_iter->size
					&& file_handle->pos_alignment() > 0)
				{
					// we were writing, and we just wrote the last block of the file
					// we likely wrote a bit too much, since we're restricted to
					// a specific alignment for writes. Make sure to truncate the size

					// TODO: what if file_base is used to merge several virtual files
					// into a single physical file?
					file_handle->set_size(file_iter->size, ec.ec);
					if (ec)
					{
						ec.file = combine_path(m_save_path, files().file_path(*file_iter));
						ec.operation = op.operation_name;
						return -1;
					}
				}
			}
			else
			{
				bytes_transferred = (int)((*file_handle).*op.regular_op)(adjusted_offset
					, tmp_bufs, num_tmp_bufs, ec.ec, op.flags);
			}
			file_offset = 0;

			if (ec)
			{
				ec.file = combine_path(m_save_path, files().file_path(*file_iter));
				ec.operation = op.operation_name;
				return -1;
			}

			TORRENT_ASSERT(file_bytes_left >= bytes_transferred);
			if (file_bytes_left != bytes_transferred)
				return bytes_transferred;

			advance_bufs(current_buf, bytes_transferred);
			TORRENT_ASSERT(count_bufs(current_buf, bytes_left - file_bytes_left) <= num_bufs);
		}
		return size;
	}

	// these functions are inefficient, but should be fairly uncommon. The read
	// case happens if unaligned files are opened in no_buffer mode or if clients
	// makes unaligned requests (and the disk cache is disabled or fully utilized
	// for write cache).

	// they read an unaligned buffer from a file that requires aligned access

	size_type default_storage::read_unaligned(boost::intrusive_ptr<file> const& file_handle
		, size_type file_offset, file::iovec_t const* bufs, int num_bufs, error_code& ec)
	{
		const int pos_align = file_handle->pos_alignment()-1;
		const int size_align = file_handle->size_alignment()-1;

		const int size = bufs_size(bufs, num_bufs);
		const int start_adjust = file_offset & pos_align;
		TORRENT_ASSERT(start_adjust == (file_offset % file_handle->pos_alignment()));
		const size_type aligned_start = file_offset - start_adjust;
		const int aligned_size = ((size+start_adjust) & size_align)
			? ((size+start_adjust) & ~size_align) + size_align + 1 : size + start_adjust;
		TORRENT_ASSERT((aligned_size & size_align) == 0);

		// allocate a temporary, aligned, buffer
		aligned_holder aligned_buf(aligned_size);
		file::iovec_t b = {aligned_buf.get(), aligned_size};
		size_type ret = file_handle->readv(aligned_start, &b, 1, ec);
		if (ret < 0)
		{
			TORRENT_ASSERT(ec);
			return ret;
		}
		if (ret - start_adjust < size) return (std::max)(ret - start_adjust, size_type(0));

		char* read_buf = aligned_buf.get() + start_adjust;
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i != end; ++i)
		{
			memcpy(i->iov_base, read_buf, i->iov_len);
			read_buf += i->iov_len;
		}

		return size;
	}

	// this is the really expensive one. To write unaligned, we need to read
	// an aligned block, overlay the unaligned buffer, and then write it back
	size_type default_storage::write_unaligned(boost::intrusive_ptr<file> const& file_handle
		, size_type file_offset, file::iovec_t const* bufs, int num_bufs, error_code& ec)
	{
		const int pos_align = file_handle->pos_alignment()-1;
		const int size_align = file_handle->size_alignment()-1;

		const int size = bufs_size(bufs, num_bufs);
		const int start_adjust = file_offset & pos_align;
		TORRENT_ASSERT(start_adjust == (file_offset % file_handle->pos_alignment()));
		const size_type aligned_start = file_offset - start_adjust;
		const int aligned_size = ((size+start_adjust) & size_align)
			? ((size+start_adjust) & ~size_align) + size_align + 1 : size + start_adjust;
		TORRENT_ASSERT((aligned_size & size_align) == 0);

		// allocate a temporary, aligned, buffer
		aligned_holder aligned_buf(aligned_size);
		file::iovec_t b = {aligned_buf.get(), aligned_size};
		size_type ret = file_handle->readv(aligned_start, &b, 1, ec);
		if (ret < 0)
		{
			TORRENT_ASSERT(ec);
			return ret;
		}

		// OK, we read the portion of the file. Now, overlay the buffer we're writing 

		char* write_buf = aligned_buf.get() + start_adjust;
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i != end; ++i)
		{
			memcpy(write_buf, i->iov_base, i->iov_len);
			write_buf += i->iov_len;
		}

		// write the buffer back to disk
		ret = file_handle->writev(aligned_start, &b, 1, ec);

		if (ret < 0)
		{
			TORRENT_ASSERT(ec);
			return ret;
		}
		if (ret - start_adjust < size) return (std::max)(ret - start_adjust, size_type(0));
		return size;
	}

	int default_storage::write(
		const char* buf
		, int slot
		, int offset
		, int size
		, storage_error& ec)
	{
		file::iovec_t b = { (file::iovec_base_t)buf, size };
		return writev(&b, slot, offset, 1, ec);
	}

	int default_storage::read(
		char* buf
		, int slot
		, int offset
		, int size
		, storage_error& ec)
	{
		file::iovec_t b = { (file::iovec_base_t)buf, size };
		return readv(&b, slot, offset, 1, ec);
	}

	boost::intrusive_ptr<file> default_storage::open_file(file_storage::iterator fe, int mode
		, error_code& ec) const
	{
		// io_submit only works on files opened with O_DIRECT, so this
		// is not optional if we're using io_submit
#if !USE_IOSUBMIT
		int cache_setting = m_settings ? settings().disk_io_write_mode : 0;
		if (cache_setting == session_settings::disable_os_cache
			|| (cache_setting == session_settings::disable_os_cache_for_aligned_files
			&& ((fe->offset + files().file_base(*fe)) & (m_page_size-1)) == 0))
#endif
			mode |= file::no_buffer;
		bool lock_files = m_settings ? settings().lock_files : false;
		if (lock_files) mode |= file::lock_file;
		if (!m_allocate_files) mode |= file::sparse;
		if (m_settings && settings().no_atime_storage) mode |= file::no_atime;

		return m_pool.open_file(const_cast<default_storage*>(this), m_save_path, fe, files(), mode, ec);
	}

	storage_interface* default_storage_constructor(file_storage const& fs
		, file_storage const* mapped, std::string const& path, file_pool& fp
		, std::vector<boost::uint8_t> const& file_prio)
	{
		return new default_storage(fs, mapped, path, fp, file_prio);
	}

	int disabled_storage::readv(file::iovec_t const* bufs, int slot, int offset
		, int num_bufs, storage_error& ec)
	{
		int ret = 0;
		for (int i = 0; i < num_bufs; ++i)
			ret += bufs[i].iov_len;
		return ret;
	}

	int disabled_storage::writev(file::iovec_t const* bufs, int slot, int offset
		, int num_bufs, storage_error& ec)
	{
		int ret = 0;
		for (int i = 0; i < num_bufs; ++i)
			ret += bufs[i].iov_len;
		return ret;
	}

	file::aiocb_t* disabled_storage::async_readv(file::iovec_t const* bufs, int slot
		, int offset, int num_bufs, boost::function<void(async_handler*)> const& handler)
	{
		return 0;
	}

	file::aiocb_t* disabled_storage::async_writev(file::iovec_t const* bufs, int slot
		, int offset, int num_bufs, boost::function<void(async_handler*)> const& handler)
	{
		return 0;
	}

	storage_interface* disabled_storage_constructor(file_storage const& fs
		, file_storage const* mapped, std::string const& path, file_pool& fp
		, std::vector<boost::uint8_t> const&)
	{
		return new disabled_storage(fs.piece_length());
	}

	// -- piece_manager -----------------------------------------------------

	piece_manager::piece_manager(
		boost::shared_ptr<void> const& torrent
		, boost::intrusive_ptr<torrent_info const> info
		, std::string const& save_path
		, disk_io_thread& io
		, storage_constructor_type sc
		, storage_mode_t sm
		, std::vector<boost::uint8_t> const& file_prio)
		: m_info(info)
		, m_files(m_info->files())
		, m_storage(sc(m_info->orig_files(), &m_info->files() != &m_info->orig_files()
			? &m_info->files() : 0, save_path, io.files(), file_prio))
		, m_storage_mode(sm)
		, m_save_path(complete(save_path))
		, m_state(state_none)
		, m_current_slot(0)
		, m_out_of_place(false)
		, m_scratch_piece(-1)
		, m_last_piece(-1)
		, m_storage_constructor(sc)
		, m_io_thread(io)
		, m_torrent(torrent)
	{
		m_storage->m_disk_pool = &m_io_thread;
		m_storage->m_aiocb_pool = m_io_thread.aiocbs();
	}

	void piece_manager::finalize_file(int index, storage_error& ec)
	{ m_storage->finalize_file(index, ec); }

	piece_manager::~piece_manager() {}

	void piece_manager::async_finalize_file(int file)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::finalize_file;
		j.piece = file;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_get_cache_info(cache_status* ret
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::get_cache_info;
		j.buffer = (char*)ret;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_save_resume_data(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::save_resume_data;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_clear_read_cache(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::clear_read_cache;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_release_files(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::release_files;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::abort_disk_io()
	{
		m_io_thread.stop(this);
	}

	void piece_manager::async_delete_files(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::delete_files;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_move_storage(std::string const& p
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::move_storage;
		j.str = p;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_check_fastresume(lazy_entry const* resume_data
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		TORRENT_ASSERT(resume_data != 0);
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::check_fastresume;
		j.buffer = (char*)resume_data;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_rename_file(int index, std::string const& name
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.piece = index;
		j.str = name;
		j.action = disk_io_job::rename_file;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_check_files(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::check_files;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_read_and_hash(
		peer_request const& r
		, boost::function<void(int, disk_io_job const&)> const& handler
		, int cache_expiry)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::read_and_hash;
		j.piece = r.piece;
		j.offset = r.start;
		j.buffer_size = r.length;
		j.buffer = 0;
		j.cache_min_time = cache_expiry;
		TORRENT_ASSERT(r.length <= 16 * 1024);
		j.callback = handler;
		m_io_thread.add_job(j);
#ifdef TORRENT_DEBUG
		mutex::scoped_lock l(m_mutex);
		// if this assert is hit, it suggests
		// that check_files was not successful
		TORRENT_ASSERT(slot_for(r.piece) >= 0);
#endif
	}

	void piece_manager::async_cache(int piece
		, boost::function<void(int, disk_io_job const&)> const& handler
		, int cache_expiry)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::cache_piece;
		j.piece = piece;
		j.offset = 0;
		j.buffer_size = 0;
		j.buffer = 0;
		j.cache_min_time = cache_expiry;
		j.callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_read(
		peer_request const& r
		, boost::function<void(int, disk_io_job const&)> const& handler
		, int cache_line_size
		, int cache_expiry)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::read;
		j.piece = r.piece;
		j.offset = r.start;
		j.buffer_size = r.length;
		j.buffer = 0;
		j.max_cache_line = cache_line_size;
		j.cache_min_time = cache_expiry;

		// if a buffer is not specified, only one block can be read
		// since that is the size of the pool allocator's buffers
		TORRENT_ASSERT(r.length <= 16 * 1024);
		j.callback = handler;
		m_io_thread.add_job(j);
#ifdef TORRENT_DEBUG
		mutex::scoped_lock l(m_mutex);
		// if this assert is hit, it suggests
		// that check_files was not successful
		TORRENT_ASSERT(slot_for(r.piece) >= 0);
#endif
	}

	int piece_manager::async_write(
		peer_request const& r
		, disk_buffer_holder& buffer
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		TORRENT_ASSERT(r.length <= 16 * 1024);
		// the buffer needs to be allocated through the io_thread
		TORRENT_ASSERT(m_io_thread.is_disk_buffer(buffer.get()));

		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::write;
		j.piece = r.piece;
		j.offset = r.start;
		j.buffer_size = r.length;
		j.buffer = buffer.get();
		j.callback = handler;
		int queue_size = m_io_thread.add_job(j);
		buffer.release();

		return queue_size;
	}

	void piece_manager::async_hash(int piece
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job j;
		j.storage = this;
		j.action = disk_io_job::hash;
		j.piece = piece;
		j.callback = handler;
		j.buffer_size = 0;
		m_io_thread.add_job(j);
	}

	std::string piece_manager::save_path() const
	{
		mutex::scoped_lock l(m_mutex);
		return m_save_path;
	}

	void piece_manager::move_storage_impl(std::string const& save_path, storage_error& ec)
	{
		m_storage->move_storage(save_path, ec);
		if (ec) return;
		m_save_path = complete(save_path);
	}

	void piece_manager::write_resume_data(entry& rd, storage_error& ec) const
	{
		mutex::scoped_lock lock(m_mutex);

		INVARIANT_CHECK;

		m_storage->write_resume_data(rd, ec);

		if (m_storage_mode == storage_mode_compact)
		{
			entry::list_type& slots = rd["slots"].list();
			slots.clear();
			std::vector<int>::const_reverse_iterator last; 
			for (last = m_slot_to_piece.rbegin();
				last != m_slot_to_piece.rend(); ++last)
			{
				if (*last != unallocated) break;
			}

			for (std::vector<int>::const_iterator i =
				m_slot_to_piece.begin();
				i != last.base(); ++i)
			{
				slots.push_back((*i >= 0) ? *i : unassigned);
			}
		}

		rd["allocation"] = m_storage_mode == storage_mode_sparse?"sparse"
			:m_storage_mode == storage_mode_allocate?"full":"compact";
	}

	void piece_manager::mark_failed(int piece_index)
	{
		mutex::scoped_lock lock(m_mutex);

		INVARIANT_CHECK;

		if (m_storage_mode != storage_mode_compact) return;

		TORRENT_ASSERT(piece_index >= 0 && piece_index < (int)m_piece_to_slot.size());
		int slot_index = m_piece_to_slot[piece_index];
		TORRENT_ASSERT(slot_index >= 0);

		m_slot_to_piece[slot_index] = unassigned;
		m_piece_to_slot[piece_index] = has_no_slot;
		m_free_slots.push_back(slot_index);
	}

	void piece_manager::hint_read_impl(int piece_index, int offset, int size)
	{
		m_last_piece = piece_index;
		int slot = slot_for(piece_index);
		if (slot <= 0) return;
		m_storage->hint_read(slot, offset, size);
	}

	file::aiocb_t* piece_manager::read_async_impl(
		file::iovec_t* bufs
		, int piece_index
		, int offset
		, int num_bufs
		, boost::function<void(async_handler*)> const& handler)
	{
		TORRENT_ASSERT(bufs);
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(num_bufs > 0);
		m_last_piece = piece_index;
		int slot = slot_for(piece_index);
		return m_storage->async_readv(bufs, slot, offset, num_bufs, handler);
	}

	file::aiocb_t* piece_manager::write_async_impl(
		file::iovec_t* bufs
		, int piece_index
		, int offset
		, int num_bufs
		, boost::function<void(async_handler*)> const& handler)
	{
		TORRENT_ASSERT(bufs);
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(num_bufs > 0);
		TORRENT_ASSERT(piece_index >= 0 && piece_index < m_files.num_pieces());

		file::iovec_t* iov = TORRENT_ALLOCA(file::iovec_t, num_bufs);
		std::copy(bufs, bufs + num_bufs, iov);
		m_last_piece = piece_index;
		int slot = allocate_slot_for_piece(piece_index);
		file::aiocb_t* ret = m_storage->async_writev(bufs, slot, offset, num_bufs, handler);
		return ret;
	}

	size_type piece_manager::physical_offset(
		int piece_index
		, int offset)
	{
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(piece_index >= 0 && piece_index < m_files.num_pieces());

		int slot = slot_for(piece_index);
		// we may not have a slot for this piece yet.
		// assume there is no re-mapping of slots
		if (slot < 0) slot = piece_index;
		return m_storage->physical_offset(slot, offset);
	}

	int piece_manager::identify_data(
		sha1_hash const& large_hash
		, sha1_hash const& small_hash
		, int current_slot)
	{
//		INVARIANT_CHECK;
		typedef std::multimap<sha1_hash, int>::const_iterator map_iter;
		map_iter begin1;
		map_iter end1;
		map_iter begin2;
		map_iter end2;

		// makes the lookups for the small digest and the large digest
		boost::tie(begin1, end1) = m_hash_to_piece.equal_range(small_hash);
		boost::tie(begin2, end2) = m_hash_to_piece.equal_range(large_hash);

		// copy all potential piece indices into this vector
		std::vector<int> matching_pieces;
		for (map_iter i = begin1; i != end1; ++i)
			matching_pieces.push_back(i->second);
		for (map_iter i = begin2; i != end2; ++i)
			matching_pieces.push_back(i->second);

		// no piece matched the data in the slot
		if (matching_pieces.empty())
			return unassigned;

		// ------------------------------------------
		// CHECK IF THE PIECE IS IN ITS CORRECT PLACE
		// ------------------------------------------

		if (std::find(
			matching_pieces.begin()
			, matching_pieces.end()
			, current_slot) != matching_pieces.end())
		{
			// the current slot is among the matching pieces, so
			// we will assume that the piece is in the right place
			const int piece_index = current_slot;

			int other_slot = m_piece_to_slot[piece_index];
			if (other_slot >= 0)
			{
				// we have already found a piece with
				// this index.

				// take one of the other matching pieces
				// that hasn't already been assigned
				int other_piece = -1;
				for (std::vector<int>::iterator i = matching_pieces.begin();
					i != matching_pieces.end(); ++i)
				{
					if (m_piece_to_slot[*i] >= 0 || *i == piece_index) continue;
					other_piece = *i;
					break;
				}
				if (other_piece >= 0)
				{
					// replace the old slot with 'other_piece'
					m_slot_to_piece[other_slot] = other_piece;
					m_piece_to_slot[other_piece] = other_slot;
				}
				else
				{
					// this index is the only piece with this
					// hash. The previous slot we found with
					// this hash must be the same piece. Mark
					// that piece as unassigned, since this slot
					// is the correct place for the piece.
					m_slot_to_piece[other_slot] = unassigned;
					if (m_storage_mode == storage_mode_compact)
						m_free_slots.push_back(other_slot);
				}
				TORRENT_ASSERT(m_piece_to_slot[piece_index] != current_slot);
				TORRENT_ASSERT(m_piece_to_slot[piece_index] >= 0);
				m_piece_to_slot[piece_index] = has_no_slot;
			}
			
			TORRENT_ASSERT(m_piece_to_slot[piece_index] == has_no_slot);

			return piece_index;
		}

		// find a matching piece that hasn't
		// already been assigned
		int free_piece = unassigned;
		for (std::vector<int>::iterator i = matching_pieces.begin();
			i != matching_pieces.end(); ++i)
		{
			if (m_piece_to_slot[*i] >= 0) continue;
			free_piece = *i;
			break;
		}

		if (free_piece >= 0)
		{
			TORRENT_ASSERT(m_piece_to_slot[free_piece] == has_no_slot);
			return free_piece;
		}
		else
		{
			TORRENT_ASSERT(free_piece == unassigned);
			return unassigned;
		}
	}

	int piece_manager::check_no_fastresume(storage_error& ec)
	{
		bool has_files = false;
		if (!m_storage->settings().no_recheck_incomplete_resume)
		{
			has_files = m_storage->has_any_file(ec);

			if (ec) return fatal_disk_error; 

			if (has_files)
			{
				m_state = state_full_check;
				m_piece_to_slot.clear();
				m_piece_to_slot.resize(m_files.num_pieces(), has_no_slot);
				m_slot_to_piece.clear();
				m_slot_to_piece.resize(m_files.num_pieces(), unallocated);
				if (m_storage_mode == storage_mode_compact)
				{
					m_unallocated_slots.clear();
					m_free_slots.clear();
				}
				TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());
				return need_full_check;
			}
		}

		if (m_storage_mode == storage_mode_compact)
		{
			// in compact mode without checking, we need to
			// populate the unallocated list
			TORRENT_ASSERT(m_unallocated_slots.empty());
			for (int i = 0, end(m_files.num_pieces()); i < end; ++i)
				m_unallocated_slots.push_back(i);
			m_piece_to_slot.clear();
			m_piece_to_slot.resize(m_files.num_pieces(), has_no_slot);
			m_slot_to_piece.clear();
			m_slot_to_piece.resize(m_files.num_pieces(), unallocated);
		}
	
		return check_init_storage(ec);
	}
	
	int piece_manager::check_init_storage(storage_error& ec)
	{
		m_storage->initialize(m_storage_mode == storage_mode_allocate, ec);
		if (ec) return fatal_disk_error;
		m_state = state_finished;
		m_scratch_buffer.reset();
		m_scratch_buffer2.reset();
		if (m_storage_mode != storage_mode_compact)
		{
			// if no piece is out of place
			// since we're in full allocation mode, we can
			// forget the piece allocation tables
			std::vector<int>().swap(m_piece_to_slot);
			std::vector<int>().swap(m_slot_to_piece);
			std::vector<int>().swap(m_free_slots);
			std::vector<int>().swap(m_unallocated_slots);
		}
		return no_error;
	}

	// check if the fastresume data is up to date
	// if it is, use it and return true. If it 
	// isn't return false and the full check
	// will be run
	int piece_manager::check_fastresume(
		lazy_entry const& rd, storage_error& ec)
	{
		mutex::scoped_lock lock(m_mutex);

		INVARIANT_CHECK;

		TORRENT_ASSERT(m_files.piece_length() > 0);
		
		m_current_slot = 0;

		// if we don't have any resume data, return
		if (rd.type() == lazy_entry::none_t) return check_no_fastresume(ec);

		if (rd.type() != lazy_entry::dict_t)
		{
			ec.ec = errors::not_a_dictionary;
			return check_no_fastresume(ec);
		}

		int block_size = (std::min)(16 * 1024, m_files.piece_length());
		int blocks_per_piece = int(rd.dict_find_int_value("blocks per piece", -1));
		if (blocks_per_piece != -1
			&& blocks_per_piece != m_files.piece_length() / block_size)
		{
			ec.ec = errors::invalid_blocks_per_piece;
			return check_no_fastresume(ec);
		}

		storage_mode_t storage_mode = storage_mode_compact;
		if (rd.dict_find_string_value("allocation") != "compact")
			storage_mode = storage_mode_sparse;

		if (!m_storage->verify_resume_data(rd, ec))
			return check_no_fastresume(ec);

		// assume no piece is out of place (i.e. in a slot
		// other than the one it should be in)
		bool out_of_place = false;

		// if we don't have a piece map, we need the slots
		// if we're in compact mode, we also need the slots map
		if (storage_mode == storage_mode_compact || rd.dict_find("pieces") == 0)
		{
			// read slots map
			lazy_entry const* slots = rd.dict_find_list("slots");
			if (slots == 0)
			{
				ec.ec = errors::missing_slots;
				return check_no_fastresume(ec);
			}

			if ((int)slots->list_size() > m_files.num_pieces())
			{
				ec.ec = errors::too_many_slots;
				return check_no_fastresume(ec);
			}

			if (m_storage_mode == storage_mode_compact)
			{
				int num_pieces = int(m_files.num_pieces());
				m_slot_to_piece.resize(num_pieces, unallocated);
				m_piece_to_slot.resize(num_pieces, has_no_slot);
				for (int i = 0; i < slots->list_size(); ++i)
				{
					lazy_entry const* e = slots->list_at(i);
					if (e->type() != lazy_entry::int_t)
					{
						ec.ec = errors::invalid_slot_list;
						return check_no_fastresume(ec);
					}

					int index = int(e->int_value());
					if (index >= num_pieces || index < -2)
					{
						ec.ec = errors::invalid_piece_index;
						return check_no_fastresume(ec);
					}
					if (index >= 0)
					{
						m_slot_to_piece[i] = index;
						m_piece_to_slot[index] = i;
						if (i != index) out_of_place = true;
					}
					else if (index == unassigned)
					{
						if (m_storage_mode == storage_mode_compact)
							m_free_slots.push_back(i);
					}
					else
					{
						TORRENT_ASSERT(index == unallocated);
						if (m_storage_mode == storage_mode_compact)
							m_unallocated_slots.push_back(i);
					}
				}
			}
			else
			{
				for (int i = 0; i < slots->list_size(); ++i)
				{
					lazy_entry const* e = slots->list_at(i);
					if (e->type() != lazy_entry::int_t)
					{
						ec.ec = errors::invalid_slot_list;
						return check_no_fastresume(ec);
					}

					int index = int(e->int_value());
					if (index != i && index >= 0)
					{
						ec.ec = errors::invalid_piece_index;
						return check_no_fastresume(ec);
					}
				}
			}

			// This will corrupt the storage
			// use while debugging to find
			// states that cannot be scanned
			// by check_pieces.
			//		m_storage->shuffle();

			if (m_storage_mode == storage_mode_compact)
			{
				if (m_unallocated_slots.empty()) switch_to_full_mode();
			}
			else
			{
				TORRENT_ASSERT(m_free_slots.empty());
				TORRENT_ASSERT(m_unallocated_slots.empty());

				if (out_of_place)
				{
					// in this case we're in full allocation mode, but
					// we're resuming a compact allocated storage
					m_state = state_expand_pieces;
					m_current_slot = 0;
					ec.ec = errors::pieces_need_reorder;
					TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());
					return need_full_check;
				}
			}

		}
		else if (m_storage_mode == storage_mode_compact)
		{
			// read piece map
			lazy_entry const* pieces = rd.dict_find("pieces");
			if (pieces == 0 || pieces->type() != lazy_entry::string_t)
			{
				ec.ec = errors::missing_pieces;
				return check_no_fastresume(ec);
			}

			if ((int)pieces->string_length() != m_files.num_pieces())
			{
				ec.ec = errors::too_many_slots;
				return check_no_fastresume(ec);
			}

			int num_pieces = int(m_files.num_pieces());
			m_slot_to_piece.resize(num_pieces, unallocated);
			m_piece_to_slot.resize(num_pieces, has_no_slot);
			char const* have_pieces = pieces->string_ptr();
			for (int i = 0; i < num_pieces; ++i)
			{
				if (have_pieces[i] & 1)
				{
					m_slot_to_piece[i] = i;
					m_piece_to_slot[i] = i;
				}
				else
				{
					m_free_slots.push_back(i);
				}
			}
			if (m_unallocated_slots.empty()) switch_to_full_mode();
		}

		return check_init_storage(ec);
	}

/*
   state chart:

   check_fastresume()  ----------+
                                 |
      |        |                 |
      |        v                 v
      |  +------------+   +---------------+
      |  | full_check |-->| expand_pieses |
      |  +------------+   +---------------+
      |        |                 |
      |        v                 |
      |  +--------------+        |
      +->|   finished   | <------+
         +--------------+
*/


	// performs the full check and full allocation
	// (if necessary). returns true if finished and
	// false if it should be called again
	// the second return value is the progress the
	// file check is at. 0 is nothing done, and 1
	// is finished
	int piece_manager::check_files(int& current_slot, int& have_piece, storage_error& error)
	{
		if (m_state == state_none) return check_no_fastresume(error);

		TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());

		current_slot = m_current_slot;
		have_piece = -1;
		if (m_state == state_expand_pieces)
		{
			INVARIANT_CHECK;

			if (m_scratch_piece >= 0)
			{
				int piece = m_scratch_piece;
				int other_piece = m_slot_to_piece[piece];
				m_scratch_piece = -1;

				if (other_piece >= 0)
				{
					if (!m_scratch_buffer2.get())
						m_scratch_buffer2.reset(page_aligned_allocator::malloc(m_files.piece_length()));

					int piece_size = m_files.piece_size(other_piece);
					if (m_storage->read(m_scratch_buffer2.get(), piece, 0, piece_size, error)
						!= piece_size)
						return fatal_disk_error;
					m_scratch_piece = other_piece;
					m_piece_to_slot[other_piece] = unassigned;
				}
				
				// the slot where this piece belongs is
				// free. Just move the piece there.
				int piece_size = m_files.piece_size(piece);
				if (m_storage->write(m_scratch_buffer.get(), piece, 0, piece_size, error) != piece_size)
					return fatal_disk_error;

				m_piece_to_slot[piece] = piece;
				m_slot_to_piece[piece] = piece;

				if (other_piece >= 0) m_scratch_buffer.swap(m_scratch_buffer2);
		
				TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());
				return need_full_check;
			}

			while (m_current_slot < m_files.num_pieces()
				&& (m_slot_to_piece[m_current_slot] == m_current_slot
				|| m_slot_to_piece[m_current_slot] < 0))
			{
				++m_current_slot;
			}

			if (m_current_slot == m_files.num_pieces())
			{
				return check_init_storage(error);
			}

			TORRENT_ASSERT(m_current_slot < m_files.num_pieces());

			int piece = m_slot_to_piece[m_current_slot];
			TORRENT_ASSERT(piece >= 0);
			int other_piece = m_slot_to_piece[piece];
			if (other_piece >= 0)
			{
				// there is another piece in the slot
				// where this one goes. Store it in the scratch
				// buffer until next iteration.
				if (!m_scratch_buffer.get())
					m_scratch_buffer.reset(page_aligned_allocator::malloc(m_files.piece_length()));
			
				int piece_size = m_files.piece_size(other_piece);
				if (m_storage->read(m_scratch_buffer.get(), piece, 0, piece_size, error) != piece_size)
					return fatal_disk_error;

				m_scratch_piece = other_piece;
				m_piece_to_slot[other_piece] = unassigned;
			}

			// the slot where this piece belongs is
			// free. Just move the piece there.
			m_last_piece = piece;
			m_storage->move_slot(m_current_slot, piece, error);
			if (error.ec) return -1;

			m_piece_to_slot[piece] = piece;
			m_slot_to_piece[m_current_slot] = unassigned;
			m_slot_to_piece[piece] = piece;
		
			TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());
			return need_full_check;
		}

		TORRENT_ASSERT(m_state == state_full_check);
		if (m_state == state_finished) return 0;

		int skip = check_one_piece(have_piece, error);
		TORRENT_ASSERT(m_current_slot <= m_files.num_pieces());

		if (skip == -1)
		{
			TORRENT_ASSERT(error.ec);
			return fatal_disk_error;
		}

		if (skip > 0)
		{
			// skip means that the piece we checked failed to be read from disk
			// completely. This may be caused by the file not being there, or the
			// piece overlapping with a sparse region. We should skip 'skip' number
			// of pieces

			if (m_storage_mode == storage_mode_compact)
			{
				for (int i = m_current_slot; i < m_current_slot + skip - 1; ++i)
				{
					TORRENT_ASSERT(m_slot_to_piece[i] == unallocated);
					m_unallocated_slots.push_back(i);
				}
			}

			// current slot will increase by one below
			m_current_slot += skip - 1;
			TORRENT_ASSERT(m_current_slot <= m_files.num_pieces());
		}

		++m_current_slot;
		current_slot = m_current_slot;

		if (m_current_slot >= m_files.num_pieces())
		{
			TORRENT_ASSERT(m_current_slot == m_files.num_pieces());

			// clear the memory we've been using
			std::multimap<sha1_hash, int>().swap(m_hash_to_piece);

			if (m_storage_mode != storage_mode_compact)
			{
				if (!m_out_of_place)
				{
					// if no piece is out of place
					// since we're in full allocation mode, we can
					// forget the piece allocation tables

					std::vector<int>().swap(m_piece_to_slot);
					std::vector<int>().swap(m_slot_to_piece);
					return check_init_storage(error);
				}
				else
				{
					// in this case we're in full allocation mode, but
					// we're resuming a compact allocated storage
					m_state = state_expand_pieces;
					m_current_slot = 0;
					current_slot = m_current_slot;
					TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());
					return need_full_check;
				}
			}
			else if (m_unallocated_slots.empty())
			{
				switch_to_full_mode();
			}
			return check_init_storage(error);
		}

		TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());
		return need_full_check;
	}

	int piece_manager::skip_file() const
	{
		size_type file_offset = 0;
		size_type current_offset = size_type(m_current_slot) * m_files.piece_length();
		for (file_storage::iterator i = m_files.begin()
			, end(m_files.end()); i != end; ++i)
		{
			file_offset += i->size;
			if (file_offset > current_offset) break;
		}

		TORRENT_ASSERT(file_offset > current_offset);
		int ret = static_cast<int>(
			(file_offset - current_offset + m_files.piece_length() - 1)
			/ m_files.piece_length());
		TORRENT_ASSERT(ret >= 1);
		return ret;
	}

	// -1 = error, 0 = ok, >0 = skip this many pieces
	int piece_manager::check_one_piece(int& have_piece, storage_error& ec)
	{
		// ------------------------
		//    DO THE FULL CHECK
		// ------------------------

		TORRENT_ASSERT(int(m_piece_to_slot.size()) == m_files.num_pieces());
		TORRENT_ASSERT(int(m_slot_to_piece.size()) == m_files.num_pieces());
		TORRENT_ASSERT(have_piece == -1);

		// initialization for the full check
		if (m_hash_to_piece.empty())
		{
			for (int i = 0; i < m_files.num_pieces(); ++i)
				m_hash_to_piece.insert(std::pair<const sha1_hash, int>(m_info->hash_for_piece(i), i));
		}

		partial_hash ph;
		int num_read = 0;
		int piece_size = m_files.piece_size(m_current_slot);
		int small_piece_size = m_files.piece_size(m_files.num_pieces() - 1);
		bool read_short = true;
		sha1_hash small_hash;
		if (piece_size == small_piece_size)
		{
			num_read = hash_for_slot(m_current_slot, ph, piece_size, ec);
		}
		else
		{
			num_read = hash_for_slot(m_current_slot, ph, piece_size, ec
				, small_piece_size, &small_hash);
		}
		read_short = num_read != piece_size;

		if (read_short)
		{
			if (ec
#ifdef TORRENT_WINDOWS
				&& ec.ec != error_code(ERROR_PATH_NOT_FOUND, get_system_category())
				&& ec.ec != error_code(ERROR_FILE_NOT_FOUND, get_system_category())
				&& ec.ec != error_code(ERROR_HANDLE_EOF, get_system_category())
				&& ec.ec != error_code(ERROR_INVALID_HANDLE, get_system_category()))
#else
				&& ec.ec != error_code(ENOENT, get_posix_category()))
#endif
			{
				return -1;
			}
			ec.ec.clear();
			// if the file is incomplete, skip the rest of it
			return skip_file();
		}

		sha1_hash large_hash = ph.h.final();
		int piece_index = identify_data(large_hash, small_hash, m_current_slot);

		if (piece_index >= 0) have_piece = piece_index;

		if (piece_index != m_current_slot
			&& piece_index >= 0)
			m_out_of_place = true;

		TORRENT_ASSERT(piece_index == unassigned || piece_index >= 0);

		const bool this_should_move = piece_index >= 0 && m_slot_to_piece[piece_index] != unallocated;
		const bool other_should_move = m_piece_to_slot[m_current_slot] != has_no_slot;

		// check if this piece should be swapped with any other slot
		// this section will ensure that the storage is correctly sorted
		// libtorrent will never leave the storage in a state that
		// requires this sorting, but other clients may.

		// example of worst case:
		//                          | m_current_slot = 5
		//                          V
		//  +---+- - - +---+- - - +---+- -
		//  | x |      | 5 |      | 3 |     <- piece data in slots
		//  +---+- - - +---+- - - +---+- -
		//    3          y          5       <- slot index

		// in this example, the data in the m_current_slot (5)
		// is piece 3. It has to be moved into slot 3. The data
		// in slot y (piece 5) should be moved into the m_current_slot.
		// and the data in slot 3 (piece x) should be moved to slot y.

		// there are three possible cases.
		// 1. There's another piece that should be placed into this slot
		// 2. This piece should be placed into another slot.
		// 3. There's another piece that should be placed into this slot
		//    and this piece should be placed into another slot

		// swap piece_index with this slot

		// case 1
		if (this_should_move && !other_should_move)
		{
			TORRENT_ASSERT(piece_index != m_current_slot);

			const int other_slot = piece_index;
			TORRENT_ASSERT(other_slot >= 0);
			int other_piece = m_slot_to_piece[other_slot];

			m_slot_to_piece[other_slot] = piece_index;
			m_slot_to_piece[m_current_slot] = other_piece;
			m_piece_to_slot[piece_index] = piece_index;
			if (other_piece >= 0) m_piece_to_slot[other_piece] = m_current_slot;

			if (other_piece == unassigned)
			{
				std::vector<int>::iterator i =
					std::find(m_free_slots.begin(), m_free_slots.end(), other_slot);
				TORRENT_ASSERT(i != m_free_slots.end());
				if (m_storage_mode == storage_mode_compact)
				{
					m_free_slots.erase(i);
					m_free_slots.push_back(m_current_slot);
				}
			}

			m_last_piece = piece_index;
			storage_error ec;
			if (other_piece >= 0)
				m_storage->swap_slots(other_slot, m_current_slot, ec);
			else
				m_storage->move_slot(m_current_slot, other_slot, ec);

			if (ec) return skip_file();

			TORRENT_ASSERT(m_slot_to_piece[m_current_slot] == unassigned
				|| m_piece_to_slot[m_slot_to_piece[m_current_slot]] == m_current_slot);
		}
		// case 2
		else if (!this_should_move && other_should_move)
		{
			TORRENT_ASSERT(piece_index != m_current_slot);

			const int other_piece = m_current_slot;
			const int other_slot = m_piece_to_slot[other_piece];
			TORRENT_ASSERT(other_slot >= 0);

			m_slot_to_piece[m_current_slot] = other_piece;
			m_slot_to_piece[other_slot] = piece_index;
			m_piece_to_slot[other_piece] = m_current_slot;

			if (piece_index == unassigned
				&& m_storage_mode == storage_mode_compact)
				m_free_slots.push_back(other_slot);

			storage_error ec;
			if (piece_index >= 0)
			{
				m_piece_to_slot[piece_index] = other_slot;
				m_storage->swap_slots(other_slot, m_current_slot, ec);
			}
			else
			{
				m_storage->move_slot(other_slot, m_current_slot, ec);

			}
			m_last_piece = other_piece;
			if (ec) return skip_file();

			TORRENT_ASSERT(m_slot_to_piece[m_current_slot] == unassigned
				|| m_piece_to_slot[m_slot_to_piece[m_current_slot]] == m_current_slot);
		}
		else if (this_should_move && other_should_move)
		{
			TORRENT_ASSERT(piece_index != m_current_slot);
			TORRENT_ASSERT(piece_index >= 0);

			const int piece1 = m_slot_to_piece[piece_index];
			const int piece2 = m_current_slot;
			const int slot1 = piece_index;
			const int slot2 = m_piece_to_slot[piece2];

			TORRENT_ASSERT(slot1 >= 0);
			TORRENT_ASSERT(slot2 >= 0);
			TORRENT_ASSERT(piece2 >= 0);

			if (slot1 == slot2)
			{
				// this means there are only two pieces involved in the swap
				TORRENT_ASSERT(piece1 >= 0);

				// movement diagram:
				// +-------------------------------+
				// |                               |
				// +--> slot1 --> m_current_slot --+

				m_slot_to_piece[slot1] = piece_index;
				m_slot_to_piece[m_current_slot] = piece1;

				m_piece_to_slot[piece_index] = slot1;
				m_piece_to_slot[piece1] = m_current_slot;

				TORRENT_ASSERT(piece1 == m_current_slot);
				TORRENT_ASSERT(piece_index == slot1);

				m_last_piece = piece_index;
				storage_error ec;
				m_storage->swap_slots(m_current_slot, slot1, ec);
				// #error report errors here!

				TORRENT_ASSERT(m_slot_to_piece[m_current_slot] == unassigned
					|| m_piece_to_slot[m_slot_to_piece[m_current_slot]] == m_current_slot);
			}
			else
			{
				TORRENT_ASSERT(slot1 != slot2);
				TORRENT_ASSERT(piece1 != piece2);

				// movement diagram:
				// +-----------------------------------------+
				// |                                         |
				// +--> slot1 --> slot2 --> m_current_slot --+

				m_slot_to_piece[slot1] = piece_index;
				m_slot_to_piece[slot2] = piece1;
				m_slot_to_piece[m_current_slot] = piece2;

				m_piece_to_slot[piece_index] = slot1;
				m_piece_to_slot[m_current_slot] = piece2;

				if (piece1 == unassigned)
				{
					std::vector<int>::iterator i =
						std::find(m_free_slots.begin(), m_free_slots.end(), slot1);
					TORRENT_ASSERT(i != m_free_slots.end());
					if (m_storage_mode == storage_mode_compact)
					{
						m_free_slots.erase(i);
						m_free_slots.push_back(slot2);
					}
				}

				storage_error ec;
				if (piece1 >= 0)
				{
					m_piece_to_slot[piece1] = slot2;
					m_storage->swap_slots3(m_current_slot, slot1, slot2, ec);
				}
				else
				{
					m_storage->move_slot(m_current_slot, slot1, ec);
					m_storage->move_slot(slot2, m_current_slot, ec);
				}

				m_last_piece = piece_index;
				if (ec) return skip_file();

				TORRENT_ASSERT(m_slot_to_piece[m_current_slot] == unassigned
					|| m_piece_to_slot[m_slot_to_piece[m_current_slot]] == m_current_slot);
			}
		}
		else
		{
			TORRENT_ASSERT(m_piece_to_slot[m_current_slot] == has_no_slot || piece_index != m_current_slot);
			TORRENT_ASSERT(m_slot_to_piece[m_current_slot] == unallocated);
			TORRENT_ASSERT(piece_index == unassigned || m_piece_to_slot[piece_index] == has_no_slot);

			// the slot was identified as piece 'piece_index'
			if (piece_index != unassigned)
				m_piece_to_slot[piece_index] = m_current_slot;
			else if (m_storage_mode == storage_mode_compact)
				m_free_slots.push_back(m_current_slot);

			m_slot_to_piece[m_current_slot] = piece_index;

			TORRENT_ASSERT(m_slot_to_piece[m_current_slot] == unassigned
				|| m_piece_to_slot[m_slot_to_piece[m_current_slot]] == m_current_slot);
		}

		if (piece_index == unassigned)
		{
			// the data did not match any piece. Maybe we're reading
			// from a sparse region, see if we are and skip
			if (m_current_slot == m_files.num_pieces() -1) return 0;

			int next_slot = m_storage->sparse_end(m_current_slot + 1);
			if (next_slot > m_current_slot + 1) return next_slot - m_current_slot;
		}

		return 0;
	}

	void piece_manager::switch_to_full_mode()
	{
		TORRENT_ASSERT(m_storage_mode == storage_mode_compact);	
		TORRENT_ASSERT(m_unallocated_slots.empty());	
		// we have allocated all slots, switch to
		// full allocation mode in order to free
		// some unnecessary memory.
		m_storage_mode = storage_mode_sparse;
		std::vector<int>().swap(m_unallocated_slots);
		std::vector<int>().swap(m_free_slots);
		std::vector<int>().swap(m_piece_to_slot);
		std::vector<int>().swap(m_slot_to_piece);
	}

	int piece_manager::allocate_slot_for_piece(int piece_index)
	{
		mutex::scoped_lock lock(m_mutex);

		if (m_storage_mode != storage_mode_compact) return piece_index;

		INVARIANT_CHECK;

		TORRENT_ASSERT(piece_index >= 0);
		TORRENT_ASSERT(piece_index < (int)m_piece_to_slot.size());
		TORRENT_ASSERT(m_piece_to_slot.size() == m_slot_to_piece.size());

		int slot_index = m_piece_to_slot[piece_index];

		if (slot_index != has_no_slot)
		{
			TORRENT_ASSERT(slot_index >= 0);
			TORRENT_ASSERT(slot_index < (int)m_slot_to_piece.size());
			return slot_index;
		}

		if (m_free_slots.empty())
		{
			allocate_slots_impl(1, lock);
			TORRENT_ASSERT(!m_free_slots.empty());
		}

		std::vector<int>::iterator iter(
			std::find(
				m_free_slots.begin()
				, m_free_slots.end()
				, piece_index));

		if (iter == m_free_slots.end())
		{
			TORRENT_ASSERT(m_slot_to_piece[piece_index] != unassigned);
			TORRENT_ASSERT(!m_free_slots.empty());
			iter = m_free_slots.end() - 1;

			// special case to make sure we don't use the last slot
			// when we shouldn't, since it's smaller than ordinary slots
			if (*iter == m_files.num_pieces() - 1 && piece_index != *iter)
			{
				if (m_free_slots.size() == 1)
					allocate_slots_impl(1, lock);
				TORRENT_ASSERT(m_free_slots.size() > 1);
				// assumes that all allocated slots
				// are put at the end of the free_slots vector
				iter = m_free_slots.end() - 1;
			}
		}

		slot_index = *iter;
		m_free_slots.erase(iter);

		TORRENT_ASSERT(m_slot_to_piece[slot_index] == unassigned);

		m_slot_to_piece[slot_index] = piece_index;
		m_piece_to_slot[piece_index] = slot_index;

		// there is another piece already assigned to
		// the slot we are interested in, swap positions
		if (slot_index != piece_index
			&& m_slot_to_piece[piece_index] >= 0)
		{

#if defined TORRENT_DEBUG && defined TORRENT_STORAGE_DEBUG && TORRENT_USE_IOSTREAM
			std::stringstream s;

			s << "there is another piece at our slot, swapping..";

			s << "\n   piece_index: ";
			s << piece_index;
			s << "\n   slot_index: ";
			s << slot_index;
			s << "\n   piece at our slot: ";
			s << m_slot_to_piece[piece_index];
			s << "\n";

			print_to_log(s.str());
			debug_log();
#endif

			int piece_at_our_slot = m_slot_to_piece[piece_index];
			TORRENT_ASSERT(m_piece_to_slot[piece_at_our_slot] == piece_index);

			std::swap(
				m_slot_to_piece[piece_index]
				, m_slot_to_piece[slot_index]);

			std::swap(
				m_piece_to_slot[piece_index]
				, m_piece_to_slot[piece_at_our_slot]);

			m_last_piece = piece_index;
			storage_error ec;
			m_storage->move_slot(piece_index, slot_index, ec);
			// #error report errors here!

			TORRENT_ASSERT(m_slot_to_piece[piece_index] == piece_index);
			TORRENT_ASSERT(m_piece_to_slot[piece_index] == piece_index);

			slot_index = piece_index;

#if defined TORRENT_DEBUG && defined TORRENT_STORAGE_DEBUG
			debug_log();
#endif
		}
		TORRENT_ASSERT(slot_index >= 0);
		TORRENT_ASSERT(slot_index < (int)m_slot_to_piece.size());

		if (m_free_slots.empty() && m_unallocated_slots.empty())
			switch_to_full_mode();
		
		return slot_index;
	}

	bool piece_manager::allocate_slots_impl(int num_slots, mutex::scoped_lock& l
		, bool abort_on_disk)
	{
		TORRENT_ASSERT(num_slots > 0);

#ifdef TORRENT_EXPENSIVE_INVARIANT_CHECKS
		INVARIANT_CHECK;
#endif

		TORRENT_ASSERT(!m_unallocated_slots.empty());
		TORRENT_ASSERT(m_storage_mode == storage_mode_compact);

		bool written = false;

		for (int i = 0; i < num_slots && !m_unallocated_slots.empty(); ++i)
		{
			int pos = m_unallocated_slots.front();
			TORRENT_ASSERT(m_slot_to_piece[pos] == unallocated);
			TORRENT_ASSERT(m_piece_to_slot[pos] != pos);

			int new_free_slot = pos;
			if (m_piece_to_slot[pos] != has_no_slot)
			{
				m_last_piece = pos;
				new_free_slot = m_piece_to_slot[pos];
				storage_error ec;
				m_storage->move_slot(new_free_slot, pos, ec);
				// #error report errors here!
				m_slot_to_piece[pos] = pos;
				m_piece_to_slot[pos] = pos;
				written = true;
			}
			m_unallocated_slots.erase(m_unallocated_slots.begin());
			m_slot_to_piece[new_free_slot] = unassigned;
			m_free_slots.push_back(new_free_slot);
			if (abort_on_disk && written) break;
		}

		TORRENT_ASSERT(m_free_slots.size() > 0);
		return written;
	}

	int piece_manager::slot_for(int piece) const
	{
		if (m_storage_mode != storage_mode_compact) return piece;
		TORRENT_ASSERT(piece < int(m_piece_to_slot.size()));
		TORRENT_ASSERT(piece >= 0);
		return m_piece_to_slot[piece];
	}

	int piece_manager::piece_for(int slot) const
	{
		if (m_storage_mode != storage_mode_compact) return slot;
		TORRENT_ASSERT(slot < int(m_slot_to_piece.size()));
		TORRENT_ASSERT(slot >= 0);
		return m_slot_to_piece[slot];
	}
		
#ifdef TORRENT_DEBUG
	void piece_manager::check_invariant() const
	{
		TORRENT_ASSERT(m_current_slot <= m_files.num_pieces());
		
		if (m_unallocated_slots.empty()
			&& m_free_slots.empty()
			&& m_state == state_finished)
		{
			TORRENT_ASSERT(m_storage_mode != storage_mode_compact
				|| m_files.num_pieces() == 0);
		}
		
		if (m_storage_mode != storage_mode_compact)
		{
			TORRENT_ASSERT(m_unallocated_slots.empty());
			TORRENT_ASSERT(m_free_slots.empty());
		}
		
		if (m_storage_mode != storage_mode_compact
			&& m_state != state_expand_pieces
			&& m_state != state_full_check)
		{
			TORRENT_ASSERT(m_piece_to_slot.empty());
			TORRENT_ASSERT(m_slot_to_piece.empty());
		}
		else
		{
			if (m_piece_to_slot.empty()) return;

			TORRENT_ASSERT((int)m_piece_to_slot.size() == m_files.num_pieces());
			TORRENT_ASSERT((int)m_slot_to_piece.size() == m_files.num_pieces());

			for (std::vector<int>::const_iterator i = m_free_slots.begin();
					i != m_free_slots.end(); ++i)
			{
				TORRENT_ASSERT(*i < (int)m_slot_to_piece.size());
				TORRENT_ASSERT(*i >= 0);
				TORRENT_ASSERT(m_slot_to_piece[*i] == unassigned);
				TORRENT_ASSERT(std::find(i+1, m_free_slots.end(), *i)
						== m_free_slots.end());
			}

			for (std::vector<int>::const_iterator i = m_unallocated_slots.begin();
					i != m_unallocated_slots.end(); ++i)
			{
				TORRENT_ASSERT(*i < (int)m_slot_to_piece.size());
				TORRENT_ASSERT(*i >= 0);
				TORRENT_ASSERT(m_slot_to_piece[*i] == unallocated);
				TORRENT_ASSERT(std::find(i+1, m_unallocated_slots.end(), *i)
						== m_unallocated_slots.end());
			}

			for (int i = 0; i < m_files.num_pieces(); ++i)
			{
				// Check domain of piece_to_slot's elements
				if (m_piece_to_slot[i] != has_no_slot)
				{
					TORRENT_ASSERT(m_piece_to_slot[i] >= 0);
					TORRENT_ASSERT(m_piece_to_slot[i] < (int)m_slot_to_piece.size());
				}

				// Check domain of slot_to_piece's elements
				if (m_slot_to_piece[i] != unallocated
						&& m_slot_to_piece[i] != unassigned)
				{
					TORRENT_ASSERT(m_slot_to_piece[i] >= 0);
					TORRENT_ASSERT(m_slot_to_piece[i] < (int)m_piece_to_slot.size());
				}

				// do more detailed checks on piece_to_slot
				if (m_piece_to_slot[i] >= 0)
				{
					TORRENT_ASSERT(m_slot_to_piece[m_piece_to_slot[i]] == i);
					if (m_piece_to_slot[i] != i)
					{
						TORRENT_ASSERT(m_slot_to_piece[i] == unallocated);
					}
				}
				else
				{
					TORRENT_ASSERT(m_piece_to_slot[i] == has_no_slot);
				}

				// do more detailed checks on slot_to_piece

				if (m_slot_to_piece[i] >= 0)
				{
					TORRENT_ASSERT(m_slot_to_piece[i] < (int)m_piece_to_slot.size());
					TORRENT_ASSERT(m_piece_to_slot[m_slot_to_piece[i]] == i);
#ifdef TORRENT_STORAGE_DEBUG
					TORRENT_ASSERT(
							std::find(
								m_unallocated_slots.begin()
								, m_unallocated_slots.end()
								, i) == m_unallocated_slots.end()
							);
					TORRENT_ASSERT(
							std::find(
								m_free_slots.begin()
								, m_free_slots.end()
								, i) == m_free_slots.end()
							);
#endif
				}
				else if (m_slot_to_piece[i] == unallocated)
				{
#ifdef TORRENT_STORAGE_DEBUG
					TORRENT_ASSERT(m_unallocated_slots.empty()
							|| (std::find(
									m_unallocated_slots.begin()
									, m_unallocated_slots.end()
									, i) != m_unallocated_slots.end())
							);
#endif
				}
				else if (m_slot_to_piece[i] == unassigned)
				{
#ifdef TORRENT_STORAGE_DEBUG
					TORRENT_ASSERT(
							std::find(
								m_free_slots.begin()
								, m_free_slots.end()
								, i) != m_free_slots.end()
							);
#endif
				}
				else
				{
					TORRENT_ASSERT(false && "m_slot_to_piece[i] is invalid");
				}
			}
		}
	}

#if defined(TORRENT_STORAGE_DEBUG) && TORRENT_USE_IOSTREAM
	void piece_manager::debug_log() const
	{
		std::stringstream s;

		s << "index\tslot\tpiece\n";

		for (int i = 0; i < m_files.num_pieces(); ++i)
		{
			s << i << "\t" << m_slot_to_piece[i] << "\t";
			s << m_piece_to_slot[i] << "\n";
		}

		s << "---------------------------------\n";

		print_to_log(s.str());
	}
#endif
#endif
} // namespace libtorrent

