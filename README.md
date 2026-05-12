# Ableton Link Extender

**Sync your DAW sessions across the internet — transparently.**

Ableton Link is a local-network protocol that lets DAWs and music apps lock their tempo and transport together. *Ableton Link Extender* (ALE) bridges two (or more) Link networks over any IP connection, making every remote node appear as if it were sitting right next to you on the same LAN.

---

## How it works

ALE runs as a lightweight relay on each side of the connection.

```
  ┌──────────────────────────┐          internet          ┌──────────────────────────┐
  │   Local Network (A)      │                            │   Local Network (B)      │
  │                          │                            │                          │
  │  DAW ─┐                  │                            │                  ┌─ DAW  │
  │  DAW ─┤─ Ableton Link ── ALE ──── UDP tunnel ──── ALE ── Ableton Link ─┤─ DAW  │
  │  DAW ─┘                  │                            │                  └─ DAW  │
  └──────────────────────────┘                            └──────────────────────────┘
```

ALE intercepts the Link discovery and state messages on each side and re-injects them on the other, so every node sees every other node as a genuine Link peer. No special DAW configuration needed.

The UDP tunnel uses a simple handshake (HELLO / KEEPALIVE / BYE) with automatic reconnection, so the session recovers gracefully if connectivity is briefly lost.

---

## Features

- **Invisible pass-through** — remote nodes show up as local Link peers; no DAW changes required
- **Multi-peer** — connect more than two sites by passing `--peer` multiple times
- **Flexible addressing** — accepts hostnames, IPv4 addresses, and bracketed IPv6 addresses
- **Auto-reconnect** — keepalive heartbeats every 5 s; peers are re-dialed if they go quiet for 15 s
- **Cross-platform** — Linux, macOS (Apple Silicon & Intel), Windows
- **Embeddable API** — use `LinkExtender` as a C++ library with tempo / peer-count / transport callbacks

---

## Building

**Requirements:** CMake ≥ 3.21, a C++17 compiler, Ninja (recommended)

```bash
git clone --recurse-submodules https://github.com/Sgorpi/AbletonLinkExtender.git
cd AbletonLinkExtender
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary lands at `build/ableton_link_extender` (or `.exe` on Windows).

Pre-built binaries for each platform are attached to every CI run on GitHub Actions.

---

## Usage

Run one instance per site. Each instance needs to know its own listening port and the address of every other ALE instance it should connect to.

```
ableton_link_extender --port <N> --peer <HOST>:<PORT> [--peer ...]
```

| Argument | Description |
|---|---|
| `--port <N>` | Local UDP port to listen on (default: `20909`) |
| `--peer <HOST>:<PORT>` | Remote ALE instance to connect to (repeatable) |
| `--help` | Show usage and exit |

`HOST` can be a **`hostname`**, an **`IPv4`** address, or a bracketed **`[IPv6]`** address.

### Two-site example

**Site A** (public IP `203.0.113.1`, port `9000`):
```bash
ableton_link_extender --port 9000 --peer 198.51.100.5:9000
```

**Site B** (public IP `198.51.100.5`, port `9000`):
```bash
ableton_link_extender --port 9000 --peer 203.0.113.1:9000
```

Both sides need the chosen port open in their firewall / router for inbound UDP.

### Three-site mesh

Add a `--peer` for every other site:

```bash
# Site A
ableton_link_extender --port 9000 --peer siteB.example.com:9000 --peer siteC.example.com:9000

# Site B
ableton_link_extender --port 9000 --peer siteA.example.com:9000 --peer siteC.example.com:9000

# Site C
ableton_link_extender --port 9000 --peer siteA.example.com:9000 --peer siteB.example.com:9000
```


## Interactive controls

| Key | Action |
|---|---|
| `q` | Quit |

The terminal display updates every 10 ms and shows:

```
local peers | remote peers | tempo   | playing
2           | 3            | 120.00  | [playing]
```

---

## Roadmap

- **Version 1** *(current)* — invisible pass-through; all nodes communicate directly through ALE
- **Version 1.5** (planned) — Additional GUI application
- **Version 2** *(planned)* — smarter relay that forwards only session-state deltas, reducing internet bandwidth at the cost of nodes not appearing individually on the remote side

---

## License

ALE itself is released under the **GNU GPL v2**.  
The included [Ableton Link](link/) library is licensed under the **GNU GPL v2**.
