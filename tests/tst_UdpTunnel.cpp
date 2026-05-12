// Config.hpp must come before UdpTunnel.hpp — provides link::platform::Random.
#include <ableton/platforms/Config.hpp>
#include <ableton/extender/UdpTunnel.hpp>
#include <ableton/discovery/AsioTypes.hpp>
#include <ableton/test/CatchWrapper.hpp>
#include <ableton/test/serial_io/SchedulerTree.hpp>
#include <ableton/test/serial_io/Timer.hpp>
#include <ableton/util/Injected.hpp>
#include <ableton/util/Log.hpp>
#include "TestHelpers.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace ableton
{
namespace extender
{
namespace
{

using namespace ableton::extender::helpers;
using UdpEndpoint = discovery::UdpEndpoint;
using SchedulerTree = test::serial_io::SchedulerTree;

// =============================================================================
// MockTimer
//
// SchedulerTree's TimerHandler is void(int) but UdpTunnel's async_wait lambdas
// take const asio::error_code&.  This thin wrapper adapts between the two.
// =============================================================================

struct MockTimer
{
    using ErrorCode = LINK_ASIO_NAMESPACE::error_code;

    MockTimer(SchedulerTree::TimerId id,
              SchedulerTree::TimePoint& now,
              std::shared_ptr<SchedulerTree> sched)
        : mId(id)
        , mNow(now)
        , mpScheduler(std::move(sched))
    {
    }

    ~MockTimer()
    {
        if (!mbMovedFrom)
            cancel();
    }

    MockTimer(const MockTimer&) = delete;

    MockTimer(MockTimer&& rhs)
        : mId(rhs.mId)
        , mNow(rhs.mNow)
        , mExpiration(rhs.mExpiration)
        , mpScheduler(std::move(rhs.mpScheduler))
    {
        rhs.mbMovedFrom = true;
    }

    template <typename Rep, typename Period>
    void expires_from_now(std::chrono::duration<Rep, Period> d)
    {
        mExpiration = mNow + d;
    }

    void cancel()
    {
        if (auto p = mpScheduler.lock())
            p->cancelTimer(mId);
    }

    template <typename Handler>
    void async_wait(Handler handler)
    {
        if (auto p = mpScheduler.lock())
        {
            p->setTimer(mId, mExpiration,
                [h = std::move(handler)](int ec) mutable
                {
                    ErrorCode aec{};
                    if (ec != 0)
                        aec = LINK_ASIO_NAMESPACE::error::operation_aborted;
                    h(aec);
                });
        }
    }

    SchedulerTree::TimerId mId;
    SchedulerTree::TimePoint& mNow;
    SchedulerTree::TimePoint mExpiration{};
    std::weak_ptr<SchedulerTree> mpScheduler;
    bool mbMovedFrom{false};
};

// =============================================================================
// MockIoContext
//
// async() runs its callable immediately (synchronous) so UdpTunnel's
// constructor side-effects (listen, runMaintenance) are observable right away.
// Timer creation delegates to the SchedulerTree so tests can advance simulated
// time via advanceTime().
// =============================================================================

struct MockIoContext
{
    template <std::size_t N>
    using Socket = TestSocket;

    using Timer = MockTimer;
    using Log = util::NullLog;

    MockIoContext(SchedulerTree::TimePoint& now, std::shared_ptr<SchedulerTree> scheduler)
        : mNow(now)
        , mpScheduler(std::move(scheduler))
    {
    }

    template <std::size_t N>
    TestSocket openBoundUdpSocket(uint16_t /*port*/)
    {
        auto s = std::make_shared<TestSocketState>();
        s->localEndpoint = UdpEndpoint{discovery::makeAddress("127.0.0.1"), nextPort++};
        boundSockets.push_back(s);
        return TestSocket{s};
    }

    template <typename Fn>
    void async(Fn fn)
    {
        fn();
    }

    Timer makeTimer() { return {mNextTimerId++, mNow, mpScheduler}; }

    Log& log() { return mLog; }

    SchedulerTree::TimePoint& mNow;
    std::shared_ptr<SchedulerTree> mpScheduler;
    SchedulerTree::TimerId mNextTimerId{0};
    Log mLog;

    std::vector<std::shared_ptr<TestSocketState>> boundSockets;
    uint16_t nextPort{20000};
};

// =============================================================================
// MockGateway
//
// Minimal Gateway stand-in: records every forwardMessageLocally() call.
// stop_listening() is a no-op (UdpTunnel::stop_listening calls it on each
// registered gateway).
// =============================================================================

struct ForwardedCall
{
    TunnelMessageType type;
    link::NodeId fromNode;
    std::optional<link::NodeId> toNode;
    std::vector<uint8_t> bytes;
};

struct MockGateway
{
    void forwardMessageLocally(TunnelMessageType type,
                               const link::NodeId& from,
                               unsigned char* begin,
                               unsigned char* end,
                               const std::optional<link::NodeId>& to)
    {
        calls.push_back({type, from, to, {begin, end}});
    }

    void stop_listening() {}

    std::vector<ForwardedCall> calls;
};

// =============================================================================
// Type alias
// =============================================================================

using TunnelT = UdpTunnel<MockIoContext&, MockGateway>;

// =============================================================================
// Fixture
// =============================================================================

const UdpEndpoint kPeerA{discovery::makeAddress("192.168.1.10"), 20808};
const UdpEndpoint kPeerB{discovery::makeAddress("192.168.1.11"), 20808};

struct Fixture
{
    Fixture()
        : mpScheduler(std::make_shared<SchedulerTree>())
        , mNow(std::chrono::system_clock::time_point{std::chrono::milliseconds{123456789}})
        , mockIo(mNow, mpScheduler)
        , mockGateway(std::make_shared<MockGateway>())
        , tunnel(std::make_shared<TunnelT>(
              util::injectRef(mockIo), uint16_t{0}, std::vector<UdpEndpoint>{}))
    {
        // UdpTunnel constructor ran listen() and runMaintenance() synchronously via
        // async(); the socket callback is now installed and the maintenance timer armed.
        // Clear any noise before tests observe the state.
        tunnel->addGateway(mockGateway);
        sentMessages().clear();
        mockGateway->calls.clear();
    }

    ~Fixture() { mpScheduler->run(); }

    std::vector<TestSocketState::SentMessage>& sentMessages()
    {
        return mockIo.boundSockets[0]->sentMessages;
    }

    // Inject bytes as if they arrived from `from` on the bound socket.
    void injectWireMessage(const UdpEndpoint& from, const std::vector<uint8_t>& data)
    {
        REQUIRE(!mockIo.boundSockets.empty());
        auto& cb = mockIo.boundSockets[0]->callback;
        REQUIRE(cb != nullptr);
        MsgArray arr{};
        const std::size_t n = std::min(data.size(), arr.size());
        std::copy(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n), arr.begin());
        cb(from, arr.cbegin(), arr.cbegin() + static_cast<std::ptrdiff_t>(n));
    }

    // Perform full handshake with `peer` and leave sentMessages cleared.
    void connectPeer(const UdpEndpoint& peer)
    {
        tunnel->addPeer(peer);
        injectWireMessage(peer, makeWireMessage(TUNNEL_HELLO, makeNodeId()));
        sentMessages().clear();
        mockGateway->calls.clear();
    }

    void advanceTime(std::chrono::seconds dur)
    {
        const auto target = mNow + dur;
        mpScheduler->run();
        auto next = mpScheduler->nextTimerExpiration();
        while (next <= target)
        {
            mNow = next;
            mpScheduler->triggerTimersUntil(mNow);
            mpScheduler->run();
            next = mpScheduler->nextTimerExpiration();
        }
        mNow = target;
    }

    bool sentToEndpoint(const UdpEndpoint& ep) const
    {
        for (const auto& [bytes, dest] : mockIo.boundSockets[0]->sentMessages)
            if (dest == ep)
                return true;
        return false;
    }

    bool sentMessageOfType(TunnelMessageType type) const
    {
        for (const auto& [bytes, dest] : mockIo.boundSockets[0]->sentMessages)
            if (!bytes.empty() && bytes[0] == static_cast<uint8_t>(type))
                return true;
        return false;
    }

    std::shared_ptr<SchedulerTree> mpScheduler;
    SchedulerTree::TimePoint mNow;
    MockIoContext mockIo;
    std::shared_ptr<MockGateway> mockGateway;
    std::shared_ptr<TunnelT> tunnel;
};

} // anonymous namespace

// =============================================================================
// Connection lifecycle
// =============================================================================

TEST_CASE("UdpTunnel | C1 | HELLO from known peer → CONNECTED, echoes HELLO back")
{
    Fixture f;
    f.tunnel->addPeer(kPeerA);
    f.sentMessages().clear();

    f.injectWireMessage(kPeerA, makeWireMessage(TUNNEL_HELLO, makeNodeId()));

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(f.sentMessageOfType(TUNNEL_HELLO));
}

TEST_CASE("UdpTunnel | C2 | Second HELLO from already-connected peer does not re-echo HELLO")
{
    Fixture f;
    f.connectPeer(kPeerA);

    // Peer is already CONNECTED — wasConnected==true, no echo.
    f.injectWireMessage(kPeerA, makeWireMessage(TUNNEL_HELLO, makeNodeId()));

    CHECK(f.sentMessages().empty());
}

TEST_CASE("UdpTunnel | C3 | HELLO from unknown peer fires callback exactly once (mPendingPeers de-dup)")
{
    Fixture f;
    int callbackCount = 0;
    // Defer the reject so kPeerA remains in mPendingPeers when the second
    // HELLO arrives — that is when the de-dup guard actually fires.
    std::function<void()> storedReject;
    f.tunnel->setUnknownPeerCallback(
        [&callbackCount, &storedReject](UdpEndpoint, std::function<void()> /*accept*/,
                                        std::function<void()> reject)
        {
            ++callbackCount;
            storedReject = std::move(reject);
        });

    const auto msg = makeWireMessage(TUNNEL_HELLO, makeNodeId());
    f.injectWireMessage(kPeerA, msg); // fires callback; peer enters mPendingPeers
    f.injectWireMessage(kPeerA, msg); // still pending → de-dup guard fires, no callback

    CHECK(callbackCount == 1);

    if (storedReject)
        storedReject();
}

TEST_CASE("UdpTunnel | C4 | Unknown-peer accept → peer added, HELLO sent")
{
    Fixture f;
    f.tunnel->setUnknownPeerCallback(
        [](UdpEndpoint, std::function<void()> accept, std::function<void()> /*reject*/)
        { accept(); });

    f.injectWireMessage(kPeerA, makeWireMessage(TUNNEL_HELLO, makeNodeId()));

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(f.sentMessageOfType(TUNNEL_HELLO));
}

TEST_CASE("UdpTunnel | C5 | Unknown-peer reject → peer absent, nothing sent")
{
    Fixture f;
    f.tunnel->setUnknownPeerCallback(
        [](UdpEndpoint, std::function<void()> /*accept*/, std::function<void()> reject)
        { reject(); });

    f.injectWireMessage(kPeerA, makeWireMessage(TUNNEL_HELLO, makeNodeId()));

    CHECK(f.sentMessages().empty());
}

TEST_CASE("UdpTunnel | C6 | TUNNEL_BYE marks peer DISCONNECTED (maintenance reconnects it)")
{
    Fixture f;
    f.connectPeer(kPeerA);

    f.injectWireMessage(kPeerA, makeWireMessage(TUNNEL_BYE, makeNodeId()));
    f.sentMessages().clear();

    // Next maintenance tick should see DISCONNECTED and send HELLO.
    f.advanceTime(TunnelT::kMaintenanceInterval);

    CHECK(f.sentMessageOfType(TUNNEL_HELLO));
    CHECK(f.sentToEndpoint(kPeerA));
}

TEST_CASE("UdpTunnel | C7 | stop_listening sends TUNNEL_BYE to every connected peer")
{
    Fixture f;
    f.connectPeer(kPeerA);
    f.connectPeer(kPeerB);

    f.tunnel->stop_listening();

    int byeCount = 0;
    for (const auto& [bytes, dest] : f.sentMessages())
        if (!bytes.empty() && bytes[0] == static_cast<uint8_t>(TUNNEL_BYE))
            ++byeCount;
    CHECK(byeCount == 2);
}

// =============================================================================
// Maintenance (timer-driven)
// =============================================================================

TEST_CASE("UdpTunnel | M1 | Maintenance tick sends TUNNEL_HELLO to DISCONNECTED peer")
{
    Fixture f;
    f.tunnel->addPeer(kPeerA); // remains DISCONNECTED (no HELLO injected)
    f.sentMessages().clear();

    f.advanceTime(TunnelT::kMaintenanceInterval);

    CHECK(f.sentMessageOfType(TUNNEL_HELLO));
    CHECK(f.sentToEndpoint(kPeerA));
}

TEST_CASE("UdpTunnel | M2 | Maintenance tick sends TUNNEL_KEEPALIVE to CONNECTED peer")
{
    Fixture f;
    f.connectPeer(kPeerA);

    f.advanceTime(TunnelT::kMaintenanceInterval);

    CHECK(f.sentMessageOfType(TUNNEL_KEEPALIVE));
    CHECK(f.sentToEndpoint(kPeerA));
}

// M3 (keepalive wall-clock timeout) is not tested here: runMaintenance() uses
// std::chrono::system_clock::now() for last_seen comparison, which is not
// controlled by the simulated SchedulerTree clock.

TEST_CASE("UdpTunnel | M4 | After stop_listening, maintenance timer does not fire")
{
    Fixture f;
    f.connectPeer(kPeerA);
    f.tunnel->stop_listening();
    f.sentMessages().clear();

    f.advanceTime(TunnelT::kMaintenanceInterval * 3);

    CHECK(f.sentMessages().empty());
}

// =============================================================================
// Forward — outbound (UdpTunnel → wire)
// =============================================================================

TEST_CASE("UdpTunnel | F1 | BROADCAST forward reaches all CONNECTED peers")
{
    Fixture f;
    f.connectPeer(kPeerA);
    f.connectPeer(kPeerB);

    const auto fromNode = makeNodeId();
    const std::vector<uint8_t> payload{1, 2, 3};
    f.tunnel->forward(BROADCAST, nullptr, fromNode,
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        {});

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(f.sentToEndpoint(kPeerB));
}

TEST_CASE("UdpTunnel | F2 | BROADCAST forward skips DISCONNECTED peers")
{
    Fixture f;
    f.tunnel->addPeer(kPeerA); // DISCONNECTED — no handshake
    f.connectPeer(kPeerB);
    f.sentMessages().clear();

    const auto fromNode = makeNodeId();
    const std::vector<uint8_t> payload{1, 2, 3};
    f.tunnel->forward(BROADCAST, nullptr, fromNode,
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        {});

    CHECK(!f.sentToEndpoint(kPeerA));
    CHECK(f.sentToEndpoint(kPeerB));
}

TEST_CASE("UdpTunnel | F3 | UNICAST with known to_node reaches that peer's endpoint")
{
    Fixture f;
    f.connectPeer(kPeerA);

    // Establish nodeId→kPeerA mapping via an inbound BROADCAST.
    const auto remoteNodeId = makeNodeId();
    f.injectWireMessage(kPeerA, makeWireMessage(BROADCAST, remoteNodeId));
    f.sentMessages().clear();

    const auto fromNode = makeNodeId();
    const std::vector<uint8_t> payload{4, 5, 6};
    f.tunnel->forward(UNICAST, nullptr, fromNode,
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        remoteNodeId);

    CHECK(f.sentToEndpoint(kPeerA));
}

TEST_CASE("UdpTunnel | F4 | UNICAST with unknown to_node_id sends nothing")
{
    Fixture f;
    f.connectPeer(kPeerA);
    f.sentMessages().clear();

    const auto fromNode = makeNodeId();
    const auto unknownTarget = makeNodeId();
    const std::vector<uint8_t> payload{1};
    f.tunnel->forward(UNICAST, nullptr, fromNode,
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        unknownTarget);

    CHECK(f.sentMessages().empty());
}

TEST_CASE("UdpTunnel | F5 | UNICAST with nullopt to_node sends nothing")
{
    Fixture f;
    f.connectPeer(kPeerA);
    f.sentMessages().clear();

    const auto fromNode = makeNodeId();
    const std::vector<uint8_t> payload{1};
    // encapsulate() returns nullopt when to_node is absent for UNICAST.
    f.tunnel->forward(UNICAST, nullptr, fromNode,
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        {});

    CHECK(f.sentMessages().empty());
}

TEST_CASE("UdpTunnel | F6 | Sent bytes are trimmed to encapsulated size, not full 512-byte buffer")
{
    Fixture f;
    f.connectPeer(kPeerA);
    f.sentMessages().clear();

    const auto fromNode = makeNodeId();
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5};
    f.tunnel->forward(BROADCAST, nullptr, fromNode,
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        {});

    REQUIRE(!f.sentMessages().empty());
    CHECK(f.sentMessages()[0].first.size() < discovery::v1::kMaxMessageSize);
}

// =============================================================================
// Forward locally — inbound (wire → gateways)
// =============================================================================

TEST_CASE("UdpTunnel | R1 | BROADCAST from connected peer calls forwardMessageLocally on gateway")
{
    Fixture f;
    f.connectPeer(kPeerA);

    const auto remoteNodeId = makeNodeId();
    f.injectWireMessage(kPeerA, makeWireMessage(BROADCAST, remoteNodeId));

    REQUIRE(f.mockGateway->calls.size() == 1);
    CHECK(f.mockGateway->calls[0].type == BROADCAST);
    CHECK(f.mockGateway->calls[0].fromNode == remoteNodeId);
}

TEST_CASE("UdpTunnel | R2 | BROADCAST from disconnected peer is dropped")
{
    Fixture f;
    f.tunnel->addPeer(kPeerA); // DISCONNECTED

    f.injectWireMessage(kPeerA, makeWireMessage(BROADCAST, makeNodeId()));

    CHECK(f.mockGateway->calls.empty());
}

TEST_CASE("UdpTunnel | R3 | BYEBYE erases node-ID mapping and calls gateway")
{
    Fixture f;
    f.connectPeer(kPeerA);

    const auto remoteNodeId = makeNodeId();
    f.injectWireMessage(kPeerA, makeWireMessage(BROADCAST, remoteNodeId));
    f.mockGateway->calls.clear();

    f.injectWireMessage(kPeerA, makeWireMessage(BYEBYE, remoteNodeId));

    REQUIRE(f.mockGateway->calls.size() == 1);
    CHECK(f.mockGateway->calls[0].type == BYEBYE);
    CHECK(f.mockGateway->calls[0].fromNode == remoteNodeId);
}

TEST_CASE("UdpTunnel | R4 | UNICAST from connected peer calls gateway with correct to_node")
{
    Fixture f;
    f.connectPeer(kPeerA);

    const auto remoteNodeId = makeNodeId();
    const auto localNodeId = makeNodeId();
    f.injectWireMessage(kPeerA, makeWireMessage(UNICAST, remoteNodeId, {}, localNodeId));

    REQUIRE(f.mockGateway->calls.size() == 1);
    CHECK(f.mockGateway->calls[0].type == UNICAST);
    CHECK(f.mockGateway->calls[0].fromNode == remoteNodeId);
    REQUIRE(f.mockGateway->calls[0].toNode.has_value());
    CHECK(f.mockGateway->calls[0].toNode.value() == localNodeId);
}

TEST_CASE("UdpTunnel | R5 | remoteNodeIdToPeerIdx keeps first-seen mapping (emplace is idempotent)")
{
    Fixture f;
    f.connectPeer(kPeerA);
    f.connectPeer(kPeerB);

    const auto remoteNodeId = makeNodeId();
    // First-seen: comes from kPeerA.
    f.injectWireMessage(kPeerA, makeWireMessage(BROADCAST, remoteNodeId));
    // Same node ID arrives from kPeerB — must NOT override the mapping.
    f.injectWireMessage(kPeerB, makeWireMessage(BROADCAST, remoteNodeId));
    f.sentMessages().clear();

    // UNICAST targeting remoteNodeId must go to kPeerA, not kPeerB.
    const auto fromNode = makeNodeId();
    const std::vector<uint8_t> payload{7};
    f.tunnel->forward(UNICAST, nullptr, fromNode,
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        remoteNodeId);

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(!f.sentToEndpoint(kPeerB));
}

// =============================================================================
// Peer management
// =============================================================================

TEST_CASE("UdpTunnel | P1 | addPeer adds entry and immediately sends TUNNEL_HELLO")
{
    Fixture f;
    f.tunnel->addPeer(kPeerA);

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(f.sentMessageOfType(TUNNEL_HELLO));
}

TEST_CASE("UdpTunnel | P2 | addPeer with duplicate endpoint is a no-op")
{
    Fixture f;
    f.tunnel->addPeer(kPeerA);
    const std::size_t countAfterFirst = f.sentMessages().size();

    f.tunnel->addPeer(kPeerA);

    CHECK(f.sentMessages().size() == countAfterFirst);
}

TEST_CASE("UdpTunnel | P3 | removePeer sends TUNNEL_BYE and erases node-ID mapping")
{
    Fixture f;
    f.connectPeer(kPeerA);

    const auto remoteNodeId = makeNodeId();
    f.injectWireMessage(kPeerA, makeWireMessage(BROADCAST, remoteNodeId));
    f.sentMessages().clear();

    f.tunnel->removePeer(kPeerA);

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(f.sentMessageOfType(TUNNEL_BYE));

    // Mapping was erased: UNICAST to remoteNodeId must now send nothing.
    f.sentMessages().clear();
    const std::vector<uint8_t> payload{1};
    f.tunnel->forward(UNICAST, nullptr, makeNodeId(),
        reinterpret_cast<const unsigned char*>(payload.data()),
        reinterpret_cast<const unsigned char*>(payload.data() + payload.size()),
        remoteNodeId);
    CHECK(f.sentMessages().empty());
}

} // namespace extender
} // namespace ableton
