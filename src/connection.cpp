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

Connection::Connection(std::string const &host, uint16_t port, std::string return_host):m_host(host),m_port(port),m_return_host(return_host)
{
    struct addrinfo hints = {0}, *addresses;
    
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    
    std::string port_string = std::to_string(port);
    
    int ret = getaddrinfo(host.c_str(), port_string.c_str(), &hints, &addresses);
    if(ret != 0)
        throw std::runtime_error(gai_strerror(ret));
    
    for (struct addrinfo *address = addresses; address != nullptr; address = address->ai_next)
    {
      m_addresses.push_back(*reinterpret_cast<sockaddr_in*>(address->ai_addr));
      std::cerr << host << ":" << port << " adding address: " << addressToDotted(m_addresses.back()) << ":" << ntohs(m_addresses.back().sin_port) << std::endl;
      if(m_addresses.size() == 1)
        m_ip_address = addressToDotted(m_addresses.back());
    }
    freeaddrinfo(addresses);
}
    
Connection::~Connection()
{
}

const sockaddr_in& Connection::socket_address() const
{
  return m_addresses.front();
}

std::string Connection::str() const
{
    std::stringstream ret;
    ret << m_host << ":" << m_port;
    return ret.str();
}

std::string Connection::label(bool allowEmpty) const
{
    if(!allowEmpty && m_label.empty())
      return str();
    return m_label;
}

void Connection::setLabel(const std::string &label)
{
    m_label = label;
}

std::string Connection::topicLabel() const
{
  std::string ret = label();
  if(!std::isalpha(ret[0]))
    ret = "r"+ret;
  for(int i = 0; i < ret.size(); i++)
    if(!(std::isalnum(ret[i]) || ret[i] == '_' || ret[i] == '/'))
      ret[i] = '_';
  return ret;
}

const std::string& Connection::returnHost() const
{
  return m_return_host;
}

void Connection::setReturnHost(const std::string &return_host)
{
  m_return_host = return_host;
}

const std::string& Connection::host() const
{
  return m_host;
}

uint16_t Connection::port() const
{
  return m_port;
}

const std::string& Connection::ip_address() const
{
  return m_ip_address;
}

std::string Connection::ip_address_with_port() const
{
  return m_ip_address+":"+std::to_string(m_port);
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


std::shared_ptr<Connection> ConnectionManager::getConnection(std::string const &host, uint16_t port, std::string label)
{
  if(!label.empty())
  {
    for(auto& c: m_connections)
      if(c->label() == label)
        if((c->m_host == host || c->m_ip_address == host) && c->m_port == port)
          return c;
        else
          c.reset();
  }
  for(auto c: m_connections)
    if(c && (c->m_host == host || c->m_ip_address == host)  && c->m_port == port)
      return c;
  
  auto new_connection = std::shared_ptr<Connection>(new Connection(host, port));
  // Is the new connection the same ip/port as an existing one?
  for(auto& c: m_connections)
    if(c && (c->m_ip_address == new_connection->m_ip_address)  && c->m_port == port)
    {
      c->m_host = host;
      return c;
    }

  // Look for a free spot before creating a new one
  for(auto& c: m_connections)
    if(!c)
    {
      c = std::shared_ptr<Connection>(new Connection(host, port));
      return c;
    }
  m_connections.push_back(std::shared_ptr<Connection>(new Connection(host,port)));
  return m_connections.back();
}

std::shared_ptr<Connection> ConnectionManager::getConnection(std::string const &label)
{
    for(auto c: m_connections)
      if(c->label() == label || c->str() == label)
        return c;
    return std::shared_ptr<Connection>();
}

const std::vector<std::shared_ptr<Connection> > & ConnectionManager::connections() const
{
    return m_connections;
}


std::string addressToDotted(const sockaddr_in& address)
{
    return std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[0])+"."+
           std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[1])+"."+
           std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[2])+"."+
           std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[3]);
}

} // namespace udp_bridge
