#pragma once

#include <ableton/link/NodeId.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <tuple>

namespace ableton
{
namespace extender
{

// Window during which a (type, from_node, payload-hash) triple is considered
// a duplicate. Sized to absorb the small skew between gateways receiving the
// same Link broadcast on different interfaces, while staying well below
// Link's republish cadence so legitimate fresh broadcasts are not dropped.
inline constexpr std::chrono::milliseconds kDedupWindow{50};

// Default LRU capacity. Comfortably larger than the expected working set
// (a handful of nodes x a handful of message types).
inline constexpr std::size_t kDedupCacheCapacity = 256;

// Bounded LRU that answers "have we forwarded this (type, from_node, payload)
// in the last kDedupWindow?".  Thread-safe; gateways forward from the IO
// thread but multiple gateways may dispatch concurrently.
class DedupCache
{
  public:
    using NodeId = ableton::link::NodeId;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit DedupCache(std::size_t capacity = kDedupCacheCapacity,
                        std::chrono::milliseconds window = kDedupWindow,
                        Clock clock = [] { return std::chrono::steady_clock::now(); })
        : mCapacity(capacity)
        , mWindow(window)
        , mClock(std::move(clock))
    {
    }

    // Returns true if the (type, from_node, payload) triple was seen within
    // mWindow.  Updates the entry's LRU position regardless; refreshes its
    // timestamp only on a miss or when the previous entry is stale.
    bool isDuplicate(uint8_t type,
                     const NodeId& from_node,
                     const unsigned char* begin,
                     const unsigned char* end)
    {
        const auto hash = fnv1a64(begin, end);
        const Key key{type, from_node, hash};
        const auto now = mClock();

        std::lock_guard<std::mutex> lock(mMutex);

        auto it = mIndex.find(key);
        if (it != mIndex.end())
        {
            auto entryIt = it->second;
            const bool fresh = (now - entryIt->timestamp) < mWindow;
            mEntries.splice(mEntries.begin(), mEntries, entryIt);
            if (fresh)
                return true;
            entryIt->timestamp = now;
            return false;
        }

        if (mEntries.size() >= mCapacity)
        {
            mIndex.erase(mEntries.back().key);
            mEntries.pop_back();
        }
        mEntries.push_front({key, now});
        mIndex[key] = mEntries.begin();
        return false;
    }

  private:
    using Key = std::tuple<uint8_t, NodeId, uint64_t>;

    struct Entry
    {
        Key key;
        std::chrono::steady_clock::time_point timestamp;
    };

    static uint64_t fnv1a64(const unsigned char* begin, const unsigned char* end)
    {
        constexpr uint64_t kOffset = 14695981039346656037ULL;
        constexpr uint64_t kPrime = 1099511628211ULL;
        uint64_t h = kOffset;
        for (auto p = begin; p != end; ++p)
        {
            h ^= static_cast<uint64_t>(*p);
            h *= kPrime;
        }
        return h;
    }

    const std::size_t mCapacity;
    const std::chrono::milliseconds mWindow;
    Clock mClock;
    std::mutex mMutex;
    std::list<Entry> mEntries;
    std::map<Key, typename std::list<Entry>::iterator> mIndex;
};

} // namespace extender
} // namespace ableton
