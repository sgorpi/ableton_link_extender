#pragma once

#include <chrono>
#include <map>
#include <memory>

#include <ableton/discovery/NetworkByteStreamSerializable.hpp>
#include <ableton/discovery/PeerGateway.hpp>
#include <ableton/link/MeasurementEndpointV4.hpp>
#include <ableton/link/MeasurementEndpointV6.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/link/PeerState.hpp>
#include <ableton/link/v1/Messages.hpp>

#include "Tunnel.hpp"

namespace ableton
{
namespace extender
{

using UdpEndpoint = LINK_ASIO_NAMESPACE::ip::udp::endpoint;


template <typename LocalPeerObserver, typename RemotePeerObserver, typename IoContext>
class TunnelGateway
    : public std::enable_shared_from_this<
          TunnelGateway<LocalPeerObserver, RemotePeerObserver, IoContext>>
{
  public:
    using NodeState = ableton::link::NodeState;
    using PeerState = ableton::link::PeerState;
    using NodeId = ableton::link::NodeId;

    using Socket = typename util::Injected<IoContext>::type::template Socket<
        ableton::discovery::v1::kMaxMessageSize>;
    using Timer = typename util::Injected<IoContext>::type::Timer;
    using TimerError = typename Timer::ErrorCode;
    using PeerTimeout = std::pair<std::chrono::system_clock::time_point, NodeId>;
    using PeerTimeouts = std::vector<PeerTimeout>;

    TunnelGateway(ableton::util::Injected<IoContext> io,
                  ableton::discovery::IpAddress interface_address,
                  util::Injected<LocalPeerObserver> local_observer,
                  util::Injected<RemotePeerObserver> remote_observer,
                  TunnelPtr<IoContext, TunnelGateway> tunnel)
        : mIo(std::move(io))
        , local_interface_address(std::move(interface_address))
        , mLocalObserver(std::move(local_observer))
        , mRemoteObserver(std::move(remote_observer))
        , mTunnel(std::move(tunnel))
        , mMulticastReceiveSocket(
              mIo->template openMulticastSocket<ableton::discovery::v1::kMaxMessageSize>(
                  interface_address))
        , mLocalPruneTimer(mIo->makeTimer())
        , mRemotePruneTimer(mIo->makeTimer())
    {
        debug(mIo->log()) << "TunnelGateway constructor";
    }

    ~TunnelGateway()
    {
        debug(mIo->log()) << "TunnelGateway destructor";
        stop_listening();
        // TODO: inform tunnel that this gateway doesn't exist anymore
        mTunnel.reset();
    }

    void listen()
    {
        is_running = true;
        mTunnel->addGateway(this->shared_from_this());
        receive(util::makeAsyncSafe(this->shared_from_this()));
    }

    void stop_listening()
    {
        is_running = false;
        for (auto& surrogate : mRemoteNodeIdToRemoteNodeSurrogate)
        {
            surrogate.second->stop_listening();
        }
        for (auto& surrogate : mRemoteNodeIdToRemoteMeasurementSurrogate)
        {
            surrogate.second->stop_listening();
        }
        mRemoteNodeIdToRemoteNodeSurrogate.clear();
        mRemoteNodeIdToRemoteMeasurementSurrogate.clear();
        localNodeIdToEndpointMap.clear();
        localMeasurementEndpointToNodeId.clear();
        mLocalPeerTimeouts.clear();
        mRemotePeerTimeouts.clear();
    }

    template <typename Handler>
    void receive(Handler handler)
    {
        mMulticastReceiveSocket.receive(SocketReceiver<Handler>(handler));
    }

    bool hasLocalNode(const NodeId& id) const
    {
        return localNodeIdToEndpointMap.find(id) != localNodeIdToEndpointMap.end();
    }

    template <typename It>
    void receiveByeBye(const NodeId& from_node_id,
                       const It messageBegin,
                       const It messageEnd,
                       std::optional<NodeId> to_node_id = std::nullopt)
    {
        forwardMessage(BYEBYE, from_node_id, messageBegin, messageEnd, to_node_id);
        peerLeft(*mLocalObserver, from_node_id);
        mLocalPeerTimeouts.erase(
            std::remove_if(mLocalPeerTimeouts.begin(), mLocalPeerTimeouts.end(),
                [&from_node_id](const PeerTimeout& pt) { return pt.second == from_node_id; }),
            mLocalPeerTimeouts.end());
        // and clean up local administration
        localMeasurementEndpointToNodeId.erase(createKeyFromEndpoint(
            localNodeIdToEndpointMap[from_node_id].measurementEndpoint));
        localNodeIdToEndpointMap.erase(from_node_id);
    }

    template <typename It>
    void receivePeerState(const ableton::discovery::UdpEndpoint& fromEndpoint,
                          ableton::discovery::v1::MessageHeader<NodeId> header,
                          It wholeMessageBegin,
                          It payloadBegin,
                          It wholeMessageEnd,
                          std::optional<NodeId> to_node_id = std::nullopt)
    {
        try
        {

            auto state = PeerState::fromPayload(std::move(header.ident),
                                                std::move(payloadBegin),
                                                std::move(wholeMessageEnd));

            localNodeIdToEndpointMap[header.ident] = {fromEndpoint, state.endpoint};
            auto key = createKeyFromEndpoint(state.endpoint);
            debug(mIo->log()) << "Received peer state with endpoint: " << key;
            localMeasurementEndpointToNodeId[key] = header.ident;
            sawPeer(*mLocalObserver, state);
            updateLocalPeerTimeout(state.ident(), header.ttl);

            // we don't really care about the content, but the measurement endpoint at
            // the end shall be stripped out, so that on the other end of the tunnel
            // we can put in a replacement measurement endpoint.
            auto measEndpointEntrySize =
                sizeInByteStream(ableton::discovery::PayloadEntryHeader{})
                + sizeInByteStream(ableton::link::MeasurementEndpointV4{state.endpoint})
                + sizeInByteStream(ableton::link::MeasurementEndpointV6{state.endpoint});

            // now forward the nodestate (without the endpoint)
            forwardMessage(
                (to_node_id ? UNICAST : BROADCAST),
                header.ident,
                wholeMessageBegin,
                wholeMessageEnd
                    - measEndpointEntrySize, /* remove the endpoint from the message */
                to_node_id);
            //   // Handlers must only be called once
            //   auto handler = std::move(mPeerStateHandler);
            //   mPeerStateHandler = [](PeerState<NodeState>) {};
            //   handler(PeerState<NodeState>{std::move(state), header.ttl});
        }
        catch (const std::runtime_error& err)
        {
            info(mIo->log()) << "Ignoring peer state message: " << err.what();
        }
    }

    template <typename It>
    void handle_state_message(const ableton::discovery::UdpEndpoint& fromEndpoint,
                              ableton::discovery::v1::MessageHeader<NodeId> header,
                              It wholeMessageBegin,
                              It payloadBegin,
                              It wholeMessageEnd,
                              std::optional<NodeId> to_node_id = std::nullopt)
    {
        if (hasRemoteNodeSurrogate(header.ident))
            return; // ignore (broadcast) messages from our own RemoteNodeSurrogates

        debug(mIo->log()) << "Received message from: " << fromEndpoint.address()
                          << " from peer " << header.ident
                          << " type: " << static_cast<int>(header.messageType);

        /**
         * Todo:
         * - check if this is the main gateway for peer.ident
         *   - if so, update the peer state in the observer
         *   - and forward the message to the tunnel
         */

        switch (header.messageType)
        {
        case ableton::discovery::v1::kAlive:
        case ableton::discovery::v1::kResponse:
            receivePeerState(fromEndpoint,
                             header,
                             wholeMessageBegin,
                             payloadBegin,
                             wholeMessageEnd,
                             to_node_id);
            break;
        case ableton::discovery::v1::kByeBye:
            receiveByeBye(header.ident, wholeMessageBegin, wholeMessageEnd, to_node_id);
            break;
        default:
            info(mIo->log()) << "Unknown message received of type: "
                             << header.messageType;
        }
    }

    template <typename It>
    void handle_measurement_message(const ableton::discovery::UdpEndpoint& fromEndpoint,
                                    ableton::link::v1::MessageHeader header,
                                    It wholeMessageBegin,
                                    It payloadBegin,
                                    It wholeMessageEnd,
                                    NodeId to_node_id)
    {
        auto key = createKeyFromEndpoint(fromEndpoint);
        // measurements are sent from a different socket than the node's reported
        // 'measurement endpoint'.
        // thus we need to store the 'fromEndpoint', and use that somehow as the 'from'
        // id.
        debug(mIo->log()) << "Received measurement message "
                          << (header.messageType == ableton::link::v1::kPing ? "ping"
                                                                             : "pong")
                          << " from: " << key;
        if (header.messageType == ableton::link::v1::kPing)
        {
            // we know the 'fromEndpoint' isn't a measurement endpoint, but 'to_node_id'
            // should be correct. Thus, make a fake 'from' node ID.
            // note: measurements happen roughly every 30 seconds. Measurement sockets are
            // not re-used.
            if (localMeasurementEndpointToNodeId.find(key)
                == localMeasurementEndpointToNodeId.end())
            {
                auto nodeId = NodeId::random<ableton::link::platform::Random>();
                // abusing these a bit:
                localMeasurementEndpointToNodeId[key] = nodeId;
                localNodeIdToEndpointMap[nodeId] = {{}, fromEndpoint};
                debug(mIo->log())
                    << "New measurement endpoint: " << key << " -> " << nodeId;
            }
            else
            {
                debug(mIo->log())
                    << "Measurement endpoint known for: " << key
                    << ", nodeId: " << localMeasurementEndpointToNodeId[key];
            }
            forwardMessage(MEASUREMENT_PING,
                           localMeasurementEndpointToNodeId[key],
                           wholeMessageBegin,
                           wholeMessageEnd,
                           to_node_id);
        }
        else
        {
            // the pong reply is the 'measurement endpoint' of a local node, and
            // to_node_id should be correct.
            if (localMeasurementEndpointToNodeId.find(key)
                != localMeasurementEndpointToNodeId.end())
            {
                forwardMessage(MEASUREMENT_PONG,
                               localMeasurementEndpointToNodeId[key],
                               wholeMessageBegin,
                               wholeMessageEnd,
                               to_node_id);
            }
            else
            {
                debug(mIo->log())
                    << "No measurement endpoint found for endpoint: " << key;
            }
        }
    }

    template <typename It>
    void operator()(const ableton::discovery::UdpEndpoint& fromEndpoint,
                    const It messageBegin,
                    const It messageEnd,
                    std::optional<NodeId> to_node_id = std::nullopt)
    {
        auto result =
            ableton::discovery::v1::parseMessageHeader<NodeId>(messageBegin, messageEnd);
        const auto& header = result.first;

        auto ignoreIpV4Message = false;
        if (fromEndpoint.address().is_v4() && local_interface_address.is_v4())
        {
            const auto subnet = LINK_ASIO_NAMESPACE::ip::make_network_v4(
                local_interface_address.to_v4(), 24);
            const auto fromAddr = LINK_ASIO_NAMESPACE::ip::make_network_v4(
                fromEndpoint.address().to_v4(), 32);
            ignoreIpV4Message = !fromAddr.is_subnet_of(subnet);
        }

        if (!ignoreIpV4Message)
        {
            handle_state_message(fromEndpoint,
                                 header,
                                 messageBegin,
                                 result.second,
                                 messageEnd,
                                 to_node_id);
        }
        // TODO: abstract away this logic with e.g. the caller providing a function
        // pointer
        if (is_running)
        {
            if (to_node_id)
                mRemoteNodeIdToRemoteNodeSurrogate[*to_node_id]->receiveState(
                    util::makeAsyncSafe(this->shared_from_this()));
            else
                receive(util::makeAsyncSafe(this->shared_from_this()));
        }
    }


    template <typename It>
    void sendUdpMessage(Socket& socket,
                        It messageBegin,
                        It messageEnd,
                        const UdpEndpoint& to)
    {
        const auto numBytes =
            static_cast<size_t>(std::distance(messageBegin, messageEnd));
        try
        {
            socket.send(messageBegin, numBytes, to);
        }
        catch (const std::runtime_error& err)
        {
            throw ableton::discovery::UdpSendException{err, local_interface_address};
        }
    }

    UdpEndpoint getBroadcastEndpoint()
    {
        if (local_interface_address.is_v4())
        {
            return ableton::discovery::multicastEndpointV4();
        }
        else
        {
            return ableton::discovery::multicastEndpointV6(
                local_interface_address.to_v6().scope_id());
        }
    }


    template <typename It>
    void forwardMessageLocally(TunnelMessageType messageType,
                               const NodeId& from_node_id,
                               It messageBegin,
                               It messageEnd,
                               const std::optional<NodeId>& to_node_id)
    {
        if ((messageType == UNICAST || messageType == MEASUREMENT_PING)
            && !hasLocalNode(*to_node_id))
            return;

        Socket* socket = nullptr;
        UdpEndpoint to{};

        // check preconditions:
        if (messageType != BYEBYE && messageType != BROADCAST
            && (!to_node_id || !hasLocalNode(*to_node_id)))
            return;

        try
        {
            switch (messageType)
            {
            case MEASUREMENT_PING:
                // pings come from an unknown nodeId but have a known 'to'
                to = localNodeIdToEndpointMap.at(*to_node_id).measurementEndpoint;
                socket = getSocketForRemoteMeasurement(from_node_id);
                break;
            case MEASUREMENT_PONG:
                // pongs are replies from the measurement endpoint of the surrogate to an
                // unknown nodeId's socket. But we've created a random nodeId to store its
                // endpoint
                to = localNodeIdToEndpointMap.at(*to_node_id).measurementEndpoint;
                socket = getSocketForRemoteNode(from_node_id, true);
                break;
            case BYEBYE:
            case BROADCAST:
                to = getBroadcastEndpoint();
                socket = getSocketForRemoteNode(from_node_id);
                break;
            case UNICAST:
            default:
                to = localNodeIdToEndpointMap.at(*to_node_id).stateEndpoint;
                socket = getSocketForRemoteNode(from_node_id);
                break;
            }

            if (messageType == BROADCAST || messageType == UNICAST)
            {
                observeRemotePeerState(from_node_id, messageBegin, messageEnd);
            }
            else if (messageType == BYEBYE)
            {
                onRemotePeerLeft(from_node_id);
            }

            if (messageType != MEASUREMENT_PING && messageType != BYEBYE)
            {
                appendMeasurementEndpoint(from_node_id, messageBegin, messageEnd);
            }

            const auto toEndpoint = to.address().is_v4() ? to : ipV6Endpoint(to);

            sendUdpMessage(*socket, messageBegin, messageEnd, toEndpoint);
        }
        catch (...)
        {
            debug(mIo->log()) << "Error forwarding message locally";
        }
    }

    template <typename It>
    void appendMeasurementEndpoint(const ableton::link::NodeId& from_node_id,
                                   It& messageBegin,
                                   It& messageEnd)
    {
        auto measurementSocket = getSocketForRemoteNode(from_node_id, true);
        auto measurementPayload =
            ableton::discovery::makePayload(
                ableton::link::MeasurementEndpointV4{measurementSocket->endpoint()})
            + ableton::discovery::makePayload(
                ableton::link::MeasurementEndpointV6{measurementSocket->endpoint()});
        auto measurementPayloadSize = sizeInByteStream(measurementPayload);
        if (std::distance(messageBegin, messageEnd) + measurementPayloadSize
            < ableton::discovery::v1::kMaxMessageSize)
            messageEnd = toNetworkByteStream(measurementPayload, std::move(messageEnd));
        else
            debug(mIo->log()) << "Message too large to send. Ignoring";
    }

  private:
    template <typename Handler>
    struct SocketReceiver
    {
        SocketReceiver(Handler handler, std::optional<NodeId> to_node_id = std::nullopt)
            : mHandler(std::move(handler))
            , mOptionalToNodeId(to_node_id)
        {
        }

        template <typename It>
        void operator()(const ableton::discovery::UdpEndpoint& from,
                        const It messageBegin,
                        const It messageEnd)
        {
            mHandler(from, messageBegin, messageEnd, mOptionalToNodeId);
        }

        Handler mHandler;
        std::optional<NodeId> mOptionalToNodeId;
    };

    struct RemoteNodeSurrogate : public std::enable_shared_from_this<RemoteNodeSurrogate>
    {
        RemoteNodeSurrogate(ableton::util::Injected<IoContext> io,
                            const NodeId& ident,
                            ableton::discovery::IpAddress& local_interface_address,
                            const std::shared_ptr<TunnelGateway>& gateway)
            : mRemoteNodeId(ident)
            , stateSocket(
                  io->template openUnicastSocket<ableton::discovery::v1::kMaxMessageSize>(
                      local_interface_address))
            , measurementSocket(
                  io->template openUnicastSocket<ableton::discovery::v1::kMaxMessageSize>(
                      local_interface_address))
            , mGateway(gateway)
        {
            debug(io->log()) << "Creating surrogate for remote node " << mRemoteNodeId;
            debug(io->log()) << "- stateSocket: "
                             << stateSocket.endpoint().address().to_string() << " : "
                             << stateSocket.endpoint().port();
            debug(io->log()) << "- measurementSocket: "
                             << measurementSocket.endpoint().address().to_string() << ":"
                             << measurementSocket.endpoint().port();
        }

        void listen()
        {
            is_running = true;
            receiveState(util::makeAsyncSafe(mGateway));
            receiveMeasurement(util::makeAsyncSafe(this->shared_from_this()));
        }

        void stop_listening() { is_running = false; }

        template <typename Handler>
        void receiveState(Handler handler)
        {
            stateSocket.receive(SocketReceiver<Handler>(handler, mRemoteNodeId));
        }

        template <typename Handler>
        void receiveMeasurement(Handler handler)
        {
            measurementSocket.receive(SocketReceiver<Handler>(handler, mRemoteNodeId));
        }

        // handler for 'measurement' messages
        template <typename It>
        void operator()(const ableton::discovery::UdpEndpoint& fromEndpoint,
                        const It messageBegin,
                        const It messageEnd,
                        std::optional<NodeId> to_node_id = std::nullopt)
        {
            const auto result =
                ableton::link::v1::parseMessageHeader(messageBegin, messageEnd);
            const auto& header = result.first;
            const auto payloadBegin = result.second;

            // measurement messages don't carry a node_id
            mGateway->handle_measurement_message(fromEndpoint,
                                                 header,
                                                 messageBegin,
                                                 payloadBegin,
                                                 messageEnd,
                                                 mRemoteNodeId);
            if (is_running)
            {
                receiveMeasurement(util::makeAsyncSafe(this->shared_from_this()));
            }
        }

        const NodeId mRemoteNodeId;
        Socket stateSocket;
        Socket measurementSocket;
        const std::shared_ptr<TunnelGateway> mGateway;
        bool is_running;
    };

    struct RemoteMeasurementSurrogate
        : public std::enable_shared_from_this<RemoteMeasurementSurrogate>
    {
        RemoteMeasurementSurrogate(ableton::util::Injected<IoContext> io,
                                   const NodeId& ident,
                                   ableton::discovery::IpAddress& local_interface_address,
                                   const std::shared_ptr<TunnelGateway>& gateway)
            : mRemoteNodeId(ident)
            , measurementSocket(
                  io->template openUnicastSocket<ableton::discovery::v1::kMaxMessageSize>(
                      local_interface_address))
            , mGateway(gateway)
        {
        }

        void listen()
        {
            is_running = true;
            measurementSocket.receive(SocketReceiver(
                util::makeAsyncSafe(this->shared_from_this()), mRemoteNodeId));
        }

        void stop_listening() { is_running = false; }

        template <typename It>
        void operator()(const ableton::discovery::UdpEndpoint& fromEndpoint,
                        const It messageBegin,
                        const It messageEnd,
                        std::optional<NodeId> to_node_id = std::nullopt)
        {
            const auto result =
                ableton::link::v1::parseMessageHeader(messageBegin, messageEnd);
            const auto& header = result.first;
            const auto payloadBegin = result.second;
            mGateway->handle_measurement_message(fromEndpoint,
                                                 header,
                                                 messageBegin,
                                                 payloadBegin,
                                                 messageEnd,
                                                 mRemoteNodeId);
            if (is_running)
            {
                measurementSocket.receive(SocketReceiver(
                    util::makeAsyncSafe(this->shared_from_this()), mRemoteNodeId));
            }
        }
        const NodeId mRemoteNodeId;
        Socket measurementSocket;
        const std::shared_ptr<TunnelGateway> mGateway;
        bool is_running;
    };

    struct LocalNodeEndpoints
    {
        ableton::discovery::UdpEndpoint stateEndpoint;
        ableton::discovery::UdpEndpoint measurementEndpoint;
    };

    template <typename It>
    void forwardMessage(TunnelMessageType messageType,
                        const NodeId& from_node_id,
                        const It messageBegin,
                        const It messageEnd,
                        std::optional<NodeId> to_node_id = std::nullopt)
    {
        mTunnel->forward(messageType,
                         this->shared_from_this(),
                         from_node_id,
                         messageBegin,
                         messageEnd,
                         to_node_id);
    }

    bool hasRemoteNodeSurrogate(const NodeId& ident) const
    {
        return mRemoteNodeIdToRemoteNodeSurrogate.find(ident)
               != mRemoteNodeIdToRemoteNodeSurrogate.end();
    }

    Socket* getSocketForRemoteNode(const NodeId& ident, bool getMeasurementSocket = false)
    /** Remote node sockets are created on demand, and belong to a specific Ableton Link
     *  participant. These sockets don't change over time. */
    {
        if (!hasRemoteNodeSurrogate(ident))
        {
            debug(mIo->log()) << "Creating new RemoteNodeSurrogate instance for node: "
                              << ident;
            mRemoteNodeIdToRemoteNodeSurrogate[ident] =
                std::make_shared<RemoteNodeSurrogate>(
                    mIo, ident, local_interface_address, this->shared_from_this());
            // and start listening:
            mRemoteNodeIdToRemoteNodeSurrogate[ident]->listen();
        }
        if (getMeasurementSocket)
            return &(mRemoteNodeIdToRemoteNodeSurrogate[ident]->measurementSocket);
        return &(mRemoteNodeIdToRemoteNodeSurrogate[ident]->stateSocket);
    }

    Socket* getSocketForRemoteMeasurement(const NodeId& ident)
    /** Measurement sockets are created on demand, and their node ID is not related to an
     *  actual Ableton Link participant. */
    {
        if (mRemoteNodeIdToRemoteMeasurementSurrogate.find(ident)
            == mRemoteNodeIdToRemoteMeasurementSurrogate.end())
        {
            debug(mIo->log())
                << "Creating new RemoteMeasurementSurrogate instance for node: " << ident;
            mRemoteNodeIdToRemoteMeasurementSurrogate[ident] =
                std::make_shared<RemoteMeasurementSurrogate>(
                    mIo, ident, local_interface_address, this->shared_from_this());
            // and start listening:
            mRemoteNodeIdToRemoteMeasurementSurrogate[ident]->listen();
        }
        return &(mRemoteNodeIdToRemoteMeasurementSurrogate[ident]->measurementSocket);
    }

    using ENDPOINT_KEY_TYPE = std::string;
    ENDPOINT_KEY_TYPE createKeyFromEndpoint(const UdpEndpoint& endpoint)
    {
        return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }

    UdpEndpoint ipV6Endpoint(const UdpEndpoint& endpoint)
    {
        auto v6Address = endpoint.address().to_v6();
        v6Address.scope_id(local_interface_address.to_v6().scope_id());
        return {v6Address, endpoint.port()};
    }

    struct TimeoutCompare
    {
        bool operator()(const PeerTimeout& lhs, const PeerTimeout& rhs) const
        {
            return lhs.first < rhs.first;
        }
    };

    void updateLocalPeerTimeout(const NodeId& nodeId, uint8_t ttl)
    {
        using namespace std;
        mLocalPeerTimeouts.erase(
            remove_if(mLocalPeerTimeouts.begin(), mLocalPeerTimeouts.end(),
                [&nodeId](const PeerTimeout& pt) { return pt.second == nodeId; }),
            mLocalPeerTimeouts.end());
        auto newTimeout = make_pair(mLocalPruneTimer.now() + chrono::seconds(ttl), nodeId);
        mLocalPeerTimeouts.insert(
            upper_bound(mLocalPeerTimeouts.begin(), mLocalPeerTimeouts.end(),
                        newTimeout, TimeoutCompare{}),
            move(newTimeout));
        scheduleLocalPruning();
    }

    void scheduleLocalPruning()
    {
        if (!mLocalPeerTimeouts.empty())
        {
            const auto t = mLocalPeerTimeouts.front().first + std::chrono::seconds(1);
            mLocalPruneTimer.expires_at(t);
            mLocalPruneTimer.async_wait([this](const TimerError e)
            {
                if (!e)
                    pruneLocalPeers();
            });
        }
    }

    void pruneLocalPeers()
    {
        using namespace std;
        if (!is_running)
            return;
        const auto test = make_pair(mLocalPruneTimer.now(), NodeId{});
        const auto endExpired = lower_bound(
            mLocalPeerTimeouts.begin(), mLocalPeerTimeouts.end(), test, TimeoutCompare{});
        for_each(mLocalPeerTimeouts.begin(), endExpired, [this](const PeerTimeout& pto)
        {
            peerTimedOut(*mLocalObserver, pto.second);
            auto endpointIt = localNodeIdToEndpointMap.find(pto.second);
            if (endpointIt != localNodeIdToEndpointMap.end())
            {
                localMeasurementEndpointToNodeId.erase(
                    createKeyFromEndpoint(endpointIt->second.measurementEndpoint));
                localNodeIdToEndpointMap.erase(endpointIt);
            }
        });
        mLocalPeerTimeouts.erase(mLocalPeerTimeouts.begin(), endExpired);
        scheduleLocalPruning();
    }

    template <typename It>
    void observeRemotePeerState(const NodeId& from_node_id, It messageBegin, It messageEnd)
    {
        try
        {
            const auto result =
                ableton::discovery::v1::parseMessageHeader<NodeId>(messageBegin, messageEnd);
            auto peerState = PeerState::fromPayload(from_node_id, result.second, messageEnd);
            sawPeer(*mRemoteObserver, peerState);
            updateRemotePeerTimeout(peerState.ident(), result.first.ttl);
        }
        catch (const std::runtime_error& err)
        {
            debug(mIo->log()) << "Could not parse remote peer state for observation: "
                              << err.what();
        }
    }

    void onRemotePeerLeft(const NodeId& nodeId)
    {
        peerLeft(*mRemoteObserver, nodeId);
        mRemotePeerTimeouts.erase(
            std::remove_if(mRemotePeerTimeouts.begin(), mRemotePeerTimeouts.end(),
                [&nodeId](const PeerTimeout& pt) { return pt.second == nodeId; }),
            mRemotePeerTimeouts.end());
    }

    void updateRemotePeerTimeout(const NodeId& nodeId, uint8_t ttl)
    {
        using namespace std;
        mRemotePeerTimeouts.erase(
            remove_if(mRemotePeerTimeouts.begin(), mRemotePeerTimeouts.end(),
                [&nodeId](const PeerTimeout& pt) { return pt.second == nodeId; }),
            mRemotePeerTimeouts.end());
        auto newTimeout = make_pair(mRemotePruneTimer.now() + chrono::seconds(ttl), nodeId);
        mRemotePeerTimeouts.insert(
            upper_bound(mRemotePeerTimeouts.begin(), mRemotePeerTimeouts.end(),
                        newTimeout, TimeoutCompare{}),
            move(newTimeout));
        scheduleRemotePruning();
    }

    void scheduleRemotePruning()
    {
        if (!mRemotePeerTimeouts.empty())
        {
            const auto t = mRemotePeerTimeouts.front().first + std::chrono::seconds(1);
            mRemotePruneTimer.expires_at(t);
            mRemotePruneTimer.async_wait([this](const TimerError e)
            {
                if (!e)
                    pruneRemotePeers();
            });
        }
    }

    void pruneRemotePeers()
    {
        using namespace std;
        if (!is_running)
            return;
        const auto test = make_pair(mRemotePruneTimer.now(), NodeId{});
        const auto endExpired = lower_bound(
            mRemotePeerTimeouts.begin(), mRemotePeerTimeouts.end(), test, TimeoutCompare{});
        for_each(mRemotePeerTimeouts.begin(), endExpired, [this](const PeerTimeout& pto)
        {
            peerTimedOut(*mRemoteObserver, pto.second);
            auto it = mRemoteNodeIdToRemoteNodeSurrogate.find(pto.second);
            if (it != mRemoteNodeIdToRemoteNodeSurrogate.end())
            {
                it->second->stop_listening();
                mRemoteNodeIdToRemoteNodeSurrogate.erase(it);
            }
            auto mit = mRemoteNodeIdToRemoteMeasurementSurrogate.find(pto.second);
            if (mit != mRemoteNodeIdToRemoteMeasurementSurrogate.end())
            {
                mit->second->stop_listening();
                mRemoteNodeIdToRemoteMeasurementSurrogate.erase(mit);
            }
        });
        mRemotePeerTimeouts.erase(mRemotePeerTimeouts.begin(), endExpired);
        scheduleRemotePruning();
    }

    ableton::util::Injected<IoContext> mIo;
    ableton::discovery::IpAddress local_interface_address;
    util::Injected<LocalPeerObserver> mLocalObserver;
    util::Injected<RemotePeerObserver> mRemoteObserver;
    TunnelPtr<IoContext, TunnelGateway> mTunnel;

    Socket mMulticastReceiveSocket;
    Timer mLocalPruneTimer;
    Timer mRemotePruneTimer;
    PeerTimeouts mLocalPeerTimeouts;
    PeerTimeouts mRemotePeerTimeouts;

    using RemoteNodeSurrogatePtr = std::shared_ptr<RemoteNodeSurrogate>;
    std::map<NodeId, RemoteNodeSurrogatePtr> mRemoteNodeIdToRemoteNodeSurrogate;

    using RemoteMeasurementSurrogatePtr = std::shared_ptr<RemoteMeasurementSurrogate>;
    std::map<NodeId, RemoteMeasurementSurrogatePtr>
        mRemoteNodeIdToRemoteMeasurementSurrogate;

    std::map<NodeId, LocalNodeEndpoints> localNodeIdToEndpointMap;
    std::map<ENDPOINT_KEY_TYPE, NodeId> localMeasurementEndpointToNodeId;

    bool is_running;
};

} // namespace extender
} // namespace ableton
