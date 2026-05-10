# Ableton Link Extender - documenation.

We can think of two versions of Ableton Link Extender:

- **Version 1**: Act as an 'invisible' pass-through, where each Node on your local network will show up as a Node on the remote network, and vice versa. This will have quite some communication overhead over the internet, as all nodes will need to communicate with each other.

- **Version 1.5**: Add a GUI application next to the terminal one.

- **Version 2**: Act as a separate Node with a lot of delay on the local network, and pass only the state information over the internet. This will be more efficient, but dives deeper into the Ableton Link protocol.

Currently, we only have the first version implemented.



## Version 1: Invisible Pass-through

### Local testing

There are a few ways to test:
- Omit `--peer` to use the built-in shared-memory tunnel — useful for smoke-testing two Link apps on the same machine but with divided network namespaces. You can find a script to setup network namespaces under linux in the [tests/](tests/) directory.

```bash
ip netns exec ns0 su - "$SUDO_USER" -c ableton_link_extender   # instance 1 — shared memory mode
ip netns exec ns1 su - "$SUDO_USER" -c ableton_link_extender   # instance 2 — same machine, same tunnel
```

- A Linux network-namespace integration test exercises the full UDP tunnel between two isolated network stacks on a single machine. See [test_tunnel.sh](tests/test_tunnel.sh):

```bash
sudo ./tests/setup_netns.sh   # create ns0 and ns1 (run once)
./tests/test_tunnel.sh   # start two ALE instances and verify handshake
```

### Architecture Overview

Most logic is contained in the "TunnelGateway", which acts as a relay between the local network and the Tunnel.
The Tunnel is an abstract concept that can be implemented in multiple ways, such as using shared memory or UDP.

Below included is a sequence diagram of the main flow:

![Sequence Diagram](diagrams/seq_manual.svg)



The core is the **TunnelGateway**, which bridges the local Link peer discovery with the Tunnel abstraction. Two tunnel backends exist:

| Backend | Used when | Purpose |
|---|---|---|
| `UdpTunnel` | `--peer` arguments given | Internet / LAN bridging |
| `ShmTunnel` | No `--peer` arguments | Same-host testing via shared memory |

Both implement a common `Tunnel<IoContext, Gateway>` interface, making them interchangeable. The `Controller` wires everything together: it owns two independent `Peers` registries (local and remote), feeds them through the gateway, and fires user-supplied callbacks when tempo, transport state, or peer counts change.

A sequence diagram of the main flow is in [doc/documentation.md](doc/documentation.md).

---

## Embedding the library

`LinkExtender` can be used as a C++ object in your own application:

```cpp
#include "LinkExtender.hpp"

ableton::LinkExtender::Config config;
config.tunnel_port = 9000;
config.peers.push_back(/* ASIO udp::endpoint */);

ableton::LinkExtender extender{config};

extender.setTempoCallback([](double bpm) {
    // called on a Link-managed thread
});
extender.setNumPeersCallback([](std::size_t local, std::size_t remote) {
    // local  = Link peers on your LAN
    // remote = Link peers on the far side
});
extender.setStartStopCallback([](bool isPlaying) { /* ... */ });

// Peers can also be added/removed at runtime:
extender.addPeer(endpoint);
extender.removePeer(endpoint);
```

