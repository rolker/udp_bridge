#include "udp_bridge/connection.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
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
  const float old_limit = static_cast<float>(data_rate_limit_);
  if(maximum_bytes_per_second == 0)
    data_rate_limit_ = default_rate_limit;
  else
    data_rate_limit_ = maximum_bytes_per_second;
  // AIMD cap follow/clamp (issue #43). If no backoff had accumulated
  // (effective was at the old limit — including the freshly-constructed
  // state where both hold default_rate_limit), the effective cap
  // follows the new limit so configuring a larger cap takes effect
  // immediately. If backoff HAD accumulated, clamp to
  // min(effective, new) — never reset: CONNECT/adopt and addRemote
  // re-apply the rate limit even when unchanged, and a reset would
  // wipe the backoff and burst a congested link at full rate on every
  // reconnect flap. A backed-off connection reaches a raised limit via
  // normal additive recovery in updateAdmissionControl.
  if(effective_rate_limit_ >= old_limit)
    effective_rate_limit_ = static_cast<float>(data_rate_limit_);
  else
    effective_rate_limit_ = std::min(effective_rate_limit_,
                                     static_cast<float>(data_rate_limit_));
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
  // NaN has no nearest bound — std::min/max would pass it through to
  // the most-permissive 1.0 — so it falls back to the default instead.
  if(std::isnan(fraction))
    fraction = kDefaultResendBudgetFraction;
  fraction = std::max(0.0f, std::min(1.0f, fraction));
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  resend_budget_fraction_ = fraction;
}

float Connection::resendBudgetFraction() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return resend_budget_fraction_;
}

void Connection::setAdmissionFloorBytesPerSecond(float bytes_per_second)
{
  // NaN and negative values fall back to the default (the contract in
  // connection.h). A negative floor must NOT be silently clamped to 0:
  // 0 disables the lockout floor entirely, removing the control-tier /
  // feedback-loop protection the floor exists to guarantee — a
  // misconfiguration should degrade to the safe default, not to "off".
  // No upper clamp here — the floor is bounded by the configured rate
  // limit where it is applied, so a connection whose limit is later
  // raised does not carry a silently truncated floor.
  if(std::isnan(bytes_per_second) || bytes_per_second < 0.0f)
    bytes_per_second = kDefaultAdmissionFloorBytesPerSecond;
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  admission_floor_bytes_per_second_ = bytes_per_second;
}

float Connection::admissionFloorBytesPerSecond() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return admission_floor_bytes_per_second_;
}

void Connection::setAdmissionRefractoryPeriodSeconds(double seconds)
{
  // NaN and negative values fall back to the default (the contract in
  // connection.h). 0 is accepted and means "no gate": every sample is
  // acted on, which is the pre-#52 behaviour. It is reachable on purpose
  // — an operator reproducing the 2026-08-25 collapse needs it — but it
  // is not a safe default, so a garbage value must not land there.
  if(std::isnan(seconds) || seconds < 0.0)
    seconds = kDefaultAdmissionRefractoryPeriodSeconds;
  // Clamp the top end too. +Inf (and any large finite typo) used to be
  // accepted verbatim: one congested sample then froze the controller
  // for the life of the node — no decrease, no recovery, nothing logged.
  // This is the only admission setter that lacked an upper clamp. See
  // kMaximumAdmissionRefractoryPeriodSeconds for why the bound is where
  // it is; clamping (rather than falling back to the default) keeps an
  // operator's intent to slow the loop down while keeping it alive.
  seconds = std::min(seconds, kMaximumAdmissionRefractoryPeriodSeconds);
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  admission_refractory_period_seconds_ = seconds;
  // Reset the in-force window and any accumulated growth. Without this a
  // reconfiguration made while a grown window is open would leave the
  // controller frozen under the OLD value it was just told to abandon.
  admission_refractory_current_seconds_ = seconds;
  admission_decrease_outstanding_ = false;
  last_admission_decrease_time_ = 0.0;
}

double Connection::admissionRefractoryPeriodSeconds() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return admission_refractory_period_seconds_;
}

double Connection::currentAdmissionRefractoryWindowSeconds() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return admission_refractory_current_seconds_;
}

float Connection::goodputBytesPerSecond() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return goodput_bytes_per_second_;
}

uint32_t Connection::effectiveRateLimit() const
{
  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  return static_cast<uint32_t>(effective_rate_limit_);
}

void Connection::updateAdmissionControl(float remote_received_bps,
                                        float remote_duplicate_bps,
                                        rclcpp::Time now)
{
  // Locking: read the stats and receive-history values under their own
  // mutexes first, then take config_mutex_ for the AIMD update — the
  // mutexes are never held simultaneously, so no lock-ordering
  // constraint is created (send() likewise acquires them one at a
  // time). Keep it that way: nesting any of them would introduce an
  // ordering requirement that nothing else in Connection has.
  // Measure OUR send rate over the same window the remote used to
  // produce the figure it is echoing back (issue #52). Previously this
  // read PacketSendStatistics::get(), a variable-span 1-10 s filter,
  // and compared it against the remote's 5 s box filter: 16.1% of the
  // 2026-08-25 field samples reported the remote receiving MORE than we
  // sent, which is physically impossible if both described the same
  // interval and identifies filter skew — not loss — as what the ratio
  // was actually measuring during a transient.
  float sent_bps;
  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    sent_bps = sent_packet_statistics_.success_rate_in_window(
      now, kAdmissionSendRateWindowSeconds);
  }
  const double receive_time = last_receive_time();  // own mutex inside

  // Stale-feedback fallback: nothing received on THIS connection for a
  // starvation interval means its feedback (and likely its delivery)
  // is gone — treat as congested. The 0.0 never-received sentinel is
  // not stale (see resend_packets). Scope boundary: this method runs
  // on BridgeInfo receipt, so a connection in total inbound blackout
  // while no OTHER connection delivers BridgeInfo gets no calls at all
  // — documented in doc/admission_control_design.md.
  const bool feedback_stale = receive_time > 0.0 &&
    (now.seconds() - receive_time) >= kAckStarvationThreshold.count();

  // Congestion trigger: the remote reports receiving meaningfully less
  // than we sent, or we sent and heard nothing back (stale feedback).
  // An idle link (sent_bps == 0) can never be congested — including
  // the stale-feedback case: a dormant connection we are not offering
  // traffic to must not accumulate backoff it would then have to
  // recover from when traffic resumes. The ratio is a trend signal,
  // not an exact loss measure — see the design note's caveats
  // (duplicate/resend inflation, differing smoothing windows,
  // propagation delay).
  //
  // DETECTION compares RAW received against sent, deliberately NOT
  // goodput (#52). Both sides of the ratio are inflated by the same
  // resends: sent_bps counts every byte we put on the wire (fresh +
  // resend), and remote_received_bps counts every byte the remote saw
  // (fresh + duplicate), so the inflation largely cancels in the ratio.
  // Substituting goodput (received − duplicates) on the left while sent
  // still includes our resends would make a healthy link that merely
  // duplicates/reorders read as congested and throttle itself for no
  // loss. Goodput is the right basis for the DECREASE TARGET below (how
  // far to back off once congested), not for deciding WHETHER we are.
  //
  // A non-finite delivery report (NaN/Inf) is unusable feedback, not a
  // clean sample: NaN fails every comparison, so leaving it in the ratio
  // would silently read as "not congested" and defeat the AIMD decrease.
  // Treat it like stale feedback — count it as congestion and fall back
  // to the plain multiplicative decrease, since there is no trustworthy
  // goodput reading to target (#52).
  //
  // The DUPLICATE channel is validated the same way. A non-finite
  // remote_duplicate_bps, or a duplicate rate exceeding received (the
  // remote cannot have duplicated more than it received — a smoothing-
  // window skew, or a hostile report), makes goodput = received −
  // duplicate untrustworthy: it collapses to 0, and left in the headroom
  // target below that slams the cap to the floor on a single sample —
  // the very pathology the received-channel guard prevents, re-entered
  // through the duplicate channel. Mark it unusable so it falls back to
  // the plain halving instead (#52).
  //
  // Negative rates on EITHER channel are nonsensical (a rate cannot be
  // below zero) and reach us over an unauthenticated transport (#53).
  // They pass the finite and duplicate>received checks — e.g. received
  // = −100, duplicate = −200 gives duplicate > received == false — yet a
  // negative received is below any congestion threshold, so the sample
  // would be treated as congested with a headroom target of 0 and slam
  // the cap to the floor in one sample. Reject negatives here so they
  // fall back to the plain halving instead (#52).
  const bool feedback_unusable =
    feedback_stale || !std::isfinite(remote_received_bps) ||
    !std::isfinite(remote_duplicate_bps) ||
    remote_received_bps < 0.0f || remote_duplicate_bps < 0.0f ||
    remote_duplicate_bps > remote_received_bps;
  const bool congested = sent_bps > 0.0f &&
    (feedback_unusable ||
     remote_received_bps < (1.0f - kAdmissionLossThreshold) * sent_bps);

  // Goodput: what the remote reports receiving, less what it reports as
  // duplicates. Resends and duplicates inflate received_bytes_per_second
  // exactly when amplification is worst, so the raw figure flatters the
  // link at the moment we most need the truth (issue #52).
  //
  // Clamp the stored value to [0, received]. A peer reporting a negative
  // duplicate rate would otherwise inflate goodput above what it received
  // (received − (−dup) > received), and the resend-budget basis downstream
  // would size retransmission against a fabricated number. Storing 0 when
  // either channel is non-finite keeps a NaN from ever reaching a consumer
  // (the sample is already treated as unusable feedback above).
  const float goodput =
    (std::isfinite(remote_received_bps) && std::isfinite(remote_duplicate_bps))
      ? std::clamp(remote_received_bps - remote_duplicate_bps,
                   0.0f, std::max(0.0f, remote_received_bps))
      : 0.0f;

  std::lock_guard<std::recursive_mutex> lock(config_mutex_);
  // Store goodput BEFORE the refractory gate below. The gate freezes the
  // controller's DECISION, not its perception: goodput is a measurement
  // the resend budget also consumes, and withholding it would starve an
  // unrelated loop of fresh data for the length of the window.
  goodput_bytes_per_second_ = goodput;

  // REFRACTORY GATE (issue #52). One decision per congestion epoch.
  //
  // The freeze is unconditional on sample type: inside the window a
  // CLEAN sample gets no additive recovery either. Recovering mid-window
  // would re-open the loop from the other side — recover, re-cross the
  // threshold on the next differently-filtered sample, halve again —
  // and it changes the outcome of both committed field-replay
  // regression tests.
  //
  // A backwards clock step (elapsed < 0) expires the window rather than
  // freezing the controller until the clock catches up.
  //
  // "Is a decrease outstanding" is its own boolean and NOT
  // `last_admission_decrease_time_ > 0.0`. 0.0 is a legal clock reading,
  // and a decrease recorded there read as "no decrease outstanding",
  // leaving the gate silently inert and halving the cap on every sample
  // — precisely the cascade this branch exists to stop (#52, review
  // round 1).
  //
  // Reaching it takes more than a clock that merely READS zero: a
  // congested sample needs send history, and `Statistics::add`
  // (statistics.h) drops records stamped exactly 0, so `use_sim_time`
  // before the first `/clock` has nothing to be congested about. What
  // reaches it is a BACKWARDS clock step — a looping bag replay, a sim
  // reset — where history recorded before the step is still in the
  // deque. `AdmissionControl.RefractoryGateHoldsAtClockZero` drives that
  // path and was verified to fail against the sentinel form.
  if(admission_decrease_outstanding_)
  {
    const double elapsed = now.seconds() - last_admission_decrease_time_;
    if(elapsed >= 0.0 && elapsed < admission_refractory_current_seconds_)
      return;
  }

  // The floor is absolute (#52) but can never exceed the configured
  // cap: an operator who sets a floor above a connection's own limit
  // gets the limit, not a floor that would raise it.
  const float floor =
    std::min(admission_floor_bytes_per_second_, static_cast<float>(data_rate_limit_));

  if(congested)
  {
    // Grow the window if this episode never resolved — the recorded
    // onset reads congested on the raw ratio for its full ~14 s, because
    // each decrease shrinks our own send rate and so widens the very
    // filter skew that triggered the detector. A fixed window still
    // permits a third and fourth halving inside one such episode.
    //
    // The grown window is bounded TWICE, and both bounds are load-bearing
    // (#52, review round 2). The multiple bounds it relative to the
    // configured base; kMaximumAdmissionRefractoryPeriodSeconds bounds it
    // absolutely. Without the second bound a connection configured at the
    // 60 s maximum base freezes for 120 s on its second decrease — twice
    // the ceiling whose own rationale (resend_constants.h) says a window
    // that long leaves the controller unable to answer a link change in
    // any operator-observable timescale. The setter's clamp is on the
    // BASE only; growth happens after it, so it has to be re-applied here.
    if(admission_decrease_outstanding_)
      admission_refractory_current_seconds_ =
        std::min({admission_refractory_current_seconds_ * kAdmissionRefractoryGrowthFactor,
                  admission_refractory_period_seconds_ * kAdmissionRefractoryMaximumMultiple,
                  kMaximumAdmissionRefractoryPeriodSeconds});
    else
      admission_refractory_current_seconds_ = admission_refractory_period_seconds_;
    admission_decrease_outstanding_ = true;
    last_admission_decrease_time_ = now.seconds();

    // Purely multiplicative decrease from the controller's own last
    // known-good state. The (1 - link_headroom_fraction) * goodput
    // clamp that used to bound this was removed on 2026-08-25: goodput
    // is depressed by the throttling the clamp was computing, so it fed
    // back on itself. On the recorded onset the clamp's FIRST step alone
    // landed at 0.8 x (254615 - 51715) = 162320 — already below the
    // regression bound — and no refractory tuning rescued it. Nor is
    // goodput a capacity estimate when the gate is not limiting: the
    // code comment above says it is bounded by OFFERED load, and the
    // field day's operating point was ~495 kB/s offered against a
    // 1500000 cap, making the clamp a 3.8x cut carrying no capacity
    // information at all.
    effective_rate_limit_ =
      std::max(floor, effective_rate_limit_ * kAdmissionDecreaseFactor);
  }
  else
  {
    // The episode resolved: clear the outstanding decrease and give back
    // the accumulated exponential growth, so the NEXT episode starts
    // from the base window rather than inheriting this one's ceiling.
    admission_decrease_outstanding_ = false;
    last_admission_decrease_time_ = 0.0;
    admission_refractory_current_seconds_ = admission_refractory_period_seconds_;

    // Additive recovery relative to where the controller currently is,
    // with an absolute minimum so escaping the floor is not
    // asymptotically slow. Cap-relative steps were the #52 defect: on a
    // link delivering 1.6% of its configured cap, one clean sample
    // added six times the whole link and undid three halvings.
    const float step = std::max(kAdmissionAdditiveStepMinimumBytesPerSecond,
                                kAdmissionAdditiveStepFraction * effective_rate_limit_);
    effective_rate_limit_ =
      std::min(static_cast<float>(data_rate_limit_), effective_rate_limit_ + step);
  }
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
  // A connection whose host has never been advertised has nowhere to send.
  // Short-circuit here, BEFORE the per-packet loop below copies each packet
  // into a WrappedPacket and files it in sent_packets_. That buffer exists so
  // a peer can ask for a resend; a peer we have never heard from cannot ask,
  // so every entry is a copy and a map insert that can only ever be evicted by
  // cleanup. The inner per-packet send() already refuses to call sendto()
  // without an address, so this costs the copies and the bookkeeping for
  // nothing.
  //
  // Measured on BizzyBoat 2026-08-20: the operator station was reachable only
  // over vpn and cell and never advertised a wifi address, so the configured
  // wifi connection sat at host='' with the full topic list pointed at it --
  // roughly 1.8 MB/s of copies, map churn and resend bookkeeping, holding a
  // CPU core at ~96% indefinitely while delivering exactly zero bytes.
  //
  // Statistics are still recorded, so an unaddressed connection stays VISIBLE
  // in topic_statistics as failing. Dropping it silently would turn a
  // misconfigured or out-of-range link into an invisible one, which is how
  // this went unnoticed in the first place.
  bool no_address;
  {
    std::lock_guard<std::recursive_mutex> lock(config_mutex_);
    no_address = addresses_.empty();
  }
  if(no_address)
  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    for(const auto& p: packets)
    {
      PacketSizeData size_data;
      size_data.timestamp = now;
      size_data.size = p.packet_size;
      if(is_overhead)
        size_data.category = PacketSendCategory::overhead;
      else
        size_data.category = PacketSendCategory::message;
      size_data.send_result = SendResult::failed;
      sent_packet_statistics_.add(size_data);
    }
    return SendResult::failed;
  }

  // Sum the actual byte vectors, which is exactly what each fragment
  // will release via sendPacket(..., bytes_pre_reserved=true) below.
  // p.packet_size is equal to it by construction (wrapped_packet.cpp),
  // but the reservation accounting must not depend on a header field
  // agreeing with the buffer it describes — summing the buffers makes
  // the reserve and the releases provably the same quantity.
  uint32_t total_size = 0;
  for(const auto& p: packets)
    total_size += static_cast<uint32_t>(p.packet.size());

  uint32_t rate_limit;
  {
    std::lock_guard<std::recursive_mutex> lock(config_mutex_);
    // Meter against the AIMD-adjusted cap (issue #43), not the static
    // configured limit — admission backs off when delivery drops.
    rate_limit = static_cast<uint32_t>(effective_rate_limit_);
  }

  // AGGREGATE CHECK-AND-RESERVE — the message is admitted or dropped as
  // one unit (issue #52).
  //
  // This used to be a check ONLY: on the success path the lock was
  // released without reserving anything, deliberately, so two concurrent
  // callbacks (republish_group_ is Reentrant) could both pass here and
  // both fall through to the per-packet loop, where each packet made its
  // own independent can_send call. That made the per-packet send the
  // sole enforcement point — and with it, the drop granularity. If the
  // budget ran out partway through a multi-fragment message, the later
  // fragments were dropped individually while the earlier ones had
  // already gone out. Those bytes can never be reassembled into a
  // deliverable message, so on an already-degraded link they were pure
  // waste: the receiver holds an incomplete set until the defragmenter
  // times it out. Message-level granularity is what `cce4401` had before
  // the 2026-05-18 concurrency fix (`1489cfb`/`0c8b75f`) replaced it
  // with per-packet checks.
  //
  // Reserving total_size here closes that TOCTOU window: the first
  // batch's reservation is visible to the second batch's can_send, so
  // two concurrent messages can no longer jointly over-admit.
  //
  // CONCURRENCY DISCIPLINE IS UNCHANGED, and must stay that way. This is
  // still two bookkeeping operations under ONE brief acquisition of
  // sent_packet_statistics_mutex_; the blocking-capable sendto poll loop
  // for each fragment still runs with NO lock held (see sendPacket
  // below). Holding this mutex across the I/O would stall
  // UDPBridge::sendBridgeInfo — which calls data_sent_rate while holding
  // remote_nodes_mutex_ — and through it the socket-drain path: the #10
  // wedge the callback-group split exists to prevent.
  //
  // BOUNDING THE RESERVATION'S LIFETIME (#52 review round 1). The
  // reservation is taken here and released fragment-by-fragment inside
  // the loop below, so it is held for as long as that loop takes — up to
  // ~200 ms per fragment under kernel back-pressure (the sendto poll
  // budget), and reserved_bytes_in_flight_ has no time dimension of its
  // own: it is a level, not a windowed rate. While it is held, a
  // concurrent send on this connection — INCLUDING the overhead tier,
  // which shares this budget — sees the reserved bytes in can_send and
  // may be refused.
  //
  // So a large or slow message can, in principle, cause our OUTBOUND
  // BridgeInfo and resend requests to be dropped for the duration of its
  // loop. Two loops are harmed by that, and neither is this connection's
  // own congestion detector:
  //
  //   - the REMOTE's admission controller, which is fed by our BridgeInfo
  //     and by our echo of what we received. Starve it and it decides on
  //     stale evidence, or on none.
  //   - our own resend path, whose re-requests do not go out.
  //
  // What it does NOT do is set our own `feedback_stale`. That flag is
  // driven by `last_receive_time` — INBOUND traffic from the remote —
  // which a reservation held on our send side does not touch. (An
  // earlier version of this comment had that causal chain backwards.)
  // Nor does the large message itself read as an idle link: its own
  // fragments are succeeding, so `sent_bps > 0` throughout.
  //
  // This is NOT a deadlock: the reservation is released unconditionally
  // on every exit path (see the batch guard below), and the hold is
  // bounded by the sendto poll budget. But it is bounded by nothing
  // ELSE, and the harm lands on a loop we cannot see from here. If the
  // overhead tier is ever observed starving in the field, the fix is a
  // separate reservation ledger for it rather than a longer refractory
  // period.
  //
  // is_overhead traffic (BridgeInfo, resend requests, topic lists)
  // reaches this same overload from the same single call site
  // (udp_bridge.cpp), and gets the same atomic treatment ON PURPOSE.
  // Overhead messages are one packet in the overwhelming majority of
  // cases, so for them "atomic" and "per-packet" coincide; where a topic
  // list does fragment, half a topic list is no more useful than half a
  // video frame. The failure path already dropped them atomically before
  // this change — it is the success path that is now consistent with it.
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
    reserved_bytes_in_flight_ += total_size;
  }

  // Guard for the portion of the reservation not yet handed to a
  // fragment. Ownership of each fragment's bytes transfers to sendPacket
  // BEFORE the call (unreserved -= p.packet_size), because sendPacket
  // releases exactly data.size() on every one of its exit paths,
  // including the throw path. Decrementing after the call instead would
  // double-release on a throw. Whatever is left when this guard unwinds
  // belongs to fragments that were never attempted.
  //
  // The accounting rests on packet.packet.size() == p.packet_size, which
  // holds by construction: WrappedPacket's copy constructor
  // (wrapped_packet.cpp) copies both the byte vector and packet_size and
  // rewrites only the fixed-size header fields in place. So the
  // per-fragment releases sum to exactly total_size regardless of which
  // exit path each fragment takes.
  struct BatchReservationGuard
  {
    std::mutex& mutex;
    uint32_t& reservation;
    uint32_t unreserved;
    ~BatchReservationGuard()
    {
      if(unreserved > 0)
      {
        std::lock_guard<std::mutex> lock(mutex);
        reservation -= unreserved;
      }
    }
  };
  BatchReservationGuard batch_guard{sent_packet_statistics_mutex_, reserved_bytes_in_flight_, total_size};

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
    // Clamped, not just decremented. `unreserved` is unsigned, and the
    // invariant that the per-fragment sizes sum to exactly total_size is
    // argued for above (WrappedPacket's copy constructor preserves
    // packet_size) rather than checked. If it ever stopped holding, the
    // subtraction would WRAP, the guard would then "release" a
    // near-4 GB quantity out of reserved_bytes_in_flight_, and that
    // counter — also unsigned — would wrap in turn and wedge the
    // connection permanently: can_send would refuse everything, with no
    // way back short of a restart. A clamp costs one comparison (#52,
    // review round 2).
    const uint32_t fragment_bytes = static_cast<uint32_t>(packet.packet.size());
    batch_guard.unreserved -=
      std::min(batch_guard.unreserved, fragment_bytes);
    auto send_ret = sendPacket(packet.packet, socket, category, now, true);
    if(send_ret.send_result != SendResult::success)
      ret = send_ret.send_result;
  }
  return ret;
}


PacketSizeData Connection::send(const std::vector<uint8_t> &data, int socket, PacketSendCategory category, rclcpp::Time now)
{
  return sendPacket(data, socket, category, now, false);
}

PacketSizeData Connection::sendPacket(const std::vector<uint8_t> &data, int socket, PacketSendCategory category, rclcpp::Time now, bool bytes_pre_reserved)
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
    // AIMD-adjusted cap (issue #43) — see the batch overload above.
    rate_limit = static_cast<uint32_t>(effective_rate_limit_);
  }
  if(no_address)
  {
    // A concurrent setHostAndPort() -> resolveHost() can clear addresses_
    // between the batch overload's own pre-check and this fragment, so a
    // pre-reserved caller can genuinely land here mid-message. Release
    // its bytes under the same lock as the record (issue #52, F6).
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    sent_packet_statistics_.add(ret);
    if(bytes_pre_reserved)
      reserved_bytes_in_flight_ -= static_cast<uint32_t>(data.size());
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
  // next can_send call. No record is added in this case.
  //
  // The guard is LOAD-BEARING, not cosmetic. `callIsolated`
  // (include/udp_bridge/send_isolation.h) catches ConnectionException
  // explicitly, and it wraps the batch send path, so a throw here does
  // NOT terminate the node — it is reported and the bridge carries on.
  // (An earlier version of this comment claimed nothing in the workspace
  // caught it and the process was about to die anyway; that was false,
  // and it invited relaxing the guard.) The Timeout throw is a recurrent
  // survivable path: a poll budget exhausted under back-pressure raises
  // it, the connection stays alive, and the next send calls can_send
  // again. A leaked reservation would therefore ACCUMULATE across such
  // events until reserved_bytes_in_flight_ exceeds the rate limit and
  // can_send refuses everything — a permanently wedged connection, with
  // no way back short of a restart.
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

  // Step 1 of the pattern, skipped when the caller already reserved these
  // bytes as part of a message-atomic aggregate reservation (issue #52).
  // Re-checking here would defeat the whole point — the fragment would be
  // metered a second time against a budget its own message already holds
  // — and re-reserving would double-count. Steps 2-4 below are identical
  // either way, which is what keeps the release exact on every exit.
  if(!bytes_pre_reserved)
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
  const double window_start = time - kReceiveRateWindowSeconds;
  while(!data_size_received_history_.empty() && data_size_received_history_.begin()->first < window_start)
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

  // Resend budget (issue #44, rebased by #52). Resends may consume at
  // most resend_budget_fraction_ of measured goodput per second; while
  // the connection is receiving nothing (which starves acks — retrying
  // harder cannot help), the budget halves per kAckStarvationThreshold
  // elapsed, floored at 1/2^kMaxAckStarvationBackoffShift. Bounds the
  // self-amplifying resend storms of 2026-05-01 / 2026-08-04 while the
  // overall can_send cap in send() continues to meter the total.
  uint32_t basis;
  float fraction;
  {
    std::lock_guard<std::recursive_mutex> lock(config_mutex_);
    // Basis is measured goodput, not any configured or admission-derived
    // cap. Under #44 it was resend_budget_fraction_ * effective_rate_limit_,
    // which inherited every scaling error in the admission cap: the
    // 2026-08-22 bench measured an effective cap of 2291 kB/s on a
    // 62.5 kB/s link, so the 25% budget authorized 573 kB/s of resends —
    // nine times the entire path. Sizing retransmission against what the
    // link is actually delivering is the only basis that cannot be wrong
    // by orders of magnitude.
    //
    // Before the first feedback sample goodput is 0.0; the probe
    // guarantee below still admits one packet per window, so a brand-new
    // connection can bootstrap.
    //
    // Sanity ceiling: goodput is a remote-reported figure, so clamp it to
    // the connection's own configured rate limit before sizing the budget
    // (#52). A crafted or glitched remote reporting an absurd delivery
    // rate must not be able to authorize a resend burst larger than the
    // operator-declared send budget for the link. (can_send in send()
    // bounds the total further, but the resend budget should not itself
    // be inflatable by the peer.)
    basis = static_cast<uint32_t>(
      std::min(goodput_bytes_per_second_, static_cast<float>(data_rate_limit_)));
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
    (static_cast<double>(fraction) * basis) / static_cast<double>(1u << backoff_shift);

  // Seed the accumulator with the resend bytes already SENT (success
  // or failed at the socket — not budget-dropped) in the same strict
  // 1-second window can_send uses (NOT the 0-10 s smoothed rate, which
  // lags a sustained burst and would under-shed in exactly the
  // sustained-storm mode this budget exists to stop). Budget-dropped
  // entries are deliberately excluded from the seed: they consumed no
  // link capacity, and counting them would let one shed batch block the
  // budget for the rest of the window. Within THIS call, each packet
  // handed to send() is then charged locally regardless of its outcome
  // — conservative, immune to re-query races, and O(packets).
  uint64_t attempted_bytes;
  {
    std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
    attempted_bytes = sent_packet_statistics_.bytes_in_window(PacketSendCategory::resend, now);
  }

  // Probe guarantee: on links whose floored budget is smaller than one
  // packet (e.g. a 50 kB/s goodput at max backoff: 781 bytes), a pure
  // byte comparison would shed every resend and the "probe trickle
  // keeps recovery possible" property would silently vanish. When
  // nothing has been resent in the current window, the first packet is
  // admitted regardless of its size — bounding the trickle at roughly
  // one packet per window rather than zero.
  //
  // Gated on the FRACTION, not the computed budget (#52). Only
  // `resend_budget_fraction == 0` means "the operator turned resends
  // off"; a zero *budget* now also arises from a zero goodput basis —
  // a brand-new connection before its first BridgeInfo sample, or a
  // link currently delivering nothing. Both of those are precisely the
  // cases the probe trickle exists for: without it a connection that
  // has never received feedback could never resend, and a blacked-out
  // link could never recover once the inbound path returned.
  const bool admit_probe = (attempted_bytes == 0 && fraction > 0.0f);
  for(std::size_t i = 0; i < packets_to_resend.size(); ++i)
  {
    const auto& packet = packets_to_resend[i];
    if(attempted_bytes + packet.size() > budget_bytes && !(admit_probe && i == 0))
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

uint32_t Connection::reserved_bytes_in_flight_for_test() const
{
  std::lock_guard<std::mutex> lock(sent_packet_statistics_mutex_);
  return reserved_bytes_in_flight_;
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
