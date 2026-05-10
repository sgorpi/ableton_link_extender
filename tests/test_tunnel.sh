#!/bin/bash
# Test ALE UDP tunnel between two network namespaces on the same machine.
#
# ns0 (10.200.0.1) <--veth--> host <--veth--> ns1 (10.200.1.1)
#
# Each namespace runs its own ALE instance.  They reach each other's ALE
# directly via the host routing (no public IP / hairpin NAT needed).
#
# Usage:
#   sudo ./test_tunnel.sh          # uses build/ableton_link_extender
#   sudo ./test_tunnel.sh <binary> # specify a different binary

set -euo pipefail

ALE="${1:-$(dirname "$0")/../build/ableton_link_extender}"

NS0="ns0"
NS1="ns1"
NS0_IP="10.200.0.1"
NS1_IP="10.200.1.1"
PORT0=9100
PORT1=9101
LOG0="/tmp/ale_ns0.log"
LOG1="/tmp/ale_ns1.log"
HANDSHAKE_TIMEOUT=8   # seconds to wait for "peer connected" in both logs

# ---------------------------------------------------------------------------

die() { echo "FAIL: $*" >&2; exit 1; }

cleanup() {
    echo ""
    echo "--- Stopping ALE instances ---"
    kill "$PID0" "$PID1" 2>/dev/null || true
    wait "$PID0" "$PID1" 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Preflight checks
# ---------------------------------------------------------------------------

[[ -x "$ALE" ]] || die "binary not found or not executable: $ALE"

for NS in "$NS0" "$NS1"; do
    ip netns list | grep -q "^${NS}" \
        || die "network namespace '$NS' does not exist – run setup_netns.sh first"
done

# Quick connectivity check: can ns0 reach ns1's IP?
if ! sudo nsexec "$NS0" ping -c1 -W2 "$NS1_IP" &>/dev/null; then
    die "ns0 cannot ping $NS1_IP – check setup_netns.sh inter-namespace routes"
fi
echo "Connectivity: ns0 -> $NS1_IP OK"

# ---------------------------------------------------------------------------
# Start ALE instances
# ---------------------------------------------------------------------------

echo "--- Starting ALE in $NS0 (port $PORT0, peer $NS1_IP:$PORT1) ---"
sudo nsexec "$NS0" "$ALE" --port "$PORT0" --peer "$NS1_IP:$PORT1" >"$LOG0" 2>&1 &
PID0=$!

echo "--- Starting ALE in $NS1 (port $PORT1, peer $NS0_IP:$PORT0) ---"
sudo nsexec "$NS1" "$ALE" --port "$PORT1" --peer "$NS0_IP:$PORT0" >"$LOG1" 2>&1 &
PID1=$!

# ---------------------------------------------------------------------------
# Wait for both sides to report a completed handshake
# ---------------------------------------------------------------------------

echo "--- Waiting up to ${HANDSHAKE_TIMEOUT}s for handshake ---"

wait_for_connected() {
    local log="$1" label="$2"
    local deadline=$(( $(date +%s) + HANDSHAKE_TIMEOUT ))
    while [[ $(date +%s) -lt $deadline ]]; do
        grep -q "UdpTunnel: peer connected:" "$log" 2>/dev/null && return 0
        sleep 0.25
    done
    return 1
}

PASS=true

if wait_for_connected "$LOG0" "$NS0"; then
    echo "PASS: $NS0 handshake complete"
else
    echo "FAIL: $NS0 did not complete handshake within ${HANDSHAKE_TIMEOUT}s"
    PASS=false
fi

if wait_for_connected "$LOG1" "$NS1"; then
    echo "PASS: $NS1 handshake complete"
else
    echo "FAIL: $NS1 did not complete handshake within ${HANDSHAKE_TIMEOUT}s"
    PASS=false
fi

# ---------------------------------------------------------------------------
# Let them run a bit longer to verify keepalives don't break anything
# ---------------------------------------------------------------------------

if $PASS; then
    echo "--- Handshake OK; running 6 more seconds to verify keepalives ---"
    sleep 6
    # Both processes should still be alive
    if ! kill -0 "$PID0" 2>/dev/null; then
        echo "FAIL: $NS0 ALE exited unexpectedly"; PASS=false
    fi
    if ! kill -0 "$PID1" 2>/dev/null; then
        echo "FAIL: $NS1 ALE exited unexpectedly"; PASS=false
    fi
fi

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

echo ""
echo "=== ns0 log ==="
cat "$LOG0"
echo ""
echo "=== ns1 log ==="
cat "$LOG1"
echo ""

$PASS && echo "=== RESULT: PASS ===" || { echo "=== RESULT: FAIL ==="; exit 1; }
