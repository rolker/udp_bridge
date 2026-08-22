#include "udp_bridge/remote_node.h"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge/connection.h"
#include "udp_bridge/resend_constants.h"
#include "udp_bridge/utilities.h"
#include "rmw/qos_profiles.h"

#include <cassert>

namespace udp_bridge
{

using namespace udp_bridge_interfaces::msg;

RemoteNode::RemoteNode(std::string remote_name, std::string local_name, NodeInterfaces node): name_(remote_name), local_name_(local_name), logger_(node.get_node_logging_interface()->get_logger()),clock_(node.get_node_clock_interface()->get_clock())
{
  assert(remote_name != local_name);

  rclcpp::QoS latched_qos(1);
  latched_qos.transient_local();
  latched_qos.keep_last(1);

  std::string node_name = node.get_node_base_interface()->get_name();
  bridge_info_publisher_ = rclcpp::create_publisher<BridgeInfo>(node, node_name+"/remotes/"+topicName()+"/bridge_info", latched_qos);

  topic_statistics_publisher_ = rclcpp::create_publisher<TopicStatisticsArray>(node, node_name+"/remotes/"+topicName()+"/topic_statistics", latched_qos);
}

void RemoteNode::update(const Remote& remote_message)
{
  assert(name_ == remote_message.name);
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  for(auto c: remote_message.connections)
  {
    // Use the canonicalizing helper rather than a raw `connections_[]`
    // index — bare `operator[]` with the untruncated configured id
    // would default-construct a null shared_ptr at the *uncanonical*
    // key for any id ≥ maximum_connection_id_size. That left an orphan
    // slot alongside the real canonical-keyed entry from
    // newConnection(), and on the next refresh through this loop the
    // (still-null) orphan re-entered the `if(!connection)` branch and
    // newConnection's overwrite path wiped the live Connection's
    // sent_packets_ / stats. Caught by R9 review on PR #25 and three
    // independent local reviewers (Adversarial, Governance, Copilot CLI).
    auto connection = this->connection(c.connection_id);
    if(!connection)
      connection = newConnection(c.connection_id, c.host, c.port);
    connection->setReturnHostAndPort(c.return_host, c.return_port);
    connection->setRateLimit(c.maximum_bytes_per_second);
  }
}

void RemoteNode::update(const BridgeInfo& bridge_info, const SourceInfo& source_info)
{
  // Publish first, without the lock — bridge_info_publisher_ is set once
  // at construction and rclcpp publishers are thread-safe; holding our
  // mutex across publish() would violate the "no slow operations under
  // lock" discipline.
  bridge_info_publisher_->publish(bridge_info);

  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  for(auto remote: bridge_info.remotes)
    if(remote.name == local_name_)
    {
      for(auto connection_info: remote.connections)
      {
        auto c = connection(connection_info.connection_id);
        auto host = connection_info.return_host;
        if(host.empty())
          host = source_info.host;
        auto port = connection_info.return_port;
        if(port == 0)
          port = source_info.port;
        if(!c)
          c = newConnection(connection_info.connection_id, host, port);
        else
        {
          if(!connection_info.return_host.empty() || connection_info.return_port != 0)
            c->setHostAndPort(connection_info.return_host, connection_info.return_port);
        }
        c->setSourceIPAndPort(source_info.host, source_info.port);
        // Delivered-vs-sent admission control (issue #43): the remote's
        // received_bytes_per_second for this connection is what it
        // actually received from us — feed it to the AIMD loop, along
        // with the duplicate rate it reports, so the loop can work from
        // goodput rather than a figure that resends inflate (issue #52).
        c->updateAdmissionControl(connection_info.received_bytes_per_second,
                                  connection_info.duplicate_bytes_per_second,
                                  clock_->now());
      }
      if(bridge_info.next_packet_number < next_packet_number_)
      {
        RCLCPP_WARN_STREAM(logger_, "Received next packet number that is less than previous one: " << bridge_info.next_packet_number << " time: " << rclcpp::Time(bridge_info.last_packet_time).seconds() << " (previous: " << next_packet_number_ << " time: " << rclcpp::Time(last_packet_time_).seconds() << ")");
        if(bridge_info.next_packet_number == 0 || rclcpp::Time(bridge_info.last_packet_time) > last_packet_time_)
        {
          // Try to detect remote restart of udp_bridge. Assume a reset if next packet number is zero
          // or if a timestamp for a lower numbered packet is greater than what we last saw for a larger
          // packet number.
          RCLCPP_WARN_STREAM(logger_, "Assuming remote udp_bridge restart");
          received_packet_times_.clear();
          resend_state_.clear();
          given_up_packet_numbers_.clear();
          // The sender's packet_number space resets to 0 on restart, so
          // the stale-packet high-water marks must reset too — otherwise
          // every post-restart packet would be rejected as stale by
          // admitForPublish.
          highest_published_packet_number_.clear();
          // The reorder/jitter buffer (issue #35) holds packets keyed on
          // the same pre-restart number space, so drop any held packets
          // too — releasing them post-restart would publish stale data
          // and, worse, advance the freshly-cleared high-water mark to a
          // pre-restart number, re-rejecting every post-restart packet.
          reorder_buffer_.clear();
        }
      }
      next_packet_number_ = bridge_info.next_packet_number;
      last_packet_time_ = bridge_info.last_packet_time;
    }
}


std::string RemoteNode::topicName() const
{
  return topicString(name_);
}

std::shared_ptr<Connection> RemoteNode::connection(std::string connection_id)
{
  // Canonicalize lookup key so callers can pass either the configured
  // (untruncated) id or the on-wire (truncated) form and get the same
  // connection. connections_ is keyed on the canonical (truncated)
  // form by construction (see newConnection / adoptConnection).
  connection_id = truncate_connection_id(connection_id);
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if(connections_.find(connection_id) != connections_.end())
    return connections_[connection_id];
  return {};
}

std::vector<std::shared_ptr<Connection> > RemoteNode::connections()
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  std::vector<std::shared_ptr<Connection> > ret;
  for(auto connection: connections_)
    if(connection.second)
      ret.push_back(connection.second);
  return ret;
}

std::shared_ptr<Connection> RemoteNode::newConnection(std::string connection_id, std::string host, uint16_t port)
{
  // Canonicalize to the on-wire form so connections_ keys, Connection::id(),
  // and rr.connection_id (which carries the wire-truncated form on the
  // resend path) all agree. Surface a WARN when the input actually
  // shortens so operators learn at startup that an ≥8-char configured
  // id is being normalized — and that two configured ids with the same
  // first 7 chars would collide under the canonical form.
  const std::string canonical_id = truncate_connection_id(connection_id);
  if(canonical_id != connection_id)
  {
    RCLCPP_WARN_STREAM(logger_,
      "Connection id '" << connection_id << "' truncated to '"
      << canonical_id << "' for the on-wire form (limit "
      << (maximum_connection_id_size - 1) << " chars). Rename the "
         "configured id to avoid the truncation; two configured ids "
         "sharing the first " << (maximum_connection_id_size - 1)
      << " characters will collide under this canonical form.");
  }
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto& slot = connections_[canonical_id];
  if(slot)
  {
    // A Connection already exists at this canonical key. Two cases:
    //   1) Same (host, port): caller missed a connection() lookup and
    //      reached us defensively. Return the existing entry — clobbering
    //      it would wipe sent_packets_ / received-rate state for no
    //      benefit. The other call sites already do the
    //      `if(!c) c = newConnection(...)` dance; the no-op here makes
    //      the rule "newConnection is idempotent for a given canonical
    //      key" hold for any future caller too.
    //   2) Different (host, port): canonical-id collision — two distinct
    //      configured ids share the first `maximum_connection_id_size - 1`
    //      chars (e.g. "starlink-ku" / "starlink-vp" both canonicalize
    //      to "starlin"). The first-registered Connection is kept; the
    //      second is rejected. Log an ERROR so a misconfigured
    //      deployment surfaces at startup rather than presenting as
    //      silent state-clobber on the next RemoteNode::update(Remote)
    //      refresh.
    if(slot->host() != host || slot->port() != port)
    {
      RCLCPP_ERROR_STREAM(logger_,
        "Connection id collision: '" << connection_id
        << "' canonicalizes to '" << canonical_id
        << "' but a Connection already exists at that key with "
           "host=" << slot->host() << " port=" << slot->port()
        << "; new request was host=" << host << " port=" << port
        << ". The first-registered connection is kept. Rename one of "
           "the configured ids so they don't share the first "
        << (maximum_connection_id_size - 1) << " characters.");
    }
    return slot;
  }
  slot = std::make_shared<Connection>(canonical_id, host, port);
  return slot;
}

void RemoteNode::adoptConnection(std::shared_ptr<Connection> connection)
{
  if(!connection)
    return;
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto& slot = connections_[connection->id()];
  if(!slot)
    slot = connection;
}

void RemoteNode::dispatchResendRequest(const ResendRequest& rr, int socket, rclcpp::Time now)
{
  // Snapshot the target connection (or, on miss, the set of known ids
  // and a flag for whether this is the first miss for this id) under
  // state_mutex_, then release the mutex before calling resend_packets
  // / emitting the log — Connection::resend_packets does socket I/O
  // and rclcpp logging macros do their own internal locking that we
  // don't want nested under state_mutex_.
  std::shared_ptr<Connection> target;
  std::vector<std::string> known_ids;
  bool first_miss_for_id = false;
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    auto it = connections_.find(rr.connection_id);
    if(it != connections_.end())
      target = it->second;
    if(!target)
    {
      // First-miss-per-id bookkeeping with a bounded FIFO. We pick
      // WARN vs DEBUG below by whether this id has already been
      // warned for: first occurrence surfaces (so a real config
      // mismatch is diagnosable at default log levels); subsequent
      // occurrences drop to DEBUG so legitimate CONNECT-cycle races
      // don't spam. When the bookkeeping table is full, we evict the
      // oldest entry FIFO-style — that id becomes WARN-eligible
      // again if seen later, which is the right behavior for a
      // rate-limit table (vs permanently silencing a stale id). See
      // the field comment on dispatch_miss_warned_ids_ in
      // remote_node.h for the threat-model rationale (network-
      // supplied strings shouldn't be able to grow this table
      // without bound, even with the 7-byte size clamp).
      if(dispatch_miss_warned_ids_.count(rr.connection_id) != 0)
      {
        first_miss_for_id = false;
      }
      else
      {
        if(dispatch_miss_warned_order_.size() >= kDispatchMissWarnedCap)
        {
          dispatch_miss_warned_ids_.erase(dispatch_miss_warned_order_.front());
          dispatch_miss_warned_order_.pop_front();
        }
        dispatch_miss_warned_order_.push_back(rr.connection_id);
        dispatch_miss_warned_ids_.insert(rr.connection_id);
        first_miss_for_id = true;
      }
      // Snapshot known ids only when we'll WARN below — the DEBUG
      // branch omits the list (already shown on the prior WARN line)
      // so building it here would be wasted work on the common
      // repeated-miss / cap-saturated paths.
      if(first_miss_for_id)
      {
        known_ids.reserve(connections_.size());
        for(const auto& kv : connections_)
          known_ids.push_back(kv.first);
      }
    }
  }
  if(target)
  {
    target->resend_packets(rr.missing_packets, socket, now);
    return;
  }
  // Lookup miss. Two operational scenarios share this path:
  //   - Legitimate race: receiver tore the connection down for a
  //     CONNECT cycle or config reload between the sender stamping the
  //     request and us decoding it. Resolves on the next BridgeInfo.
  //   - Coordinated-redeploy mismatch: the two bridges' connection-id
  //     strings don't agree (or, with this PR's truncation, an id
  //     ≥ maximum_connection_id_size that was somehow stamped
  //     untruncated reaches the receiver). Does not self-resolve.
  // Both deserve diagnostic output. The known-ids list and the
  // first-vs-subsequent split together let an operator distinguish:
  // a one-shot WARN that never repeats is a transient race; repeated
  // hits on the same id (now DEBUG) suggest the race is ongoing. The
  // DEBUG path omits the known-ids list — the WARN that fired on the
  // first miss already showed it, and avoiding the per-DEBUG join
  // keeps the lookup-miss path cheap even when the table is
  // saturated (cap-eviction scenarios re-WARN with a fresh known_ids
  // list, so an operator chasing a repeating issue always has the
  // current ids on the most recent WARN line).
  if(first_miss_for_id)
  {
    std::string joined;
    for(size_t i = 0; i < known_ids.size(); ++i)
    {
      if(i > 0) joined += ", ";
      joined += known_ids[i];
    }
    RCLCPP_WARN_STREAM(logger_,
      "Dropping ResendRequest from " << name_
      << " stamped for connection_id='" << rr.connection_id
      << "' (no matching connection; known ids: [" << joined << "]). "
         "Further misses for this id will log at DEBUG (until the "
         "rate-limit table evicts this id under pressure, at which "
         "point the WARN re-fires on next sighting).");
  }
  else
  {
    RCLCPP_DEBUG_STREAM(logger_,
      "Dropping ResendRequest from " << name_
      << " stamped for connection_id='" << rr.connection_id
      << "' (no matching connection; see the prior WARN line for "
         "this id for the known-ids snapshot)");
  }
}


std::vector<uint8_t> RemoteNode::unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  if(message.size() < sizeof(SequencedPacketHeader) || reinterpret_cast<const SequencedPacketHeader*>(message.data())->packet_size != message.size())
  {
    RCLCPP_ERROR_STREAM(logger_, "Can't unwrap packet of size " << message.size());
    if(message.size() >= sizeof(SequencedPacketHeader))
      RCLCPP_ERROR_STREAM(logger_, "Packet reports size: " << reinterpret_cast<const SequencedPacketHeader*>(message.data())->packet_size);
    return {};
  }

  auto packet = reinterpret_cast<const SequencedPacket*>(message.data());
  std::shared_ptr<Connection> c;
  bool duplicate = false;
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    duplicate = received_packet_times_.find(packet->packet_number) != received_packet_times_.end();
    c = connection(packet->connection_id);  // re-enters mutex (recursive)
    if(!c)
      c = newConnection(packet->connection_id, source_info.host, source_info.port);
    auto now = clock_->now();
    if(!duplicate)
      recordPacketArrival(packet->packet_number, now);
  }

  // update_last_receive_time uses Connection's own mutex; safe to call
  // outside our state_mutex_.
  c->update_last_receive_time(clock_->now().seconds(), message.size(), duplicate);

  if(!duplicate)
  {
    try
    {
      return std::vector<uint8_t>(message.begin()+sizeof(SequencedPacketHeader), message.end());
    }
    catch(const std::exception& e)
    {
      RCLCPP_ERROR_STREAM(logger_, "problem decoding packet: " << e.what());
    }
  }
  return {};
}

bool RemoteNode::admitForPublish(const std::string& topic, uint64_t packet_number)
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto it = highest_published_packet_number_.find(topic);
  if(it != highest_published_packet_number_.end())
  {
    // Strict: an equal-or-newer message for this topic was already
    // admitted, so this is a superseded late resend. (Exact
    // packet_number duplicates never reach here — RemoteNode::unwrap
    // filters them via received_packet_times_ before decode.)
    if(packet_number < it->second)
      return false;
    it->second = packet_number;
  }
  else
  {
    highest_published_packet_number_[topic] = packet_number;
  }
  return true;
}

RemoteNode::AdmitResult RemoteNode::admitOrBuffer(const std::string& topic,
                                                  uint64_t packet_number,
                                                  PublishItem&& item,
                                                  rclcpp::Time now)
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  AdmitResult result;

  auto buf_it = reorder_buffer_.find(topic);
  if(buf_it != reorder_buffer_.end())
  {
    const uint64_t buffered_number = buf_it->second.packet_number;
    if(packet_number == buffered_number)
    {
      // Same number as the held packet. Exact-number duplicates are
      // filtered upstream by received_packet_times_ in unwrap(), so this
      // is defensive — keep the held copy, drop the arrival.
      result.decision = AdmitDecision::Drop;
      return result;
    }
    if(packet_number < buffered_number)
    {
      // The arrival belongs *before* the held packet — a candidate
      // gap-filler. Admit it against the high-water mark: if it's still
      // stale (below the mark) admitForPublish drops it and the held
      // packet is untouched.
      if(admitForPublish(topic, packet_number))
      {
        result.decision = AdmitDecision::Admit;
        result.to_publish.push_back(std::move(item));
        // Did admitting this packet make the held one contiguous? The
        // mark now equals packet_number, so B==packet_number+1 means the
        // gap is closed — release the held packet right behind it, in
        // order.
        if(buffered_number == packet_number + 1)
        {
          result.to_publish.push_back(std::move(buf_it->second.item));
          reorder_buffer_.erase(buf_it);
          admitForPublish(topic, buffered_number);  // advance mark to B
        }
      }
      else
      {
        result.decision = AdmitDecision::Drop;
      }
      return result;
    }
    // packet_number > buffered_number: the arrival is newer than the held
    // packet. At-most-one slot overflow — flush the held packet now (in
    // order, ahead of anything the arrival triggers) and advance the mark
    // to it, then resolve the arrival against the advanced mark below.
    result.to_publish.push_back(std::move(buf_it->second.item));
    admitForPublish(topic, buffered_number);
    reorder_buffer_.erase(buf_it);
  }

  // No packet buffered for this topic (either never was, or the overflow
  // branch above just flushed it). Resolve the arrival against the
  // high-water mark, mirroring admitForPublish's admit rule but splitting
  // the gap case out to a Buffer decision.
  auto hw_it = highest_published_packet_number_.find(topic);
  if(hw_it == highest_published_packet_number_.end())
  {
    // First packet seen for this topic — always admitted (matches
    // admitForPublish). No prior number, so no gap can be inferred.
    admitForPublish(topic, packet_number);
    result.decision = AdmitDecision::Admit;
    result.to_publish.push_back(std::move(item));
  }
  else if(packet_number < hw_it->second)
  {
    // Stale: superseded by a newer already-published packet.
    result.decision = AdmitDecision::Drop;
  }
  else if(packet_number <= hw_it->second + 1)
  {
    // Contiguous (== high-water+1) or equal to the mark: admit and
    // advance. (An exact-mark == is not a duplicate here — duplicates are
    // filtered upstream; see admitForPublish.)
    admitForPublish(topic, packet_number);
    result.decision = AdmitDecision::Admit;
    result.to_publish.push_back(std::move(item));
  }
  else
  {
    // Gap (>= high-water+2): hold the packet without advancing the mark,
    // recording its arrival time for the window-expiry check.
    ReorderBufferEntry entry;
    entry.packet_number = packet_number;
    entry.arrival_time = now;
    entry.item = std::move(item);
    reorder_buffer_[topic] = std::move(entry);
    ++reorder_buffered_total_;
    result.decision = AdmitDecision::Buffer;
  }
  return result;
}

std::vector<PublishItem> RemoteNode::flushExpiredBuffer(rclcpp::Time now,
                                                        rclcpp::Duration hold_window)
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  std::vector<PublishItem> released;
  for(auto it = reorder_buffer_.begin(); it != reorder_buffer_.end(); )
  {
    if((now - it->second.arrival_time) >= hold_window)
    {
      // Window expired before the gap was filled: release the held packet
      // and advance the mark to it. A later-arriving lower (gap-filling)
      // packet is then correctly dropped as stale by the gate.
      admitForPublish(it->first, it->second.packet_number);
      released.push_back(std::move(it->second.item));
      it = reorder_buffer_.erase(it);
      ++reorder_expired_total_;
    }
    else
    {
      ++it;
    }
  }
  return released;
}

std::size_t RemoteNode::reorderBufferOccupancy() const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return reorder_buffer_.size();
}

uint32_t RemoteNode::reorderBufferedTotal() const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return reorder_buffered_total_;
}

uint32_t RemoteNode::reorderExpiredTotal() const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return reorder_expired_total_;
}

void RemoteNode::clearReorderBuffer()
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  reorder_buffer_.clear();
}

Defragmenter& RemoteNode::defragmenter()
{
  return defragmenter_;
}

void RemoteNode::publishTopicStatistics(const TopicStatisticsArray& statistics)
{
  topic_statistics_publisher_->publish(statistics);
}

void RemoteNode::clearReceivedPacketTimesBefore(rclcpp::Time time)
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  std::vector<uint64_t> expired;
  for(auto pt: received_packet_times_)
    if(pt.second < time)
      expired.push_back(pt.first);
  for(auto e: expired)
    received_packet_times_.erase(e);

  // Eviction of resend_state_ entries by first_request_time used to
  // live here too. It's now handled by three other paths: the give-up
  // sweep at the top of getMissingPackets() (removes entries whose
  // first_request_time < now - kSentPacketTTL), recordPacketArrival
  // (removes entries when the missing packet finally arrives), and
  // the remote-restart clear in update(BridgeInfo).
  //
  // Under the current default (kReceiveHistoryWindow == kSentPacketTTL),
  // the old eviction-by-time sweep and the give-up sweep would have
  // hit exactly the same entries, so removing the old sweep was a
  // pure no-op.
  //
  // Under a future narrowing of kReceiveHistoryWindow (the
  // static_assert in resend_constants.h enforces `<=`, so narrowing
  // is allowed), the math reverses: the `time` cutoff passed in here
  // (now - kReceiveHistoryWindow) is *newer* than the give-up cutoff
  // (now - kSentPacketTTL), so the give-up sweep evicts a *subset*
  // of what the old eviction would have. Entries with
  // first_request_time in [giveup_cutoff, time) — still inside the
  // sender's TTL window but older than the receiver's history window
  // — would be left behind. They become orphans when the gap-scan
  // range (bounded by received_packet_times_.begin()/rbegin()) no
  // longer includes their packet number; nothing reads or modifies
  // them until first_request_time finally ages past kSentPacketTTL,
  // at which point the give-up sweep evicts them and bumps the
  // counter. Memory growth is bounded by
  // (kSentPacketTTL - kReceiveHistoryWindow) * packet_rate. If
  // narrowing is ever adopted, decide then whether to re-add a
  // time-based eviction here.

  // Prune given-up packet numbers that have fallen out of the
  // gap-scan range. The gap-scan walks [received_packet_times_.begin()
  // + 1, rbegin()), so any given-up packet number <= begin()->first
  // can never be re-flagged as missing — drop it. If
  // received_packet_times_ is empty after the eviction above, no
  // future gap-scan in this call's lifetime can re-flag anything, so
  // clear the whole set.
  if(received_packet_times_.empty())
  {
    given_up_packet_numbers_.clear();
  }
  else
  {
    auto begin_packet = received_packet_times_.begin()->first;
    auto it = given_up_packet_numbers_.begin();
    while(it != given_up_packet_numbers_.end() && *it <= begin_packet)
      it = given_up_packet_numbers_.erase(it);
  }
}

uint32_t RemoteNode::resendGiveupCount() const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return resend_giveup_count_;
}

#ifdef UDP_BRIDGE_BUILD_TESTING
void RemoteNode::recordReceivedPacketTimeForTest(uint64_t packet_number, rclcpp::Time time)
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  recordPacketArrival(packet_number, time);
}

std::size_t RemoteNode::dispatchMissWarnedIdCountForTest() const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return dispatch_miss_warned_ids_.size();
}

bool RemoteNode::isDispatchMissWarnedForTest(const std::string& id) const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return dispatch_miss_warned_ids_.count(id) != 0;
}

int64_t RemoteNode::highestPublishedForTest(const std::string& topic) const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto it = highest_published_packet_number_.find(topic);
  if(it == highest_published_packet_number_.end())
    return -1;
  return static_cast<int64_t>(it->second);
}

int64_t RemoteNode::bufferedPacketNumberForTest(const std::string& topic) const
{
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto it = reorder_buffer_.find(topic);
  if(it == reorder_buffer_.end())
    return -1;
  return static_cast<int64_t>(it->second.packet_number);
}
#endif  // UDP_BRIDGE_BUILD_TESTING

void RemoteNode::recordPacketArrival(uint64_t packet_number, rclcpp::Time time)
{
  // Caller holds state_mutex_. An arrived packet is no longer missing,
  // so the receive-time map is updated AND any pending resend tracking
  // is cleared. Without the resend_state_ erase, the give-up sweep in
  // getMissingPackets() would later iterate the lingering entry and
  // falsely treat the arrived packet as abandoned, inflating
  // resend_giveup_count_ for packets that actually came through.
  // given_up_packet_numbers_ is left alone: if we already gave up and
  // the packet arrived late, the give-up was wasted but unwinding the
  // counter is wrong.
  received_packet_times_[packet_number] = time;
  resend_state_.erase(packet_number);
}

ResendRequest RemoteNode::getMissingPackets()
{
  return getMissingPacketsAt(clock_->now());
}

#ifdef UDP_BRIDGE_BUILD_TESTING
ResendRequest RemoteNode::getMissingPackets(rclcpp::Time now)
{
  return getMissingPacketsAt(now);
}
#endif  // UDP_BRIDGE_BUILD_TESTING

ResendRequest RemoteNode::getMissingPacketsAt(rclcpp::Time now)
{
  // Zero-time guard for pre-clock-sync callers: a zero-init
  // rclcpp::Time would make `now - rclcpp::Duration(kSentPacketTTL)`
  // construct a negative time, which rclcpp::Time throws on. Lives in
  // the shared worker so both public entry points (the production
  // no-arg variant and the UDP_BRIDGE_BUILD_TESTING-gated time-injection
  // overload) get the same safety contract from a single source.
  if(now.nanoseconds() == 0)
    return {};
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  auto giveup_cutoff = now - rclcpp::Duration(kSentPacketTTL);

  // Pass 1: give-up sweep. Process resend_state_ entries whose
  // first_request_time has aged past kSentPacketTTL BEFORE running
  // clearReceivedPacketTimesBefore. The static_assert in
  // resend_constants.h enforces kReceiveHistoryWindow <=
  // kSentPacketTTL — they're equal today by default but the assert
  // allows the receive window to be narrowed. Even when the two
  // cutoffs differ, the give-up sweep must run first: it would still
  // be wrong to evict a state entry whose first_request_time is
  // older than the eviction cutoff without counting the give-up
  // (resend_giveup_count_ would stop tracking abandoned packets,
  // breaking the operational-visibility contract on Remote.msg). The
  // packet number is moved into given_up_packet_numbers_ so the
  // backoff loop below can't re-request it via
  // operator[]-default-construct on the resend_state_ map.
  std::vector<uint64_t> giveup_now;
  for(auto& kv: resend_state_)
    if(kv.second.attempts > 0 && kv.second.first_request_time < giveup_cutoff)
      giveup_now.push_back(kv.first);
  for(auto m: giveup_now)
  {
    auto& s = resend_state_[m];
    // Invariant: every resend_state_ entry is created with attempts=1
    // in the backoff loop below; nothing decrements. The runtime
    // `attempts > 0` filter above is the Release-build safety net; the
    // assert below is the Debug-build tripwire. Keep both — the
    // runtime cost is one integer compare and a Release build with a
    // zero-attempts entry would otherwise log "after 0 attempts"
    // silently.
    assert(s.attempts > 0);
    RCLCPP_DEBUG_STREAM(logger_,
      "Giving up on resend of packet " << m << " from remote '"
      << name_ << "' after " << s.attempts
      << " attempts (first requested "
      << (now - s.first_request_time).seconds()
      << " s ago, sender TTL is " << kSentPacketTTL.count() << " s)");
    resend_state_.erase(m);
    given_up_packet_numbers_.insert(m);
    ++resend_giveup_count_;
  }

  auto too_old = now - rclcpp::Duration(kReceiveHistoryWindow);
  clearReceivedPacketTimesBefore(too_old);  // re-enters mutex (recursive)
  auto debounce_cutoff = now - rclcpp::Duration(kResendDebounceHold);

  // Walk packet numbers in [first+1, last-1] of the known-received range
  // and flag gaps as missing. Each gap is debounced against the receive
  // time of its previous known neighbor: a reordered late-arriving
  // packet within kResendDebounceHold of its neighbor is given time to
  // arrive before we request a resend. prev_received_time tracks the
  // most recent receive time as we walk forward; for runs of consecutive
  // gaps it stays pinned to the packet just before the run, which is
  // the right anchor since those gaps were sent at roughly the same
  // wall-clock time.
  std::vector<uint64_t> missing;
  if(!received_packet_times_.empty())
  {
    rclcpp::Time prev_received_time = received_packet_times_.begin()->second;
    for(auto i = received_packet_times_.begin()->first+1; i < received_packet_times_.rbegin()->first; i++)
    {
      auto it = received_packet_times_.find(i);
      if(it == received_packet_times_.end())
      {
        if(prev_received_time < debounce_cutoff)
          missing.push_back(i);
      }
      else
      {
        prev_received_time = it->second;
      }
    }
  }

  // Backoff for missing packets observed in the gap-scan above. The
  // give-up case (first_request_time < giveup_cutoff) was already
  // handled in pass 1 at the top of this function.
  //
  // Invariant: when multiple missing packets cross their backoff
  // boundary in the same tick, they bundle into one ResendRequest. By
  // design — don't split into per-packet requests in a future
  // refactor.
  ResendRequest rr;
  for(auto m: missing)
  {
    // Skip packets we have already given up on. Without this guard,
    // resend_state_[m] below would operator[]-default-construct a
    // fresh entry (attempts=0), hit the "first observed missing"
    // branch, and re-request the packet — re-arming a full
    // kSentPacketTTL window of attempts for a packet the sender has
    // already evicted. The given_up set is pruned by
    // clearReceivedPacketTimesBefore once the packet number falls
    // out of the gap-scan range.
    // count() rather than C++20 contains() — udp_bridge's CMakeLists
    // sets CXX_STANDARD 17.
    if(given_up_packet_numbers_.count(m))
      continue;
    auto& state = resend_state_[m];
    if(state.attempts == 0)
    {
      rr.missing_packets.push_back(m);
      state.first_request_time = now;
      state.last_request_time = now;
      state.attempts = 1;
      continue;
    }
    // Clamp the shift so a stuck packet that somehow climbs past the
    // attempts a normal kSentPacketTTL window admits (~6) cannot hit
    // undefined behavior at the shift expression. The bound
    // (kMaxBackoffShift) is chosen so the shifted value is already
    // strictly greater than kResendBackoffCap; the cap clause below
    // then pins the cooldown to the cap, so observable behavior is
    // unchanged — the clamp purely guards the shift's well-definedness.
    uint32_t shift = state.attempts - 1;
    if(shift > kMaxBackoffShift)
      shift = kMaxBackoffShift;
    double cooldown = kResendBackoffBase.count() * (1u << shift);
    if(cooldown > kResendBackoffCap.count())
      cooldown = kResendBackoffCap.count();
    if((now - state.last_request_time).seconds() >= cooldown)
    {
      rr.missing_packets.push_back(m);
      state.last_request_time = now;
      ++state.attempts;
    }
  }
  return rr;
}

} // namespace udp_bridge
