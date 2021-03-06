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

#include <boost/bind.hpp>
#include "libtorrent/config.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/connection_queue.hpp"
#include "libtorrent/io_service.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/connection_interface.hpp"

#if defined TORRENT_ASIO_DEBUGGING
#include "libtorrent/debug.hpp"
#endif

namespace libtorrent
{

	connection_queue::connection_queue(io_service& ios): m_next_ticket(0)
		, m_half_open_limit(0)
		, m_num_timers(0)
		, m_timer(ios)
#ifdef TORRENT_DEBUG
		, m_in_timeout_function(false)
#endif
	{
#ifdef TORRENT_CONNECTION_LOGGING
		m_log.open("connection_queue.log");
#endif
	}

	int connection_queue::free_slots() const
	{
		TORRENT_ASSERT(is_single_thread());
		return m_half_open_limit == 0 ? (std::numeric_limits<int>::max)()
			: m_half_open_limit - m_queue.size();
	}

	void connection_queue::enqueue(connection_interface* conn
		, time_duration timeout, int priority)
	{
		TORRENT_ASSERT(is_single_thread());

		INVARIANT_CHECK;

		TORRENT_ASSERT(priority >= 0);
		TORRENT_ASSERT(priority < 3);

		queue_entry e;
		e.priority = priority;
		e.conn = conn;
		e.timeout = timeout;

		switch (priority)
		{
			case 0:
				m_queue.push_back(e);
				break;
			case 1:
			case 2:
				m_queue.insert(m_queue.begin(), e);
				break;
			default: return;
		}

		if (num_connecting() < m_half_open_limit
			|| m_half_open_limit == 0)
			m_timer.get_io_service().post(boost::bind(
				&connection_queue::on_try_connect, this));

	}

	void connection_queue::cancel(connection_interface* conn)
	{
		std::vector<queue_entry>::iterator i = std::find_if(
			m_queue.begin(), m_queue.end(), boost::bind(&queue_entry::conn, _1) == conn);

		if (i == m_queue.end())
		{
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		// assert the connection is not in the connecting list
			for (std::map<int, connect_entry>::iterator i = m_connecting.begin();
				i != m_connecting.end(); ++i)
			{
				TORRENT_ASSERT(i->second.conn != conn);
			}
#endif
			return;
		}

		m_queue.erase(i);
	}

	void connection_queue::done(int ticket)
	{
		TORRENT_ASSERT(is_single_thread());

		INVARIANT_CHECK;

		std::map<int, connect_entry>::iterator i = m_connecting.find(ticket);
		// this might not be here in case on_timeout calls remove
		if (i == m_connecting.end()) return;

		m_connecting.erase(i);

		if (num_connecting() < m_half_open_limit
			|| m_half_open_limit == 0)
			m_timer.get_io_service().post(boost::bind(
				&connection_queue::on_try_connect, this));
	}

	void connection_queue::close()
	{
		TORRENT_ASSERT(is_single_thread());

		error_code ec;
		if (num_connecting() == 0) m_timer.cancel(ec);

		std::vector<queue_entry> tmp;
		tmp.swap(m_queue);

		while (!tmp.empty())
		{
			queue_entry& e = tmp.front();
			if (e.priority > 1)
			{
				m_queue.push_back(e);
				tmp.erase(tmp.begin());
				continue;
			}
			TORRENT_TRY {
				e.conn->on_allow_connect(-1);
			} TORRENT_CATCH(std::exception&) {}
			tmp.erase(tmp.begin());
		}

		std::vector<std::pair<int, connect_entry> > tmp2;

		for (std::map<int, connect_entry>::iterator i = m_connecting.begin();
			i != m_connecting.end();)
		{
			if (i->second.priority <= 1)
			{
				tmp2.push_back(*i);
				m_connecting.erase(i++);
			}
			else
			{
				++i;
			}
		}

		while (!tmp2.empty())
		{
			std::pair<int, connect_entry>& e = tmp2.back();
			TORRENT_TRY {
				e.second.conn->on_connect_timeout();
			} TORRENT_CATCH(std::exception&) {}
			tmp2.erase(tmp2.rbegin().base());
		}
	}

	void connection_queue::limit(int limit)
	{
		TORRENT_ASSERT(limit >= 0);
		m_half_open_limit = limit;
	}

	int connection_queue::limit() const
	{ return m_half_open_limit; }

#ifdef TORRENT_DEBUG

	void connection_queue::check_invariant() const
	{
	}

#endif

	void connection_queue::try_connect()
	{
		TORRENT_ASSERT(is_single_thread());
		INVARIANT_CHECK;

#ifdef TORRENT_CONNECTION_LOGGING
		m_log << log_time() << " " << free_slots() << std::endl;
#endif

		if (num_connecting() >= m_half_open_limit
			&& m_half_open_limit > 0) return;
	
		if (m_queue.empty() && m_connecting.empty())
		{
			error_code ec;
			m_timer.cancel(ec);
			return;
		}

		// all entries are connecting, no need to look for new ones
		if (m_queue.empty()) return;

		while (!m_queue.empty())
		{
			if (num_connecting() >= m_half_open_limit
				&& m_half_open_limit > 0) break;

			queue_entry e = m_queue.front();
			m_queue.erase(m_queue.begin());

			ptime expire = time_now_hires() + e.timeout;
			if (num_connecting() == 0)
			{
#if defined TORRENT_ASIO_DEBUGGING
				add_outstanding_async("connection_queue::on_timeout");
#endif
				error_code ec;
				m_timer.expires_at(expire, ec);
				m_timer.async_wait(boost::bind(&connection_queue::on_timeout, this, _1));
				++m_num_timers;
			}
			connect_entry ce;
			ce.conn = e.conn;
			ce.priority = e.priority;
			ce.expires = time_now_hires() + e.timeout;

			int ticket = ++m_next_ticket;
			m_connecting.insert(std::make_pair(ticket, ce));

			TORRENT_TRY {
				ce.conn->on_allow_connect(ticket);
			} TORRENT_CATCH(std::exception&) {}

#ifdef TORRENT_CONNECTION_LOGGING
			m_log << log_time() << " " << free_slots() << std::endl;
#endif
		}
	}

#ifdef TORRENT_DEBUG
	struct function_guard
	{
		function_guard(bool& v): val(v) { TORRENT_ASSERT(!val); val = true; }
		~function_guard() { val = false; }

		bool& val;
	};
#endif
	
	void connection_queue::on_timeout(error_code const& e)
	{
#if defined TORRENT_ASIO_DEBUGGING
		complete_async("connection_queue::on_timeout");
#endif
		--m_num_timers;

		INVARIANT_CHECK;
#ifdef TORRENT_DEBUG
		function_guard guard_(m_in_timeout_function);
#endif

		TORRENT_ASSERT(!e || e == error::operation_aborted);

		// if there was an error, it's most likely operation aborted,
		// we should just quit. However, in case there are still connections
		// in connecting state, and there are no other timer invocations
		// we need to stick around still.
		if (e && (num_connecting() == 0 || m_num_timers > 0)) return;

		ptime next_expire = max_time();
		ptime now = time_now_hires() + milliseconds(100);
		std::vector<connect_entry> timed_out;
		for (std::map<int, connect_entry>::iterator i = m_connecting.begin();
			!m_connecting.empty() && i != m_connecting.end(); ++i)
		{
			if (i->second.expires < now)
			{
				timed_out.push_back(i->second);
				continue;
			}
			if (i->second.expires < next_expire)
				next_expire = i->second.expires;
		}

		for (std::vector<connect_entry>::iterator i = timed_out.begin()
			, end(timed_out.end()); i != end; ++i)
		{
			TORRENT_TRY {
				i->conn->on_connect_timeout();
			} TORRENT_CATCH(std::exception&) {}
		}
		
		if (next_expire < max_time())
		{
#if defined TORRENT_ASIO_DEBUGGING
			add_outstanding_async("connection_queue::on_timeout");
#endif
			error_code ec;
			m_timer.expires_at(next_expire, ec);
			m_timer.async_wait(boost::bind(&connection_queue::on_timeout, this, _1));
			++m_num_timers;
		}
		try_connect();
	}

	void connection_queue::on_try_connect()
	{
		try_connect();
	}
}

