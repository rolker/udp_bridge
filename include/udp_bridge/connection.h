#ifndef UDP_BRIDGE_CONNECTION_H
#define UDP_BRIDGE_CONNECTION_H

#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <list>

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
    
    //void send(std::vector<uint8_t> const &data);
    void send(std::shared_ptr<std::vector<uint8_t> > data);
    std::string str() const;
    int sendBufferSize() const;

    // Returns the label, or optionally the string representation
    // of the host and port if the label is empty and allowEmpty
    // is false.
    // This makes the label() call always return something
    // useful to display by default.
    std::string label(bool allowEmpty = false) const;
    void setLabel(const std::string &label);

    // Used to tell the remote host the address to get back to us.
    const std::string& returnHost() const;
    void setReturnHost(const std::string &return_host);
private:
    friend class ConnectionManager;
    
    Connection(std::string const &host, uint16_t port, std::string return_host=std::string());
    
    std::string m_host;
    uint16_t m_port;
    int m_socket;
    int m_send_buffer_size;
    std::string m_label;

    // Used by the remote to refer to us. Useful if they are behind a nat
    std::string m_return_host;

    std::list<std::shared_ptr<std::vector<uint8_t> > > packet_buffer_;
    int packet_buffer_length_ = 100;
};

class ConnectionManager
{
public:
    // Returns a connection to host:port, creating one if it does not yet exist
    // If label is not empty and a connection exists with given label, replace it if necessary.
    std::shared_ptr<Connection> getConnection(std::string const &host, uint16_t port, std::string label=std::string());

    // Returns a connection with the given label, or matching the string representation
    // of the form host:port. Returns an empty pointer if not found.
    std::shared_ptr<Connection> getConnection(std::string const &label);

    const std::vector<std::shared_ptr<Connection> > & connections() const;
private:
    std::vector<std::shared_ptr<Connection> > m_connections;
};

    
} // namespace udp_bridge

#endif
