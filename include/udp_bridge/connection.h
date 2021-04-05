#ifndef UDP_BRIDGE_CONNECTION_H
#define UDP_BRIDGE_CONNECTION_H

#include <vector>
#include <cstdint>
#include <string>
#include <memory>

namespace udp_bridge
{

class ConnectionException
{
  public:
    ConnectionException(const std::string &msg):m_msg(msg) {}
    const std::string & getMessage() const {return m_msg;}
  private:
    std::string m_msg;
};

class ConnectionManager;

class Connection
{
public:
    ~Connection();
    
    // return 0 if ok, errno if error occured
    void send(std::vector<uint8_t> const &data);
    std::string str() const;
    int sendBufferSize() const;

    // Returns the label, or optionally the string representation
    // of the host and port if the label is empty and allowEmpty
    // is false.
    // This makes the label() call always return something
    // useful to display by default.
    std::string label(bool allowEmpty = false) const;
    void setLabel(const std::string &label);
private:
    friend class ConnectionManager;
    
    Connection(std::string const &host, uint16_t port);
    
    std::string m_host;
    uint16_t m_port;
    int m_socket;
    int m_send_buffer_size;
    std::string m_label;
};

class ConnectionManager
{
public:
    // Returns a connection to host:port, creating one if it does not yet exist
    std::shared_ptr<Connection> getConnection(std::string const &host, uint16_t port);

    // Returns a connection with the given label, or matching the string representation
    // of the form host:port. Returns an empty pointer is not found.
    std::shared_ptr<Connection> getConnection(std::string const &label);

    const std::vector<std::shared_ptr<Connection> > & connections() const;
private:
    std::vector<std::shared_ptr<Connection> > m_connections;
};

    
} // namespace udp_bridge

#endif
