// Config.hpp must come first — it defines LINK_ASIO_NAMESPACE used everywhere.
#include <ableton/platforms/Config.hpp>
#include <ableton/extender/Controller.hpp>
#include <ableton/discovery/AsioTypes.hpp>
#include <ableton/link/StartStopState.hpp>
#include <ableton/link/Tempo.hpp>
#include <ableton/link/Timeline.hpp>
#include <ableton/test/CatchWrapper.hpp>
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

// =============================================================================
// MockTimer
//
// Stores async_wait handlers as std::function<void(const error_code&)> so it
// is compatible with both UdpTunnel (hardcodes error_code) and InterfaceScanner
// (uses Timer::ErrorCode). cancel() fires the stored handler with
// operation_aborted, matching the real Asio timer behaviour.
// =============================================================================

struct MockTimer
{
    using ErrorCode = LINK_ASIO_NAMESPACE::error_code;

    std::chrono::system_clock::time_point now() const
    {
        return std::chrono::system_clock::time_point{std::chrono::milliseconds{123456789}};
    }

    void expires_at(std::chrono::system_clock::time_point) {}

    template <typename Rep, typename Period>
    void expires_from_now(std::chrono::duration<Rep, Period>)
    {
    }

    ErrorCode cancel()
    {
        if (mHandler)
        {
            mHandler(LINK_ASIO_NAMESPACE::error::operation_aborted);
            mHandler = nullptr;
        }
        return {};
    }

    template <typename H>
    void async_wait(H handler)
    {
        mHandler = std::move(handler);
    }

    std::function<void(const ErrorCode&)> mHandler;
};

// =============================================================================
// MockIoContext
//
// Satisfies the IoContext concept required by:
//   • InterfaceScanner / PeerGateways  (scanNetworkInterfaces, makeTimer, log)
//   • TunnelGateway                    (openUnicastSocket, openMulticastSocket)
//   • UdpTunnel                        (openBoundUdpSocket, async)
//   • Controller destructor            (async, stop)
//
// scanNetworkInterfaces() returns empty so PeerGateways never fires
// GatewayFactory — the gateway layer stays inert throughout all tests.
// async() runs its callable synchronously so UdpTunnel's listen() and
// runMaintenance() side-effects are observable immediately after construction.
// =============================================================================

struct MockIoContext
{
    template <std::size_t N>
    using Socket = TestSocket;

    using Timer = MockTimer;
    using Log = util::NullLog;

    // Controller constructs IoContext via IoContext{UdpSendExceptionHandler{this}}.
    MockIoContext() = default;
    template <typename ExceptionHandler>
    explicit MockIoContext(ExceptionHandler)
    {
    }

    // PeerGateways / TunnelGateway
    template <std::size_t N>
    TestSocket openUnicastSocket(const IpAddress&)
    {
        auto s = std::make_shared<TestSocketState>();
        s->localEndpoint = UdpEndpoint{discovery::makeAddress("127.0.0.1"), nextPort++};
        unicastSockets.push_back(s);
        return TestSocket{s};
    }

    template <std::size_t N>
    TestSocket openMulticastSocket(const IpAddress&)
    {
        auto s = std::make_shared<TestSocketState>();
        multicastSockets.push_back(s);
        return TestSocket{s};
    }

    // UdpTunnel
    template <std::size_t N>
    TestSocket openBoundUdpSocket(uint16_t)
    {
        auto s = std::make_shared<TestSocketState>();
        s->localEndpoint = UdpEndpoint{discovery::makeAddress("127.0.0.1"), nextPort++};
        boundSockets.push_back(s);
        return TestSocket{s};
    }

    std::vector<IpAddress> scanNetworkInterfaces() { return {}; }

    Timer makeTimer() { return {}; }

    Log& log() { return mLog; }

    template <typename Fn>
    void async(Fn fn)
    {
        fn();
    }

    void stop() {}

    Log mLog;
    std::vector<std::shared_ptr<TestSocketState>> unicastSockets;
    std::vector<std::shared_ptr<TestSocketState>> multicastSockets;
    std::vector<std::shared_ptr<TestSocketState>> boundSockets;
    uint16_t nextPort{20000};
};

// =============================================================================
// TestController
//
// Subclass of Controller<MockIoContext> that exposes protected members for
// white-box testing without changing any production logic.
// =============================================================================

struct TestController : Controller<MockIoContext>
{
    using Base = Controller<MockIoContext>;
    using Base::Base;

    // Expose SessionPeerCounter so PC tests can instantiate it directly.
    using SessionPeerCounter = Base::SessionPeerCounter;

    // Access the MockIoContext stored inside Injected<MockIoContext>.
    MockIoContext& io() { return *mIo; }
    const MockIoContext& io() const { return *mIo; }

    // Trigger notifyPeerCount (protected) so CB1 can observe the callback.
    void triggerNotifyPeerCount() { Base::notifyPeerCount(); }

    // Create + fire a SessionTimelineCallback. Tests CB2 / CB3.
    void triggerTimelineCallback(bool updateTempo, const link::Timeline& tl)
    {
        link::SessionId sid{};
        Base::SessionTimelineCallback cb{*this, &sid, updateTempo};
        cb(sid, tl);
    }

    // Create + fire a SessionStartStopStateCallback. Tests CB4.
    void triggerStartStopCallback(bool isPlaying)
    {
        link::StartStopState ss{};
        ss.isPlaying = isPlaying;
        Base::SessionStartStopStateCallback cb{*this};
        cb(link::NodeId{}, ss);
    }

    // Return true when mTunnel holds a UdpTunnel. Tests C2 / C3 / AP4.
    bool isUdpTunnel() const
    {
        using UdpT = UdpTunnel<IoType&, ControllerTunnelGateway>;
        return dynamic_cast<const UdpT*>(mTunnel.get()) != nullptr;
    }
};

// =============================================================================
// Fixture
//
// Owns the TestController and exposes helpers for inspecting socket state and
// injecting wire messages into the bound socket callback.
// =============================================================================

const UdpEndpoint kPeerA{discovery::makeAddress("192.168.1.10"), 20808};
const UdpEndpoint kPeerB{discovery::makeAddress("192.168.1.11"), 20808};
const UdpEndpoint kUnknownPeer{discovery::makeAddress("192.168.1.99"), 20808};

struct Fixture
{
    explicit Fixture(TestController::Config cfg = {})
        : ctrl(std::move(cfg))
    {
    }

    std::vector<TestSocketState::SentMessage>& sentMessages()
    {
        return ctrl.io().boundSockets[0]->sentMessages;
    }

    // Inject bytes as if they arrived from `from` on the bound UDP socket.
    void injectWireMessage(const UdpEndpoint& from, const std::vector<uint8_t>& data)
    {
        REQUIRE(!ctrl.io().boundSockets.empty());
        auto& cb = ctrl.io().boundSockets[0]->callback;
        REQUIRE(cb != nullptr);
        MsgArray arr{};
        const std::size_t n = std::min(data.size(), arr.size());
        std::copy(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n), arr.begin());
        cb(from, arr.cbegin(), arr.cbegin() + static_cast<std::ptrdiff_t>(n));
    }

    // Send HELLO + echo back HELLO so the peer reaches CONNECTED state.
    void connectPeer(const UdpEndpoint& ep)
    {
        ctrl.addPeer(ep);
        injectWireMessage(ep, makeWireMessage(TUNNEL_HELLO, makeNodeId()));
        sentMessages().clear();
    }

    bool sentToEndpoint(const UdpEndpoint& ep) const
    {
        const auto& msgs = ctrl.io().boundSockets[0]->sentMessages;
        for (const auto& [bytes, dest] : msgs)
            if (dest == ep)
                return true;
        return false;
    }

    bool sentMessageOfType(TunnelMessageType type) const
    {
        const auto& msgs = ctrl.io().boundSockets[0]->sentMessages;
        for (const auto& [bytes, dest] : msgs)
            if (!bytes.empty() && bytes[0] == static_cast<uint8_t>(type))
                return true;
        return false;
    }

    TestController ctrl;
};

} // anonymous namespace

// =============================================================================
// C — Construction / tunnel-type selection
// =============================================================================

TEST_CASE("Controller | C1 | Default config constructs without crash, no gateway sockets opened")
{
    Fixture f;
    // scanNetworkInterfaces() returns {} so GatewayFactory is never invoked.
    CHECK(f.ctrl.io().unicastSockets.empty());
    CHECK(f.ctrl.io().multicastSockets.empty());
}

TEST_CASE("Controller | C2 | Default config selects UdpTunnel")
{
    Fixture f;
    CHECK(f.ctrl.isUdpTunnel());
}

// C3 / AP4 / UC2 — ShmTunnel path — omitted.
// ShmTunnel::listen() tail-calls mIo->async(listen) to reschedule itself.
// With a synchronous MockIoContext::async() that invokes the callable inline,
// this creates unbounded recursion and a stack overflow. Testing the ShmTunnel
// path requires a genuinely asynchronous (thread-based) IoContext.

// =============================================================================
// CB — Callback registration and dispatch
// =============================================================================

TEST_CASE("Controller | CB1 | setNumPeersCallback fires when notifyPeerCount is triggered")
{
    Fixture f;
    std::size_t localSeen{99}, remoteSeen{99};
    f.ctrl.setNumPeersCallback(
        [&](std::size_t local, std::size_t remote)
        {
            localSeen = local;
            remoteSeen = remote;
        });

    f.ctrl.triggerNotifyPeerCount();

    // No real peers exist, so both counts should be 0.
    CHECK(localSeen == 0);
    CHECK(remoteSeen == 0);
}

TEST_CASE("Controller | CB2 | SessionTimelineCallback with updateTempo=true fires tempoCallback")
{
    Fixture f;
    std::vector<double> tempos;
    f.ctrl.setTempoCallback([&](double bpm) { tempos.push_back(bpm); });

    const link::Timeline tl{link::Tempo{140.}, link::Beats{0.}, std::chrono::microseconds{0}};
    f.ctrl.triggerTimelineCallback(/*updateTempo=*/true, tl);

    REQUIRE(tempos.size() == 1);
    CHECK(tempos[0] == Approx(140.));
}

TEST_CASE("Controller | CB3 | SessionTimelineCallback with updateTempo=false does NOT fire tempoCallback")
{
    Fixture f;
    std::vector<double> tempos;
    f.ctrl.setTempoCallback([&](double bpm) { tempos.push_back(bpm); });

    const link::Timeline tl{link::Tempo{120.}, link::Beats{0.}, std::chrono::microseconds{0}};
    f.ctrl.triggerTimelineCallback(/*updateTempo=*/false, tl);

    CHECK(tempos.empty());
}

TEST_CASE("Controller | CB4 | SessionStartStopStateCallback fires startStopCallback with correct isPlaying")
{
    Fixture f;
    std::vector<bool> states;
    f.ctrl.setStartStopCallback([&](bool playing) { states.push_back(playing); });

    f.ctrl.triggerStartStopCallback(true);
    f.ctrl.triggerStartStopCallback(false);

    REQUIRE(states.size() == 2);
    CHECK(states[0] == true);
    CHECK(states[1] == false);
}

// =============================================================================
// PC — SessionPeerCounter standalone behaviour
// =============================================================================

TEST_CASE("Controller | PC1 | SessionPeerCounter fires notify on 0->1 count change")
{
    std::size_t currentCount = 0;
    std::vector<std::size_t> notifications;

    TestController::SessionPeerCounter counter{[&] { return currentCount; },
                                               [&](std::size_t n) { notifications.push_back(n); }};

    currentCount = 1;
    counter();

    REQUIRE(notifications.size() == 1);
    CHECK(notifications[0] == 1);
}

TEST_CASE("Controller | PC2 | SessionPeerCounter does NOT fire notify when count is unchanged")
{
    std::size_t currentCount = 1;
    std::vector<std::size_t> notifications;

    TestController::SessionPeerCounter counter{[&] { return currentCount; },
                                               [&](std::size_t n) { notifications.push_back(n); }};

    counter(); // fires: 0 -> 1
    notifications.clear();

    counter(); // no change: still 1
    CHECK(notifications.empty());
}

TEST_CASE("Controller | PC3 | SessionPeerCounter fires notify on 1->2 and 2->1 transitions")
{
    std::size_t currentCount = 0;
    std::vector<std::size_t> notifications;

    TestController::SessionPeerCounter counter{[&] { return currentCount; },
                                               [&](std::size_t n) { notifications.push_back(n); }};

    currentCount = 1;
    counter(); // 0 -> 1
    currentCount = 2;
    counter(); // 1 -> 2
    currentCount = 1;
    counter(); // 2 -> 1

    REQUIRE(notifications.size() == 3);
    CHECK(notifications[0] == 1);
    CHECK(notifications[1] == 2);
    CHECK(notifications[2] == 1);
}

// =============================================================================
// AP — addPeer / removePeer delegation to UdpTunnel
// =============================================================================

TEST_CASE("Controller | AP1 | addPeer sends TUNNEL_HELLO to peer endpoint")
{
    Fixture f;
    f.sentMessages().clear();

    f.ctrl.addPeer(kPeerA);

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(f.sentMessageOfType(TUNNEL_HELLO));
}

TEST_CASE("Controller | AP2 | addPeer duplicate endpoint sends only one HELLO")
{
    Fixture f;
    f.ctrl.addPeer(kPeerA);
    const std::size_t countAfterFirst = f.sentMessages().size();

    f.ctrl.addPeer(kPeerA); // de-dup guard in UdpTunnel

    CHECK(f.sentMessages().size() == countAfterFirst);
}

TEST_CASE("Controller | AP3 | removePeer after connect sends TUNNEL_BYE")
{
    Fixture f;
    f.connectPeer(kPeerA);

    f.ctrl.removePeer(kPeerA);

    CHECK(f.sentToEndpoint(kPeerA));
    CHECK(f.sentMessageOfType(TUNNEL_BYE));
}


// =============================================================================
// UC — setUnknownPeerCallback
// =============================================================================

TEST_CASE("Controller | UC1 | HELLO from unknown peer fires the unknown-peer callback once")
{
    Fixture f;
    int callbackCount = 0;
    std::function<void()> storedReject;

    f.ctrl.setUnknownPeerCallback(
        [&](UdpEndpoint, std::function<void()> /*accept*/, std::function<void()> reject)
        {
            ++callbackCount;
            storedReject = std::move(reject);
        });

    const auto msg = makeWireMessage(TUNNEL_HELLO, makeNodeId());
    f.injectWireMessage(kUnknownPeer, msg); // fires callback; peer enters mPendingPeers
    f.injectWireMessage(kUnknownPeer, msg); // still pending — de-dup guard, no second fire

    CHECK(callbackCount == 1);

    if (storedReject)
        storedReject();
}


// =============================================================================
// D — Destructor
// =============================================================================

TEST_CASE("Controller | D1 | Destroying controller with connected peer sends TUNNEL_BYE")
{
    // Hold shared_ptr to socket state so we can inspect it after ctrl is gone.
    std::shared_ptr<TestSocketState> socketState;
    {
        Fixture f;
        socketState = f.ctrl.io().boundSockets[0];
        f.connectPeer(kPeerA);
        socketState->sentMessages.clear();
        // Fixture destructor destroys ctrl here.
    }

    bool foundBye = false;
    for (const auto& [bytes, dest] : socketState->sentMessages)
        if (!bytes.empty() && bytes[0] == static_cast<uint8_t>(TUNNEL_BYE))
            foundBye = true;
    CHECK(foundBye);
}

TEST_CASE("Controller | D2 | Destroying controller with gatewayDiscovery enabled does not crash")
{
    // GatewayDiscovery is always enabled after construction.
    // This test verifies that the destructor's async + condition_variable
    // handshake completes cleanly with a synchronous MockIoContext::async().
    { Fixture f; }
    CHECK(true);
}

} // namespace extender
} // namespace ableton
