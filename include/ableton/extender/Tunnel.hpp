#pragma once

#include <ableton/discovery/NetworkByteStreamSerializable.hpp>
#include <ableton/discovery/v1/Messages.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/link/PeerState.hpp>
#include <ableton/util/Injected.hpp>

#include <map>
#include <optional>
#include <vector>

#include "ableton/extender/DedupCache.hpp"

namespace ableton
{
namespace extender
{

/**
* The tunnel can receive the same broadcast on many interfaces from the same NodeID.
*   We need to cache the TunnelGateway associated with the peer's NodeID
*   The TunnelGateway needs to cache the IP/port associated with the NodeID
*   We need to cache the last broadcast of a Node ID to avoid sending the same broadcast
multiple times.

* When the unique broadcast of a NodeID is received in the other side of the tunnel,
*   it is forwarded to all TunnelGateways.
* Each TunnelGateway will create/update an Endpoint
*   for the NodeID the broadcast came from.
* The broadcast will then be sent on all Endpoints.
*
* When a peer responds to the broadcast, it'll send an unicast to the specific Endpoint.
*   We need to cache the TunnelGateway associated with the peer's NodeID
*   The TunnelGateway needs to cache the IP/port associated with the NodeID
* The unicast will be received and sent to the other side of the tunnel,
*   where an Endpoint is created/updated for the Node ID of the peer on each interface.
* The unicast will then be forwarded to the known Peer interface/IP/port.
*/

enum TunnelMessageType : uint8_t
{
    TUNNEL_HELLO = 'i',     // in tunnel only: connection handshake
    TUNNEL_BYE = 'y',       // in tunnel only: graceful disconnect
    TUNNEL_KEEPALIVE = 'k', // in tunnel only: keepalive/heartbeat
    BROADCAST = 'b',        // ableton discovery
    UNICAST = 'u',          // ableton discovery
    BYEBYE = 'z',           // ableton discovery
    MEASUREMENT_PING = 'm', // ableton link
    MEASUREMENT_PONG = 'r', // ableton link
};

template <typename IoContext, typename Gateway>
class Tunnel
{
  public:
    using NodeId = ableton::link::NodeId;
    using MessageBuffer = ableton::discovery::v1::MessageBuffer;
    using GatewayPtr = std::shared_ptr<Gateway>;


    Tunnel(ableton::util::Injected<IoContext> io)
        : mIo(std::move(io))
    {
    }
    virtual ~Tunnel() {}

    virtual void forward(TunnelMessageType messageType,
                         std::shared_ptr<Gateway> from_gateway,
                         const NodeId& from_node_id,
                         const unsigned char* messageBegin,
                         const unsigned char* messageEnd,
                         const std::optional<NodeId>& to_node_id) = 0;
    virtual void listen() = 0;
    virtual void stop_listening() = 0;

    void addGateway(GatewayPtr g)
    {
        if (std::find(gateways.begin(), gateways.end(), g) == gateways.end())
        {
            gateways.push_back(g);
        }
    }

    void removeGateway(GatewayPtr g)
    {
        auto it = std::find(gateways.begin(), gateways.end(), g);
        if (it != gateways.end())
        {
            gateways.erase(it);
        }
    }

  protected:
    // Returns true if the caller should forward this message; false if the
    // message is a duplicate (already forwarded within kDedupWindow) and
    // should be dropped. Only BROADCAST / UNICAST / BYEBYE are deduplicated:
    // MEASUREMENT_PING/PONG ride per-interface surrogate sockets so cannot
    // duplicate, and TUNNEL_* control messages originate inside the tunnel
    // itself rather than from gateways.
    bool shouldForward(TunnelMessageType messageType,
                       const NodeId& from_node_id,
                       const unsigned char* messageBegin,
                       const unsigned char* messageEnd)
    {
        if (messageType != BROADCAST && messageType != UNICAST
            && messageType != BYEBYE)
            return true;
        return !mDedupCache.isDuplicate(static_cast<uint8_t>(messageType),
                                        from_node_id,
                                        messageBegin,
                                        messageEnd);
    }

    std::vector<GatewayPtr> gateways;
    ableton::util::Injected<IoContext> mIo;
    DedupCache mDedupCache;

    struct MessageContext
    {
        using NodeId = ableton::link::NodeId;
        NodeId from_node;
        TunnelMessageType type;
        std::optional<NodeId> to_node;
        size_t size;
    };
    void forward_locally(TunnelMessageType messageType,
                         const NodeId& from_node_id,
                         unsigned char* messageBegin,
                         unsigned char* messageEnd,
                         const std::optional<NodeId>& to_node_id)
    {
        // all gateways that know 'to_node_id' will forward the message
        for (auto& gateway : gateways)
        {
            gateway->forwardMessageLocally(
                messageType, from_node_id, messageBegin, messageEnd, to_node_id);
        }
    }

    template <typename It>
    std::optional<MessageBuffer> encapsulate(const MessageContext context,
                                             const It messageBegin,
                                             const It messageEnd)
    {
        MessageBuffer msg{};
        auto it = msg.begin();

        it = ableton::discovery::toNetworkByteStream(context.type, it);
        it = ableton::discovery::toNetworkByteStream(context.from_node, it);

        if (context.type == UNICAST || context.type == MEASUREMENT_PING
            || context.type == MEASUREMENT_PONG)
        {
            if (context.to_node.has_value())
            {
                it = ableton::discovery::toNetworkByteStream(context.to_node.value(), it);
            }
            else
            {
                debug(mIo->log()) << "Error: UNICAST message must have a to_node_id.";
                return {};
            }
        } // broadcast doesn't need a target node ID
        uint16_t message_size = std::distance(messageBegin, messageEnd);
        it = ableton::discovery::toNetworkByteStream(message_size, it);

        std::copy(messageBegin, messageEnd, it);
        return msg;
    }

    std::optional<std::pair<MessageContext, MessageBuffer>> decapsulate(
        MessageBuffer& msg)
    {
        MessageBuffer original_link_msg{};
        MessageContext context{};
        auto begin = msg.begin();
        auto end = msg.end();


        uint8_t type{};
        uint16_t message_size{};

        std::tie(type, begin) =
            ableton::discovery::Deserialize<uint8_t>::fromNetworkByteStream(begin, end);
        context.type = static_cast<TunnelMessageType>(type);
        std::tie(context.from_node, begin) =
            ableton::discovery::Deserialize<NodeId>::fromNetworkByteStream(begin, end);

        switch (context.type)
        {
        case TUNNEL_HELLO:
        case TUNNEL_BYE:
        case TUNNEL_KEEPALIVE:
        case BROADCAST:
        case BYEBYE:
            break;
        case MEASUREMENT_PING:
        case MEASUREMENT_PONG:
        case UNICAST:
            std::tie(context.to_node, begin) =
                ableton::discovery::Deserialize<NodeId>::fromNetworkByteStream(
                    begin, end);
            break;
        default:
            break;
        }

        std::tie(message_size, begin) =
            ableton::discovery::Deserialize<uint16_t>::fromNetworkByteStream(begin, end);

        context.size = message_size;

        debug(mIo->log()) << "<- Tunnel: message from " << context.from_node
                          << " of type: " << context.type << ", size: " << context.size;

        // now <begin, end> holds the ableton link packet and we know what to do
        if (begin != end)
        {
            std::copy(begin, end, original_link_msg.begin());
            return std::make_pair(context, original_link_msg);
        }
        return std::nullopt;
    }
};

template <typename IoContext, typename Gateway>
using TunnelPtr = std::shared_ptr<Tunnel<IoContext, Gateway>>;

} // namespace extender
} // namespace ableton
