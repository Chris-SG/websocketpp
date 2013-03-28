/*
 * Copyright (c) 2011, Peter Thorson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the WebSocket++ Project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL PETER THORSON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */

#ifndef HTTP_PARSER_RESPONSE_IMPL_HPP
#define HTTP_PARSER_RESPONSE_IMPL_HPP

#include <algorithm>
#include <sstream>

#include <websocketpp/http/parser.hpp>

namespace websocketpp {
namespace http {
namespace parser {

inline size_t response::consume(const char *buf, size_t len) {
	if (m_state == DONE) {return 0;}
	
	if (m_state == BODY) {
		return this->process_body(buf,len);
	}
	
	if (m_read + len > MAX_HEADER_SIZE) {
		// exceeded max header size
		throw exception("Maximum header size exceeded.",
				        status_code::REQUEST_HEADER_FIELDS_TOO_LARGE);
	}
	
	// copy new header bytes into buffer
	m_buf->append(buf,len);
	
	// Search for delimiter in buf. If found read until then. If not read all    	
	std::string::iterator begin = m_buf->begin();
	std::string::iterator end = begin;
	
	
	for (;;) {
		// search for delimiter
		end = std::search(
			begin,
			m_buf->end(),
			HEADER_DELIMITER,
			HEADER_DELIMITER + sizeof(HEADER_DELIMITER) - 1
		);
					
		if (end == m_buf->end()) {
			// we are out of bytes. Discard the processed bytes and copy the
			// remaining unprecessed bytes to the beginning of the buffer
			std::copy(begin,end,m_buf->begin());
			m_buf->resize(static_cast<std::string::size_type>(end-begin));
			
			m_read +=len;
					
			return len;
		}
		
		//the range [begin,end) now represents a line to be processed.
		
		if (end-begin == 0) {    			
			// we got a blank line
			if (m_state == RESPONSE_LINE) {
				throw exception("Incomplete Request",status_code::BAD_REQUEST);
			}
			
			// TODO: grab content-length
			std::string length = get_header("Content-Length");
			
			if (length == "") {
				// no content length found, read indefinitely
				m_read = 0;
			} else {
				std::istringstream ss(length);
				
				if ((ss >> m_read).fail()) {
					throw exception("Unable to parse Content-Length header",
							        status_code::BAD_REQUEST);
				}
			}
			
			m_state = BODY;
			
			// calc header bytes processed (starting bytes - bytes left)
			size_t read = (
				len - static_cast<std::string::size_type>(m_buf->end() - end)
				+ sizeof(HEADER_DELIMITER) - 1
			);
			
			// if there were bytes left process them as body bytes
			if (read < len) {
				read += this->process_body(buf+read,(len-read));
			}
			
			// frees memory used temporarily during header parsing
			m_buf.reset();
			
			return read;
		} else {
			if (m_state == RESPONSE_LINE) {
				this->process(begin,end);
				m_state = HEADERS;
			} else {
				this->process_header(begin,end);
			}
		}
		
		begin = end+sizeof(HEADER_DELIMITER) - 1;
	}
}

inline bool response::parse_complete(std::istream& s) {
	// parse a complete header (ie \r\n\r\n MUST be in the input stream)
	std::string response;
	
	// get status line
	std::getline(s, response);
	
	if (response[response.size()-1] == '\r') {
		response.erase(response.end()-1);
		
		std::stringstream   ss(response);
		std::string         str_val;
		int                 int_val;
		char                char_val[256];
		
		ss >> str_val;
		set_version(str_val);
		
		ss >> int_val;
		ss.getline(char_val,256);
		set_status(status_code::value(int_val),std::string(char_val));
	} else {
		return false;
	}
	
	return parse_headers(s);
}

inline std::string response::raw() const {
	// TODO: validation. Make sure all required fields have been set?
	
	std::stringstream raw;
	
	raw << get_version() << " " << m_status_code << " " << m_status_msg;
	raw << "\r\n" << raw_headers() << "\r\n";
	
	raw << m_body;
	
	return raw.str();
}
    
inline void response::set_status(status_code::value code) {
	// TODO: validation?
	m_status_code = code;
	m_status_msg = get_string(code);
}

inline void response::set_status(status_code::value code, const std::string& 
    msg)
{
	// TODO: validation?
	m_status_code = code;
	m_status_msg = msg;
}

inline void response::process(std::string::iterator begin, 
    std::string::iterator end)
{
	std::string::iterator cursor_start = begin;
	std::string::iterator cursor_end = std::find(begin,end,' ');
	
	if (cursor_end == end) {
		throw exception("Invalid response line",status_code::BAD_REQUEST);
	}
	
	set_version(std::string(cursor_start,cursor_end));
	
	cursor_start = cursor_end+1;
	cursor_end = std::find(cursor_start,end,' ');
	
	if (cursor_end == end) {
		throw exception("Invalid request line",status_code::BAD_REQUEST);
	}
	
	int code;
	
	std::istringstream ss(std::string(cursor_start,cursor_end));
				
	if ((ss >> code).fail()) {
		throw exception("Unable to parse response code",status_code::BAD_REQUEST);
	}
	
	set_status(status_code::value(code),std::string(cursor_end+1,end));
}

inline size_t response::process_body(const char *buf, size_t len) {
	// If no content length was set then we read forever and never set m_ready
	if (m_read == 0) {
		m_body.append(buf,len);
		return len;
	}
	
	// Otherwise m_read is the number of bytes left.
	size_t to_read;
	
	if (len >= m_read) {
		// if we have more bytes than we need read, read only the amount needed 
		// then set done state
		to_read = m_read;
		m_state = DONE;
	} else {
		// we need more bytes than are avaliable, read them all
		to_read = len;
	}
	
	m_body.append(buf,to_read);
	m_read -= to_read;
	return to_read;
}

} // namespace parser
} // namespace http
} // namespace websocketpp

#endif // HTTP_PARSER_RESPONSE_IMPL_HPP