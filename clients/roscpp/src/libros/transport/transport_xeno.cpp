/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "ros/transport/transport_xeno.h"
#include "ros/poll_set.h"
#include "ros/file_log.h"

#include <ros/assert.h>
#include <boost/bind.hpp>

#include <fcntl.h>
#if defined(__APPLE__)
  // For readv() and writev()
  #include <sys/types.h>
  #include <sys/uio.h>
  #include <unistd.h>
#endif

namespace ros
{

TransportXeno::TransportXeno(PollSet* poll_set, int flags, int max_datagram_size)
: req_sock_(-1)
, rep_sock_(-1)
, req_closed_(false)
, rep_closed_(false)
, expecting_read_(false)
, expecting_write_(false)
, is_server_(false)
, server_port_(-1)
, poll_set_(poll_set)
, flags_(flags)
, connection_id_(0)
, current_message_id_(0)
, total_blocks_(0)
, last_block_(0)
, max_datagram_size_(max_datagram_size)
, data_filled_(0)
, reorder_buffer_(0)
, reorder_bytes_(0)
{
  // This may eventually be machine dependent
  if (max_datagram_size_ == 0)
    max_datagram_size_ = 1500;
  reorder_buffer_ = new uint8_t[max_datagram_size_];
  reorder_start_ = reorder_buffer_;
  data_buffer_ = new uint8_t[max_datagram_size_];
  data_start_ = data_buffer_;
}

TransportXeno::~TransportXeno()
{
  ROS_ASSERT_MSG(req_sock_ == ROS_INVALID_SOCKET, "TransportXeno request socket [%d] was never closed", req_sock_);
  ROS_ASSERT_MSG(rep_sock_ == ROS_INVALID_SOCKET, "TransportXeno reply socket [%d] was never closed", rep_sock_);
  delete [] reorder_buffer_;
  delete [] data_buffer_;
}

bool TransportXeno::setReqSocket(int sock)
{
  req_sock_ = sock;
  return initializeReqSocket();
}

bool TransportXeno::setRepSocket(int sock)
{
  rep_sock_ = sock;
  return initializeRepSocket();
}

void TransportXeno::reqSocketUpdate(int events)
{
  {
    boost::mutex::scoped_lock lock(req_close_mutex_);

    if (req_closed_)
    {
      return;
    }
  }

  if((events & POLLERR) ||
     (events & POLLHUP) ||
     (events & POLLNVAL))
  {
    ROSCPP_LOG_DEBUG("Socket (request) %d closed with (ERR|HUP|NVAL) events %d", req_sock_, events);
    close();
  }
  else
  {

    // This is a write-only socket, we do not expect any reads
//    if ((events & POLLIN) && expecting_read_)
//    {
//      if (read_cb_)
//      {
//        read_cb_(shared_from_this());
//      }
//    }

    if ((events & POLLOUT) && expecting_write_)
    {
      if (write_cb_)
      {
        write_cb_(shared_from_this());
      }
    }
  }

}

void TransportXeno::repSocketUpdate(int events)
{
  {
    boost::mutex::scoped_lock lock(rep_close_mutex_);

    if (rep_closed_)
    {
      return;
    }
  }

  if((events & POLLERR) ||
     (events & POLLHUP) ||
     (events & POLLNVAL))
  {
    ROSCPP_LOG_DEBUG("Socket (reply) %d closed with (ERR|HUP|NVAL) events %d", rep_sock_, events);
    close();
  }
  else
  {
    if ((events & POLLIN) && expecting_read_)
    {
      if (read_cb_)
      {
        read_cb_(shared_from_this());
      }
    }

    // This is a read-only socket, we do not expect any writes
//    if ((events & POLLOUT) && expecting_write_)
//    {
//      if (write_cb_)
//      {
//        write_cb_(shared_from_this());
//      }
//    }
  }

}

std::string TransportXeno::getTransportInfo()
{
  return "XENOROS connection to [" + cached_remote_host_ + "]";
}

bool TransportXeno::connect(int port, int connection_id)
{
  std::string label_req = label + "-req";
  std::string label_rep = label + "-rep";

  req_sock_ = socket(AF_RTIPC, SOCK_DGRAM, IPCPROTO_IDDP);
  if (req_sock_ == ROS_INVALID_SOCKET)
  {
    ROS_ERROR("socket() failed with error [%s]",  last_socket_error_string());
    return false;
  }
  rep_sock_ = socket(AF_RTIPC, SOCK_DGRAM, IPCPROTO_IDDP);
  if (rep_sock_ == ROS_INVALID_SOCKET)
  {
    ROS_ERROR("socket() failed with error [%s]",  last_socket_error_string());
    return false;
  }

  connection_id_ = connection_id;

  sockaddr_ipc sipc_req;
  sockaddr_ipc sipc_rep;

  sipc_req.sipc_family = AF_RTIPC;
  sipc_req.sipc_port = -1; // pick next free

  sipc_rep.sipc_family = AF_RTIPC;
  sipc_rep.sipc_port = -1; // pick next free

  if (!label.empty())
  {
    int ret;
    if (label_req.length() > XNOBJECT_NAME_LEN)
    {
      ROS_ERROR("Host name %s is too long for Xenomai socket. Maximum length is %d bytes, but %d bytes provided", label_req.c_str(), XNOBJECT_NAME_LEN, label_req.length());
      return false;
    }

    strcpy(plabel_req_.label, label_req.c_str());
    ret = setsockopt(req_sock_, SOL_IDDP, IDDP_LABEL, &plabel_req_, sizeof(plabel_req_));
    if (ret)
    {
      ROS_ERROR("Unable to set Xenomai socket label: %s", label_req.c_str());
      return false;
    }
    strcpy(plabel_rep_.label, label_rep.c_str());
    ret = setsockopt(rep_sock_, SOL_IDDP, IDDP_LABEL, &plabel_rep_, sizeof(plabel_rep_));
    if (ret)
    {
      ROS_ERROR("Unable to set Xenomai socket label: %s", label_rep.c_str());
      return false;
    }
  }

  if (::connect(req_sock_, (sockaddr *)&sipc_req, sizeof(sipc_req)))
  {
    ROSCPP_LOG_DEBUG("Connect to xenoros host [%s] failed with error [%s]", label_req.c_str(),  last_socket_error_string());
    close();

    return false;
  }
  if (!initializeReqSocket())
  {
    return false;
  }
  ROSCPP_LOG_DEBUG("Connect succeeded to [%s] on socket [%d]", label_req.c_str(), req_sock_);

  if (::bind(rep_sock_, (sockaddr *)&sipc_rep, sizeof(sipc_rep)))
  {
    ROSCPP_LOG_DEBUG("Bind xenoros [%s] failed with error [%s]", label_rep.c_str(),  last_socket_error_string());
    close();

    return false;
  }
  if (!initializeRepSocket())
  {
    return false;
  }
  ROSCPP_LOG_DEBUG("Bind succeeded to [%s] on socket [%d]", label_rep.c_str(), rep_sock_);

  return true;
}

bool TransportXeno::createIncoming(int port, bool is_server)
{
  is_server_ = is_server;

  std::string label_req = label + "-req";
  std::string label_rep = label + "-rep";

  req_sock_ = socket(AF_RTIPC, SOCK_DGRAM, IPCPROTO_IDDP);
  if (req_sock_ == ROS_INVALID_SOCKET)
  {
    ROS_ERROR("socket() failed with error [%s]",  last_socket_error_string());
    return false;
  }
  rep_sock_ = socket(AF_RTIPC, SOCK_DGRAM, IPCPROTO_IDDP);
  if (rep_sock_ == ROS_INVALID_SOCKET)
  {
    ROS_ERROR("socket() failed with error [%s]",  last_socket_error_string());
    return false;
  }

  sockaddr_ipc sipc_req;
  sockaddr_ipc sipc_rep;

  sipc_req.sipc_family = AF_RTIPC;
  sipc_req.sipc_port = -1; // pick next free

  sipc_rep.sipc_family = AF_RTIPC;
  sipc_rep.sipc_port = -1; // pick next free

  if (!label.empty())
  {
    int ret;
    if (label_req.length() > XNOBJECT_NAME_LEN)
    {
      ROS_ERROR("Host name %s is too long for Xenomai socket. Maximum length is %d bytes, but %d bytes provided", label_req.c_str(), XNOBJECT_NAME_LEN, label_req.length());
      return false;
    }

    strcpy(plabel_req_.label, label_req.c_str());
    ret = setsockopt(req_sock_, SOL_IDDP, IDDP_LABEL, &plabel_req_, sizeof(plabel_req_));
    if (ret)
    {
      ROS_ERROR("Unable to set Xenomai socket label: %s", label_req.c_str());
      return false;
    }
    strcpy(plabel_rep_.label, label_rep.c_str());
    ret = setsockopt(rep_sock_, SOL_IDDP, IDDP_LABEL, &plabel_rep_, sizeof(plabel_rep_));
    if (ret)
    {
      ROS_ERROR("Unable to set Xenomai socket label: %s", label_rep.c_str());
      return false;
    }
  }

  // This is server, so bind request socket first
  if (::bind(req_sock_, (sockaddr *)&sipc_req, sizeof(sipc_req)))
  {
    ROSCPP_LOG_DEBUG("Bind xenoros [%s] failed with error [%s]", label_req.c_str(),  last_socket_error_string());
    close();

    return false;
  }
  if (!initializeReqSocket())
  {
    return false;
  }
  ROSCPP_LOG_DEBUG("Bind succeeded to [%s] on socket [%d]", label_req.c_str(), req_sock_);

  // Now connect to client's reply socket
  if (::connect(rep_sock_, (sockaddr *)&sipc_rep, sizeof(sipc_rep)))
  {
    ROSCPP_LOG_DEBUG("Connect to xenoros host [%s] failed with error [%s]", label_rep.c_str(),  last_socket_error_string());
    close();

    return false;
  }
  if (!initializeRepSocket())
  {
    return false;
  }
  ROSCPP_LOG_DEBUG("Connect succeeded to [%s] on socket [%d]", label_rep.c_str(), rep_sock_);

  enableRead();

  return true;
}

bool TransportXeno::initializeReqSocket()
{
  ROS_ASSERT(req_sock_ != ROS_INVALID_SOCKET);

  if (!(flags_ & SYNCHRONOUS))
  {
          int result = set_non_blocking(req_sock_);
          if ( result != 0 ) {
              ROS_ERROR("setting request socket [%d] as non_blocking failed with error [%d]", req_sock_, result);
      close();
      return false;
    }
  }

  ROS_ASSERT(poll_set_ || (flags_ & SYNCHRONOUS));
  if (poll_set_)
  {
    poll_set_->addSocket(req_sock_, boost::bind(&TransportXeno::reqSocketUpdate, this, _1), shared_from_this());
  }

  return true;
}

bool TransportXeno::initializeRepSocket()
{
  ROS_ASSERT(rep_sock_ != ROS_INVALID_SOCKET);

  if (!(flags_ & SYNCHRONOUS))
  {
          int result = set_non_blocking(rep_sock_);
          if ( result != 0 ) {
              ROS_ERROR("setting reply socket [%d] as non_blocking failed with error [%d]", rep_sock_, result);
      close();
      return false;
    }
  }

  ROS_ASSERT(poll_set_ || (flags_ & SYNCHRONOUS));
  if (poll_set_)
  {
    poll_set_->addSocket(rep_sock_, boost::bind(&TransportXeno::repSocketUpdate, this, _1), shared_from_this());
  }

  return true;
}

void TransportXeno::close()
{
  Callback disconnect_cb;

  if (!req_closed_)
  {
    {
      boost::mutex::scoped_lock lock(req_close_mutex_);

      if (!req_closed_)
      {
        req_closed_ = true;

        ROSCPP_LOG_DEBUG("Xeno socket [%d] closed", req_sock_);

        ROS_ASSERT(req_sock_ != ROS_INVALID_SOCKET);

        if (poll_set_)
        {
          poll_set_->delSocket(req_sock_);
        }

        if ( close_socket(req_sock_) != 0 )
        {
          ROS_ERROR("Error closing socket [%d]: [%s]", req_sock_, last_socket_error_string());
        }

        req_sock_ = ROS_INVALID_SOCKET;

        disconnect_cb = disconnect_cb_;

        disconnect_cb_ = Callback();
        read_cb_ = Callback();
        write_cb_ = Callback();
      }
    }
  }

  if (disconnect_cb)
  {
    disconnect_cb(shared_from_this());
  }
}

int32_t TransportXeno::read(uint8_t* buffer, uint32_t size)
{
  {
    boost::mutex::scoped_lock lock(req_close_mutex_);
    if (req_closed_)
    {
      ROSCPP_LOG_DEBUG("Tried to read on a closed socket [%d]", req_sock_);
      return -1;
    }
  }

  ROS_ASSERT((int32_t)size > 0);

  uint32_t bytes_read = 0;

  while (bytes_read < size)
  {
    TransportXenoHeader header;

    // Get the data either from the reorder buffer or the socket
    // copy_bytes will contain the read size.
    // from_previous is true if the data belongs to the previous Xeno datagram.
    uint32_t copy_bytes = 0;
    bool from_previous = false;
    if (reorder_bytes_)
    {
      if (reorder_start_ != reorder_buffer_)
      {
        from_previous = true;
      }

      copy_bytes = std::min(size - bytes_read, reorder_bytes_);
      header = reorder_header_;
      memcpy(buffer + bytes_read, reorder_start_, copy_bytes);
      reorder_bytes_ -= copy_bytes;
      reorder_start_ += copy_bytes;
    }
    else
    {
      if (data_filled_ == 0)
      {
        ssize_t num_bytes;
        struct iovec iov[2];
        iov[0].iov_base = &header;
        iov[0].iov_len = sizeof(header);
        iov[1].iov_base = data_buffer_;
        iov[1].iov_len = max_datagram_size_ - sizeof(header);
        // Read a datagram with header
        num_bytes = readv(req_sock_, iov, 2);

        if (num_bytes < 0)
        {
          if ( last_socket_error_is_would_block() )
          {
            num_bytes = 0;
            break;
          }
          else
          {
            ROSCPP_LOG_DEBUG("readv() failed with error [%s]",  last_socket_error_string());
            close();
            break;
          }
        }
        else if (num_bytes == 0)
        {
          ROSCPP_LOG_DEBUG("Socket [%d] received 0/%d bytes, closing", req_sock_, size);
          close();
          return -1;
        }
        else if (num_bytes < (unsigned) sizeof(header))
        {
          ROS_ERROR("Socket [%d] received short header (%d bytes): %s", req_sock_, int(num_bytes),  last_socket_error_string());
          close();
          return -1;
        }

		num_bytes -= sizeof(header);
        data_filled_ = num_bytes;
        data_start_ = data_buffer_;
      }
      else
      {
        from_previous = true;
      }

      copy_bytes = std::min(size - bytes_read, data_filled_);
      // Copy from the data buffer, whether it has data left in it from a previous datagram or
      // was just filled by readv()
      memcpy(buffer + bytes_read, data_start_, copy_bytes);
      data_filled_ -= copy_bytes;
      data_start_ += copy_bytes;
    }


    if (from_previous)
    {
      // We are simply reading data from the last Xeno datagram, nothing to
      // parse
      bytes_read += copy_bytes;
    }
    else
    {
      // This datagram is new, process header
      switch (header.op_)
      {
        case ROS_XENO_DATA0:
          if (current_message_id_)
          {
            ROS_DEBUG("Received new message [%d:%d], while still working on [%d] (block %d of %d)", header.message_id_, header.block_, current_message_id_, last_block_ + 1, total_blocks_);
            reorder_header_ = header;

            // Copy the entire data buffer to the reorder buffer, as we will
            // need to replay this Xeno datagram in the next call.
            reorder_bytes_ = data_filled_ + (data_start_ - data_buffer_);
            memcpy(reorder_buffer_, data_buffer_, reorder_bytes_);
            reorder_start_ = reorder_buffer_;
            current_message_id_ = 0;
            total_blocks_ = 0;
            last_block_ = 0;

            data_filled_ = 0;
            data_start_ = data_buffer_;
            return -1;
          }
          total_blocks_ = header.block_;
          last_block_ = 0;
          current_message_id_ = header.message_id_;
          break;
        case ROS_XENO_DATAN:
          if (header.message_id_ != current_message_id_)
          {
            ROS_DEBUG("Message Id mismatch: %d != %d", header.message_id_, current_message_id_);
            data_filled_ = 0; // discard datagram
            return 0;
          }
          if (header.block_ != last_block_ + 1)
          {
            ROS_DEBUG("Expected block %d, received %d", last_block_ + 1, header.block_);
            data_filled_ = 0; // discard datagram
            return 0;
          }
          last_block_ = header.block_;

          break;
        default:
          ROS_ERROR("Unexpected Xeno header OP [%d]", header.op_);
          return -1;
      }

      bytes_read += copy_bytes;

      if (last_block_ == (total_blocks_ - 1))
      {
        current_message_id_ = 0;
        break;
      }
    }
  }

  return bytes_read;
}

int32_t TransportXeno::write(uint8_t* buffer, uint32_t size)
{
  {
    boost::mutex::scoped_lock lock(req_close_mutex_);

    if (req_closed_)
    {
      ROSCPP_LOG_DEBUG("Tried to write on a closed socket [%d]", req_sock_);
      return -1;
    }
  }

  ROS_ASSERT((int32_t)size > 0);

  const uint32_t max_payload_size = max_datagram_size_ - sizeof(TransportXenoHeader);

  uint32_t bytes_sent = 0;
  uint32_t this_block = 0;
  if (++current_message_id_ == 0)
    ++current_message_id_;
  while (bytes_sent < size)
  {
    TransportXenoHeader header;
    header.connection_id_ = connection_id_;
    header.message_id_ = current_message_id_;
    if (this_block == 0)
    {
      header.op_ = ROS_XENO_DATA0;
      header.block_ = (size + max_payload_size - 1) / max_payload_size;
    }
    else
    {
      header.op_ = ROS_XENO_DATAN;
      header.block_ = this_block;
    }
    ++this_block;

    struct iovec iov[2];
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = buffer + bytes_sent;
    iov[1].iov_len = std::min(max_payload_size, size - bytes_sent);
    ssize_t num_bytes = writev(req_sock_, iov, 2);

    //usleep(100);
    if (num_bytes < 0)
    {
      if( !last_socket_error_is_would_block() ) // Actually EAGAIN or EWOULDBLOCK on posix
      {
        ROSCPP_LOG_DEBUG("writev() failed with error [%s]", last_socket_error_string());
        close();
        break;
      }
      else
      {
        num_bytes = 0;
      }
    }
    else if (num_bytes < (unsigned) sizeof(header))
    {
      ROSCPP_LOG_DEBUG("Socket [%d] short write (%d bytes), closing", req_sock_, int(num_bytes));
      close();
      break;
    }
    else
    {
      num_bytes -= sizeof(header);
    }
    bytes_sent += num_bytes;
  }

  return bytes_sent;
}

void TransportXeno::enableRead()
{
  if(is_server_)
  {
    {
      boost::mutex::scoped_lock lock(req_close_mutex_);

      if (req_closed_)
      {
        return;
      }
    }
    if (!expecting_read_)
    {
      poll_set_->addEvents(req_sock_, POLLIN);
      expecting_read_ = true;
    }
  }
  else
  {
    {
      boost::mutex::scoped_lock lock(rep_close_mutex_);

      if (rep_closed_)
      {
        return;
      }
    }
    if (!expecting_read_)
    {
      poll_set_->addEvents(rep_sock_, POLLIN);
      expecting_read_ = true;
    }
  }
}

void TransportXeno::disableRead()
{
  ROS_ASSERT(!(flags_ & SYNCHRONOUS));

  if(is_server_)
  {
    {
      boost::mutex::scoped_lock lock(req_close_mutex_);

      if (req_closed_)
      {
        return;
      }
    }
    if (expecting_read_)
    {
      poll_set_->delEvents(req_sock_, POLLIN);
      expecting_read_ = false;
    }
  }
  else
  {
    {
      boost::mutex::scoped_lock lock(rep_close_mutex_);

      if (rep_closed_)
      {
        return;
      }
    }
    if (expecting_read_)
    {
      poll_set_->delEvents(rep_sock_, POLLIN);
      expecting_read_ = false;
    }
  }
}

void TransportXeno::enableWrite()
{
  if(is_server_)
  {
    {
      boost::mutex::scoped_lock lock(req_close_mutex_);

      if (req_closed_)
      {
        return;
      }
    }
    if (!expecting_read_)
    {
      poll_set_->addEvents(req_sock_, POLLOUT);
      expecting_read_ = true;
    }
  }
  else
  {
    {
      boost::mutex::scoped_lock lock(rep_close_mutex_);

      if (rep_closed_)
      {
        return;
      }
    }
    if (!expecting_read_)
    {
      poll_set_->addEvents(rep_sock_, POLLOUT);
      expecting_read_ = true;
    }
  }
}

void TransportXeno::disableWrite()
{
  if(is_server_)
  {
    {
      boost::mutex::scoped_lock lock(req_close_mutex_);

      if (req_closed_)
      {
        return;
      }
    }
    if (expecting_read_)
    {
      poll_set_->delEvents(req_sock_, POLLOUT);
      expecting_read_ = false;
    }
  }
  else
  {
    {
      boost::mutex::scoped_lock lock(rep_close_mutex_);

      if (rep_closed_)
      {
        return;
      }
    }
    if (expecting_read_)
    {
      poll_set_->delEvents(rep_sock_, POLLOUT);
      expecting_read_ = false;
    }
  }
}

TransportXenoPtr TransportXeno::createOutgoing(const std::string& label, int connection_id, int max_datagram_size)
{
  ROS_ASSERT(is_server_);
  
  TransportXenoPtr transport(new TransportXeno(poll_set_, flags_, max_datagram_size));
  if (!transport->connect(label, connection_id))
  {
    ROS_ERROR("Failed to create outgoing connection");
    return TransportXenoPtr();
  }
  return transport;

}

}
