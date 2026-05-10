#pragma once

#include <ableton/discovery/PeerGateways.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/platforms/Config.hpp>

#include "ableton/extender/ShmTunnel.hpp"
#include "ableton/extender/TunnelGateway.hpp"
#include "ableton/extender/UdpTunnel.hpp"

#include <iostream>
#include <mutex>
#include <vector>

namespace ableton
{
namespace extender
{

using NodeState = ableton::link::NodeState;
using IoContext = ableton::link::platform::IoContext;

using PeerCountCallback = std::function<void(std::size_t localPeers, std::size_t remotePeers)>;
using TempoCallback = std::function<void(ableton::link::Tempo)>;
using StartStopStateCallback = std::function<void(bool)>;

template <typename IoContext>
class Controller
{
  protected:
    struct SessionTimelineCallback
    {
        void operator()(ableton::link::SessionId id, ableton::link::Timeline timeline)
        {
            mController.mSessionId = id;
            std::lock_guard<std::mutex> lock(mController.mCallbackMutex);
            mController.mTempoCallback(timeline.tempo);
        }

        Controller& mController;
    };

    struct SessionStartStopStateCallback
    {
        void operator()(ableton::link::NodeId /*sessionId*/,
                        ableton::link::StartStopState startStopState)
        {
            std::lock_guard<std::mutex> lock(mController.mCallbackMutex);
            mController.mStartStopCallback(startStopState.isPlaying);
        }

        Controller& mController;
    };

    struct SessionPeerCounter
    {
        using CountFn = std::function<std::size_t()>;
        using NotifyFn = std::function<void(std::size_t)>;

        SessionPeerCounter(CountFn countFn, NotifyFn notify)
            : mCountFn(std::move(countFn))
            , mNotify(std::move(notify))
            , mSessionPeerCount(0)
        {
        }

        void operator()()
        {
            const auto count = mCountFn();
            const auto oldCount = mSessionPeerCount.exchange(count);
            if (oldCount != count)
            {
                mNotify(count);
            }
        }

        CountFn mCountFn;
        NotifyFn mNotify;
        std::atomic<std::size_t> mSessionPeerCount;
    };

  public:
    using IoType = typename util::Injected<IoContext>::type;
    using ControllerPeers =
        ableton::link::Peers<IoType&,
                             std::reference_wrapper<SessionPeerCounter>,
                             SessionTimelineCallback,
                             SessionStartStopStateCallback>;

    using ControllerTunnelGateway =
        TunnelGateway<typename ControllerPeers::GatewayObserver,
                      typename ControllerPeers::GatewayObserver,
                      IoType&>;
    using TunnelGatewayPtr = std::shared_ptr<ControllerTunnelGateway>;

    // Configuration passed at construction time. When peers is non-empty a
    // UdpTunnel is created; otherwise the ShmTunnel proof-of-concept is used.
    struct Config
    {
        uint16_t tunnel_port = 12345;
        std::vector<LINK_ASIO_NAMESPACE::ip::udp::endpoint> peers;
    };

    Controller(Config config = {})
        : mIo(IoContext{UdpSendExceptionHandler{this}})
        , mConfig(std::move(config))
        , notParticipatingNodeState({})
        , mTunnel(createTunnel())
        , gatewayDiscovery(std::chrono::seconds(5),
                           std::move(notParticipatingNodeState),
                           GatewayFactory{*this},
                           util::injectRef(*mIo))
        , mSessionPeerCounter([this]()
                              { return mLocalPeers.uniqueSessionPeerCount(mSessionId); },
                              [this](std::size_t) { notifyPeerCount(); })
        , mLocalPeers(util::injectRef(*mIo),
                      std::ref(mSessionPeerCounter),
                      SessionTimelineCallback{*this},
                      SessionStartStopStateCallback{*this})
        , mRemoteSessionPeerCounter(
              [this]() { return mRemotePeers.uniqueSessionPeerCount(mSessionId); },
              [this](std::size_t) { notifyPeerCount(); })
        , mRemotePeers(util::injectRef(*mIo),
                       std::ref(mRemoteSessionPeerCounter),
                       SessionTimelineCallback{*this},
                       SessionStartStopStateCallback{*this})
    {
        gatewayDiscovery.enable(true);
    }
    Controller(const Controller&) = delete;
    Controller(Controller&&) = delete;

    Controller& operator=(const Controller&) = delete;
    Controller& operator=(Controller&&) = delete;

    ~Controller()
    {
        std::mutex mutex;
        std::condition_variable condition;
        auto stopped = false;

        mTunnel->stop_listening();
        mIo->async(
            [this, &mutex, &condition, &stopped]()
            {
                gatewayDiscovery.enable(false);
                std::unique_lock<std::mutex> lock(mutex);
                stopped = true;
                condition.notify_one();
            });

        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&stopped] { return stopped; });

        mTunnel.reset();
        mIo->stop();
    }

    // Add a remote peer to the UdpTunnel at runtime (no-op if using ShmTunnel).
    void addPeer(LINK_ASIO_NAMESPACE::ip::udp::endpoint ep)
    {
        using UdpT = UdpTunnel<IoType&, ControllerTunnelGateway>;
        if (auto* t = dynamic_cast<UdpT*>(mTunnel.get()))
            t->addPeer(std::move(ep));
    }

    // Remove a previously added peer (no-op if using ShmTunnel).
    void removePeer(LINK_ASIO_NAMESPACE::ip::udp::endpoint ep)
    {
        using UdpT = UdpTunnel<IoType&, ControllerTunnelGateway>;
        if (auto* t = dynamic_cast<UdpT*>(mTunnel.get()))
            t->removePeer(std::move(ep));
    }

    template <typename Callback>
    void setNumPeersCallback(Callback callback)
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mPeerCountCallback = [callback](const std::size_t local, const std::size_t remote)
        { callback(local, remote); };
    }

    template <typename Callback>
    void setTempoCallback(Callback callback)
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mTempoCallback = [callback](const ableton::link::Tempo tempo) { callback(tempo.bpm()); };
    }

    template <typename Callback>
    void setStartStopCallback(Callback callback)
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mStartStopCallback = callback;
    }

  protected:
    struct GatewayFactory
    {
        TunnelGatewayPtr operator()(NodeState state,
                                    ableton::util::Injected<IoContext&> io,
                                    const ableton::discovery::IpAddress& addr)
        {
            debug(io->log()) << "Creating new TunnelGateway for " << addr.to_string();

            auto t = std::make_shared<ControllerTunnelGateway>(
                std::move(io),
                addr,
                util::injectVal(makeGatewayObserver(mController.mLocalPeers, addr)),
                util::injectVal(makeGatewayObserver(mController.mRemotePeers, addr)),
                mController.mTunnel);
            t->listen();

            return t;
        }
        Controller& mController;
    };

    struct UdpSendExceptionHandler
    {
        using Exception = ableton::discovery::UdpSendException;
        void operator()(const Exception exception)
        {
            mpController->mIo->async(
                [this, exception]
                {
                    mpController->gatewayDiscovery.repairGateway(exception.interfaceAddr);
                });
        }

        Controller* mpController;
    };

    void notifyPeerCount()
    {
        const auto local = mLocalPeers.uniqueSessionPeerCount(mSessionId);
        const auto remote = mRemotePeers.uniqueSessionPeerCount(mSessionId);
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mPeerCountCallback(local, remote);
    }

    using TunnelGateways =
        ableton::discovery::PeerGateways<NodeState, GatewayFactory, IoType&>;

    TunnelPtr<IoType&, ControllerTunnelGateway> createTunnel()
    {
        if (mConfig.peers.empty())
        {
            return makeShmTunnel<IoType&, ControllerTunnelGateway>(util::injectRef(*mIo));
        }
        return makeUdpTunnel<IoType&, ControllerTunnelGateway>(
            util::injectRef(*mIo), mConfig.tunnel_port, mConfig.peers);
    }

    std::mutex mCallbackMutex;
    PeerCountCallback mPeerCountCallback = [](std::size_t, std::size_t) {};
    TempoCallback mTempoCallback = [](ableton::link::Tempo) {};
    StartStopStateCallback mStartStopCallback = [](bool) {};

    ableton::util::Injected<IoContext> mIo;
    Config mConfig;
    NodeState notParticipatingNodeState;
    ableton::link::SessionId mSessionId;
    TunnelPtr<IoType&, ControllerTunnelGateway> mTunnel;
    TunnelGateways gatewayDiscovery;
    SessionPeerCounter mSessionPeerCounter;
    ControllerPeers mLocalPeers;
    SessionPeerCounter mRemoteSessionPeerCounter;
    ControllerPeers mRemotePeers;
};
} // namespace extender
} // namespace ableton
