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
#include "udp_bridge/DataRates.h"

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

/// Represents a connection to a remote node.
/// Multiple connections to a remote node may be used for redundancy.
class Connection
{
public:
  Connection(std::string id, std::string const &host, uint16_t port, std::string return_host=std::string(), uint16_t return_port=0);

  void setHostAndPort(const std::string& host, uint16_t port);
  void setRateLimit(uint32_t maximum_bytes_per_second);
  uint32_t rateLimit() const;

  std::string str() const;

  const std::string &id() const;

  // Used to tell the remote host the address to get back to us.
  const std::string& returnHost() const;
  uint16_t returnPort() const;
  void setReturnHostAndPort(const std::string &return_host, uint16_t return_port);

  // IP address and port from incoming packets. May differ from where packets are sent depending
  // on network architecture.
  const std::string& sourceIPAddress() const;
  uint16_t sourcePort() const;
  void setSourceIPAndPort(const std::string &source_ip, uint16_t source_port);


  const std::string& host() const;
  uint16_t port() const;
  const std::string& ip_address() const;
  std::string ip_address_with_port() const;

  const sockaddr_in* socket_address() const;

  const double& last_receive_time() const;
  void update_last_receive_time(double t, int data_size, bool duplicate);

  void resend_packets(const std::vector<uint64_t> &missing_packets, int socket);

  SendResult send(const std::vector<WrappedPacket>& packets, int socket, const std::string& remote, bool is_overhead);

  PacketSizeData send(const std::vector<uint8_t> &data, int socket, PacketSendCategory category);

  //bool can_send(uint32_t byte_count, double time);

  /// Returns the average received data rate at the specified time.
  /// The first element of the returned pair is for unique bytes/second and the second
  /// element is for duplicated bytes per second. Duplicated bytes are for packets
  /// that had already been received on another connection.
  std::pair<double, double>  data_receive_rate(double time);

  /// Returns the average data rate.
  DataRates data_sent_rate(ros::Time time, PacketSendCategory category);

  /// Remove saved sent packets older than cutoff_time.
  void cleanup_sent_packets(ros::Time cutoff_time);

private:
  void resolveHost();

  /// connection_id of the connection
  std::string id_;

  std::string host_;
  std::string ip_address_; ///< resolved ip address
  uint16_t port_;

  std::vector<sockaddr_in> addresses_;

  /// Used by the remote to refer to us. Useful if they are behind a nat
  std::string return_host_;
  uint16_t return_port_ = 0;

  std::string source_ip_address_; ///< source ip address of incoming packets from remote
  uint16_t source_port_ = 0; ///< source port of incoming packets from remote

  /// Time in seconds since epoch that last packet was received
  double last_receive_time_ = 0.0;

  static constexpr uint32_t default_rate_limit = 50000;

  /// Maximum bytes per second to send.
  uint32_t data_rate_limit_ = default_rate_limit;

  /// Info about a received packet useful for data rate statistics.
  struct ReceivedSize
  {
    /// size in bytes of a received packet
    uint16_t size = 0;

    /// indicates if this is a duplicate packet that has already been seen on a
    /// different channel.
    bool duplicate = false;
  };
  std::map<double, std::vector<ReceivedSize> > data_size_received_history_;

  /// History of recently sent packets.
  /// Each connection keeps a buffer of sent packets in case a resend is needed.
  /// The same data packets are replicated for each connection to account for 
  /// different source_node or connection_id.
  std::map<uint64_t, WrappedPacket> sent_packets_;

  PacketSendStatistics sent_packet_statistics_;
};


std::string addressToDotted(const sockaddr_in &address);
    
} // namespace udp_bridge

#endif
