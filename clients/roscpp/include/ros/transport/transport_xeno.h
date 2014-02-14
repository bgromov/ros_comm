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

#ifndef ROSCPP_TRANSPORT_XENO_H
#define ROSCPP_TRANSPORT_XENO_H

#include <ros/types.h>
#include <ros/transport/transport.h>

#include <boost/thread/mutex.hpp>
#include "ros/io.h"
#include <ros/common.h>

#include <xenomai/rtdm/rtipc.h>
#include <xenomai/nucleus/types.h>

namespace ros
{

class TransportXeno;
typedef boost::shared_ptr<TransportXeno> TransportXenoPtr;

class PollSet;

#define ROS_XENO_DATA0 0
#define ROS_XENO_DATAN 1
#define ROS_XENO_PING 2
#define ROS_XENO_ERR 3
typedef struct TransportXenoHeader {
  uint32_t connection_id_;
  uint8_t op_;
  uint8_t message_id_;
  uint16_t block_;
} TransportXenoHeader;

/**
 * \brief XENOROS transport
 */
class ROSCPP_DECL TransportXeno : public Transport
{
public:
  enum Flags
  {
    SYNCHRONOUS = 1<<0,
  };

  TransportXeno(PollSet* poll_set, int flags = 0, int max_datagram_size = 0);
  virtual ~TransportXeno();

  /**
   * \brief Connect to a remote host.
   * \param host The hostname/IP to connect to
   * \param port The port to connect to
   * \return Whether or not the connection was successful
   */
  bool connect(const std::string& label, int conn_id);

  /**
   * \brief Returns the URI of the remote host
   */
  std::string getClientURI();

  /**
   * \brief Start a server socket and listen on a port
   * \param port The port to listen on
   */
  bool createIncoming(int port, bool is_server);
  /**
   * \brief Create a connection to a server socket.
   */
  TransportXenoPtr createOutgoing(int port, int conn_id, int max_datagram_size);
  /**
   * \brief Returns the port this transport is listening on
   */
  int getServerPort() const {return server_port_;}

  // overrides from Transport
  virtual int32_t read(uint8_t* buffer, uint32_t size);
  virtual int32_t write(uint8_t* buffer, uint32_t size);

  virtual void enableWrite();
  virtual void disableWrite();
  virtual void enableRead();
  virtual void disableRead();

  virtual void close();

  virtual std::string getTransportInfo();

  virtual bool requiresHeader() {return false;}

  virtual const char* getType() {return "XenoROS";}

  int getMaxDatagramSize() const {return max_datagram_size_;}

private:
  /**
   * \brief Initializes the assigned socket -- sets it to non-blocking and enables reading
   */
  bool initializeReqSocket();

  /**
   * \brief Initializes the assigned socket -- sets it to non-blocking and enables writing
   */
  bool initializeRepSocket();

  /**
   * \brief Set the socket to be used by this transport for reading
   * \param sock A valid Xeno socket
   * \return Whether setting the socket was successful
   */
  bool setReqSocket(int sock);

  /**
   * \brief Set the socket to be used by this transport for writing
   * \param sock A valid Xeno socket
   * \return Whether setting the socket was successful
   */
  bool setRepSocket(int sock);

  void reqSocketUpdate(int events);
  void repSocketUpdate(int events);

  socket_fd_t req_sock_;
  socket_fd_t rep_sock_;
  bool req_closed_;
  bool rep_closed_;
  boost::mutex req_close_mutex_;
  boost::mutex rep_close_mutex_;

  bool expecting_read_;
  bool expecting_write_;

  bool is_server_;
  rtipc_port_label plabel_req_;
  rtipc_port_label plabel_rep_;
  sockaddr_ipc server_address_;
  int server_port_;

  std::string cached_remote_host_;

  PollSet* poll_set_;
  int flags_;

  uint32_t connection_id_;
  uint8_t current_message_id_;
  uint16_t total_blocks_;
  uint16_t last_block_;

  uint32_t max_datagram_size_;

  uint8_t* data_buffer_;
  uint8_t* data_start_;
  uint32_t data_filled_;

  uint8_t* reorder_buffer_;
  uint8_t* reorder_start_;
  TransportXenoHeader reorder_header_;
  uint32_t reorder_bytes_;
};

}

#endif // ROSCPP_TRANSPORT_XENO_H

