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

#ifndef TORRENT_INTRUSIVE_PTR_BASE
#define TORRENT_INTRUSIVE_PTR_BASE

#include <boost/detail/atomic_count.hpp>
#include <cassert>
#include "libtorrent/config.hpp"

namespace libtorrent
{
	template<class T>
	struct intrusive_ptr_base
	{
		friend void intrusive_ptr_add_ref(intrusive_ptr_base<T> const* s)
		{
			assert(s->m_refs >= 0);
			assert(s != 0);
			++s->m_refs;
		}

		friend void intrusive_ptr_release(intrusive_ptr_base<T> const* s)
		{
			assert(s->m_refs > 0);
			assert(s != 0);
			if (--s->m_refs == 0)
				delete static_cast<T const*>(s);
		}

		int refcount() const { return m_refs; }

		intrusive_ptr_base(): m_refs(0) {}
	private:
		// reference counter for intrusive_ptr
		mutable boost::detail::atomic_count m_refs;
	};

}

#endif

