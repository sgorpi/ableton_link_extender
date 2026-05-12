#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(LINK_PLATFORM_UNIX)
#include <termios.h>
#endif

#include "LinkExtender.hpp"

namespace
{

struct State
{
    std::atomic<bool> running;
    std::atomic<std::size_t> localPeers;
    std::atomic<std::size_t> remotePeers;
    std::atomic<double> tempo;
    std::atomic<bool> isPlaying;
    ableton::LinkExtender linkExtender;

    struct PendingApproval
    {
        uint64_t id;
        LINK_ASIO_NAMESPACE::ip::udp::endpoint ep;
        std::function<void()> accept;
        std::function<void()> reject;
    };

    struct SharedPending
    {
        std::mutex mutex;
        std::queue<PendingApproval> approvals;
        uint64_t nextId{0};
        bool alive{true};
    };
    std::shared_ptr<SharedPending> pending{std::make_shared<SharedPending>()};

    explicit State(ableton::LinkExtender::Config config)
        : running(true)
        , localPeers(0)
        , remotePeers(0)
        , tempo(0.0)
        , isPlaying(false)
        , linkExtender(std::move(config))
    {
        linkExtender.setNumPeersCallback(
            [this](std::size_t local, std::size_t remote)
            {
                localPeers = local;
                remotePeers = remote;
            });
        linkExtender.setTempoCallback([this](double bpm) { tempo = bpm; });
        linkExtender.setStartStopCallback([this](bool playing) { isPlaying = playing; });

        auto sp = pending;
        auto printNextPrompt = [sp]()
        {
            // caller must hold sp->mutex
            std::cout << "\nUnknown peer: " << sp->approvals.front().ep
                      << " — [a]ccept [d]eny (auto-reject in 60s)\n"
                      << std::flush;
        };

        linkExtender.setUnknownPeerCallback(
            [sp, printNextPrompt](LINK_ASIO_NAMESPACE::ip::udp::endpoint ep,
                                  std::function<void()> accept,
                                  std::function<void()> reject)
            {
                uint64_t id;
                bool wasEmpty;
                {
                    std::lock_guard<std::mutex> lock(sp->mutex);
                    id = sp->nextId++;
                    wasEmpty = sp->approvals.empty();
                    sp->approvals.push({id, ep, std::move(accept), std::move(reject)});
                    if (wasEmpty)
                        printNextPrompt();
                }

                std::thread(
                    [sp, id, ep, printNextPrompt]()
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(60));

                        std::optional<PendingApproval> entry;
                        {
                            std::lock_guard<std::mutex> lock(sp->mutex);
                            if (!sp->alive)
                                return;
                            if (!sp->approvals.empty() && sp->approvals.front().id == id)
                            {
                                entry = sp->approvals.front();
                                sp->approvals.pop();
                            }
                        }
                        if (!entry)
                            return;

                        entry->reject();
                        std::cout << "\nAuto-rejected " << ep << "\n" << std::flush;
                        {
                            std::lock_guard<std::mutex> lock(sp->mutex);
                            if (!sp->approvals.empty() && sp->alive)
                                printNextPrompt();
                        }
                    })
                    .detach();
            });
    }

    ~State()
    {
        // Set alive=false and drain under the mutex. Timer threads capture 'pending'
        // by shared_ptr, so the allocation outlives State — they will find
        // alive==false and become no-ops. reject() is called at most once per
        // entry: drain and timer both hold the mutex while popping.
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->alive = false;
        while (!pending->approvals.empty())
        {
            pending->approvals.front().reject();
            pending->approvals.pop();
        }
    }
};

void print_exception(const std::exception& e, int level = 0)
{
    std::cerr << std::string(level, ' ') << "exception: " << e.what() << std::endl;
    try
    {
        std::rethrow_if_nested(e);
    }
    catch (const std::exception& nestedException)
    {
        print_exception(nestedException, level + 1);
    }
    catch (...)
    {
    }
}

void disableBufferedInput()
{
#if defined(LINK_PLATFORM_UNIX)
    termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= static_cast<unsigned long>(~ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
}

void enableBufferedInput()
{
#if defined(LINK_PLATFORM_UNIX)
    termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= ICANON;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
}

void clearLine()
{
    std::cout << "   \r" << std::flush;
    std::cout.fill(' ');
}

void printStateHeader()
{
    std::cout << "local peers | remote peers | tempo   | playing" << std::endl;
}

void printState(const State& state)
{
    using namespace std;
    const auto playing = state.isPlaying ? "[playing]" : "[stopped]";
    cout << left << setw(11) << state.localPeers << " | " << setw(12) << state.remotePeers
         << " | " << fixed << setprecision(2) << setw(7) << state.tempo << " | "
         << playing;
    clearLine();
}

void resolvePendingApproval(State& state, bool accept)
{
    std::optional<State::PendingApproval> entry;
    bool hasNext = false;
    {
        std::lock_guard<std::mutex> lock(state.pending->mutex);
        if (!state.pending->approvals.empty())
        {
            entry = state.pending->approvals.front();
            state.pending->approvals.pop();
            hasNext = !state.pending->approvals.empty();
        }
    }
    if (!entry)
        return;

    if (accept)
    {
        std::cout << "Accepting " << entry->ep << std::endl;
        entry->accept();
    }
    else
    {
        std::cout << "Rejecting " << entry->ep << std::endl;
        entry->reject();
    }

    if (hasNext)
    {
        std::lock_guard<std::mutex> lock(state.pending->mutex);
        if (!state.pending->approvals.empty())
            std::cout << "\nUnknown peer: " << state.pending->approvals.front().ep
                      << " — [a]ccept [d]eny (auto-reject in 60s)\n"
                      << std::flush;
    }
}

void input(State& state)
{
    char in;

    for (;;)
    {
#if defined(LINK_PLATFORM_WINDOWS)
        HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
        DWORD numCharsRead;
        INPUT_RECORD inputRecord;
        do
        {
            ReadConsoleInput(stdinHandle, &inputRecord, 1, &numCharsRead);
        } while ((inputRecord.EventType != KEY_EVENT)
                 || inputRecord.Event.KeyEvent.bKeyDown);
        in = inputRecord.Event.KeyEvent.uChar.AsciiChar;
#elif defined(LINK_PLATFORM_UNIX)
        in = static_cast<char>(std::cin.get());
#endif

        switch (in)
        {
        case 'q':
            state.running = false;
            clearLine();
            return;
        case 'a':
            resolvePendingApproval(state, true);
            break;
        case 'd':
            resolvePendingApproval(state, false);
            break;
        default:
            break;
        }
    }
}

// Parse "host:port" or "[ipv6]:port" into a UDP endpoint.
// Accepts numeric IPv4/IPv6 addresses and hostnames (resolved via DNS).
LINK_ASIO_NAMESPACE::ip::udp::endpoint parseEndpoint(const std::string& s)
{
    std::string host;
    std::string portStr;

    if (!s.empty() && s.front() == '[')
    {
        // IPv6 bracketed form: [addr]:port
        const auto close = s.find(']');
        if (close == std::string::npos)
            throw std::invalid_argument("peer has unmatched '[', got: " + s);
        if (close + 1 >= s.size() || s[close + 1] != ':')
            throw std::invalid_argument("peer must be in [ipv6]:port form, got: " + s);
        host = s.substr(1, close - 1);
        portStr = s.substr(close + 2);
    }
    else
    {
        // IPv4 form: addr:port
        const auto colon = s.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == s.size())
            throw std::invalid_argument("peer must be in host:port form, got: " + s);
        host = s.substr(0, colon);
        portStr = s.substr(colon + 1);
    }

    if (portStr.empty())
        throw std::invalid_argument("peer is missing port, got: " + s);

    // Try numeric address first (no DNS round-trip needed for the common case).
    LINK_ASIO_NAMESPACE::error_code ec;
    const auto addr = LINK_ASIO_NAMESPACE::ip::make_address(host, ec);
    if (!ec)
        return {addr, static_cast<uint16_t>(std::stoul(portStr))};

    // Fall back to hostname resolution.
    LINK_ASIO_NAMESPACE::io_context ioc;
    LINK_ASIO_NAMESPACE::ip::udp::resolver resolver{ioc};
    const auto results = resolver.resolve(host, portStr, ec);
    if (ec)
        throw std::invalid_argument("cannot resolve '" + host + "': " + ec.message());
    if (results.empty())
        throw std::invalid_argument("no addresses found for '" + host + "'");

    return results.begin()->endpoint();
}

void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog << " [--port N] [--peer host:port] ...\n"
              << "\n"
              << "  --port N           Local UDP port to listen on (default: 20909)\n"
              << "  --peer host:port   Remote ALE instance to connect to (repeatable)\n"
              << "                     host may be a hostname, IPv4, or [IPv6] address\n"
              << "  --shm              Use shared-memory tunnel instead of UDP\n"
              << "                     (useful for testing with network namespaces on "
                 "the same host)\n"
              << "\n"
              << "  Keys while running:\n"
              << "    q   quit\n"
              << "    a   accept pending unknown peer\n"
              << "    d   deny pending unknown peer\n";
}

ableton::LinkExtender::Config parseArgs(int argc, char** argv)
{
    ableton::LinkExtender::Config config;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            printUsage(argv[0]);
            std::exit(0);
        }
        else if (arg == "--port")
        {
            if (++i >= argc)
                throw std::invalid_argument("--port requires a value");
            config.tunnel_port = static_cast<uint16_t>(std::stoul(argv[i]));
        }
        else if (arg == "--peer")
        {
            if (++i >= argc)
                throw std::invalid_argument("--peer requires a value");
            config.peers.push_back(parseEndpoint(argv[i]));
        }
        else if (arg == "--shm")
        {
            config.use_shm = true;
        }
        else
        {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    return config;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        auto config = parseArgs(argc, argv);

        if (config.use_shm)
        {
            std::cout << "Using shared-memory tunnel.\n";
        }
        else
        {
            std::cout << "UDP tunnel on port " << config.tunnel_port;
            if (config.peers.empty())
            {
                std::cout << " (no configured peers — awaiting incoming connections)\n";
            }
            else
            {
                std::cout << " with " << config.peers.size() << " peer(s):\n";
                for (const auto& ep : config.peers)
                    std::cout << "  " << ep << "\n";
            }
        }

        State state{std::move(config)};
        std::thread thread(input, std::ref(state));
        disableBufferedInput();

        printStateHeader();
        while (state.running)
        {
            printState(state);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cout << "Stopped running..." << std::endl;
        enableBufferedInput();
        thread.join();
    }
    catch (const std::invalid_argument& e)
    {
        std::cerr << "Error: " << e.what() << "\n\n";
        printUsage(argv[0]);
        return 1;
    }
    catch (std::exception& e)
    {
        print_exception(e);
        return 1;
    }
    catch (...)
    {
        std::cerr << "An unhandleable exception occurred" << std::endl;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::cout << "Done" << std::endl;
    return 0;
}
