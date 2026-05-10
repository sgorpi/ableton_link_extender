#pragma once

#include <ableton/link/NodeId.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/link/PeerState.hpp>
#include <ableton/util/Injected.hpp>

#include "ableton/extender/Tunnel.hpp"
// #include <boost/circular_buffer.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace ableton
{
namespace extender
{

constexpr size_t MAX_MESSAGES = 100;
template <typename MessageBuffer>
struct SharedMessages
{
    struct Message
    {
        size_t source;
        MessageBuffer message;
    };
    boost::interprocess::interprocess_mutex mutex;
    // boost::interprocess::interprocess_condition cond_available;
    std::array<Message, MAX_MESSAGES> messages;
    size_t _messages_front;
    size_t _messages_back;

    void push(size_t& source, MessageBuffer& message)
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(
            mutex);
        if (is_full())
            // cond_available.wait(lock); // we're dropping the message if the buffer
            // is full, simulating UDP network reliability
            return;
        messages[_messages_back] = {source, message};
        _messages_back = (_messages_back + 1) % MAX_MESSAGES;
        // cond_available.notify_one();
    }
    bool pop(size_t& source, MessageBuffer& message)
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(
            mutex);
        // if (is_empty())
        //     cond_available.wait(lock);
        if (!is_empty())
        {
            source = messages[_messages_front].source;
            message = messages[_messages_front].message;
            _messages_front = (_messages_front + 1) % MAX_MESSAGES;
            return true;
        }
        return false;
    }
    bool is_empty() const { return _messages_front == _messages_back; }
    bool is_full() const
    {
        return (_messages_back + 1) % MAX_MESSAGES == _messages_front;
    }
    void clear()
    {
        _messages_front = 0;
        _messages_back = 0;
        // cond_available.notify_one();
    }
};
/**
 * The shared memory will support up to 5 clients. Each client has a 'broadcast'
 * buffer and a 'unicast' buffer.
 */
constexpr size_t MAX_CLIENTS = 5;
template <typename MessageBuffer>
struct SharedMemory
{
    size_t num_clients;
    std::array<SharedMessages<MessageBuffer>, MAX_CLIENTS> broadcast;
};

const char* SHM_NAME = "LinkExtender_Shm";
constexpr size_t SHM_SIZE = 1024 * 1024; // 1MB

template <typename IoContext, typename Gateway>
class ShmTunnel : public Tunnel<IoContext, Gateway>
{
  public:
    using MessageBuffer = ableton::discovery::v1::MessageBuffer;
    using NodeId = ableton::link::NodeId;

    enum TunnelClientState
    {
        TUNNEL_CLIENT_STATE_UNKNOWN = 0,
        TUNNEL_CLIENT_STATE_HELLO,
        TUNNEL_CLIENT_STATE_CONNECTED,
    };


    ShmTunnel(ableton::util::Injected<IoContext> io)
        : Tunnel<IoContext, Gateway>(std::move(io))
        , mSharedMemory(nullptr)
        , shm(boost::interprocess::open_or_create,
              SHM_NAME,
              boost::interprocess::read_write)
        , is_running(true)
    {
        shm.truncate(SHM_SIZE);
        boost::interprocess::mapped_region reg(shm, boost::interprocess::read_write);
        region = std::move(reg);

        mSharedMemory =
            reinterpret_cast<SharedMemory<MessageBuffer>*>(region.get_address());
        mClientId = mSharedMemory->num_clients;
        if (mClientId == 0)
        {
            debug(this->mIo->log()) << "Creating new SharedMemory instance";
            SharedMemory<MessageBuffer>* newObj =
                new (mSharedMemory) SharedMemory<MessageBuffer>();
            newObj->num_clients = 1;
        }
        else
        {
            mSharedMemory->num_clients++;
            if (mSharedMemory->num_clients >= MAX_CLIENTS)
            {
                throw std::runtime_error("Maximum number of clients reached");
            }
            say_in_tunnel(TUNNEL_HELLO, "Hello");
        }

        this->mIo->async([this] { listen(); }); // shouldn't this be somewhere else?

        debug(this->mIo->log()) << "SharedMemory initialized with size: " << SHM_SIZE;
        debug(this->mIo->log()) << "Number of clients: " << mSharedMemory->num_clients;
    }

    ~ShmTunnel()
    {
        debug(this->mIo->log()) << "ShmTunnel destructor.";
        if (mSharedMemory != nullptr)
        {
            if (mSharedMemory->num_clients > 1)
            {
                mSharedMemory->num_clients--;
                debug(this->mIo->log())
                    << "ShmTunnel destructor. Number of clients remaining: "
                    << mSharedMemory->num_clients;
            }
            else
            {
                boost::interprocess::shared_memory_object::remove(SHM_NAME);
                debug(this->mIo->log()) << "No clients remaining. SharedMemory removed";
            }
        }
    }


    virtual void forward(TunnelMessageType messageType,
                         std::shared_ptr<Gateway> from_gateway,
                         const NodeId& from_node_id,
                         const unsigned char* messageBegin,
                         const unsigned char* messageEnd,
                         const std::optional<NodeId>& to_node_id) override
    {
        debug(this->mIo->log()) << "-> Tunnel: " << messageType << ", "
                                << std::distance(messageBegin, messageEnd) << " bytes"
                                << " from " << from_node_id;
        if (to_node_id)
            debug(this->mIo->log()) << " to " << *to_node_id;
        // TODO: filter double messages from different gateways

        auto optional_msg = this->encapsulate(
            {from_node_id, messageType, to_node_id, {}}, messageBegin, messageEnd);
        if (!optional_msg)
            return;

        // only for unicast and measurement we need a 'to' node
        if (messageType == UNICAST || messageType == MEASUREMENT_PING)
        {
            if (to_node_id
                && remoteNodeIdToTunnelClientIdx.find(*to_node_id)
                       != remoteNodeIdToTunnelClientIdx.end())
            {
                size_t clientIdx = remoteNodeIdToTunnelClientIdx[*to_node_id];
                mSharedMemory->broadcast[clientIdx].push(mClientId, *optional_msg);
            }
            else
            {
                debug(this->mIo->log())
                    << "NodeID not found in remoteNodeIdToTunnelClientIdx map. Not "
                       "sending...";
            }
        }
        else // broadcast
        {
            for (size_t clientIdx = 0; clientIdx < mSharedMemory->broadcast.size();
                 clientIdx++)
            {
                if (clientIdx == mClientId)
                    continue;
                mSharedMemory->broadcast[clientIdx].push(mClientId, *optional_msg);
            }
        }
    }

    virtual void listen() override
    {
        size_t source{};
        MessageBuffer msg{};
        while (mSharedMemory->broadcast[mClientId].pop(source, msg) && is_running)
        {
            auto optional_msg_and_context = this->decapsulate(msg);
            if (optional_msg_and_context)
            {
                auto context = optional_msg_and_context.value().first;
                auto messageBegin = optional_msg_and_context.value().second.begin();
                auto messageEnd = messageBegin + context.size;
                switch (context.type)
                {
                case TUNNEL_HELLO:
                    // todo: have some ping-pong protocol to check if the other end of the
                    // tunnel is alive
                    if (tunnelClientStates.find(source) == tunnelClientStates.end())
                    {
                        tunnelClientStates[source] = TUNNEL_CLIENT_STATE_HELLO;
                        debug(this->mIo->log()) << "Tunnel client observed: " << source;
                        say_in_tunnel(TUNNEL_HELLO, "ello", source);
                    }
                    else
                    {
                        tunnelClientStates[source] = TUNNEL_CLIENT_STATE_CONNECTED;
                        debug(this->mIo->log()) << "Tunnel client connected: " << source;
                    }
                    break;
                case TUNNEL_BYE:
                    tunnelClientStates.erase(source);
                    debug(this->mIo->log()) << "Tunnel client disconnected: " << source;
                    break;
                case BYEBYE:
                {
                    auto it = remoteNodeIdToTunnelClientIdx.find(context.from_node);
                    if (it != remoteNodeIdToTunnelClientIdx.end())
                        remoteNodeIdToTunnelClientIdx.erase(it);
                    break;
                }
                case BROADCAST:
                case MEASUREMENT_PING:
                case MEASUREMENT_PONG:
                case UNICAST:
                default:
                {
                    auto it = remoteNodeIdToTunnelClientIdx.find(context.from_node);
                    if (it == remoteNodeIdToTunnelClientIdx.end()) // keep first source:
                        remoteNodeIdToTunnelClientIdx[context.from_node] = source;
                    this->forward_locally(context.type,
                                          context.from_node,
                                          messageBegin,
                                          messageEnd,
                                          context.to_node);
                    break;
                }
                }
            }
        }

        // continue asynchrounously
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        if (is_running)
        {
            this->mIo->async([this] { listen(); });
        }
    }

    virtual void stop_listening() override
    {
        is_running = false;
        for (auto& gateway : this->gateways)
        {
            gateway->stop_listening();
        }

        this->gateways.clear();
        mSharedMemory->broadcast[mClientId].clear();
        say_in_tunnel(TUNNEL_BYE, "Bye");
    }

  protected:
    SharedMemory<MessageBuffer>* mSharedMemory;
    boost::interprocess::shared_memory_object shm;
    boost::interprocess::mapped_region region;

    size_t mClientId;
    std::atomic<bool> is_running;
    std::map<NodeId, size_t> remoteNodeIdToTunnelClientIdx;
    std::map<size_t, TunnelClientState> tunnelClientStates;


    void say_in_tunnel(const TunnelMessageType message_type,
                       const std::string msg,
                       std::optional<size_t> to_tunnel_client_idx = std::nullopt)
    {
        // ToDo: expect other Tunnel instance to say 'ello' if they haven't seen me
        // before
        NodeId from_node_id{};

        auto optional_msg = this->encapsulate(
            {from_node_id, message_type, {}, {}}, msg.begin(), msg.end());
        if (!optional_msg)
            return;

        if (to_tunnel_client_idx)
        {
            mSharedMemory->broadcast[*to_tunnel_client_idx].push(
                mClientId, *optional_msg);
        }
        else
        {
            for (size_t clientIdx = 0; clientIdx < mSharedMemory->num_clients;
                 ++clientIdx)
            {
                if (mClientId != clientIdx)
                {
                    mSharedMemory->broadcast[clientIdx].push(mClientId, *optional_msg);
                }
            }
        }
    }
};

template <typename IoContext, typename Gateway>
TunnelPtr<IoContext, Gateway> makeShmTunnel(ableton::util::Injected<IoContext> io)
{
    return std::make_shared<ShmTunnel<IoContext, Gateway>>(injectRef(*io));
}

} // namespace extender
} // namespace ableton
