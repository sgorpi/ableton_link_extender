#pragma once

#include <ableton/discovery/PeerGateways.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/platforms/Config.hpp>

#include "ShmTunnel.hpp"
#include "TunnelGateway.hpp"

namespace ableton
{
namespace extender
{

using NodeState = ableton::link::NodeState;
using IoContext = ableton::link::platform::IoContext;

using PeerCountCallback = std::function<void(std::size_t)>;
using TempoCallback = std::function<void(ableton::link::Tempo)>;
using StartStopStateCallback = std::function<void(bool)>;

void cb_peer_count(std::size_t count)
{
    std::cout << "Peer count: " << count << std::endl;
}

template <typename IoContext>
class Controller
{
  protected:
    struct SessionTimelineCallback
    {
        void operator()(ableton::link::NodeId id, ableton::link::Timeline timeline)
        {
            std::cout << "Session " << id << ": Timeline tempo: " << timeline.tempo.bpm();
        }

        Controller& mController;
    };

    struct SessionStartStopStateCallback
    {
        void operator()(ableton::link::NodeId sessionId,
                        ableton::link::StartStopState startStopState)
        {
            std::cout << "Session " << sessionId
                      << " is playing: " << startStopState.isPlaying;
        }

        Controller& mController;
    };

    struct SessionPeerCounter
    {
        SessionPeerCounter(Controller& controller, PeerCountCallback callback)
            : mController(controller)
            , mCallback(std::move(callback))
            , mSessionPeerCount(0)
        {
        }

        void operator()()
        {
            std::cout << "Session Peer Count callback" << mSessionPeerCount << std::endl;
            mCallback(mSessionPeerCount);
        }

        Controller& mController;
        PeerCountCallback mCallback;
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
        TunnelGateway<typename ControllerPeers::GatewayObserver, IoType&>;
    using TunnelGatewayPtr = std::shared_ptr<ControllerTunnelGateway>;

    Controller()
        : mIo(IoContext{UdpSendExceptionHandler{this}})
        , notParticipatingNodeState({})
        , mTunnel(makeShmTunnel<IoType&, ControllerTunnelGateway>(util::injectRef(*mIo)))
        , gatewayDiscovery(std::chrono::seconds(5),
                           std::move(notParticipatingNodeState),
                           GatewayFactory{*this},
                           util::injectRef(*mIo))
        , mSessionPeerCounter(*this, &cb_peer_count)
        , mLocalPeers(util::injectRef(*mIo),
                      std::ref(mSessionPeerCounter),
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

    using TunnelGateways =
        ableton::discovery::PeerGateways<NodeState, GatewayFactory, IoType&>;

    ableton::util::Injected<IoContext> mIo;
    NodeState notParticipatingNodeState;
    TunnelPtr<IoContext&, ControllerTunnelGateway> mTunnel;
    TunnelGateways gatewayDiscovery;
    SessionPeerCounter mSessionPeerCounter;
    ControllerPeers mLocalPeers;
};
} // namespace extender
} // namespace ableton