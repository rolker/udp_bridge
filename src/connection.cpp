#include "udp_bridge/connection.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>
#include <poll.h>

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


SendResult Connection::send(const std::vector<WrappedPacket>& packets, int socket, const std::string& remote, bool is_overhead)
{
  uint32_t total_size = 0;
  for(const auto& p: packets)
    total_size += p.packet_size;

  auto now = ros::Time::now();
  if(!sent_packet_statistics_.can_send(total_size, data_rate_limit_, now))
  {
    for(const auto& p: packets)
    {

      PacketSizeData size_data;
      size_data.timestamp = now;
      size_data.size = p.packet_size;
      if(is_overhead)
        size_data.category = PacketSendCategory::overhead;
      else
        size_data.category = PacketSendCategory::message;
      size_data.send_result = SendResult::dropped;
      sent_packet_statistics_.add(size_data);
    }
    return SendResult::dropped;
  }

  SendResult ret = SendResult::success;
  for(const auto& p: packets)
  {
    auto packet = WrappedPacket(p, remote, id_);
    sent_packets_[packet.packet_number] = packet;
    PacketSendCategory category = PacketSendCategory::message;
    if(is_overhead)
      category = PacketSendCategory::overhead;
    auto send_ret = send(packet.packet, socket, category);
    if(send_ret.send_result != SendResult::success)
      ret = send_ret.send_result;
  }
  return ret;
}


PacketSizeData Connection::send(const std::vector<uint8_t> &data, int socket, PacketSendCategory category)
{
  PacketSizeData ret;
  ret.timestamp = ros::Time::now();
  ret.size = data.size();
  ret.category = category;
  ret.send_result = SendResult::failed;
  if(addresses_.empty())
  {
    sent_packet_statistics_.add(ret);
    return ret;
  }

  if(!sent_packet_statistics_.can_send(data.size(), data_rate_limit_, ret.timestamp))
  {
    ret.send_result = SendResult::dropped;
    sent_packet_statistics_.add(ret);
    return ret;
  }

  int bytes_sent = 0;
  try
  {
    int tries = 0;
    while (true)
    {
      pollfd p;
      p.fd = socket;
      p.events = POLLOUT;
      int poll_ret = poll(&p, 1, 10);
      if(poll_ret > 0 && p.revents & POLLOUT)
      {
        bytes_sent = sendto(socket, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(addresses_.data()), sizeof(sockaddr_in));
        if(bytes_sent == -1)
          switch(errno)
          {
            case EAGAIN:
              tries+=1;
              break;
            case ECONNREFUSED:
              sent_packet_statistics_.add(ret);
              return ret;
            default:
              throw(ConnectionException(strerror(errno)));
          }
        if(bytes_sent < data.size())
          throw(ConnectionException("only "+std::to_string(bytes_sent) +" of " +std::to_string(data.size()) + " sent"));
        else
        {
          ret.send_result = SendResult::success;
          sent_packet_statistics_.add(ret);
          return ret;
        }
      }
      else
        tries+=1;

      if(tries >= 20)
        if(poll_ret == 0)
          throw(ConnectionException("Timeout"));
        else
          throw(ConnectionException(std::to_string(errno) +": "+ strerror(errno)));
    }
  }
  catch(const ConnectionException& e)
  {
      ROS_WARN_STREAM("error sending data of size " << data.size() << ": " << e.getMessage());
  }
  sent_packet_statistics_.add(ret);
  return ret;
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

DataRates Connection::data_sent_rate(ros::Time time, PacketSendCategory category)
{
  return sent_packet_statistics_.get(category);
}


void Connection::resend_packets(const std::vector<uint64_t> &missing_packets, int socket)
{
  for(auto packet_number: missing_packets)
  {
    auto packet_to_resend = sent_packets_.find(packet_number);
    if(packet_to_resend != sent_packets_.end())
      send(packet_to_resend->second.packet, socket, PacketSendCategory::resend);
  }
}

void Connection::cleanup_sent_packets(ros::Time cutoff_time)
{
  std::vector<uint64_t> expired;
  for(auto sp: sent_packets_)
    if(sp.second.timestamp < cutoff_time)
      expired.push_back(sp.first);
  for(auto e: expired)
    sent_packets_.erase(e);
}




std::string addressToDotted(const sockaddr_in& address)
{
  return std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[0])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[1])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[2])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[3]);
}

} // namespace udp_bridge
