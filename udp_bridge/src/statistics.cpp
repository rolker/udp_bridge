#include "udp_bridge/statistics.h"

#include <algorithm>

namespace udp_bridge
{

void MessageStatistics::add(const MessageSizeData& data)
{
  //ROS_DEBUG_STREAM_NAMED("statistics", "msg size: " << data.message_size << " sent size: " << data.sent_size);
  for(auto result: data.send_results)
  {
    //ROS_DEBUG_STREAM_NAMED("statistics", "  remote: " << result.first);
    for(auto connection: result.second)
      switch (connection.second)
      {
      case SendResult::success:
        //ROS_DEBUG_STREAM_NAMED("statistics", "    " << connection.first << " send result: success");
        break;
      case SendResult::failed:
        //ROS_DEBUG_STREAM_NAMED("statistics", "    " << connection.first << " send result: failed");
        break;
      case SendResult::dropped:
        //ROS_DEBUG_STREAM_NAMED("statistics", "    " << connection.first << " send result: dropped");
        break;
      }
  }

  Statistics<MessageSizeData>::add(data);
}

std::vector<udp_bridge_interfaces::msg::TopicStatistics> MessageStatistics::get()
{
  struct Totals
  {
    int total_message_size = 0;
    int total_fragment_count = 0;
    int total_ok_sent_bytes = 0;
    int total_failed_sent_bytes = 0;
    uint32_t total_dropped_bytes = 0;
    int total_data_point_count = 0;
    rclcpp::Time earliest;
    rclcpp::Time latest;
  };

  std::map<std::pair<std::string, std::string>, Totals> totals_by_connection;

  for(auto data_point: data_)
    for(auto remote_send_result: data_point.send_results)
      for(auto connection_send_result: remote_send_result.second)
      {
        Totals& totals = totals_by_connection[std::make_pair(remote_send_result.first, connection_send_result.first)];
        totals.total_message_size += data_point.message_size;
        totals.total_fragment_count += data_point.fragment_count;
        switch(connection_send_result.second)
        {
        case SendResult::success:
          totals.total_ok_sent_bytes += data_point.sent_size;
          break;
        case SendResult::failed:
          totals.total_failed_sent_bytes += data_point.sent_size;
          break;
        case SendResult::dropped:
          totals.total_dropped_bytes += data_point.sent_size;
          break;
        }
        totals.total_data_point_count++;
        if(totals.latest.nanoseconds() == 0 || data_point.timestamp > totals.latest)
          totals.latest = data_point.timestamp;
        if(totals.earliest.nanoseconds() == 0 || data_point.timestamp < totals.earliest)
          totals.earliest = data_point.timestamp;
      }

  std::vector<udp_bridge_interfaces::msg::TopicStatistics> ret;

  for(auto totals: totals_by_connection)
  {
    udp_bridge_interfaces::msg::TopicStatistics ts;
    ts.destination_node = totals.first.first;
    ts.connection_id = totals.first.second;
    auto& data = totals.second;
    ts.stamp = data.latest;
    float count = data.total_data_point_count;
    ts.average_fragment_count = data.total_fragment_count/count;

    if(data.latest > data.earliest)
    {
      double time_span = (data.latest-data.earliest).seconds();

      // account for the time until the next sample
      time_span *= (count+1)/count;

      ts.messages_per_second = count/time_span;
      ts.message_bytes_per_second = data.total_message_size/time_span;
      ts.send.success_bytes_per_second = data.total_ok_sent_bytes/time_span;
      ts.send.failed_bytes_per_second = data.total_failed_sent_bytes/time_span;
      ts.send.dropped_bytes_per_second = data.total_dropped_bytes/time_span;
    }

    ret.push_back(ts);

  }

  return ret;
}

udp_bridge_interfaces::msg::DataRates PacketSendStatistics::get() const
{
  return get(nullptr);
}

udp_bridge_interfaces::msg::DataRates PacketSendStatistics::get(PacketSendCategory category) const
{
  return get(&category);
}

udp_bridge_interfaces::msg::DataRates PacketSendStatistics::get(PacketSendCategory* category) const
{
  udp_bridge_interfaces::msg::DataRates ret;
  rclcpp::Time earliest;
  rclcpp::Time latest;

  for(auto data_point: data_)
    if(category==nullptr || data_point.category == *category)
    {
      switch(data_point.send_result)
      {
      case SendResult::success:
        ret.success_bytes_per_second += data_point.size;
        break;
      case SendResult::failed:
        ret.failed_bytes_per_second += data_point.size;
        break;
      case SendResult::dropped:
        ret.dropped_bytes_per_second += data_point.size;
        break;
      }
      if(latest.nanoseconds() == 0 || data_point.timestamp > latest)
        latest = data_point.timestamp;
      if(earliest.nanoseconds() == 0 || data_point.timestamp < earliest)
        earliest = data_point.timestamp;
    }

  // If time span is less than 1 sec, assume it's 1 sec
  // to avoid data rate spikes at the start of sampling
  if(latest > earliest)
  {
    double time_span = (latest-earliest).seconds();
    if(time_span > 1.0)
    {
      ret.success_bytes_per_second /= time_span;
      ret.failed_bytes_per_second /= time_span;
      ret.dropped_bytes_per_second /= time_span;
    }
  }
  return ret;
}

uint64_t PacketSendStatistics::bytes_in_window(PacketSendCategory category, rclcpp::Time time) const
{
  // Same full-deque scan as can_send (see the ordering rationale there:
  // the reserve-then-record pattern means the deque is not monotone in
  // timestamps, so a skip-prefix scan would be incorrect; the deque is
  // bounded to ~10 s of records, so the linear scan is cheap).
  auto one_second_ago = time - rclcpp::Duration::from_seconds(1.0);
  uint64_t total = 0;
  for(const auto& entry : data_)
  {
    // Bounded at BOTH ends, for the same reason can_send is (#52, review
    // round 3): a record stamped after `time` is not evidence about the
    // window ending at `time`, and after a backwards clock step the
    // deque is full of them. Counting them here would over-report the
    // resend spend and shed resends that the budget has room for.
    if(entry.timestamp < one_second_ago || entry.timestamp > time)
      continue;
    if(entry.send_result == SendResult::dropped)
      continue;
    if(entry.category != category)
      continue;
    total += entry.size;
  }
  return total;
}

float PacketSendStatistics::success_rate_in_window(rclcpp::Time time, double window_seconds) const
{
  // Same full-deque scan as can_send / bytes_in_window — the deque is not
  // monotone in timestamps under the reserve-then-record pattern, so a
  // skip-prefix scan would be incorrect. Bounded to ~10 s of records.
  const auto window_start = time - rclcpp::Duration::from_seconds(window_seconds);
  uint64_t total = 0;
  bool have_earliest = false;
  rclcpp::Time earliest;
  for(const auto& entry : data_)
  {
    // Bounded at BOTH ends. Bounding only below let a backwards clock
    // step — a looping bag replay, a sim reset — sum up to the deque's
    // full 10 s of now-future-stamped successes and divide them by the
    // 1 s dt floor, inflating the reported rate roughly tenfold and
    // reading every following sample as congested on a link that never
    // degraded (#52, review round 2). A sample stamped after `time` is
    // not evidence about the window ending at `time`.
    if(entry.timestamp < window_start || entry.timestamp > time)
      continue;
    if(entry.send_result != SendResult::success)
      continue;
    total += entry.size;
    // `have_earliest` rather than `earliest.nanoseconds() == 0`: a
    // timestamp of 0 is a legal clock reading, not a sentinel. It is the
    // same conflation the refractory gate had removed in round 1, and
    // relying on Statistics::add dropping zero-stamped records would
    // make this correct only by an incidental property of another class.
    if(!have_earliest || entry.timestamp < earliest)
    {
      earliest = entry.timestamp;
      have_earliest = true;
    }
  }
  if(total == 0)
    return 0.0f;
  // dt floor of 1 s mirrors data_receive_rate: a window holding only a
  // few hundred milliseconds of samples must not report a rate spike.
  //
  // The span is measured from the oldest SUCCESSFUL sample in the
  // window, whereas data_receive_rate measures from the oldest sample of
  // any kind. When successes cluster late in the window — the throttled
  // regime, where early attempts fail and later ones land — this reports
  // a shorter span and so a higher rate than the receive-side filter
  // would for the same traffic, biasing the comparison toward reading
  // congestion. Left as-is deliberately: the alternative (measuring from
  // the oldest attempt, successful or not) makes the divisor depend on
  // failures the remote never saw, and the residual skew is bounded by
  // the window and is in the conservative direction — it can only make
  // the controller back off sooner, never later. Documented rather than
  // silently differing (#52, review round 2).
  double dt = 1.0;
  if(have_earliest)
    dt = std::max(dt, (time - earliest).seconds());
  return static_cast<float>(static_cast<double>(total) / dt);
}

bool PacketSendStatistics::can_send(uint32_t data_size, uint32_t reserved_bytes, uint32_t bytes_per_second_limit, rclcpp::Time time) const
{
  // Scan the whole deque rather than skip-prefix-then-sum. The earlier
  // implementation walked forward past entries with `timestamp < one_second_ago`
  // and then summed everything from that point on, which assumed the deque
  // was monotone in timestamps. PR #16's reserve-then-record pattern in
  // Connection::send breaks that assumption: with sendto running outside
  // the mutex and varying in duration under contention/back-pressure, a
  // slower thread can add its record (with an older start-timestamp) AFTER
  // a faster thread's record, leaving an old entry sitting past the "first
  // not-old" boundary and getting incorrectly summed. The deque is bounded
  // to ~10 s of records by Statistics::add's eviction, so the linear scan
  // is cheap and correct regardless of insertion order.
  auto one_second_ago = time - rclcpp::Duration::from_seconds(1.0);
  // Use uint64_t for the sum and the comparison. Each operand is uint32_t,
  // and on realistic configurations the sum stays well below UINT32_MAX
  // (deque entries are uint16_t-sized PacketSizeData::size, bounded to
  // ~10 s of records), but a future config raising default_rate_limit or
  // moving to jumbo-frame data_size could push the arithmetic into wrap
  // territory. Widening here means the comparison stays valid across the
  // full uint32_t input range with no further audit required.
  uint64_t total_sent = 0;
  for(const auto& entry : data_)
  {
    // Bounded at BOTH ends (#52, review round 3). Bounding only below
    // permanently WEDGES the connection across a backwards clock step —
    // a looping bag replay, a sim reset, an NTP correction. Every record
    // written before the step is stamped in what is now the future, and
    // summing them makes total_sent exceed any sane limit forever:
    // can_send returns false on every call, the connection admits
    // nothing, and it presents to an operator as "the link stopped".
    // Statistics::add now evicts those records too, so this is the
    // second of two independent bounds rather than the only one; keep
    // both, because the eviction runs on add and a connection that has
    // gone quiet across the step would not get one.
    //
    // A record stamped after `time` is not evidence about the window
    // ending at `time`. Excluding it can only under-count, i.e. admit
    // slightly more, which is the safe direction for a bound whose
    // failure mode is a dead link.
    if(entry.timestamp < one_second_ago || entry.timestamp > time)
      continue;
    if(entry.send_result == SendResult::dropped)
      continue;
    total_sent += entry.size;
  }
  return (total_sent
          + static_cast<uint64_t>(reserved_bytes)
          + static_cast<uint64_t>(data_size))
         < static_cast<uint64_t>(bytes_per_second_limit);
}


} // namespace udp_bridge
