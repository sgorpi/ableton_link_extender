#pragma once

#include <ableton/platforms/Config.hpp>
#include <ableton/discovery/AsioTypes.hpp>
#include <ableton/discovery/NetworkByteStreamSerializable.hpp>
#include <ableton/discovery/v1/Messages.hpp>
#include <ableton/extender/Tunnel.hpp>
#include <ableton/link/NodeId.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ableton
{
namespace extender
{
namespace helpers
{

// =============================================================================
// TestSocketState / TestSocket
//
// MsgArray-iterator callback variant — used by tst_Controller.cpp and
// tst_UdpTunnel.cpp. (tst_TunnelGateway.cpp uses a vector-callback shim
// instead and keeps its own local definitions.)
// =============================================================================

using UdpEndpoint = discovery::UdpEndpoint;
using IpAddress = discovery::IpAddress;
using MsgArray = std::array<uint8_t, discovery::v1::kMaxMessageSize>;

struct TestSocketState
{
    using SentMessage = std::pair<std::vector<uint8_t>, UdpEndpoint>;
    std::vector<SentMessage> sentMessages;

    std::function<void(const UdpEndpoint&,
                       MsgArray::const_iterator,
                       MsgArray::const_iterator)>
        callback;

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
        mState->callback = std::move(handler);
    }

    UdpEndpoint endpoint() const { return mState->localEndpoint; }

    std::shared_ptr<TestSocketState> mState;
};

// =============================================================================
// makeNodeId
// =============================================================================

inline link::NodeId makeNodeId()
{
    return link::NodeId::random<link::platform::Random>();
}

// =============================================================================
// makeWireMessage
//
// Builds the exact byte sequence Tunnel::encapsulate() would produce, so tests
// can inject realistic packets into the bound socket callback.
// =============================================================================

inline std::vector<uint8_t> makeWireMessage(
    TunnelMessageType type,
    const link::NodeId& fromNode,
    const std::vector<uint8_t>& payload = {},
    const std::optional<link::NodeId>& toNode = {})
{
    using namespace ableton::discovery;
    v1::MessageBuffer buf{};
    auto it = buf.begin();
    it = toNetworkByteStream(static_cast<uint8_t>(type), it);
    it = toNetworkByteStream(fromNode, it);
    if (type == UNICAST || type == MEASUREMENT_PING || type == MEASUREMENT_PONG)
        it = toNetworkByteStream(toNode.value(), it);
    const uint16_t sz = static_cast<uint16_t>(payload.size());
    it = toNetworkByteStream(sz, it);
    it = std::copy(payload.begin(), payload.end(), it);
    const std::size_t total =
        static_cast<std::size_t>(std::distance(buf.begin(), it));
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(total)};
}

} // namespace helpers
} // namespace extender
} // namespace ableton

