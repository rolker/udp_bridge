#ifndef UDP_BRIDGE_CONNECTION_H
#define UDP_BRIDGE_CONNECTION_H

#include <ros/ros.h>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <list>
#include <map>
#include <netinet/in.h>
#include <udp_bridge/packet.h>
#include "udp_bridge/types.h"
#include "udp_bridge/wrapped_packet.h"

namespace udp_bridge
{

class ConnectionException
{
  public:
    ConnectionException(const std::string &msg):msg_(msg) {}
    const std::string & getMessage() const {return msg_;}
  private:
    std::string msg_;
};

class Connection
{
public:
  //Connection(std::string remote, std::string id);
  Connection(std::string remote, std::string id, std::string const &host, uint16_t port, std::string return_host=std::string(), uint16_t return_port=0);

  void setHostAndPort(const std::string& host, uint16_t port);

  std::string str() const;

  const std::string &remote() const;
  const std::string &id() const;

  // Used to tell the remote host the address to get back to us.
  const std::string& returnHost() const;
  uint16_t returnPort() const;
  void setReturnHostAndPort(const std::string &return_host, uint16_t return_port);

  const std::string& host() const;
  uint16_t port() const;
  const std::string& ip_address() const;
  std::string ip_address_with_port() const;

  const sockaddr_in* socket_address() const;

  const double& last_receive_time() const;
  void update_last_receive_time(double t, int data_size);

  bool can_send(uint32_t byte_count, double time);
  double data_receive_rate(double time);

  std::map<uint64_t, WrappedPacket>& sentPackets();

private:
  void resolveHost();

  // name of the remote this connection belongs to
  std::string remote_;

  // connection_id of the connection
  std::string id_;

  std::string host_;
  std::string ip_address_; // resolved ip address
  uint16_t port_;

  std::vector<sockaddr_in> addresses_;

  // Used by the remote to refer to us. Useful if they are behind a nat
  std::string return_host_;
  uint16_t return_port_ = 0;

  // Time in seconds since epoch that last packet was received
  double last_receive_time_ = 0.0;

  // Maximum bytes per second to send
  uint32_t data_rate_limit_ = 500000;

  std::map<double, uint16_t> data_size_sent_history_;
  std::map<double, uint16_t> data_size_received_history_;

  // Each connection keeps a buffer of sent packets in case a resend is needed
  // The same data packets are replicated for each connection to account for 
  // Different source_node or connection_id.
  std::map<uint64_t, WrappedPacket> sent_packets_;
};


std::string addressToDotted(const sockaddr_in &address);
    
} // namespace udp_bridge

#endif
