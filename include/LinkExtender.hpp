#pragma once

#include <ableton/discovery/PeerGateways.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/platforms/Config.hpp>

#include "Controller.hpp"

namespace ableton
{
using IoContext = link::platform::IoContext;

class LinkExtender
{
  public:
    LinkExtender()
        : mController()
    {
    }

    /*! @brief Link Extender instances cannot be copied or moved */
    LinkExtender(const LinkExtender&) = delete;
    LinkExtender& operator=(const LinkExtender&) = delete;
    LinkExtender(LinkExtender&&) = delete;
    LinkExtender& operator=(LinkExtender&&) = delete;

  private:
    ableton::extender::Controller<IoContext> mController;
};
} // namespace ableton
