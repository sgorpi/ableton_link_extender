// Tests for DedupCache — the LRU/timestamp cache that backs Tunnel::shouldForward.

#include <ableton/platforms/Config.hpp>
#include <ableton/extender/DedupCache.hpp>
#include <ableton/extender/Tunnel.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/test/CatchWrapper.hpp>

#include <chrono>
#include <vector>

namespace ableton
{
namespace extender
{
namespace
{

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline link::NodeId makeNodeId()
{
    return link::NodeId::random<link::platform::Random>();
}

// Owns a mutable TimePoint and hands the DedupCache a lambda that reads it.
// Tests advance the clock by mutating `now`.
struct ManualClock
{
    TimePoint now{};
    auto fn()
    {
        return [this] { return now; };
    }
};

const std::vector<uint8_t> kPayload{1, 2, 3, 4, 5};

const unsigned char* pbegin(const std::vector<uint8_t>& v)
{
    return reinterpret_cast<const unsigned char*>(v.data());
}
const unsigned char* pend(const std::vector<uint8_t>& v)
{
    return reinterpret_cast<const unsigned char*>(v.data() + v.size());
}

} // namespace

TEST_CASE("DedupCache | D1 | second call with same (type,node,payload) within window is dup")
{
    ManualClock c;
    DedupCache cache{16, kDedupWindow, c.fn()};
    const auto node = makeNodeId();

    CHECK(!cache.isDuplicate(BROADCAST, node, pbegin(kPayload), pend(kPayload)));
    c.now += std::chrono::milliseconds(10);
    CHECK(cache.isDuplicate(BROADCAST, node, pbegin(kPayload), pend(kPayload)));
}

TEST_CASE("DedupCache | D2 | different NodeId with identical payload is not a dup")
{
    ManualClock c;
    DedupCache cache{16, kDedupWindow, c.fn()};
    const auto nodeA = makeNodeId();
    const auto nodeB = makeNodeId();

    CHECK(!cache.isDuplicate(BROADCAST, nodeA, pbegin(kPayload), pend(kPayload)));
    CHECK(!cache.isDuplicate(BROADCAST, nodeB, pbegin(kPayload), pend(kPayload)));
}

TEST_CASE("DedupCache | D3 | different message type with same NodeId and payload is not a dup")
{
    ManualClock c;
    DedupCache cache{16, kDedupWindow, c.fn()};
    const auto node = makeNodeId();

    CHECK(!cache.isDuplicate(BROADCAST, node, pbegin(kPayload), pend(kPayload)));
    CHECK(!cache.isDuplicate(UNICAST, node, pbegin(kPayload), pend(kPayload)));
    CHECK(!cache.isDuplicate(BYEBYE, node, pbegin(kPayload), pend(kPayload)));
}

TEST_CASE("DedupCache | D4 | same key after window elapses is forwarded again")
{
    ManualClock c;
    DedupCache cache{16, kDedupWindow, c.fn()};
    const auto node = makeNodeId();

    CHECK(!cache.isDuplicate(BROADCAST, node, pbegin(kPayload), pend(kPayload)));
    // Move past the dedup window plus a small margin.
    c.now += kDedupWindow + std::chrono::milliseconds(1);
    CHECK(!cache.isDuplicate(BROADCAST, node, pbegin(kPayload), pend(kPayload)));
    // Refresh anchored the timestamp at the new `now`; a third call right after is dup.
    CHECK(cache.isDuplicate(BROADCAST, node, pbegin(kPayload), pend(kPayload)));
}

TEST_CASE("DedupCache | D5 | distinct payloads (same type+node) are not dups")
{
    ManualClock c;
    DedupCache cache{16, kDedupWindow, c.fn()};
    const auto node = makeNodeId();
    const std::vector<uint8_t> alt{9, 9, 9};

    CHECK(!cache.isDuplicate(BROADCAST, node, pbegin(kPayload), pend(kPayload)));
    CHECK(!cache.isDuplicate(BROADCAST, node, pbegin(alt), pend(alt)));
}

TEST_CASE("DedupCache | D6 | LRU evicts oldest entry when capacity exceeded")
{
    ManualClock c;
    DedupCache cache{/*capacity*/ 2, kDedupWindow, c.fn()};
    const auto nodeA = makeNodeId();
    const auto nodeB = makeNodeId();
    const auto nodeC = makeNodeId();

    CHECK(!cache.isDuplicate(BROADCAST, nodeA, pbegin(kPayload), pend(kPayload)));
    CHECK(!cache.isDuplicate(BROADCAST, nodeB, pbegin(kPayload), pend(kPayload)));
    // Cache now holds {B (front), A (back)}.
    // Inserting C evicts A (oldest), leaving {C, B}.
    CHECK(!cache.isDuplicate(BROADCAST, nodeC, pbegin(kPayload), pend(kPayload)));
    // B and C should still be present and report as duplicates.
    CHECK(cache.isDuplicate(BROADCAST, nodeB, pbegin(kPayload), pend(kPayload)));
    CHECK(cache.isDuplicate(BROADCAST, nodeC, pbegin(kPayload), pend(kPayload)));
    // A was evicted, so it is no longer a duplicate.
    CHECK(!cache.isDuplicate(BROADCAST, nodeA, pbegin(kPayload), pend(kPayload)));
}

} // namespace extender
} // namespace ableton
