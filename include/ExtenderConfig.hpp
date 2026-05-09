#pragma once

// include/ableton/platforms/asio/Context.hpp shadows the upstream version and adds
// openBoundUdpSocket(port), so this include pulls in the extended Context automatically.
#include <ableton/platforms/Config.hpp>

namespace ableton
{
namespace extender
{

// The platform IoContext, extended with openBoundUdpSocket().
// UdpTunnel uses this to bind a fixed-port UDP socket for tunnel traffic.
using IoContext = ableton::link::platform::IoContext;

} // namespace extender
} // namespace ableton
