/*

Copyright (c) 2011, Arvid Norberg
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

#include "libtorrent/disk_io_job.hpp"
#include "libtorrent/storage.hpp"

namespace libtorrent
{
	disk_io_job::disk_io_job()
		: buffer(0)
		, requester(0)
		, piece(0)
		, action(read)
		, ret(0)
		, flags(0)
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		, in_use(false)
		, callback_called(false)
#endif
	{
		d.io.offset = 0;
		d.io.buffer_size = 0;
		d.io.max_cache_line = 0;
	}

	disk_io_job::~disk_io_job()
	{
		if (action == rename_file || action == move_storage)
			free(buffer);
		if (action == save_resume_data)
			delete (entry*)buffer;
	}

	bool is_job_immediate(int type)
	{
		return type == disk_io_job::get_cache_info
			|| type == disk_io_job::update_settings
			|| type == disk_io_job::aiocb_complete
			|| type == disk_io_job::hash_complete
			|| type == disk_io_job::sync_piece;
	}
}

