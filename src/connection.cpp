#include "udp_bridge/connection.h"
#include <udp_bridge/ChannelStatisticsArray.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace udp_bridge
{

// Connection::Connection(std::string remote, std::string id):
//   remote_(remote), id_(id)
// {
// }

Connection::Connection(std::string remote, std::string id, std::string const &host, uint16_t port, std::string return_host, uint16_t return_port):
  remote_(remote), id_(id), host_(host), port_(port), return_host_(return_host), return_port_(return_port)
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

void Connection::update_last_receive_time(double t, int data_size)
{
  last_receive_time_ = t;
  data_size_received_history_[t] += data_size;
}

bool Connection::can_send(uint32_t byte_count, double time)
{
  while(!data_size_sent_history_.empty() && data_size_sent_history_.begin()->first < time-1.0)
    data_size_sent_history_.erase(data_size_sent_history_.begin());
  uint32_t bytes_sent_last_second = 0;
  for(auto ds: data_size_sent_history_)
    bytes_sent_last_second += ds.second;
  if(bytes_sent_last_second+byte_count <= data_rate_limit_)
  {
    data_size_sent_history_[time] += byte_count;
    return true;
  }
  return false;
}

double Connection::data_receive_rate(double time)
{
  double five_secs_ago = time - 5;
  while(!data_size_received_history_.empty() && data_size_received_history_.begin()->first < five_secs_ago)
    data_size_received_history_.erase(data_size_received_history_.begin());

  double dt = 1.0;
  if(!data_size_received_history_.empty())
    dt = std::max(dt, data_size_received_history_.rbegin()->first - data_size_received_history_.begin()->first);

  uint32_t sum = 0;
  for(auto ds: data_size_received_history_)
    sum += ds.second;

  return sum/dt;
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
