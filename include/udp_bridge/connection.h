#ifndef UDP_BRIDGE_CONNECTION_H
#define UDP_BRIDGE_CONNECTION_H

#include <vector>
#include <cstdint>
#include <string>
#include <memory>

namespace udp_bridge
{

class ConnectionManager;

class Connection
{
public:
    ~Connection();
    
    // return 0 if ok, errno if error occured
    int send(std::vector<uint8_t> const &data);
    std::string str() const;
    int sendBufferSize() const;
private:
    friend class ConnectionManager;
    
    Connection(std::string const &host, uint16_t port);
    
    std::string m_host;
    uint16_t m_port;
    int m_socket;
    int m_send_buffer_size;
};

class ConnectionManager
{
public:
    std::shared_ptr<Connection> getConnection(std::string const &host, uint16_t port);
private:
    std::vector<std::shared_ptr<Connection> > m_connections;
};

    
} // namespace udp_bridge

#endif
