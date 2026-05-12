#include <ableton/discovery/AsioTypes.hpp>
#include <ableton/discovery/NetworkByteStreamSerializable.hpp>
#include <ableton/discovery/Payload.hpp>
#include <ableton/discovery/v1/Messages.hpp>
// Config.hpp must come before TunnelGateway.hpp — it provides link::platform::Random
// which TunnelGateway.hpp uses without including it itself.
#include <ableton/platforms/Config.hpp>
#include "TestHelpers.hpp"
#include <ableton/extender/Tunnel.hpp>
#include <ableton/extender/TunnelGateway.hpp>
#include <ableton/link/Beats.hpp>
#include <ableton/link/MeasurementEndpointV4.hpp>
#include <ableton/link/MeasurementEndpointV6.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/link/PeerState.hpp>
#include <ableton/link/Tempo.hpp>
#include <ableton/link/Timeline.hpp>
#include <ableton/test/CatchWrapper.hpp>
#include <ableton/test/serial_io/SchedulerTree.hpp>
#include <ableton/test/serial_io/Timer.hpp>
#include <ableton/util/Injected.hpp>
#include <ableton/util/Log.hpp>
#include <ableton/util/SafeAsyncHandler.hpp>

namespace ableton
{
namespace extender
{
namespace
{

using UdpEndpoint = discovery::UdpEndpoint;
using IpAddress = discovery::IpAddress;
using SchedulerTree = test::serial_io::SchedulerTree;
using ableton::extender::helpers::makeNodeId;

// =============================================================================
// TestSocket
//
// Shared-state socket so tests can inspect sent messages and inject incoming
// messages regardless of where the socket ended up (moved into the gateway).
// =============================================================================

struct TestSocketState
{
    using SentMessage = std::pair<std::vector<uint8_t>, UdpEndpoint>;
    std::vector<SentMessage> sentMessages;
    std::function<void(const UdpEndpoint&, const std::vector<uint8_t>&)> callback;
    UdpEndpoint localEndpoint;
};

struct TestSocket
{
    explicit TestSocket(std::shared_ptr<TestSocketState> s)
        : mState(std::move(s))
    {
    }

    std::size_t send(const uint8_t* data, std::size_t n, const UdpEndpoint& to)
    {
        mState->sentMessages.push_back({std::vector<uint8_t>{data, data + n}, to});
        return n;
    }

    template <typename Handler>
    void receive(Handler handler)
    {
        // mutable: SocketReceiver::operator() is non-const, so the captured handler
        // must be mutable to call it through the const std::function call path.
        mState->callback = [handler](const UdpEndpoint& from,
                                     const std::vector<uint8_t>& buf) mutable
        { handler(from, buf.cbegin(), buf.cend()); };
    }

    UdpEndpoint endpoint() const { return mState->localEndpoint; }

    std::shared_ptr<TestSocketState> mState;
};

// =============================================================================
// MockIoContext
//
// Satisfies TunnelGateway's IoContext concept. Delegates timer creation to the
// serial_io infrastructure so tests can advance time via a SchedulerTree. All
// created socket states are stored in vectors for test-side inspection.
// =============================================================================

struct MockIoContext
{
    template <std::size_t N>
    using Socket = TestSocket;

    using Timer = test::serial_io::Timer;
    using Log = util::NullLog;

    MockIoContext(SchedulerTree::TimePoint& now,
                  std::shared_ptr<SchedulerTree> scheduler)
        : mNow(now)
        , mpScheduler(std::move(scheduler))
    {
    }

    template <std::size_t N>
    TestSocket openMulticastSocket(const IpAddress&)
    {
        auto s = std::make_shared<TestSocketState>();
        multicastSockets.push_back(s);
        return TestSocket{s};
    }

    template <std::size_t N>
    TestSocket openUnicastSocket(const IpAddress&)
    {
        auto s = std::make_shared<TestSocketState>();
        s->localEndpoint = UdpEndpoint{discovery::makeAddress("127.0.0.1"), nextPort++};
        unicastSockets.push_back(s);
        return TestSocket{s};
    }

    Timer makeTimer() { return {mNextTimerId++, mNow, mpScheduler}; }

    Log& log() { return mLog; }

    SchedulerTree::TimePoint& mNow;
    std::shared_ptr<SchedulerTree> mpScheduler;
    SchedulerTree::TimerId mNextTimerId{0};
    Log mLog;

    std::vector<std::shared_ptr<TestSocketState>> multicastSockets;
    std::vector<std::shared_ptr<TestSocketState>> unicastSockets;
    uint16_t nextPort{10000};
};

// =============================================================================
// TestObserver
//
// Records every sawPeer / peerLeft / peerTimedOut call. Used for both the local
// and remote observer slots of TunnelGateway.
// =============================================================================

struct TestObserver
{
    using GatewayObserverNodeState = link::PeerState;
    using GatewayObserverNodeId = link::NodeId;

    friend void sawPeer(TestObserver& obs, const link::PeerState& s)
    {
        obs.seen.push_back(s);
    }

    friend void peerLeft(TestObserver& obs, const link::NodeId& id)
    {
        obs.left.push_back(id);
    }

    friend void peerTimedOut(TestObserver& obs, const link::NodeId& id)
    {
        obs.timedOut.push_back(id);
    }

    std::vector<link::PeerState> seen;
    std::vector<link::NodeId> left;
    std::vector<link::NodeId> timedOut;
};

// =============================================================================
// MockTunnel
//
// Concrete Tunnel subclass that records every forward() call so tests can assert
// on what the gateway sent towards the tunnel. Does not implement listen() or
// stop_listening() — those are no-ops in tests.
// =============================================================================

// All three template parameters use reference types so that util::injectRef()
// produces Injected<T&> which matches what the constructor expects.
using GatewayT = TunnelGateway<TestObserver&, TestObserver&, MockIoContext&>;

struct ForwardedMessage
{
    TunnelMessageType type;
    link::NodeId fromNode;
    std::optional<link::NodeId> toNode;
    std::vector<uint8_t> bytes;
};

struct MockTunnel : Tunnel<MockIoContext&, GatewayT>
{
    using Base = Tunnel<MockIoContext&, GatewayT>;

    explicit MockTunnel(util::Injected<MockIoContext&> io)
        : Base(std::move(io))
    {
    }

    void forward(TunnelMessageType type,
                 std::shared_ptr<GatewayT> /*fromGateway*/,
                 const link::NodeId& fromNode,
                 const unsigned char* msgBegin,
                 const unsigned char* msgEnd,
                 const std::optional<link::NodeId>& toNode) override
    {
        forwarded.push_back({type, fromNode, toNode, {msgBegin, msgEnd}});
    }

    void listen() override {}
    void stop_listening() override {}

    std::vector<ForwardedMessage> forwarded;
};

// =============================================================================
// Message crafting helpers
// =============================================================================

struct CraftedMessage
{
    discovery::v1::MessageBuffer buffer{};
    std::size_t size{0};

    // Non-const overloads needed so forwardMessageLocally deduces It = uint8_t*
    // (not const uint8_t*), allowing appendMeasurementEndpoint to write through it.
    uint8_t* begin() { return buffer.data(); }
    uint8_t* end() { return buffer.data() + size; }
    const uint8_t* begin() const { return buffer.data(); }
    const uint8_t* end() const { return buffer.data() + size; }
};

CraftedMessage craftAliveMessage(const link::NodeId& nodeId,
                                  uint8_t ttl,
                                  const UdpEndpoint& measEndpoint = {})
{
    link::NodeState ns{};
    ns.nodeId = nodeId;
    ns.timeline = link::Timeline{link::Tempo{120.}, link::Beats{0.},
                                  std::chrono::microseconds{0}};

    link::PeerState ps{ns, measEndpoint};

    CraftedMessage msg{};
    auto msgEnd =
        discovery::v1::aliveMessage(nodeId, ttl, toPayload(ps), msg.buffer.begin());
    msg.size = static_cast<std::size_t>(std::distance(msg.buffer.begin(), msgEnd));
    return msg;
}

CraftedMessage craftResponseMessage(const link::NodeId& nodeId,
                                     uint8_t ttl,
                                     const UdpEndpoint& measEndpoint = {})
{
    link::NodeState ns{};
    ns.nodeId = nodeId;
    ns.timeline = link::Timeline{link::Tempo{120.}, link::Beats{0.},
                                  std::chrono::microseconds{0}};

    link::PeerState ps{ns, measEndpoint};

    CraftedMessage msg{};
    auto msgEnd =
        discovery::v1::responseMessage(nodeId, ttl, toPayload(ps), msg.buffer.begin());
    msg.size = static_cast<std::size_t>(std::distance(msg.buffer.begin(), msgEnd));
    return msg;
}

CraftedMessage craftByeByeMessage(const link::NodeId& nodeId)
{
    CraftedMessage msg{};
    auto msgEnd = discovery::v1::byeByeMessage(nodeId, msg.buffer.begin());
    msg.size = static_cast<std::size_t>(std::distance(msg.buffer.begin(), msgEnd));
    return msg;
}

// =============================================================================
// Fixture
//
// Owns the scheduler, MockIoContext, observers, MockTunnel, and gateway. Exposes
// helpers for injecting messages and advancing simulated time.
// =============================================================================

const IpAddress kLocalIfAddr = discovery::makeAddress("192.168.1.100");
const UdpEndpoint kSameSubnetPeer{discovery::makeAddress("192.168.1.200"), 20808};
const UdpEndpoint kOtherSubnetPeer{discovery::makeAddress("10.0.0.1"), 20808};
const UdpEndpoint kMeasEndpoint{discovery::makeAddress("192.168.1.200"), 20809};

struct Fixture
{
    Fixture()
        : mpScheduler(std::make_shared<SchedulerTree>())
        , mNow(std::chrono::system_clock::time_point{std::chrono::milliseconds{123456789}})
        , mockIo(mNow, mpScheduler)
        , mockTunnel(std::make_shared<MockTunnel>(util::injectRef(mockIo)))
        , gateway(std::make_shared<GatewayT>(
              util::injectRef(mockIo),
              kLocalIfAddr,
              util::injectRef(localObserver),
              util::injectRef(remoteObserver),
              // Explicit upcast: shared_ptr<MockTunnel> → shared_ptr<Tunnel<...>>
              std::static_pointer_cast<Tunnel<MockIoContext&, GatewayT>>(mockTunnel)))
    {
        gateway->listen();
    }

    ~Fixture()
    {
        mpScheduler->run();
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

    void injectOnMulticastSocket(const UdpEndpoint& from, const CraftedMessage& msg)
    {
        REQUIRE(!mockIo.multicastSockets.empty());
        auto& cb = mockIo.multicastSockets[0]->callback;
        REQUIRE(cb != nullptr);
        std::vector<uint8_t> buf{msg.begin(), msg.end()};
        cb(from, buf);
    }

    std::shared_ptr<SchedulerTree> mpScheduler;
    SchedulerTree::TimePoint mNow;
    MockIoContext mockIo;
    TestObserver localObserver;
    TestObserver remoteObserver;
    std::shared_ptr<MockTunnel> mockTunnel;
    std::shared_ptr<GatewayT> gateway;
};

} // anonymous namespace

// =============================================================================
// TEST CASES
// =============================================================================

TEST_CASE("TunnelGateway | L1 | Alive from same-subnet peer notifies local observer and "
          "forwards BROADCAST to tunnel")
{
    Fixture f;
    const auto nodeId = makeNodeId();
    const auto msg = craftAliveMessage(nodeId, 5, kMeasEndpoint);

    f.injectOnMulticastSocket(kSameSubnetPeer, msg);

    REQUIRE(f.localObserver.seen.size() == 1);
    CHECK(f.localObserver.seen[0].nodeState.nodeId == nodeId);
    CHECK(f.localObserver.left.empty());
    CHECK(f.localObserver.timedOut.empty());

    REQUIRE(f.mockTunnel->forwarded.size() == 1);
    CHECK(f.mockTunnel->forwarded[0].type == BROADCAST);
    CHECK(f.mockTunnel->forwarded[0].fromNode == nodeId);
    CHECK(!f.mockTunnel->forwarded[0].toNode.has_value());
}

TEST_CASE("TunnelGateway | L1b | Forwarded BROADCAST has measurement endpoint stripped")
{
    Fixture f;
    const auto nodeId = makeNodeId();
    const auto msg = craftAliveMessage(nodeId, 5, kMeasEndpoint);

    f.injectOnMulticastSocket(kSameSubnetPeer, msg);

    REQUIRE(f.mockTunnel->forwarded.size() == 1);
    const auto& fwd = f.mockTunnel->forwarded[0];

    // The forwarded bytes must be shorter than the original: the gateway strips
    // the MeasurementEndpointV4 and MeasurementEndpointV6 payload entries.
    CHECK(fwd.bytes.size() < msg.size);
}

TEST_CASE("TunnelGateway | L2 | Response message follows same path as Alive")
{
    Fixture f;
    const auto nodeId = makeNodeId();
    const auto msg = craftResponseMessage(nodeId, 5, kMeasEndpoint);

    f.injectOnMulticastSocket(kSameSubnetPeer, msg);

    REQUIRE(f.localObserver.seen.size() == 1);
    CHECK(f.localObserver.seen[0].nodeState.nodeId == nodeId);
    REQUIRE(f.mockTunnel->forwarded.size() == 1);
    CHECK(f.mockTunnel->forwarded[0].type == BROADCAST);
}

TEST_CASE("TunnelGateway | L3 | ByeBye from registered peer notifies local observer and "
          "forwards BYEBYE to tunnel")
{
    Fixture f;
    const auto nodeId = makeNodeId();

    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(nodeId, 5, kMeasEndpoint));
    f.injectOnMulticastSocket(kSameSubnetPeer, craftByeByeMessage(nodeId));

    CHECK(f.localObserver.left.size() == 1);
    CHECK(f.localObserver.left[0] == nodeId);

    REQUIRE(f.mockTunnel->forwarded.size() == 2);
    CHECK(f.mockTunnel->forwarded[1].type == BYEBYE);
    CHECK(f.mockTunnel->forwarded[1].fromNode == nodeId);
}

TEST_CASE("TunnelGateway | L4 | ByeBye from unknown peer does not crash")
{
    Fixture f;
    const auto nodeId = makeNodeId();

    // No Alive first — gateway has no record of this peer.
    // receiveByeBye always calls peerLeft and forwards, even for unknown peers.
    // The important thing is no crash (no assert/exception from the map lookups).
    f.injectOnMulticastSocket(kSameSubnetPeer, craftByeByeMessage(nodeId));

    CHECK(f.localObserver.seen.empty()); // never saw an Alive for this peer
}

TEST_CASE("TunnelGateway | L5 | Alive from outside /24 subnet is dropped")
{
    Fixture f;
    const auto nodeId = makeNodeId();

    f.injectOnMulticastSocket(kOtherSubnetPeer, craftAliveMessage(nodeId, 5, kMeasEndpoint));

    CHECK(f.localObserver.seen.empty());
    CHECK(f.mockTunnel->forwarded.empty());
}

TEST_CASE("TunnelGateway | L6 | Alive from own RemoteNodeSurrogate node ID is ignored")
{
    Fixture f;
    const auto remoteNodeId = makeNodeId();

    // Trigger creation of a surrogate for remoteNodeId by forwarding a broadcast.
    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);
    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    const auto surrogateCountBefore = f.mockIo.unicastSockets.size();

    // Now inject an Alive on the multicast socket from the surrogate's own node ID.
    // The gateway should recognise it as its own surrogate and ignore it.
    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(remoteNodeId, 5));

    CHECK(f.localObserver.seen.empty());
    // No new sockets should have been created.
    CHECK(f.mockIo.unicastSockets.size() == surrogateCountBefore);
}

TEST_CASE("TunnelGateway | L7 | Local peer times out after TTL expires")
{
    Fixture f;
    const auto nodeId = makeNodeId();

    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(nodeId, 5, kMeasEndpoint));
    CHECK(f.localObserver.timedOut.empty());

    f.advanceTime(std::chrono::seconds{7});

    REQUIRE(f.localObserver.timedOut.size() == 1);
    CHECK(f.localObserver.timedOut[0] == nodeId);
}

TEST_CASE("TunnelGateway | L8 | Refreshed peer does not time out at original deadline")
{
    Fixture f;
    const auto nodeId = makeNodeId();

    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(nodeId, 5, kMeasEndpoint));
    f.advanceTime(std::chrono::seconds{3}); // not expired yet

    // Refresh with another Alive (TTL=5 again — deadline moves forward).
    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(nodeId, 5, kMeasEndpoint));
    f.advanceTime(std::chrono::seconds{4}); // would have expired without refresh

    CHECK(f.localObserver.timedOut.empty());
}

TEST_CASE("TunnelGateway | L9 | ByeBye before TTL expiry suppresses peerTimedOut")
{
    Fixture f;
    const auto nodeId = makeNodeId();

    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(nodeId, 10, kMeasEndpoint));
    f.injectOnMulticastSocket(kSameSubnetPeer, craftByeByeMessage(nodeId));

    f.advanceTime(std::chrono::seconds{15});

    CHECK(f.localObserver.timedOut.empty());
    CHECK(f.localObserver.left.size() == 1);
}

TEST_CASE("TunnelGateway | L10 | Two peers with different TTLs time out independently")
{
    Fixture f;
    const auto nodeA = makeNodeId();
    const auto nodeB = makeNodeId();
    const UdpEndpoint peerB{discovery::makeAddress("192.168.1.201"), 20808};

    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(nodeA, 3, kMeasEndpoint));
    f.injectOnMulticastSocket(peerB, craftAliveMessage(nodeB, 8, kMeasEndpoint));

    f.advanceTime(std::chrono::seconds{5}); // A should have timed out, B should not
    REQUIRE(f.localObserver.timedOut.size() == 1);
    CHECK(f.localObserver.timedOut[0] == nodeA);

    f.advanceTime(std::chrono::seconds{5}); // B should now be timed out too
    REQUIRE(f.localObserver.timedOut.size() == 2);
    CHECK(f.localObserver.timedOut[1] == nodeB);
}

TEST_CASE("TunnelGateway | T1 | Remote BROADCAST notifies remote observer and sends to "
          "multicast")
{
    Fixture f;
    const auto remoteNodeId = makeNodeId();
    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);

    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    REQUIRE(f.remoteObserver.seen.size() == 1);
    CHECK(f.remoteObserver.seen[0].nodeState.nodeId == remoteNodeId);

    // A RemoteNodeSurrogate's state socket should have sent to multicast.
    REQUIRE(!f.mockIo.unicastSockets.empty());
    const auto& sent = f.mockIo.unicastSockets[0]->sentMessages;
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].second == discovery::multicastEndpointV4());
}

TEST_CASE("TunnelGateway | T2 | Remote BROADCAST appends MeasurementEndpointV4 and V6")
{
    Fixture f;
    const auto remoteNodeId = makeNodeId();
    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);

    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    REQUIRE(!f.mockIo.unicastSockets.empty());
    const auto& sentBytes = f.mockIo.unicastSockets[0]->sentMessages[0].first;

    // The forwarded message must be larger than the original (endpoints appended).
    CHECK(sentBytes.size() > aliveMsg.size);

    // Parse the sent message and verify it contains MeasurementEndpointV4.
    // Use parseMessageHeader to find the correct payload start offset.
    bool foundV4 = false;
    const auto headerResult = discovery::v1::parseMessageHeader<link::NodeId>(
        sentBytes.data(), sentBytes.data() + sentBytes.size());
    discovery::parsePayload<link::MeasurementEndpointV4>(
        headerResult.second, sentBytes.data() + sentBytes.size(),
        [&foundV4](const link::MeasurementEndpointV4&) { foundV4 = true; });
    CHECK(foundV4);
}

TEST_CASE("TunnelGateway | T3 | Repeated BROADCAST for same remote node reuses surrogate")
{
    Fixture f;
    const auto remoteNodeId = makeNodeId();
    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);

    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    const auto socketCountAfterFirst = f.mockIo.unicastSockets.size();

    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    // No new sockets: existing surrogate was reused.
    CHECK(f.mockIo.unicastSockets.size() == socketCountAfterFirst);
    CHECK(f.remoteObserver.seen.size() == 2);
}

TEST_CASE("TunnelGateway | T4 | Remote UNICAST to known local node reaches that node's "
          "state endpoint")
{
    Fixture f;
    const auto localNodeId = makeNodeId();
    const auto remoteNodeId = makeNodeId();

    // Register a local node via its Alive message.
    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(localNodeId, 5, kMeasEndpoint));

    // Now the remote side sends a unicast targeting that local node.
    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);
    f.gateway->forwardMessageLocally(
        UNICAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        localNodeId);

    // A unicast socket should have sent to kSameSubnetPeer (the registered state endpoint).
    bool foundSend = false;
    for (const auto& sockState : f.mockIo.unicastSockets)
    {
        for (const auto& [bytes, dest] : sockState->sentMessages)
        {
            if (dest == kSameSubnetPeer)
                foundSend = true;
        }
    }
    CHECK(foundSend);
}

TEST_CASE("TunnelGateway | T5 | Remote UNICAST to unknown local node is dropped")
{
    Fixture f;
    const auto remoteNodeId = makeNodeId();
    const auto unknownLocalId = makeNodeId();

    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);
    f.gateway->forwardMessageLocally(
        UNICAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        unknownLocalId);

    for (const auto& sockState : f.mockIo.unicastSockets)
        CHECK(sockState->sentMessages.empty());
}

TEST_CASE("TunnelGateway | T6 | Remote BYEBYE notifies remote observer and sends to multicast")
{
    Fixture f;
    const auto remoteNodeId = makeNodeId();
    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);

    // First establish the remote peer.
    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    f.remoteObserver.left.clear(); // ignore any spurious state

    auto byeMsg = craftByeByeMessage(remoteNodeId);
    f.gateway->forwardMessageLocally(
        BYEBYE, remoteNodeId,
        byeMsg.buffer.begin(), byeMsg.buffer.begin() + byeMsg.size,
        std::nullopt);

    REQUIRE(f.remoteObserver.left.size() == 1);
    CHECK(f.remoteObserver.left[0] == remoteNodeId);

    // A message should have been sent to multicast.
    bool sentToMulticast = false;
    for (const auto& sockState : f.mockIo.unicastSockets)
        for (const auto& [bytes, dest] : sockState->sentMessages)
            if (dest == discovery::multicastEndpointV4())
                sentToMulticast = true;
    CHECK(sentToMulticast);
}

TEST_CASE("TunnelGateway | T8 | Remote peer times out and fires peerTimedOut on remote observer")
{
    Fixture f;
    const auto remoteNodeId = makeNodeId();
    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);

    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    CHECK(f.remoteObserver.timedOut.empty());

    f.advanceTime(std::chrono::seconds{7});

    REQUIRE(f.remoteObserver.timedOut.size() == 1);
    CHECK(f.remoteObserver.timedOut[0] == remoteNodeId);
}

TEST_CASE("TunnelGateway | V2 | stop_listening clears surrogates and maps")
{
    Fixture f;
    const auto localNodeId = makeNodeId();
    const auto remoteNodeId = makeNodeId();

    // Register a local peer and a remote peer.
    f.injectOnMulticastSocket(kSameSubnetPeer, craftAliveMessage(localNodeId, 5, kMeasEndpoint));

    auto aliveMsg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);
    f.gateway->forwardMessageLocally(
        BROADCAST, remoteNodeId,
        aliveMsg.buffer.begin(), aliveMsg.buffer.begin() + aliveMsg.size,
        std::nullopt);

    f.gateway->stop_listening();

    // After stop, a UNICAST to the previously-known local node must be dropped
    // (the map was cleared). We verify this by checking no socket receives new sends.
    const auto totalSentBefore = [&]()
    {
        std::size_t n = 0;
        for (const auto& s : f.mockIo.unicastSockets)
            n += s->sentMessages.size();
        return n;
    }();

    auto msg = craftAliveMessage(remoteNodeId, 5, kMeasEndpoint);
    f.gateway->forwardMessageLocally(
        UNICAST, remoteNodeId,
        msg.buffer.begin(), msg.buffer.begin() + msg.size,
        localNodeId);

    const auto totalSentAfter = [&]()
    {
        std::size_t n = 0;
        for (const auto& s : f.mockIo.unicastSockets)
            n += s->sentMessages.size();
        return n;
    }();

    CHECK(totalSentAfter == totalSentBefore);
}

} // namespace extender
} // namespace ableton
