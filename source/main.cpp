#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
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
    ableton::LinkExtender linkExtender;

    State()
        : running(true)
        , linkExtender()
    {
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
    std::cout << "enabled | num peers | quantum | start stop sync | tempo   | "
                 "beats   | metro"
              << std::endl;
}

void printState(State& state)
{
    using namespace std;
    int numPeers = 0;
    float beats = 0.0;

    cout << defaultfloat << left << setw(7) << state.running << " | " << setw(9)
         << numPeers << " | " << fixed << setprecision(2) << setw(7) << beats << " | ";
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
} // namespace

int main(int, char**)
{
    try
    {
        // TODO: add option to bind to specific subnet / interface only (if system
        // has multiple) i.s.o. using all

        /**
         * Ableton Link Extender (ALE)
         *   extend LAN link to WAN peers
         *
         * A 'peer' is a unique ableton link client.
         * - If on the same network (LAN-peer, local), it is identified by (source IP,
         * port)
         * - If on a different network (WAN-peer, remote), it is identified by a port
         * (owned by ALE)
         *
         * Overview:
         *   Listen on interface (create 2 sockets):
         *   1) a multicast-receive socket,
         *   2) a unicast-remote-receive socket.
         * For each received LAN state (unicast/broadcast):
         *   - if from broadcast: re-broadcast at WAN-peer
         *   - if from unicast: re-cast at WAN-peer
         * For each received LAN measurement (unicast)
         *   - if ping: 'from' is a random endpoint, 'to' is the measurement endpoint of a
         * RemoteNodeSurrogate
         *   - if pong: 'from' is a measurement endpoint of a LAN node, 'to' is a created
         * measurement endpoint For each remote received message:
         *   - if it is a state message from a new WAN-peer: Create new
         *     RemoteNodeSurrogate with relevant sockets
         *   - if it is a measurement message from a new WAN-peer: Create new
         *     RemoteMeasurementSurrogate with socket. Create a random NodeId for it.
         */

        State state;
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
