/*

Copyright (c) 2003, Magnus Jonsson, Arvid Norberg
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

//The Standard Library defines the two template functions std::min() 
//and std::max() in the <algorithm> header. In general, you should 
//use these template functions for calculating the min and max values 
//of a pair. Unfortunately, Visual C++ does not define these function
// templates. This is because the names min and max clash with 
//the traditional min and max macros defined in <windows.h>. 
//As a workaround, Visual C++ defines two alternative templates with 
//identical functionality called _cpp_min() and _cpp_max(). You can 
//use them instead of std::min() and std::max().To disable the 
//generation of the min and max macros in Visual C++, #define 
//NOMINMAX before #including <windows.h>.

#ifdef _WIN32
    //support boost1.32.0(2004-11-19 18:47)
    //now all libs can be compiled and linked with static module
	#define NOMINMAX
#endif

#include "libtorrent/allocate_resources.hpp"
#include "libtorrent/size_type.hpp"
#include "libtorrent/peer_connection.hpp"
#include "libtorrent/torrent.hpp"

#include <cassert>
#include <algorithm>
#include <boost/limits.hpp>

#if defined(_MSC_VER) && _MSC_VER < 1310
#define for if (false) {} else for
#else
#include <boost/iterator/transform_iterator.hpp>
#endif

namespace libtorrent
{
	int saturated_add(int a, int b)
	{
		assert(a >= 0);
		assert(b >= 0);
		assert(a <= resource_request::inf);
		assert(b <= resource_request::inf);
		assert(resource_request::inf + resource_request::inf < 0);

		int sum = a + b;
		if (sum < 0)
			sum = resource_request::inf;

		assert(sum >= a && sum >= b);
		return sum;
	}

	namespace
	{
		// give num_resources to r,
		// return how how many were actually accepted.
		int give(resource_request& r, int num_resources)
		{
			assert(num_resources >= 0);
			assert(r.given <= r.max);
			
			int accepted = std::min(num_resources, r.max - r.given);
			assert(accepted >= 0);

			r.given += accepted;
			assert(r.given <= r.max);

			return accepted;
		}

#ifndef NDEBUG

		template<class It, class T>
		class allocate_resources_contract_check
		{
			int m_resources;
			It m_start;
			It m_end;
			resource_request T::* m_res;

		public:
			allocate_resources_contract_check(
				int resources
				, It start
				, It end
				, resource_request T::* res)
				: m_resources(resources)
				, m_start(start)
				, m_end(end)
				, m_res(res)
			{
				assert(m_resources >= 0);
				for (It i = m_start, end(m_end); i != end; ++i)
				{
					assert(((*i).*m_res).max >= 0);
					assert(((*i).*m_res).given >= 0);
				}
			}

			~allocate_resources_contract_check()
			{
				int sum_given = 0;
				int sum_max = 0;
				int sum_min = 0;
				for (It i = m_start, end(m_end); i != end; ++i)
				{
					assert(((*i).*m_res).max >= 0);
					assert(((*i).*m_res).min >= 0);
					assert(((*i).*m_res).max >= ((*i).*m_res).min);
					assert(((*i).*m_res).given >= 0);
					assert(((*i).*m_res).given <= ((*i).*m_res).max);

					sum_given = saturated_add(sum_given, ((*i).*m_res).given);
					sum_max = saturated_add(sum_max, ((*i).*m_res).max);
					sum_min = saturated_add(sum_min, ((*i).*m_res).min);
				}
				assert(sum_given == std::min(std::max(m_resources, sum_min), sum_max));
			}
		};

#endif

		template<class It, class T>
		void allocate_resources_impl(
			int resources
			, It start
			, It end
			, resource_request T::* res)
		{
	#ifndef NDEBUG
			allocate_resources_contract_check<It, T> contract_check(
				resources
				, start
				, end
				, res);
	#endif

			if (resources == resource_request::inf)
			{
				// No competition for resources.
				// Just give everyone what they want.
				for (It i = start; i != end; ++i)
				{
					((*i).*res).given = ((*i).*res).max;
				}
				return;
			}

			// Resources are scarce

			int sum_max = 0;
			int sum_min = 0;
			for (It i = start; i != end; ++i)
			{
				sum_max = saturated_add(sum_max, ((*i).*res).max);
				assert(((*i).*res).min < resource_request::inf);
				assert(((*i).*res).min >= 0);
				assert(((*i).*res).min <= ((*i).*res).max);
				sum_min += ((*i).*res).min;
				((*i).*res).given = ((*i).*res).min;
			}

			if (resources == 0 || sum_max == 0)
				return;

			resources = std::max(resources, sum_min);
			int resources_to_distribute = std::min(resources, sum_max) - sum_min;
			assert(resources_to_distribute >= 0);

			while (resources_to_distribute > 0)
			{
				size_type total_used = 0;
				size_type max_used = 0;
				for (It i = start; i != end; ++i)
				{
					resource_request& r = (*i).*res;
					if(r.given == r.max) continue;

					assert(r.given < r.max);

					max_used = std::max(max_used, (size_type)r.used + 1);
					total_used += (size_type)r.used + 1;
				}

				size_type kNumer = resources_to_distribute;
				size_type kDenom = total_used;

				if (kNumer * max_used <= kDenom)
				{
					kNumer = 1;
					kDenom = max_used;
				}

				for (It i = start; i != end && resources_to_distribute > 0; ++i)
				{
					resource_request& r = (*i).*res;
					if (r.given == r.max) continue;

					assert(r.given < r.max);

					size_type used = (size_type)r.used + 1;
					if (used < 1) used = 1;
					size_type to_give = used * kNumer / kDenom;
					if(to_give > resource_request::inf)
						to_give = resource_request::inf;
					assert(to_give >= 0);
					resources_to_distribute -= give(r, (int)to_give);
					assert(resources_to_distribute >= 0);
				}

				assert(resources_to_distribute >= 0);
			}
		}

		peer_connection& pick_peer(
			std::pair<boost::shared_ptr<stream_socket>
			, boost::intrusive_ptr<peer_connection> > const& p)
		{
			return *p.second;
		}

		peer_connection& pick_peer2(
			std::pair<tcp::endpoint, peer_connection*> const& p)
		{
			return *p.second;
		}

		torrent& deref(std::pair<sha1_hash, boost::shared_ptr<torrent> > const& p)
		{
			return *p.second;
		}


	} // namespace anonymous

/*
	void allocate_resources(
		int resources
		, std::map<boost::shared_ptr<socket>, boost::intrusive_ptr<peer_connection> >& c
		, resource_request peer_connection::* res)
	{
		typedef std::map<boost::shared_ptr<socket>, boost::intrusive_ptr<peer_connection> >::iterator orig_iter;
		typedef std::pair<boost::shared_ptr<socket>, boost::intrusive_ptr<peer_connection> > in_param;
		typedef boost::transform_iterator<peer_connection& (*)(in_param const&), orig_iter> new_iter;

		allocate_resources_impl(
			resources
			, new_iter(c.begin(), &pick_peer)
			, new_iter(c.end(), &pick_peer)
			, res);
	}
*/
#if defined(_MSC_VER) && _MSC_VER < 1310

	namespace detail
	{
		struct iterator_wrapper
		{
			typedef std::map<sha1_hash, boost::shared_ptr<torrent> >::iterator orig_iter;

			orig_iter iter;

			iterator_wrapper(orig_iter i): iter(i) {}
			void operator++() { ++iter; }
			torrent& operator*() { return *(iter->second); }
			bool operator==(const iterator_wrapper& i) const
			{ return iter == i.iter; }
			bool operator!=(const iterator_wrapper& i) const
			{ return iter != i.iter; }
		};

		struct iterator_wrapper2
		{
			typedef std::map<tcp::endpoint, peer_connection*>::iterator orig_iter;

			orig_iter iter;

			iterator_wrapper2(orig_iter i): iter(i) {}
			void operator++() { ++iter; }
			peer_connection& operator*() { return *(iter->second); }
			bool operator==(const iterator_wrapper2& i) const
			{ return iter == i.iter; }
			bool operator!=(const iterator_wrapper2& i) const
			{ return iter != i.iter; }
		};

	}

	void allocate_resources(
		int resources
		, std::map<sha1_hash, boost::shared_ptr<torrent> >& c
		, resource_request torrent::* res)
	{
		allocate_resources_impl(
			resources
			, detail::iterator_wrapper(c.begin())
			, detail::iterator_wrapper(c.end())
			, res);
	}

	void allocate_resources(
		int resources
		, std::map<tcp::endpoint, peer_connection*>& c
		, resource_request peer_connection::* res)
	{
		allocate_resources_impl(
			resources
			, detail::iterator_wrapper2(c.begin())
			, detail::iterator_wrapper2(c.end())
			, res);
	}

#else

	void allocate_resources(
		int resources
		, std::map<sha1_hash, boost::shared_ptr<torrent> >& c
		, resource_request torrent::* res)
	{
		typedef std::map<sha1_hash, boost::shared_ptr<torrent> >::iterator orig_iter;
		typedef std::pair<sha1_hash, boost::shared_ptr<torrent> > in_param;
		typedef boost::transform_iterator<torrent& (*)(in_param const&), orig_iter> new_iter;

		allocate_resources_impl(
			resources
			, new_iter(c.begin(), &deref)
			, new_iter(c.end(), &deref)
			, res);
	}

	void allocate_resources(
		int resources
		, std::map<tcp::endpoint, peer_connection*>& c
		, resource_request peer_connection::* res)
	{
		typedef std::map<tcp::endpoint, peer_connection*>::iterator orig_iter;
		typedef std::pair<tcp::endpoint, peer_connection*> in_param;
		typedef boost::transform_iterator<peer_connection& (*)(in_param const&), orig_iter> new_iter;

		allocate_resources_impl(
			resources
			, new_iter(c.begin(), &pick_peer2)
			, new_iter(c.end(), &pick_peer2)
			, res);
	}
#endif

} // namespace libtorrent
