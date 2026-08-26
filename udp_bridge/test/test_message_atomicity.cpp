// Message-level admission granularity for Connection::send (issue #52).
//
// WHAT BROKE. A ROS message that does not fit in one datagram is
// fragmented, and every fragment must arrive for the receiver's
// defragmenter to reassemble it. Until `cce4401` was superseded, the
// batch overload of Connection::send made ONE can_send call covering the
// whole message, so a message was admitted or refused as a unit.
//
// The 2026-05-18 concurrency fix (`1489cfb`, then `0c8b75f`) moved
// enforcement to the per-packet inner send() — it had to, because
// republish_group_ became Reentrant and the aggregate check was a
// check-then-act TOCTOU. The aggregate call stayed, but as a check ONLY:
// on the success path it reserved nothing, deliberately, so two
// concurrent batches could both pass it. Each fragment then metered
// itself independently. If the budget ran out partway through a message
// — because a concurrent batch, or a sibling message on the same
// connection, consumed it in between — the later fragments were dropped
// while the earlier ones had already gone out. Those bytes can never be
// reassembled into anything, so on an already-degraded link they were
// pure waste: exactly the resource the admission gate exists to protect.
//
// WHAT THIS PINS. The aggregate check is now a check-AND-reserve, so
// message-level granularity is restored without giving back the
// concurrency property the per-packet move was made for:
//
//   1. A message that does not fit is dropped WHOLE — zero fragments on
//      the wire (PartiallyFittingMessageIsDroppedWhole).
//   2. A message that fits is sent complete and accounted exactly once
//      (FittingMessageIsSentCompleteAndAccountedOnce).
//   3. Concurrent messages cannot jointly over-admit, and no message is
//      ever partially admitted under contention
//      (ConcurrentMessagesAreAllOrNothing) — the real-threads shape,
//      because the defect being fixed was itself introduced while fixing
//      a concurrency bug.
//   4. is_overhead traffic (BridgeInfo, resend requests, topic lists)
//      gets the same atomic treatment — it reaches the SAME overload from
//      the same call site, not a separate path
//      (OverheadTrafficIsAdmittedAtomically).
//   5. The reservation is released on every exit path, so it cannot leak
//      and silently strangle the connection
//      (ReservationReleasedOn*, ReservationSurvivesAddressChurn).
//
// ON THE FOUR EXIT PATHS (issue #52, review finding F6). Success and the
// whole-message drop are asserted directly. The thrown-ConnectionException
// path is forced with an invalid socket fd, which also exercises the batch
// guard's unwind over the fragments that were never attempted. The
// mid-loop `no_address` path — a concurrent setHostAndPort() clearing
// addresses_ after the batch's own pre-check passed — is not
// deterministically reachable from a unit test, so it is exercised
// probabilistically under address churn, asserting the accounting
// invariant rather than the schedule. ECONNREFUSED is not reachable at
// all on an unconnected UDP socket; it releases through the same
// record_and_release() call the success path uses, which is asserted.
//
// LOCK DISCIPLINE IS PART OF THE CONTRACT. The aggregate reserve is two
// bookkeeping operations under one brief acquisition of
// sent_packet_statistics_mutex_; the per-fragment sendto poll loop still
// runs with no lock held. test_connection_rate_limit.cpp's
// ConcurrentSendsStayUnderLimit is the standing guard on that (the #10
// wedge lineage) and must keep passing alongside these.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include "rclcpp/rclcpp.hpp"

#include "udp_bridge/connection.h"
#include "udp_bridge/statistics.h"
#include "udp_bridge/wrapped_packet.h"

namespace
{

class SocketFd
{
public:
  SocketFd() = default;
  explicit SocketFd(int fd) : fd_(fd) {}
  ~SocketFd() { reset(); }
  SocketFd(const SocketFd&) = delete;
  SocketFd& operator=(const SocketFd&) = delete;
  SocketFd(SocketFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  SocketFd& operator=(SocketFd&& o) noexcept
  {
    if(this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
  }
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void reset() { if(fd_ >= 0) { close(fd_); fd_ = -1; } }
private:
  int fd_ = -1;
};

SocketFd bind_localhost_listener(uint16_t* port)
{
  SocketFd sock(socket(AF_INET, SOCK_DGRAM, 0));
  if(!sock.valid()) return sock;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if(bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    return SocketFd{};
  socklen_t len = sizeof(addr);
  if(getsockname(sock.get(), reinterpret_cast<sockaddr*>(&addr), &len) < 0)
    return SocketFd{};
  *port = ntohs(addr.sin_port);
  return sock;
}

constexpr int kFragmentPayload = 1000;

}  // namespace

class MessageAtomicity : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if(!rclcpp::ok())
      rclcpp::init(0, nullptr);
    listener_sock_ = bind_localhost_listener(&listener_port_);
    ASSERT_TRUE(listener_sock_.valid())
      << "Failed to bind listener socket: " << strerror(errno);
    send_sock_ = SocketFd(socket(AF_INET, SOCK_DGRAM, 0));
    ASSERT_TRUE(send_sock_.valid())
      << "Failed to open send socket: " << strerror(errno);
  }

  std::unique_ptr<udp_bridge::Connection> make_connection(uint32_t rate_limit)
  {
    auto conn = std::make_unique<udp_bridge::Connection>(
      "atomicity-test", "127.0.0.1", listener_port_, "", 0);
    conn->setRateLimit(rate_limit);
    return conn;
  }

  // A fragmented message: `fragments` WrappedPackets with consecutive
  // packet numbers, the shape udp_bridge.cpp hands to the batch overload.
  std::vector<udp_bridge::WrappedPacket> make_message(
    uint64_t first_packet_number, int fragments, rclcpp::Time now)
  {
    std::vector<udp_bridge::WrappedPacket> packets;
    std::vector<uint8_t> payload(kFragmentPayload, 0xAB);
    for(int i = 0; i < fragments; ++i)
      packets.emplace_back(first_packet_number + i, payload, now);
    return packets;
  }

  static uint32_t message_bytes(const std::vector<udp_bridge::WrappedPacket>& packets)
  {
    uint32_t total = 0;
    for(const auto& p: packets)
      total += static_cast<uint32_t>(p.packet.size());
    return total;
  }

  SocketFd listener_sock_;
  SocketFd send_sock_;
  uint16_t listener_port_ = 0;
};

TEST_F(MessageAtomicity, PartiallyFittingMessageIsDroppedWhole)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t = clock.now();

  auto probe = make_message(0, 6, t);
  const uint32_t message_size = message_bytes(probe);

  // A budget that fits ONE such message and a little more, so a second
  // one lands squarely in the "some fragments would fit, most would not"
  // regime that used to produce a half-sent message.
  auto conn = make_connection(message_size + message_size / 2);

  ASSERT_EQ(conn->send(make_message(0, 6, t), send_sock_.get(), "op", false, t),
            udp_bridge::SendResult::success);
  const auto after_first = conn->data_sent_rate(t, udp_bridge::PacketSendCategory::message);
  ASSERT_EQ(static_cast<uint32_t>(after_first.success_bytes_per_second), message_size);

  EXPECT_EQ(conn->send(make_message(100, 6, t), send_sock_.get(), "op", false, t),
            udp_bridge::SendResult::dropped);

  const auto rates = conn->data_sent_rate(t, udp_bridge::PacketSendCategory::message);
  EXPECT_EQ(static_cast<uint32_t>(rates.success_bytes_per_second), message_size)
    << "Not one fragment of the second message may go out. Half its "
       "budget would have fit, and before #52 that is exactly what "
       "happened: the fragments that fit were sent, the rest were "
       "dropped individually, and the receiver's defragmenter timed out "
       "an unreassemblable set — bytes spent on a degraded link buying "
       "nothing.";
  EXPECT_EQ(static_cast<uint32_t>(rates.dropped_bytes_per_second), message_size)
    << "The whole message must be RECORDED as dropped, not only "
       "suppressed — an invisible drop is how this went unnoticed.";
  EXPECT_EQ(conn->reserved_bytes_in_flight_for_test(), 0u)
    << "The refused message must leave no reservation behind.";
}

TEST_F(MessageAtomicity, FittingMessageIsSentCompleteAndAccountedOnce)
{
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t = clock.now();

  auto probe = make_message(0, 4, t);
  const uint32_t message_size = message_bytes(probe);
  // can_send admits while sent + reserved + size is STRICTLY under the
  // limit, so a budget of exactly two messages fits only one. The +1
  // makes "two messages fit" mean what it says.
  auto conn = make_connection(message_size * 2 + 1);

  ASSERT_EQ(conn->send(make_message(0, 4, t), send_sock_.get(), "op", false, t),
            udp_bridge::SendResult::success);

  const auto rates = conn->data_sent_rate(t, udp_bridge::PacketSendCategory::message);
  EXPECT_EQ(static_cast<uint32_t>(rates.success_bytes_per_second), message_size)
    << "Every fragment must be sent, and each accounted exactly once — "
       "the aggregate reservation must not double-count bytes the "
       "per-fragment records also add.";
  EXPECT_EQ(static_cast<uint32_t>(rates.dropped_bytes_per_second), 0u);
  EXPECT_EQ(conn->reserved_bytes_in_flight_for_test(), 0u);

  // The budget must genuinely have `message_size` left. If the aggregate
  // reservation leaked — released twice, or not at all — this second
  // message would be wrongly admitted or wrongly refused, and neither
  // shows up as an error anywhere else.
  EXPECT_EQ(conn->send(make_message(100, 4, t), send_sock_.get(), "op", false, t),
            udp_bridge::SendResult::success)
    << "The remaining budget must be exactly what the accounting says it "
       "is: a leaked reservation strangles the connection silently.";
  EXPECT_EQ(conn->reserved_bytes_in_flight_for_test(), 0u);
}

TEST_F(MessageAtomicity, OverheadTrafficIsAdmittedAtomically)
{
  // F5. The batch overload has ONE production caller
  // (udp_bridge.cpp), reached with is_overhead both false and true —
  // BridgeInfo, resend requests and topic lists come through here too,
  // not down a separate per-packet path. So the aggregate reservation
  // governs the very feedback channel updateAdmissionControl consumes,
  // and that has to be a decision with a test on it rather than an
  // assumption of no effect.
  //
  // The decision: overhead gets the SAME atomic treatment. Overhead
  // messages are one packet in the overwhelming majority of cases, so
  // for them atomic and per-packet coincide; where a topic list does
  // fragment, half a topic list is no more useful than half a video
  // frame. The failure path already dropped them atomically before this
  // change — it is the success path that is now consistent with it.
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t = clock.now();

  auto probe = make_message(0, 4, t);
  const uint32_t message_size = message_bytes(probe);
  auto conn = make_connection(message_size + message_size / 2);

  ASSERT_EQ(conn->send(make_message(0, 4, t), send_sock_.get(), "op", true, t),
            udp_bridge::SendResult::success);
  EXPECT_EQ(conn->send(make_message(100, 4, t), send_sock_.get(), "op", true, t),
            udp_bridge::SendResult::dropped);

  const auto overhead =
    conn->data_sent_rate(t, udp_bridge::PacketSendCategory::overhead);
  EXPECT_EQ(static_cast<uint32_t>(overhead.success_bytes_per_second), message_size);
  EXPECT_EQ(static_cast<uint32_t>(overhead.dropped_bytes_per_second), message_size)
    << "Overhead traffic is admitted or refused whole, and is recorded "
       "in the overhead category either way (#52, F5).";

  const auto message_rates =
    conn->data_sent_rate(t, udp_bridge::PacketSendCategory::message);
  EXPECT_EQ(static_cast<uint32_t>(message_rates.success_bytes_per_second), 0u)
    << "Overhead must not be miscategorised as message traffic — the "
       "resend budget and the admission detector both read these "
       "categories.";
  EXPECT_EQ(conn->reserved_bytes_in_flight_for_test(), 0u);
}

TEST_F(MessageAtomicity, ConcurrentMessagesAreAllOrNothing)
{
  // THIS is the discriminating test for the defect. The other cases in
  // this file pin contracts that a check-only aggregate pre-check already
  // satisfied single-threaded — sequentially, nothing can consume budget
  // between the check and the per-fragment loop, so the message was
  // already all-or-nothing. The partial send needed a second sender in
  // that gap, which is exactly the arrangement republish_group_'s
  // Reentrant callback group produces in the field.
  //
  // So: many threads released together on a barrier, each sending a
  // long-enough message that its per-fragment loop genuinely overlaps
  // the others, against a budget that fits only a few of them. Repeated
  // over fresh connections, because a race window is caught by exposure,
  // not by one attempt. Without the aggregate reservation this reports a
  // byte total that is not a whole number of messages within the first
  // rounds; with it, never.
  rclcpp::Clock clock(RCL_STEADY_TIME);

  constexpr int kThreads = 16;
  constexpr int kFragments = 40;
  constexpr int kRounds = 25;
  constexpr int kMessagesThatFit = 4;

  auto probe = make_message(0, kFragments, clock.now());
  const uint32_t message_size = message_bytes(probe);
  // +1 because can_send's comparison is strict.
  const uint32_t rate_limit = message_size * kMessagesThatFit + 1;

  // Below this many completed rounds the exposure is too small for the
  // race to be caught reliably, so a green result would not mean the
  // reservation ledger is intact — it would mean the test did not run.
  // That is a REPORTED failure, naming the environment, rather than a
  // silent skip.
  constexpr int kMinimumCompletedRounds = 10;

  int total_successes = 0;
  int skipped_rounds = 0;
  for(int round = 0; round < kRounds; ++round)
  {
    // Fixed timestamp per round so the per-second accounting window is
    // well-defined, as in test_connection_rate_limit.cpp.
    const auto t = clock.now();
    auto conn = make_connection(rate_limit);

    std::atomic<bool> go{false};
    std::atomic<int> ready{0};
    std::atomic<int> successes{0};
    std::atomic<int> send_throws{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for(int i = 0; i < kThreads; ++i)
      threads.emplace_back([&, i]
      {
        auto packets = make_message(static_cast<uint64_t>(i) * 1000, kFragments, t);
        ++ready;
        while(!go.load(std::memory_order_acquire))
          std::this_thread::yield();
        // Guard the call. ConnectionException is reachable here — the
        // send-poll budget can be exhausted on ENOBUFS with kThreads
        // threads against an undrained loopback socket — and an
        // exception escaping a std::thread's function calls
        // std::terminate, which aborts the whole test binary instead of
        // failing this test. The sibling churn test already wraps its
        // call for the same reason.
        try
        {
          if(conn->send(packets, send_sock_.get(), "op", false, t) ==
             udp_bridge::SendResult::success)
            ++successes;
        }
        catch(const udp_bridge::ConnectionException&)
        {
          ++send_throws;
        }
      });
    while(ready.load() < kThreads)
      std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for(auto& th: threads)
      th.join();

    // A throwing round is an ENVIRONMENTAL condition, not a defect: the
    // send-poll budget can be exhausted on ENOBUFS with kThreads threads
    // against an undrained loopback socket, which says something about
    // CI load and nothing about the reservation ledger. Failing on it
    // makes this test red for a reason its own message disclaims — and
    // worse, a throwing round used to fall through to the
    // `sent % message_size` assertion below, which would then
    // misattribute the environment to the atomicity logic.
    //
    // `continue`, NOT GTEST_SKIP (#52, review round 3). GTEST_SKIP
    // returns from the TEST FUNCTION, not from the round: one ENOBUFS in
    // round 0 abandoned all 25 rounds AND the closing
    // total_successes > 0 check, and the run reported green. This is the
    // only test in the package that discriminates against a regression
    // of the aggregate check-AND-reserve to a check-only pre-check, and
    // the probability of that one throw rises exactly as the machine
    // gets busier — i.e. as the race it exists to catch gets more
    // catchable. A test that can quietly stop testing is worse than no
    // test. Skipped rounds are counted and the shortfall is asserted
    // after the loop.
    if(send_throws.load() != 0)
    {
      ++skipped_rounds;
      continue;
    }

    const auto rates = conn->data_sent_rate(t, udp_bridge::PacketSendCategory::message);
    const uint32_t sent = static_cast<uint32_t>(rates.success_bytes_per_second);
    total_successes += successes.load();

    ASSERT_EQ(sent % message_size, 0u)
      << "Round " << round << ": a message was PARTIALLY admitted under "
         "contention (" << sent << " B is not a whole number of "
      << message_size << " B messages). Fragments of a message that never "
         "completes can never be reassembled, so those bytes are spent on "
         "a degraded link buying nothing — the defect this test exists "
         "for.";
    ASSERT_EQ(sent / message_size, static_cast<uint32_t>(successes.load()))
      << "Round " << round << ": a message reported successful was not "
         "fully on the wire, or one that was not reported successful was.";
    ASSERT_LE(sent, rate_limit)
      << "Round " << round << ": concurrent messages jointly over-admitted "
      << sent << " B against a " << rate_limit << " B/s budget. The "
         "aggregate reservation is what makes one batch's claim visible "
         "to another's can_send.";
    ASSERT_EQ(conn->reserved_bytes_in_flight_for_test(), 0u)
      << "Round " << round << ": a reservation outlived its senders.";
  }

  const int completed_rounds = kRounds - skipped_rounds;
  ASSERT_GE(completed_rounds, kMinimumCompletedRounds)
    << "Only " << completed_rounds << " of " << kRounds << " rounds ran; "
    << skipped_rounds << " were abandoned because a concurrent send threw "
       "(send-poll budget exhausted on ENOBUFS against an undrained "
       "loopback socket). That is an environmental condition, not a defect "
       "in the reservation ledger — but with this little exposure the race "
       "this test exists to catch would not be caught, so a pass would be "
       "meaningless. Re-run on a less loaded machine (#52).";

  EXPECT_GT(total_successes, 0)
    << "Capacity must actually be used — an over-conservative reservation "
       "that admits nothing would satisfy every bound above.";
}

TEST_F(MessageAtomicity, ReservationReleasedWhenAFragmentThrows)
{
  // F6, the throw path — and with it the batch guard's unwind over the
  // fragments that were never attempted. An invalid socket fd makes
  // poll() report neither POLLOUT nor an error, so the inner send
  // exhausts its 20-try budget and throws ConnectionException("Timeout")
  // on the FIRST fragment: the remaining five are never attempted, and
  // their share of the reservation is the batch guard's to release.
  //
  // In the node this propagates as far as `callIsolated`
  // (send_isolation.h), which catches ConnectionException explicitly and
  // reports it — the bridge SURVIVES a Timeout throw and keeps sending.
  // That is precisely why the accounting has to be exact: the connection
  // lives on, so a leaked reservation accumulates over repeated timeouts
  // until can_send refuses everything on it.
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t = clock.now();

  auto conn = make_connection(1000000);
  auto packets = make_message(0, 6, t);

  EXPECT_THROW(conn->send(packets, -1, "op", false, t),
               udp_bridge::ConnectionException);

  EXPECT_EQ(conn->reserved_bytes_in_flight_for_test(), 0u)
    << "A mid-message throw must leave reserved_bytes_in_flight_ exactly "
       "where it started: the throwing fragment releases its own bytes "
       "through its ReservationGuard, and the batch guard releases the "
       "fragments that were never attempted. Releasing either portion "
       "twice, or neither, is invisible until the connection stops "
       "admitting anything.";
}

TEST_F(MessageAtomicity, ReservationSurvivesAddressChurn)
{
  // F6, the mid-loop `no_address` path. A concurrent setHostAndPort()
  // clears addresses_, and a fragment can find them gone AFTER the
  // batch's own pre-check passed — a real interleaving (the add_remote
  // service handler and a decode handler both reach setHostAndPort), but
  // not one a unit test can schedule deterministically. So assert the
  // invariant instead of the schedule: whatever mix of exit paths the
  // fragments take, the reservation must come back to zero.
  rclcpp::Clock clock(RCL_STEADY_TIME);
  const auto t = clock.now();

  auto conn = make_connection(100000000);
  std::atomic<bool> stop{false};

  // Guard the churn thread's calls. setHostAndPort -> resolveHost throws
  // std::runtime_error when getaddrinfo fails (transient resolver
  // failure, fd exhaustion under a loaded CI box), and an exception
  // escaping a std::thread's function calls std::terminate — aborting
  // the whole test binary rather than failing this test. Exactly the
  // hazard the sibling contention test above is wrapped for.
  std::atomic<int> churn_throws{0};
  std::thread churn([&]
  {
    while(!stop)
    {
      try
      {
        conn->setHostAndPort("", 0);
        conn->setHostAndPort("127.0.0.1", listener_port_);
      }
      catch(const std::exception&)
      {
        ++churn_throws;
      }
    }
  });

  for(int round = 0; round < 200; ++round)
  {
    auto packets = make_message(static_cast<uint64_t>(round) * 100, 8, t);
    try
    {
      conn->send(packets, send_sock_.get(), "op", false, t);
    }
    catch(const udp_bridge::ConnectionException&)
    {
      // A send racing an address change may still throw; the accounting
      // assertion below is what this test is about.
    }
  }

  stop = true;
  churn.join();

  EXPECT_EQ(conn->reserved_bytes_in_flight_for_test(), 0u)
    << "Bytes reserved for a message must be released on every exit a "
       "fragment can take, including finding the address gone mid-loop.";
  if(churn_throws.load() != 0)
    GTEST_SKIP() << churn_throws.load() << " host resolutions failed in "
                    "the churn thread; the interleaving this test is "
                    "about was not reliably produced.";
}
