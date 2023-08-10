#include "udp_bridge/connection.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace udp_bridge
{

Connection::Connection(std::string id, std::string const &host, uint16_t port, std::string return_host, uint16_t return_port):
  id_(id), host_(host), port_(port), return_host_(return_host), return_port_(return_port)
{
  resolveHost();
}

void Connection::setHostAndPort(const std::string &host, uint16_t port)
{
  if(host != host_ || port != port_)
  {
    host_ = host;
    port_ = port;
    resolveHost();
  }
}

const std::string& Connection::sourceIPAddress() const
{
  return source_ip_address_;
}

uint16_t Connection::sourcePort() const
{
  return source_port_;
}

void Connection::setSourceIPAndPort(const std::string &source_ip, uint16_t source_port)
{
  source_ip_address_ = source_ip;
  source_port_ = source_port;
}

void Connection::setRateLimit(uint32_t maximum_bytes_per_second)
{
  if(maximum_bytes_per_second == 0)
    data_rate_limit_ = default_rate_limit;
  else
    data_rate_limit_ = maximum_bytes_per_second;
}

uint32_t Connection::rateLimit() const
{
  return data_rate_limit_;
}

void Connection::resolveHost()
{
  addresses_.clear();

  if(host_.empty() || port_ == 0)
    return;

  struct addrinfo hints = {0}, *addresses;
  
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  
  std::string port_string = std::to_string(port_);
  
  int ret = getaddrinfo(host_.c_str(), port_string.c_str(), &hints, &addresses);
  if(ret != 0)
    throw std::runtime_error(gai_strerror(ret));

  for (struct addrinfo *address = addresses; address != nullptr; address = address->ai_next)
  {
    addresses_.push_back(*reinterpret_cast<sockaddr_in*>(address->ai_addr));
    if(addresses_.size() == 1)
      ip_address_ = addressToDotted(addresses_.back());
  }
  freeaddrinfo(addresses);
}

const sockaddr_in* Connection::socket_address() const
{
  if(addresses_.empty())
    return nullptr;
  return &addresses_.front();
}

std::string Connection::str() const
{
  std::stringstream ret;
  ret << host_ << ":" << port_;
  return ret.str();
}

const std::string& Connection::id() const
{
  return id_;
}

const std::string& Connection::returnHost() const
{
  return return_host_;
}

void Connection::setReturnHostAndPort(const std::string &return_host, uint16_t return_port)
{
  return_host_ = return_host;
  return_port_ = return_port;
}

uint16_t Connection::returnPort() const
{
  return return_port_;
}

const std::string& Connection::host() const
{
  return host_;
}

uint16_t Connection::port() const
{
  return port_;
}

const std::string& Connection::ip_address() const
{
  return ip_address_;
}

std::string Connection::ip_address_with_port() const
{
  return ip_address_+":"+std::to_string(port_);
}

const double& Connection::last_receive_time() const
{
  return last_receive_time_;
}

void Connection::update_last_receive_time(double t, int data_size, bool duplicate)
{
  last_receive_time_ = t;
  ReceivedSize rs;
  rs.size = data_size;
  rs.duplicate = duplicate;
  data_size_received_history_[t].push_back(rs);
}

bool Connection::can_send(uint32_t byte_count, double time)
{
  ROS_DEBUG_STREAM_NAMED("rate_limit", "Can " << byte_count << " bytes be sent at " << time << "?");
  while(!data_size_sent_history_.empty() && data_size_sent_history_.begin()->first < time-1.0)
  {
    ROS_DEBUG_STREAM_NAMED("rate_limit", " erasing " << data_size_sent_history_.begin()->second << " bytes received at " << data_size_sent_history_.begin()->first);
    data_size_sent_history_.erase(data_size_sent_history_.begin());
  }
  uint32_t bytes_sent_last_second = 0;
  for(auto ds: data_size_sent_history_)
  {
    ROS_DEBUG_STREAM_NAMED("rate_limit", " adding to sum " << ds.second << " bytes sent at " << ds.first); 
    bytes_sent_last_second += ds.second;
  }
  ROS_DEBUG_STREAM_NAMED("rate_limit", " already sent: " << bytes_sent_last_second << " want to send: " << byte_count << " totaling: " << bytes_sent_last_second+byte_count << " limit: " << data_rate_limit_);
  if(bytes_sent_last_second+byte_count <= data_rate_limit_)
  {
    ROS_DEBUG_STREAM_NAMED("rate_limit","OK to send");
    data_size_sent_history_[time] += byte_count;
    return true;
  }
  ROS_DEBUG_STREAM_NAMED("rate_limit","NOT OK to send");
  return false;
}

std::pair<double, double> Connection::data_receive_rate(double time)
{
  double five_secs_ago = time - 5;
  while(!data_size_received_history_.empty() && data_size_received_history_.begin()->first < five_secs_ago)
    data_size_received_history_.erase(data_size_received_history_.begin());

  double dt = 1.0;
  if(!data_size_received_history_.empty())
    dt = std::max(dt, time - data_size_received_history_.begin()->first);

  uint32_t unique_sum = 0;
  uint32_t duplicate_sum = 0;
  for(auto dsv: data_size_received_history_)
    for(auto ds: dsv.second)
    {
      if(ds.duplicate)
        duplicate_sum += ds.size;
      else
        unique_sum += ds.size;
    }

  return std::make_pair<double, double>(unique_sum/dt, duplicate_sum/dt);
}

std::map<uint64_t, WrappedPacket>& Connection::sentPackets()
{
  return sent_packets_;
}


std::string addressToDotted(const sockaddr_in& address)
{
  return std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[0])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[1])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[2])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[3]);
}

} // namespace udp_bridge
