#include "udp_bridge/connection.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iostream>
#include <poll.h>

namespace udp_bridge
{

using namespace udp_bridge_interfaces::msg;

Connection::Connection(std::string id, std::string const &host, uint16_t port, std::string return_host, uint16_t return_port):
  id_(truncate_connection_id(id)), host_(host), port_(port), return_host_(return_host), return_port_(return_port)
{
  // id_ is canonicalized so connection->id() always returns the
  // on-wire form. Higher-level entry points (RemoteNode::newConnection)
  // surface a one-time WARN when this truncation actually shortens.
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  resolveHost();  // re-enters mutex (recursive)
}

void Connection::setHostAndPort(const std::string &host, uint16_t port)
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  if(host != host_ || port != port_)
  {
    host_ = host;
    port_ = port;
    resolveHost();  // re-enters mutex (recursive)
  }
}

std::string Connection::sourceIPAddress() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return source_ip_address_;
}

uint16_t Connection::sourcePort() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return source_port_;
}

void Connection::setSourceIPAndPort(const std::string &source_ip, uint16_t source_port)
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  source_ip_address_ = source_ip;
  source_port_ = source_port;
}

void Connection::setRateLimit(uint32_t maximum_bytes_per_second)
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  if(maximum_bytes_per_second == 0)
    data_rate_limit_ = default_rate_limit;
  else
    data_rate_limit_ = maximum_bytes_per_second;
}

uint32_t Connection::rateLimit() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return data_rate_limit_;
}

void Connection::setResendBudgetFraction(float fraction)
{
  // Clamp rather than reject: a mis-typed config value should degrade
  // to the nearest sane bound, not silently keep the previous value.
  fraction = std::max(0.0f, std::min(1.0f, fraction));
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  resend_budget_fraction_ = fraction;
}

float Connection::resendBudgetFraction() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return resend_budget_fraction_;
}

void Connection::resolveHost()
{
  // Caller (ctor or setHostAndPort) holds config_mutex_.
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

std::string Connection::str() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  std::stringstream ret;
  ret << host_ << ":" << port_;
  return ret.str();
}

const std::string& Connection::id() const
{
  return id_;
}

std::string Connection::returnHost() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return return_host_;
}

void Connection::setReturnHostAndPort(const std::string &return_host, uint16_t return_port)
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return_host_ = return_host;
  return_port_ = return_port;
}

uint16_t Connection::returnPort() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return return_port_;
}

std::string Connection::host() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return host_;
}

uint16_t Connection::port() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return port_;
}

std::string Connection::ip_address() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return ip_address_;
}

std::string Connection::ip_address_with_port() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return ip_address_+":"+std::to_string(port_);
}

double Connection::last_receive_time() const
{
  std::lock_guard<std::mutex> lock(receive_history_mutex_);
  return last_receive_time_;
}

void Connection::update_last_receive_time(double t, int data_size, bool duplicate)
{
  std::lock_guard<std::mutex> lock(receive_history_mutex_);
  last_receive_time_ = t;
  ReceivedSize rs;
  rs.size = data_size;
  rs.duplicate = duplicate;
  data_size_received_history_[t].push_back(rs);
}


SendResult Connection::send(const std::vector<WrappedPacket>& packets, int socket, const std::string& remote, bool is_overhead, rclcpp::Time now)
{
  uint32_t total_size = 0;
  for(const auto& p: packets)
    total_size += p.packet_size;

  uint32_t rate_limit;
  {
    std::lock_guard<std::recursive_mutex> lock(config_mutex_);
    rate_limit = data_rate_limit_;
  }

  // Aggregate pre-check is a fast-path optimization: if the batch as a
  // whole won't fit in the per-second budget (including bytes already
  // reserved by concurrent in-flight sends), drop all packets at once
  // rather than partial-send-then-drop. The single lock acquisition
  // below covers ONLY the over-budget path — check + per-packet
  // drop-records happen under one lock so concurrent forwarding
  // callbacks (the republish_group_ is Reentrant) can't double-account
  // drops against an inconsistent capacity snapshot. On the success
  // path the lock is released without reserving any bytes here, so two
  // concurrent callbacks can both observe capacity at this layer and
  // both proceed into the per-packet send() below; this is intentional.
  // The per-packet inner send() is the sole atomic enforcement point —
  // it uses a reserve-then-record pattern (see Q2 in
  // .agent/work-plans/issue-15/progress.md) so concurrent senders see
  // each other's in-flight bytes via reserved_bytes_in_flight_.
  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    if(!sent_packet_statistics_.can_send(total_size, reserved_bytes_in_flight_, rate_limit, now))
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
  }

  SendResult ret = SendResult::success;
  for(const auto& p: packets)
  {
    auto packet = WrappedPacket(p, remote, id_);
    {
      std::lock_guard<std::mutex> lock(sent_packets_mutex_);
      sent_packets_[packet.packet_number] = packet;
    }
    PacketSendCategory category = PacketSendCategory::message;
    if(is_overhead)
      category = PacketSendCategory::overhead;
    auto send_ret = send(packet.packet, socket, category, now);
    if(send_ret.send_result != SendResult::success)
      ret = send_ret.send_result;
  }
  return ret;
}


PacketSizeData Connection::send(const std::vector<uint8_t> &data, int socket, PacketSendCategory category, rclcpp::Time now)
{
  PacketSizeData ret;
  ret.timestamp = now;
  ret.size = data.size();
  ret.category = category;
  ret.send_result = SendResult::failed;

  // Snapshot the destination address + rate limit under config_mutex_
  // before doing any network I/O. We must NOT pass &addresses_.front()
  // across the unlock — a concurrent setHostAndPort() → resolveHost()
  // would clear() then push_back(), and the pointer in flight could
  // dangle by the time sendto() reads it.
  sockaddr_in destination;
  uint32_t rate_limit;
  bool no_address;
  {
    std::lock_guard<std::recursive_mutex> lock(config_mutex_);
    no_address = addresses_.empty();
    if(!no_address)
      destination = addresses_.front();
    rate_limit = data_rate_limit_;
  }
  if(no_address)
  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    sent_packet_statistics_.add(ret);
    return ret;
  }

  // Reserve-then-record pattern. The mutex is held only for the two
  // bookkeeping operations (check + reserve, then release + add); the
  // blocking-capable sendto() runs OUTSIDE it. Holding the mutex across
  // the sendto poll loop (the original B1/P1 design) could stall a
  // periodic-group caller (UDPBridge::sendBridgeInfo) which holds
  // remote_nodes_mutex_ while calling data_sent_rate(); a blocked
  // sendBridgeInfo then stalls the socket-drain path's
  // remote_nodes_mutex_ lookups, reintroducing the wedge mechanism the
  // callback-group split is meant to prevent.
  //
  // Under this pattern:
  //   1. Lock briefly: can_send(data.size, reserved_bytes_in_flight_,
  //      rate_limit, now). On fail → record dropped + return. On pass →
  //      reserved_bytes_in_flight_ += data.size().
  //   2. Release lock.
  //   3. Run the sendto poll loop (no lock held). MSG_DONTWAIT bounds the
  //      worst case to the 20-tries × 10 ms poll budget. Any return path
  //      (success, ECONNREFUSED, throw) must release the reservation.
  //   4. Lock briefly: add the final record + decrement
  //      reserved_bytes_in_flight_. Both happen under the same lock so
  //      another sender's can_send sees a consistent snapshot
  //      (released_reservation ↔ added_record).
  //
  // The ReservationGuard RAII helper handles throw-paths: if the sendto
  // loop throws (Timeout, partial-send, etc.), the destructor releases
  // the reservation so rate-limit accounting stays consistent for the
  // next can_send call. No record is added in this case — and there is
  // no caller in the workspace that catches ConnectionException, so the
  // throw propagates up through UDPBridge::callback to the executor and
  // the node terminates. That behavior predates the reservation pattern
  // and isn't changed by it; the value of the guard here is purely
  // keeping reserved_bytes_in_flight_ correct before the process dies,
  // so a sibling Connection on the same node doesn't observe a phantom
  // reservation in its own pre-shutdown logging window. Catching at the
  // call site to keep the bridge alive across transient socket errors
  // would be a separate follow-up.
  struct ReservationGuard
  {
    std::mutex& mutex;
    uint32_t& reservation;
    uint32_t bytes;
    bool armed = true;
    ~ReservationGuard()
    {
      if(armed)
      {
        std::lock_guard<std::mutex> lock(mutex);
        reservation -= bytes;
      }
    }
  };

  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    if(!sent_packet_statistics_.can_send(data.size(), reserved_bytes_in_flight_, rate_limit, ret.timestamp))
    {
      ret.send_result = SendResult::dropped;
      sent_packet_statistics_.add(ret);
      return ret;
    }
    reserved_bytes_in_flight_ += data.size();
  }

  ReservationGuard reservation_guard{sent_packet_statistics_mutex_, reserved_bytes_in_flight_, static_cast<uint32_t>(data.size())};

  // Record the actual outcome AND release the reservation under one
  // lock so concurrent senders see them atomically. Marks the guard
  // disarmed so its destructor doesn't double-release.
  auto record_and_release = [&]()
  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    sent_packet_statistics_.add(ret);
    reserved_bytes_in_flight_ -= data.size();
    reservation_guard.armed = false;
  };

  int bytes_sent = 0;
  int tries = 0;
  while (true)
  {
    pollfd p;
    p.fd = socket;
    p.events = POLLOUT;
    int poll_ret = poll(&p, 1, 10);
    if(poll_ret > 0 && p.revents & POLLOUT)
    {
      bytes_sent = sendto(socket, data.data(), data.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
      if(bytes_sent == -1)
      {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
          // Kernel send buffer can't accept the packet right now. Bump
          // the retry counter and fall through to the tries-budget check
          // at the bottom of this loop iteration. Do NOT fall through to
          // the bytes_sent / data.size() comparison below — bytes_sent
          // is -1, and comparing it against a size_t promotes -1 to a
          // huge unsigned, which previously masked the failure as
          // "all sent" (recorded SUCCESS for a packet that never left
          // the host).
          tries+=1;
        }
        else if(errno == ECONNREFUSED)
        {
          record_and_release();
          return ret;
        }
        else
        {
          throw(ConnectionException(strerror(errno)));
        }
      }
      else if(static_cast<size_t>(bytes_sent) < data.size())
      {
        throw(ConnectionException("only "+std::to_string(bytes_sent) +" of " +std::to_string(data.size()) + " sent"));
      }
      else
      {
        ret.send_result = SendResult::success;
        record_and_release();
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

std::pair<double, double> Connection::data_receive_rate(double time)
{
  std::lock_guard<std::mutex> lock(receive_history_mutex_);
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

DataRates Connection::data_sent_rate(rclcpp::Time time, PacketSendCategory category)
{
  (void)time;
  std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
  return sent_packet_statistics_.get(category);
}

DataRates Connection::data_sent_rate(rclcpp::Time time)
{
  (void)time;
  std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
  return sent_packet_statistics_.get();
}


void Connection::resend_packets(const std::vector<uint64_t> &missing_packets, int socket, rclcpp::Time now)
{
  // Copy out the packets we need under the lock, then send without it —
  // send() does network I/O and we don't want to hold sent_packets_mutex_
  // for that long.
  std::vector<std::vector<uint8_t>> packets_to_resend;
  {
    std::lock_guard<std::mutex> lock(sent_packets_mutex_);
#ifdef UDP_BRIDGE_BUILD_TESTING
    ++resend_call_count_for_test_;
#endif
    packets_to_resend.reserve(missing_packets.size());
    for(auto packet_number: missing_packets)
    {
      auto it = sent_packets_.find(packet_number);
      if(it != sent_packets_.end())
        packets_to_resend.push_back(it->second.packet);
    }
  }
  if(packets_to_resend.empty())
    return;

  // Resend budget (issue #44). Resends may consume at most
  // resend_budget_fraction_ of the rate limit per second; while the
  // connection is receiving nothing (which starves acks — retrying
  // harder cannot help), the budget halves per kAckStarvationThreshold
  // elapsed, floored at 1/2^kMaxAckStarvationBackoffShift. Bounds the
  // self-amplifying resend storms of 2026-05-01 / 2026-08-04 while the
  // overall can_send cap in send() continues to meter the total.
  uint32_t rate_limit;
  float fraction;
  {
    std::lock_guard<std::recursive_mutex> lock(config_mutex_);
    rate_limit = data_rate_limit_;
    fraction = resend_budget_fraction_;
  }
  uint32_t backoff_shift = 0;
  const double receive_time = last_receive_time();
  // 0.0 is the "no packet received yet" sentinel (see the getter's
  // doc): a brand-new connection has no starvation history, so it gets
  // the full budget rather than the max-backoff floor.
  if(receive_time > 0.0)
  {
    const double starved_seconds = now.seconds() - receive_time;
    if(starved_seconds >= kAckStarvationThreshold.count())
      backoff_shift = std::min(
        static_cast<uint32_t>(starved_seconds / kAckStarvationThreshold.count()),
        kMaxAckStarvationBackoffShift);
  }
  const double budget_bytes =
    (static_cast<double>(fraction) * rate_limit) / static_cast<double>(1u << backoff_shift);

  // Seed the accumulator with resend bytes already recorded in the same
  // strict 1-second window can_send uses (NOT the 0-10 s smoothed rate,
  // which lags a sustained burst and would under-shed in exactly the
  // sustained-storm mode this budget exists to stop). Attempted bytes
  // are then accumulated locally — counting attempts (rather than
  // re-querying, which would miss failed sends' budget cost) keeps the
  // bound conservative and the loop O(packets).
  uint64_t attempted_bytes;
  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    attempted_bytes = sent_packet_statistics_.bytes_in_window(PacketSendCategory::resend, now);
  }

  for(std::size_t i = 0; i < packets_to_resend.size(); ++i)
  {
    const auto& packet = packets_to_resend[i];
    if(attempted_bytes + packet.size() > budget_bytes)
    {
      // Budget exhausted: record the remainder as dropped so the
      // shedding is visible in the existing per-connection resend
      // DataRates (BridgeInfo), then stop. The receiver's re-request /
      // give-up machinery (resend_constants.h) handles their fate.
      std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
      for(std::size_t j = i; j < packets_to_resend.size(); ++j)
      {
        PacketSizeData dropped;
        dropped.timestamp = now;
        dropped.size = packets_to_resend[j].size();
        dropped.category = PacketSendCategory::resend;
        dropped.send_result = SendResult::dropped;
        sent_packet_statistics_.add(dropped);
      }
      return;
    }
    send(packet, socket, PacketSendCategory::resend, now);
    attempted_bytes += packet.size();
  }
}

void Connection::cleanup_sent_packets(rclcpp::Time cutoff_time)
{
  std::lock_guard<std::mutex> lock(sent_packets_mutex_);
  std::vector<uint64_t> expired;
  for(auto sp: sent_packets_)
    if(sp.second.timestamp < cutoff_time)
      expired.push_back(sp.first);
  for(auto e: expired)
    sent_packets_.erase(e);
}

#ifdef UDP_BRIDGE_BUILD_TESTING
void Connection::record_sent_packet_for_test(uint64_t packet_number, rclcpp::Time timestamp,
                                             std::size_t payload_size)
{
  std::lock_guard<std::mutex> lock(sent_packets_mutex_);
  // packet_number and timestamp are set; the byte vector is filled
  // with payload_size zero bytes (default 0 = empty, adequate for
  // cleanup-boundary tests that only check sent_packets_.size()).
  // Resend-budget tests pass a non-zero payload_size so
  // resend_packets() sends packets with realistic sizes. The
  // SequencedPacketHeader fields remain default-constructed.
  WrappedPacket wp;
  wp.packet_number = packet_number;
  wp.timestamp = timestamp;
  wp.packet.assign(payload_size, 0);
  sent_packets_[packet_number] = wp;
}

std::size_t Connection::sent_packet_count_for_test() const
{
  std::lock_guard<std::mutex> lock(sent_packets_mutex_);
  return sent_packets_.size();
}

std::size_t Connection::resend_call_count_for_test() const
{
  std::lock_guard<std::mutex> lock(sent_packets_mutex_);
  return resend_call_count_for_test_;
}
#endif  // UDP_BRIDGE_BUILD_TESTING



std::string addressToDotted(const sockaddr_in& address)
{
  return std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[0])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[1])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[2])+"."+
          std::to_string(reinterpret_cast<const uint8_t*>(&address.sin_addr.s_addr)[3]);
}

} // namespace udp_bridge
