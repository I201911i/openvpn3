//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2015 OpenVPN Technologies, Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

// Low-level TCP transport object.

#ifndef OPENVPN_TRANSPORT_TCPLINK_H
#define OPENVPN_TRANSPORT_TCPLINK_H

#include <deque>
#include <utility> // for std::move
#include <memory>

#include <asio.hpp>

#include <openvpn/common/size.hpp>
#include <openvpn/common/rc.hpp>
#include <openvpn/common/socktypes.hpp>
#include <openvpn/frame/frame.hpp>
#include <openvpn/log/sessionstats.hpp>
#include <openvpn/transport/pktstream.hpp>
#include <openvpn/transport/mutate.hpp>

#if defined(OPENVPN_DEBUG_TCPLINK) && OPENVPN_DEBUG_TCPLINK >= 1
#define OPENVPN_LOG_TCPLINK_ERROR(x) OPENVPN_LOG(x)
#else
#define OPENVPN_LOG_TCPLINK_ERROR(x)
#endif

#if defined(OPENVPN_DEBUG_TCPLINK) && OPENVPN_DEBUG_TCPLINK >= 3
#define OPENVPN_LOG_TCPLINK_VERBOSE(x) OPENVPN_LOG(x)
#else
#define OPENVPN_LOG_TCPLINK_VERBOSE(x)
#endif

namespace openvpn {
  namespace TCPTransport {

    typedef asio::ip::tcp::endpoint AsioEndpoint;

    struct PacketFrom
    {
      typedef std::unique_ptr<PacketFrom> SPtr;
      BufferAllocated buf;
    };

    template <typename ReadHandler, bool RAW_MODE_ONLY>
    class Link : public RC<thread_unsafe_refcount>
    {
      typedef std::deque<BufferPtr> Queue;

    public:
      typedef RCPtr<Link> Ptr;

      Link(ReadHandler read_handler_arg,
	   asio::ip::tcp::socket& socket_arg,
	   const size_t send_queue_max_size_arg, // 0 to disable
	   const size_t free_list_max_size_arg,
	   const Frame::Context& frame_context_arg,
	   const SessionStats::Ptr& stats_arg)
	: socket(socket_arg),
	  halt(false),
	  read_handler(read_handler_arg),
	  frame_context(frame_context_arg),
	  stats(stats_arg),
	  send_queue_max_size(send_queue_max_size_arg),
	  free_list_max_size(free_list_max_size_arg)
      {
	set_raw_mode(false);
      }

      // In raw mode, data is sent and received without any special encapsulation.
      // In non-raw mode, data is packetized by prepending a 16-bit length word
      // onto each packet.  The OpenVPN protocol runs in non-raw mode, while other
      // TCP protocols such as HTTP or HTTPS would run in raw mode.
      // This method is a no-op if RAW_MODE_ONLY is true.
      void set_raw_mode(const bool mode)
      {
	set_raw_mode_read(mode);
	set_raw_mode_write(mode);
      }

      void set_raw_mode_read(const bool mode)
      {
	if (RAW_MODE_ONLY)
	  raw_mode_read = true;
	else
	  raw_mode_read = mode;
      }

      void set_raw_mode_write(const bool mode)
      {
	if (RAW_MODE_ONLY)
	  raw_mode_write = true;
	else
	  raw_mode_write = mode;
      }

      bool is_raw_mode() const {
	return is_raw_mode_read() && is_raw_mode_write();
      }

      bool is_raw_mode_read() const {
	if (RAW_MODE_ONLY)
	  return true;
	else
	  return raw_mode_read;
      }

      bool is_raw_mode_write() const {
	if (RAW_MODE_ONLY)
	  return true;
	else
	  return raw_mode_write;
      }

      void set_mutate(const TransportMutateStream::Ptr& mutate_arg)
      {
	mutate = mutate_arg;
      }

      bool send_queue_empty() const
      {
	return queue.empty();
      }

      bool send(BufferAllocated& b)
      {
	if (halt)
	  return false;

	if (send_queue_max_size && queue.size() >= send_queue_max_size)
	  {
	    stats->error(Error::TCP_OVERFLOW);
	    read_handler->tcp_error_handler("TCP_OVERFLOW");
	    stop();
	    return false;
	  }

	BufferPtr buf;
	if (!free_list.empty())
	  {
	    buf = free_list.front();
	    free_list.pop_front();
	  }
	else
	  buf.reset(new BufferAllocated());
	buf->swap(b);
	if (!is_raw_mode_write())
	  PacketStream::prepend_size(*buf);
	if (mutate)
	  mutate->pre_send(*buf);
	queue.push_back(std::move(buf));
	if (queue.size() == 1) // send operation not currently active?
	  queue_send();
	return true;
      }

      void inject(const Buffer& src)
      {
	const size_t size = src.size();
	OPENVPN_LOG_TCPLINK_VERBOSE("TCP inject size=" << size);
       	if (size && !RAW_MODE_ONLY)
	  {
	    BufferAllocated buf;
	    frame_context.prepare(buf);
	    buf.write(src.c_data(), size);
	    BufferAllocated pkt;
	    put_pktstream(buf, pkt);
	  }
      }

      void start()
      {
	if (!halt)
	  queue_recv(nullptr);
      }

      void stop()
      {
	halt = true;
      }

      void reset_align_adjust(const size_t align_adjust)
      {
	frame_context.reset_align_adjust(align_adjust + (is_raw_mode() ? 0 : 2));
      }

      ~Link() { stop(); }

    private:
      void queue_send()
      {
	BufferAllocated& buf = *queue.front();
	socket.async_send(buf.const_buffers_1_clamp(),
			  [self=Ptr(this)](const asio::error_code& error, const size_t bytes_sent)
			  {
			    self->handle_send(error, bytes_sent);
			  });
      }

      void handle_send(const asio::error_code& error, const size_t bytes_sent)
      {
	if (!halt)
	  {
	    if (!error)
	      {
		OPENVPN_LOG_TCPLINK_VERBOSE("TCP send raw=" << raw_mode_write << " size=" << bytes_sent);
		stats->inc_stat(SessionStats::BYTES_OUT, bytes_sent);
		stats->inc_stat(SessionStats::PACKETS_OUT, 1);

		BufferPtr buf = queue.front();
		if (bytes_sent == buf->size())
		  {
		    queue.pop_front();
		    if (free_list.size() < free_list_max_size)
		      {
			buf->reset_content();
			free_list.push_back(std::move(buf)); // recycle the buffer for later use
		      }
		  }
		else if (bytes_sent < buf->size())
		  buf->advance(bytes_sent);
		else
		  {
		    stats->error(Error::TCP_OVERFLOW);
		    read_handler->tcp_error_handler("TCP_INTERNAL_ERROR"); // error sent more bytes than we asked for
		    stop();
		    return;
		  }
	      }
	    else
	      {
		OPENVPN_LOG_TCPLINK_ERROR("TCP send error: " << error.message());
		stats->error(Error::NETWORK_SEND_ERROR);
		read_handler->tcp_error_handler("NETWORK_SEND_ERROR");
		stop();
		return;
	      }
	    if (!queue.empty())
	      queue_send();
	    else
	      read_handler->tcp_write_queue_needs_send();
	  }
      }

      void queue_recv(PacketFrom *tcpfrom)
      {
	OPENVPN_LOG_TCPLINK_VERBOSE("TCPLink::queue_recv");
	if (!tcpfrom)
	  tcpfrom = new PacketFrom();
	frame_context.prepare(tcpfrom->buf);

	socket.async_receive(frame_context.mutable_buffers_1_clamp(tcpfrom->buf),
			     [self=Ptr(this), tcpfrom](const asio::error_code& error, const size_t bytes_recvd)
			     {
			       self->handle_recv(tcpfrom, error, bytes_recvd);
			     });
      }

      void handle_recv(PacketFrom *tcpfrom, const asio::error_code& error, const size_t bytes_recvd)
      {
	OPENVPN_LOG_TCPLINK_VERBOSE("TCPLink::handle_recv: " << error.message());
	PacketFrom::SPtr pfp(tcpfrom);
	if (!halt)
	  {
	    if (!error)
	      {
		bool requeue = true;
		OPENVPN_LOG_TCPLINK_VERBOSE("TCP recv raw=" << raw_mode_read << " size=" << bytes_recvd);
		pfp->buf.set_size(bytes_recvd);
		if (!is_raw_mode_read())
		  {
		    try {
		      BufferAllocated pkt;
		      requeue = put_pktstream(pfp->buf, pkt);
		      if (!pfp->buf.allocated() && pkt.allocated()) // recycle pkt allocated buffer
			pfp->buf.move(pkt);
		    }
		    catch (const std::exception& e)
		      {
			OPENVPN_LOG_TCPLINK_ERROR("TCP packet extract error: " << e.what());
			stats->error(Error::TCP_SIZE_ERROR);
			read_handler->tcp_error_handler("TCP_SIZE_ERROR");
			stop();
			return;
		      }
		  }
		else
		  {
		    if (mutate)
		      mutate->post_recv(pfp->buf);
		    requeue = read_handler->tcp_read_handler(pfp->buf);
		  }
		if (!halt && requeue)
		  queue_recv(pfp.release()); // reuse PacketFrom object
	      }
	    else if (error == asio::error::eof)
	      {
		OPENVPN_LOG_TCPLINK_ERROR("TCP recv EOF");
		read_handler->tcp_eof_handler();
	      }
	    else
	      {
		OPENVPN_LOG_TCPLINK_ERROR("TCP recv error: " << error.message());
		stats->error(Error::NETWORK_RECV_ERROR);
		read_handler->tcp_error_handler("NETWORK_RECV_ERROR");
		stop();
	      }
	  }
      }

      bool put_pktstream(BufferAllocated& buf, BufferAllocated& pkt)
      {
	bool requeue = true;
	stats->inc_stat(SessionStats::BYTES_IN, buf.size());
	stats->inc_stat(SessionStats::PACKETS_IN, 1);
	if (mutate)
	  mutate->post_recv(buf);
	while (buf.size())
	  {
	    pktstream.put(buf, frame_context);
	    if (pktstream.ready())
	      {
		pktstream.get(pkt);
		requeue = read_handler->tcp_read_handler(pkt);
	      }
	  }
	return requeue;
      }

      asio::ip::tcp::socket& socket;
      bool halt;
      bool raw_mode_read;
      bool raw_mode_write;
      ReadHandler read_handler;
      Frame::Context frame_context;
      SessionStats::Ptr stats;
      const size_t send_queue_max_size;
      const size_t free_list_max_size;
      Queue queue;      // send queue
      Queue free_list;  // recycled free buffers for send queue
      PacketStream pktstream;
      TransportMutateStream::Ptr mutate;
    };
  }
} // namespace openvpn

#endif
