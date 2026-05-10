#pragma once

#include <ableton/discovery/PeerGateways.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/platforms/Config.hpp>

#include "ableton/extender/Controller.hpp"

namespace ableton
{
using IoContext = link::platform::IoContext;

class LinkExtender
{
  public:
    using Config = extender::Controller<IoContext>::Config;

    explicit LinkExtender(Config config = {})
        : mController(std::move(config))
    {
    }

    /*! @brief Link Extender instances cannot be copied or moved */
    LinkExtender(const LinkExtender&) = delete;
    LinkExtender& operator=(const LinkExtender&) = delete;
    LinkExtender(LinkExtender&&) = delete;
    LinkExtender& operator=(LinkExtender&&) = delete;

    // Add a remote peer to the UDP tunnel at runtime.
    // No-op when running in shared-memory (local) mode.
    void addPeer(LINK_ASIO_NAMESPACE::ip::udp::endpoint ep)
    {
        mController.addPeer(std::move(ep));
    }

    // Remove a previously added peer from the UDP tunnel.
    void removePeer(LINK_ASIO_NAMESPACE::ip::udp::endpoint ep)
    {
        mController.removePeer(std::move(ep));
    }

    // Callback invoked on a Link-managed thread when either the local or remote
    // peer count changes. Signature: void(std::size_t localPeers, std::size_t remotePeers)
    template <typename Callback>
    void setNumPeersCallback(Callback callback)
    {
        mController.setNumPeersCallback(std::move(callback));
    }

    // Callback invoked on a Link-managed thread when the session tempo changes.
    // Signature: void(double bpm)
    template <typename Callback>
    void setTempoCallback(Callback callback)
    {
        mController.setTempoCallback(std::move(callback));
    }

    // Callback invoked on a Link-managed thread when the transport start/stop
    // state changes. Signature: void(bool isPlaying)
    template <typename Callback>
    void setStartStopCallback(Callback callback)
    {
        mController.setStartStopCallback(std::move(callback));
    }

  private:
    extender::Controller<IoContext> mController;
};
} // namespace ableton
