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

#include <vector>
#include <iostream>
#include <cctype>
#include <iomanip>
#include <sstream>

#include "zlib.h"

#include "libtorrent/tracker_manager.hpp"
#include "libtorrent/http_tracker_connection.hpp"
#include "libtorrent/udp_tracker_connection.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/torrent.hpp"

using namespace libtorrent;

namespace
{
	enum
	{
		minimum_tracker_response_length = 3,
		http_buffer_size = 2048
	};


	enum
	{
		FTEXT = 0x01,
		FHCRC = 0x02,
		FEXTRA = 0x04,
		FNAME = 0x08,
		FCOMMENT = 0x10,
		FRESERVED = 0xe0,

		GZIP_MAGIC0 = 0x1f,
		GZIP_MAGIC1 = 0x8b
	};

}

namespace libtorrent
{

	// returns -1 if gzip header is invalid or the header size in bytes
	int gzip_header(const char* buf, int size)
	{
		assert(buf != 0);
		assert(size > 0);

		const unsigned char* buffer = reinterpret_cast<const unsigned char*>(buf);
		const int total_size = size;

		// The zip header cannot be shorter than 10 bytes
		if (size < 10) return -1;

		// check the magic header of gzip
		if ((buffer[0] != GZIP_MAGIC0) || (buffer[1] != GZIP_MAGIC1)) return -1;

		int method = buffer[2];
		int flags = buffer[3];

		// check for reserved flag and make sure it's compressed with the correct metod
		if (method != Z_DEFLATED || (flags & FRESERVED) != 0) return -1;

		// skip time, xflags, OS code
		size -= 10;
		buffer += 10;

		if (flags & FEXTRA)
		{
			int extra_len;

			if (size < 2) return -1;

			extra_len = (buffer[1] << 8) | buffer[0];

			if (size < (extra_len+2)) return -1;
			size -= (extra_len + 2);
			buffer += (extra_len + 2);
		}

		if (flags & FNAME)
		{
			while (size && *buffer)
			{
				--size;
				++buffer;
			}
			if (!size || *buffer) return -1;

			--size;
			++buffer;
		}

		if (flags & FCOMMENT)
		{
			while (size && *buffer)
			{
				--size;
				++buffer;
			}
			if (!size || *buffer) return -1;

			--size;
			++buffer;
		}

		if (flags & FHCRC)
		{
			if (size < 2) return -1;

			size -= 2;
			buffer += 2;
		}

		return total_size - size;
	}

	bool inflate_gzip(
		std::vector<char>& buffer
		, request_callback* requester
		, int maximum_tracker_response_length)
	{
		assert(maximum_tracker_response_length > 0);

		int header_len = gzip_header(&buffer[0], (int)buffer.size());
		if (header_len < 0)
		{
			requester->tracker_request_error(200, "invalid gzip header in tracker response");
			return true;
		}

		// start off wth one kilobyte and grow
		// if needed
		std::vector<char> inflate_buffer(1024);

		// initialize the zlib-stream
		z_stream str;

		// subtract 8 from the end of the buffer since that's CRC32 and input size
		// and those belong to the gzip file
		str.avail_in = (int)buffer.size() - header_len - 8;
		str.next_in = reinterpret_cast<Bytef*>(&buffer[header_len]);
		str.next_out = reinterpret_cast<Bytef*>(&inflate_buffer[0]);
		str.avail_out = (int)inflate_buffer.size();
		str.zalloc = Z_NULL;
		str.zfree = Z_NULL;
		str.opaque = 0;
		// -15 is really important. It will make inflate() not look for a zlib header
		// and just deflate the buffer
		if (inflateInit2(&str, -15) != Z_OK)
		{
			requester->tracker_request_error(200, "gzip out of memory");
			return true;
		}

		// inflate and grow inflate_buffer as needed
		int ret = inflate(&str, Z_SYNC_FLUSH);
		while (ret == Z_OK)
		{
			if (str.avail_out == 0)
			{
				if (inflate_buffer.size() >= (unsigned)maximum_tracker_response_length)
				{
					inflateEnd(&str);
					requester->tracker_request_error(200, "tracker response too large");
					return true;
				}
				int new_size = (int)inflate_buffer.size() * 2;
				if (new_size > maximum_tracker_response_length) new_size = maximum_tracker_response_length;
				int old_size = (int)inflate_buffer.size();

				inflate_buffer.resize(new_size);
				str.next_out = reinterpret_cast<Bytef*>(&inflate_buffer[old_size]);
				str.avail_out = new_size - old_size;
			}

			ret = inflate(&str, Z_SYNC_FLUSH);
		}

		inflate_buffer.resize(inflate_buffer.size() - str.avail_out);
		inflateEnd(&str);

		if (ret != Z_STREAM_END)
		{
			requester->tracker_request_error(200, "gzip error");
			return true;
		}

		// commit the resulting buffer
		std::swap(buffer, inflate_buffer);
		return false;
	}

	std::string base64encode(const std::string& s)
	{
		static const char base64_table[] =
		{
			'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
			'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
			'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
			'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
			'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
			'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
			'w', 'x', 'y', 'z', '0', '1', '2', '3',
			'4', '5', '6', '7', '8', '9', '+', '/'
		};

		unsigned char inbuf[3];
		unsigned char outbuf[4];
	
		std::string ret;
		for (std::string::const_iterator i = s.begin(); i != s.end();)
		{
			// available input is 1,2 or 3 bytes
			// since we read 3 bytes at a time at most
			int available_input = std::min(3, (int)std::distance(i, s.end()));

			// clear input buffer
			std::fill(inbuf, inbuf+3, 0);

			// read a chunk of input into inbuf
			for (int j = 0; j < available_input; ++j)
			{
				inbuf[j] = *i;
				++i;
			}

			// encode inbuf to outbuf
			outbuf[0] = (inbuf[0] & 0xfc) >> 2;
			outbuf[1] = ((inbuf[0] & 0x03) << 4) | ((inbuf [1] & 0xf0) >> 4);
			outbuf[2] = ((inbuf[1] & 0x0f) << 2) | ((inbuf [2] & 0xc0) >> 6);
			outbuf[3] = inbuf[2] & 0x3f;

			// write output
			for (int j = 0; j < available_input+1; ++j)
			{
				ret += base64_table[outbuf[j]];
			}

			// write pad
			for (int j = 0; j < 3 - available_input; ++j)
			{
				ret += '=';
			}
		}
		return ret;
	}


	void tracker_manager::tick()
	{
		std::vector<boost::shared_ptr<tracker_connection> >::iterator i;
		for (i = m_connections.begin(); i != m_connections.end(); ++i)
		{
			boost::shared_ptr<tracker_connection>& c = *i;
			try
			{
				if (!c->tick()) continue;
			}
			catch (const std::exception& e)
			{
				if (c->requester())
					c->requester()->tracker_request_error(-1, e.what());
			}
			if (c->requester()) c->requester()->m_manager = 0;
			m_connections.erase(i);
			--i; // compensate for the remove
		}
	}

	void tracker_manager::queue_request(
		tracker_request const& req
		, request_callback* c
		, std::string const& password)
	{
		try
		{
			std::string hostname; // hostname only
			std::string protocol; // should be http
			int port = 80;

			// PARSE URL
			std::string::const_iterator start = req.url.begin();
			std::string::const_iterator end
				= std::find(req.url.begin(), req.url.end(), ':');
			protocol = std::string(start, end);

			if (end == req.url.end()) throw std::runtime_error("invalid url");
			++end;
			if (end == req.url.end()) throw std::runtime_error("invalid url");
			if (*end != '/') throw std::runtime_error("invalid url");
			++end;
			if (end == req.url.end()) throw std::runtime_error("invalid url");
			if (*end != '/') throw std::runtime_error("invalid url");
			++end;
			start = end;

			end = std::find(start, req.url.end(), '/');
			std::string::const_iterator port_pos
				= std::find(start, req.url.end(), ':');

			if (port_pos < end)
			{
				hostname.assign(start, port_pos);
				++port_pos;
				try
				{
					port = boost::lexical_cast<int>(std::string(port_pos, end));
				}
				catch(boost::bad_lexical_cast&)
				{
					throw std::runtime_error("invalid url");
				}
			}
			else
			{
				hostname.assign(start, end);
			}

			start = end;
			std::string request_string = std::string(start, req.url.end());

			boost::shared_ptr<tracker_connection> con;

			if (protocol == "http")
			{
				con.reset(new http_tracker_connection(
					req
					, hostname
					, port
					, request_string
					, c
					, m_settings
					, password));
			}
			else if (protocol == "udp")
			{
				con.reset(new udp_tracker_connection(
					req
					, hostname
					, port
					, c
					, m_settings));
			}
			else
			{
				throw std::runtime_error("unkown protocol in tracker url");
			}

			m_connections.push_back(con);

			if (con->requester()) con->requester()->m_manager = this;
		}
		catch (std::exception& e)
		{
			if (c) c->tracker_request_error(-1, e.what());
		}
	}

	void tracker_manager::abort_request(request_callback* c)
	{
		assert(c != 0);
		std::vector<boost::shared_ptr<tracker_connection> >::iterator i;
		for (i = m_connections.begin(); i != m_connections.end(); ++i)
		{
			if ((*i)->requester() == c)
			{
				m_connections.erase(i);
				break;
			}
		}
	}

	void tracker_manager::abort_all_requests()
	{
		// removes all connections from m_connections
		// except those with a requester == 0 (since those are
		// 'event=stopped'-requests)

		std::vector<boost::shared_ptr<tracker_connection> > keep_connections;

		for (std::vector<boost::shared_ptr<tracker_connection> >::const_iterator i =
				m_connections.begin();
			i != m_connections.end();
			++i)
		{
			if ((*i)->requester() == 0) keep_connections.push_back(*i);
		}

		std::swap(m_connections, keep_connections);
	}

	bool tracker_manager::send_finished() const
	{
		for (std::vector<boost::shared_ptr<tracker_connection> >::const_iterator i =
				m_connections.begin();
			i != m_connections.end();
			++i)
		{
			if (!(*i)->send_finished()) return false;
		}
		return true;
	}

}
