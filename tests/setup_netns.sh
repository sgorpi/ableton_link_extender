#!/bin/bash

DEVICE=eno1
# Each namespace gets a unique /30: .1 is the namespace IP, .2 is the host-side gateway.
# Using separate subnets avoids IP conflicts on the host and makes inter-namespace
# routing unambiguous.
NAMESPACES=(
    "ns0:10.200.0.1/30:10.200.0.2"
    "ns1:10.200.1.1/30:10.200.1.2"
)
VETH_HOST_IFACE="veth_host"
VETH_NS_IFACE="veth_ns"

# ---------------------------------------------------------------------------
# Teardown: remove any leftover state from a previous run
# ---------------------------------------------------------------------------
teardown_namespace() {
    local NS_NAME=$1
    local VETH_HOST="${VETH_HOST_IFACE}_${NS_NAME}"

    ip link del "$VETH_HOST" 2>/dev/null || true
    ip netns del "$NS_NAME"  2>/dev/null || true
}

echo "--- Tearing down existing namespaces ---"
for item in "${NAMESPACES[@]}"; do
    IFS=':' read -r NS_NAME _ _ <<< "$item"
    teardown_namespace "$NS_NAME"
done

# Flush any leftover forwarding rules (re-added below)
iptables -F FORWARD 2>/dev/null || true

# ---------------------------------------------------------------------------
# Global networking
# ---------------------------------------------------------------------------
sysctl -w net.ipv4.ip_forward=1

iptables -t nat -F
iptables -t nat -A POSTROUTING -o "$DEVICE" -j MASQUERADE

# ---------------------------------------------------------------------------
# Per-namespace setup
# ---------------------------------------------------------------------------
setup_namespace() {
    local NS_NAME=$1
    local NS_IP_CIDR=$2   # e.g. 10.200.0.1/30
    local GW_IP=$3         # e.g. 10.200.0.2  (host side of the veth pair)
    local NS_IP="${NS_IP_CIDR%%/*}"
    local PREFIX="${NS_IP_CIDR##*/}"
    local GW_CIDR="${GW_IP}/${PREFIX}"
    local VETH_HOST="${VETH_HOST_IFACE}_${NS_NAME}"
    local VETH_NS="${VETH_NS_IFACE}_${NS_NAME}"

    echo "--- Setting up $NS_NAME: ns=$NS_IP_CIDR gw=$GW_IP ---"

    ip netns add "$NS_NAME"

    ip link add name "$VETH_HOST" type veth peer name "$VETH_NS"
    ip link set "$VETH_NS" netns "$NS_NAME"

    # Host side: unique gateway IP for this namespace
    ip addr add "$GW_CIDR" dev "$VETH_HOST"
    ip link set dev "$VETH_HOST" up

    # Namespace side: namespace IP, default route via gateway
    ip netns exec "$NS_NAME" ip link set lo up
    ip netns exec "$NS_NAME" ip link set "$VETH_NS" up
    ip netns exec "$NS_NAME" ip addr add "$NS_IP_CIDR" dev "$VETH_NS"
    ip netns exec "$NS_NAME" ip route add default via "$GW_IP" dev "$VETH_NS"

    # DNS resolver
    mkdir -p /etc/netns/"$NS_NAME"
    echo "nameserver 8.8.8.8" > /etc/netns/"$NS_NAME"/resolv.conf

    # Allow forwarding to/from this namespace
    iptables -A FORWARD -i "$VETH_HOST" -j ACCEPT
    iptables -A FORWARD -o "$VETH_HOST" -j ACCEPT

    echo "Namespace $NS_NAME setup complete."
}

for item in "${NAMESPACES[@]}"; do
    IFS=':' read -r NS_NAME IP_CIDR GW_IP <<< "$item"
    setup_namespace "$NS_NAME" "$IP_CIDR" "$GW_IP"
done

# ---------------------------------------------------------------------------
# Inter-namespace routes: each namespace gets a /32 host route to every other
# namespace's IP via its own gateway (the host routes between them).
# Note: 'local' is only valid inside functions; use plain variables here.
# ---------------------------------------------------------------------------
echo "--- Adding inter-namespace routes ---"
for src_item in "${NAMESPACES[@]}"; do
    IFS=':' read -r SRC_NS SRC_CIDR SRC_GW <<< "$src_item"
    for dst_item in "${NAMESPACES[@]}"; do
        IFS=':' read -r DST_NS DST_CIDR DST_GW <<< "$dst_item"
        [[ "$SRC_NS" == "$DST_NS" ]] && continue
        DST_IP="${DST_CIDR%%/*}"
        ip netns exec "$SRC_NS" ip route add "${DST_IP}/32" via "$SRC_GW"
        echo "  $SRC_NS -> ${DST_IP}/32 via $SRC_GW"
    done
done
