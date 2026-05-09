#pragma once

#include <ableton/discovery/v1/Messages.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/util/Injected.hpp>

#include "Tunnel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ableton
{
namespace extender
{

template <typename IoContext, typename Gateway>
class UdpTunnel : public Tunnel<IoContext, Gateway>
{
  public:
    using MessageBuffer = ableton::discovery::v1::MessageBuffer;
    using NodeId = ableton::link::NodeId;
    using UdpEndpoint = LINK_ASIO_NAMESPACE::ip::udp::endpoint;
    using Socket = typename util::Injected<IoContext>::type::template Socket<
        ableton::discovery::v1::kMaxMessageSize>;
    using Timer = typename util::Injected<IoContext>::type::Timer;
    // Iterator type produced by the socket's receive buffer (std::array<uint8_t, N>)
    using SocketBufferIt =
        typename std::array<uint8_t,
                            ableton::discovery::v1::kMaxMessageSize>::const_iterator;

    static constexpr auto kMaintenanceInterval = std::chrono::seconds(5);
    static constexpr auto kKeepaliveTimeout = std::chrono::seconds(15);

    enum class PeerConnectionState
    {
        DISCONNECTED,
        CONNECTING,
        CONNECTED
    };

    struct PeerEntry
    {
        UdpEndpoint endpoint;
        PeerConnectionState state;
        std::chrono::system_clock::time_point last_seen;
    };

    UdpTunnel(ableton::util::Injected<IoContext> io,
              uint16_t local_port,
              std::vector<UdpEndpoint> initial_peers)
        : Tunnel<IoContext, Gateway>(io)
        , mSocket(
              this->mIo
                  ->template openBoundUdpSocket<ableton::discovery::v1::kMaxMessageSize>(
                      local_port))
        , mMaintenanceTimer(this->mIo->makeTimer())
        , is_running(true)
    {
        for (auto& ep : initial_peers)
        {
            mPeers.push_back({ep, PeerConnectionState::DISCONNECTED, {}});
            mEndpointToPeerIdx[ep] = mPeers.size() - 1;
        }

        debug(this->mIo->log()) << "UdpTunnel: listening on port " << local_port
                                << " with " << mPeers.size() << " configured peer(s)";

        // Start receive loop and first maintenance pass immediately
        this->mIo->async([this] { listen(); });
        this->mIo->async([this] { runMaintenance(); });
    }

    ~UdpTunnel() { debug(this->mIo->log()) << "UdpTunnel destructor"; }

    // Thread-safe: may be called from any thread (posts to ASIO thread).
    void addPeer(UdpEndpoint endpoint)
    {
        this->mIo->async(
            [this, endpoint]
            {
                if (mEndpointToPeerIdx.find(endpoint) != mEndpointToPeerIdx.end())
                    return;
                mPeers.push_back({endpoint, PeerConnectionState::DISCONNECTED, {}});
                mEndpointToPeerIdx[endpoint] = mPeers.size() - 1;
                debug(this->mIo->log()) << "UdpTunnel: added peer " << endpoint;
                sendHello(endpoint);
            });
    }

    // Thread-safe: may be called from any thread (posts to ASIO thread).
    void removePeer(UdpEndpoint endpoint)
    {
        this->mIo->async(
            [this, endpoint]
            {
                auto idxIt = mEndpointToPeerIdx.find(endpoint);
                if (idxIt == mEndpointToPeerIdx.end())
                    return;

                size_t idx = idxIt->second;
                sendBye(endpoint);

                // Erase NodeId mappings that belong to this peer
                for (auto it = remoteNodeIdToPeerIdx.begin();
                     it != remoteNodeIdToPeerIdx.end();)
                {
                    it = (it->second == idx) ? remoteNodeIdToPeerIdx.erase(it) : ++it;
                }

                mEndpointToPeerIdx.erase(idxIt);

                // Rebuild endpoint→index map to close the gap left by erasure
                mPeers.erase(mPeers.begin() + static_cast<ptrdiff_t>(idx));
                rebuildEndpointIndex();
            });
    }

    virtual void forward(TunnelMessageType messageType,
                         std::shared_ptr<Gateway> /*from_gateway*/,
                         const NodeId& from_node_id,
                         const unsigned char* messageBegin,
                         const unsigned char* messageEnd,
                         const std::optional<NodeId>& to_node_id) override
    {
        auto optional_msg = this->encapsulate(
            {from_node_id, messageType, to_node_id, 0}, messageBegin, messageEnd);
        if (!optional_msg)
            return;

        const size_t payloadSize =
            static_cast<size_t>(std::distance(messageBegin, messageEnd));
        const size_t sendSize = encapsulatedSize(messageType, payloadSize);

        if (messageType == UNICAST || messageType == MEASUREMENT_PING)
        {
            if (!to_node_id)
                return;
            auto it = remoteNodeIdToPeerIdx.find(*to_node_id);
            if (it != remoteNodeIdToPeerIdx.end() && it->second < mPeers.size()
                && mPeers[it->second].state == PeerConnectionState::CONNECTED)
            {
                sendRaw(*optional_msg, sendSize, mPeers[it->second].endpoint);
            }
            else
            {
                debug(this->mIo->log())
                    << "UdpTunnel: no connected peer found for node " << *to_node_id;
            }
        }
        else // BROADCAST, BYEBYE, MEASUREMENT_PONG – sent to all connected peers
        {
            for (auto& peer : mPeers)
            {
                if (peer.state == PeerConnectionState::CONNECTED)
                    sendRaw(*optional_msg, sendSize, peer.endpoint);
            }
        }
    }

    virtual void listen() override
    {
        mSocket.receive(
            [this](const UdpEndpoint& from, SocketBufferIt begin, SocketBufferIt end)
            {
                MessageBuffer buf{};
                const auto n = static_cast<size_t>(std::distance(begin, end));
                std::copy(begin, begin + static_cast<ptrdiff_t>(n), buf.begin());
                handleReceive(from, buf);
                if (is_running)
                    listen();
            });
    }

    virtual void stop_listening() override
    {
        is_running = false;
        mMaintenanceTimer.cancel();

        for (auto& peer : mPeers)
        {
            if (peer.state == PeerConnectionState::CONNECTED)
                sendBye(peer.endpoint);
        }

        for (auto& gateway : this->gateways)
            gateway->stop_listening();
        this->gateways.clear();

        remoteNodeIdToPeerIdx.clear();
        mEndpointToPeerIdx.clear();
    }

  private:
    // -----------------------------------------------------------------------
    // Send helpers
    // -----------------------------------------------------------------------

    // Returns the number of bytes that encapsulate() writes for the given type and
    // payload size.  Used to avoid sending the full 512-byte MessageBuffer over UDP.
    static size_t encapsulatedSize(TunnelMessageType type, size_t payloadSize)
    {
        using ableton::discovery::sizeInByteStream;
        const bool hasToNode =
            (type == UNICAST || type == MEASUREMENT_PING || type == MEASUREMENT_PONG);
        return sizeInByteStream(uint8_t{})  // TunnelMessageType (serialised as uint8)
               + sizeInByteStream(NodeId{}) // from_node
               + (hasToNode ? sizeInByteStream(NodeId{}) : 0) // to_node
               + sizeInByteStream(uint16_t{})                 // message_size field
               + payloadSize;
    }

    void sendRaw(const MessageBuffer& msg, size_t bytes, const UdpEndpoint& to)
    {
        try
        {
            mSocket.send(msg.data(), bytes, to);
        }
        catch (const std::exception& e)
        {
            debug(this->mIo->log())
                << "UdpTunnel send error to " << to << ": " << e.what();
        }
    }

    void sendControlMessage(TunnelMessageType type, const UdpEndpoint& to)
    {
        NodeId id{};
        // Zero-length payload: pass the same iterator for begin and end
        const std::array<unsigned char, 0> empty{};
        auto msg = this->encapsulate({id, type, {}, 0}, empty.begin(), empty.end());
        if (msg)
            sendRaw(*msg, encapsulatedSize(type, 0), to);
    }

    void sendHello(const UdpEndpoint& to) { sendControlMessage(TUNNEL_HELLO, to); }
    void sendKeepalive(const UdpEndpoint& to)
    {
        sendControlMessage(TUNNEL_KEEPALIVE, to);
    }
    void sendBye(const UdpEndpoint& to) { sendControlMessage(TUNNEL_BYE, to); }

    // -----------------------------------------------------------------------
    // Receive dispatch
    // -----------------------------------------------------------------------

    void handleReceive(const UdpEndpoint& from, MessageBuffer& buf)
    {
        auto result = this->decapsulate(buf);
        if (!result)
            return;

        auto& [context, payload_buf] = *result;
        auto messageBegin = payload_buf.begin();
        auto messageEnd = messageBegin + static_cast<ptrdiff_t>(context.size);

        auto idxIt = mEndpointToPeerIdx.find(from);

        switch (context.type)
        {
        case TUNNEL_HELLO:
        {
            if (idxIt == mEndpointToPeerIdx.end())
            {
                debug(this->mIo->log()) << "UdpTunnel: HELLO from unconfigured endpoint "
                                        << from << " – ignoring";
                return;
            }
            auto& peer = mPeers[idxIt->second];
            const bool wasConnected = (peer.state == PeerConnectionState::CONNECTED);
            peer.state = PeerConnectionState::CONNECTED;
            peer.last_seen = std::chrono::system_clock::now();
            if (!wasConnected)
            {
                std::cout << "UdpTunnel: peer connected: " << from << std::endl;
                sendHello(from); // complete the handshake
            }
            break;
        }

        case TUNNEL_KEEPALIVE:
            if (idxIt != mEndpointToPeerIdx.end())
            {
                auto& peer = mPeers[idxIt->second];
                if (peer.state == PeerConnectionState::CONNECTED)
                    peer.last_seen = std::chrono::system_clock::now();
            }
            break;

        case TUNNEL_BYE:
            if (idxIt != mEndpointToPeerIdx.end())
            {
                auto& peer = mPeers[idxIt->second];
                std::cout << "UdpTunnel: peer disconnected: " << from << std::endl;
                peer.state = PeerConnectionState::DISCONNECTED;
                erasePeerNodeIds(idxIt->second);
            }
            break;

        case BYEBYE:
        {
            if (idxIt == mEndpointToPeerIdx.end()
                || mPeers[idxIt->second].state != PeerConnectionState::CONNECTED)
                return;
            remoteNodeIdToPeerIdx.erase(context.from_node);
            this->forward_locally(context.type,
                                  context.from_node,
                                  messageBegin,
                                  messageEnd,
                                  context.to_node);
            break;
        }

        case BROADCAST:
        case UNICAST:
        case MEASUREMENT_PING:
        case MEASUREMENT_PONG:
        default:
        {
            if (idxIt == mEndpointToPeerIdx.end()
                || mPeers[idxIt->second].state != PeerConnectionState::CONNECTED)
                return;
            // Record which peer this node lives behind (keep first mapping)
            remoteNodeIdToPeerIdx.emplace(context.from_node, idxIt->second);
            this->forward_locally(context.type,
                                  context.from_node,
                                  messageBegin,
                                  messageEnd,
                                  context.to_node);
            break;
        }
        }
    }

    // -----------------------------------------------------------------------
    // Maintenance: keepalive and reconnect
    // -----------------------------------------------------------------------

    void runMaintenance()
    {
        if (!is_running)
            return;

        auto now = std::chrono::system_clock::now();

        for (auto& peer : mPeers)
        {
            switch (peer.state)
            {
            case PeerConnectionState::CONNECTED:
                if (now - peer.last_seen > kKeepaliveTimeout)
                {
                    std::cout << "UdpTunnel: peer timed out: " << peer.endpoint
                              << std::endl;
                    peer.state = PeerConnectionState::DISCONNECTED;
                    auto idx = static_cast<size_t>(&peer - mPeers.data());
                    erasePeerNodeIds(idx);
                }
                else
                {
                    sendKeepalive(peer.endpoint);
                }
                break;

            case PeerConnectionState::DISCONNECTED:
            case PeerConnectionState::CONNECTING:
                std::cout << "UdpTunnel: reconnecting to " << peer.endpoint << std::endl;
                peer.state = PeerConnectionState::CONNECTING;
                sendHello(peer.endpoint);
                break;
            }
        }

        mMaintenanceTimer.expires_from_now(kMaintenanceInterval);
        mMaintenanceTimer.async_wait(
            [this](const LINK_ASIO_NAMESPACE::error_code& ec)
            {
                if (!ec && is_running)
                    runMaintenance();
            });
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    void erasePeerNodeIds(size_t peerIdx)
    {
        for (auto it = remoteNodeIdToPeerIdx.begin(); it != remoteNodeIdToPeerIdx.end();)
            it = (it->second == peerIdx) ? remoteNodeIdToPeerIdx.erase(it) : ++it;
    }

    void rebuildEndpointIndex()
    {
        mEndpointToPeerIdx.clear();
        for (size_t i = 0; i < mPeers.size(); ++i)
            mEndpointToPeerIdx[mPeers[i].endpoint] = i;
    }

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    Socket mSocket;
    Timer mMaintenanceTimer;
    std::atomic<bool> is_running;

    std::vector<PeerEntry> mPeers;
    std::map<NodeId, size_t> remoteNodeIdToPeerIdx;
    std::map<UdpEndpoint, size_t> mEndpointToPeerIdx;
};

template <typename IoContext, typename Gateway>
TunnelPtr<IoContext, Gateway> makeUdpTunnel(
    ableton::util::Injected<IoContext> io,
    uint16_t local_port,
    std::vector<LINK_ASIO_NAMESPACE::ip::udp::endpoint> peers)
{
    return std::make_shared<UdpTunnel<IoContext, Gateway>>(
        injectRef(*io), local_port, std::move(peers));
}

} // namespace extender
} // namespace ableton
