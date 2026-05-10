#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
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
         << " | " << fixed << setprecision(2) << setw(7) << state.tempo << " | " << playing;
    clearLine();
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
            // state.link.enable(!state.link.isEnabled());
            break;
        default:
            break;
        }
    }
}

// Parse "host:port" into a UDP endpoint.  Only dotted-decimal IPv4 addresses
// are accepted for now; hostname resolution can be added later.
LINK_ASIO_NAMESPACE::ip::udp::endpoint parseEndpoint(const std::string& s)
{
    const auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == s.size())
        throw std::invalid_argument("peer must be in host:port form, got: " + s);

    const std::string host = s.substr(0, colon);
    const uint16_t port = static_cast<uint16_t>(std::stoul(s.substr(colon + 1)));

    LINK_ASIO_NAMESPACE::error_code ec;
    const auto addr = LINK_ASIO_NAMESPACE::ip::make_address(host, ec);
    if (ec)
        throw std::invalid_argument("cannot parse IP address '" + host + "': " + ec.message()
                                    + "  (hostname resolution not yet supported)");

    return {addr, port};
}

void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog << " [--port N] [--peer host:port] ...\n"
              << "\n"
              << "  --port N           Local UDP port to listen on (default: 12345)\n"
              << "  --peer host:port   Remote ALE instance to connect to (repeatable)\n"
              << "\n"
              << "  When no --peer is given the shared-memory tunnel is used instead\n"
              << "  (useful for testing with network namespaces on the same host).\n"
              << "\n"
              << "  Keys while running:\n"
              << "    q   quit\n";
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

        if (config.peers.empty())
        {
            std::cout << "No --peer arguments given; using shared-memory tunnel.\n";
        }
        else
        {
            std::cout << "UDP tunnel on port " << config.tunnel_port << " with "
                      << config.peers.size() << " peer(s):\n";
            for (const auto& ep : config.peers)
                std::cout << "  " << ep << "\n";
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
