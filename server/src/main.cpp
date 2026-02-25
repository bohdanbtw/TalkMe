#include "Logger.h"
#include "TalkMeServer.h"
#include <asio.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <csignal>

// Re-register the signal handler after each delivery so that a second Ctrl+C
// (e.g. when the server is slow to drain) is still caught gracefully rather
// than reverting to the OS default (immediate termination mid-cleanup).
static void ArmSignals(asio::signal_set& signals, asio::io_context& io_context) {
    signals.async_wait([&signals, &io_context](const std::error_code& ec, int signo) {
        if (ec) return;
        TalkMe::VoiceTrace::log("step=server_shutdown status=graceful signal="
            + std::to_string(signo));
        io_context.stop();
        // Re-arm so a second signal is also handled cleanly rather than killing
        // the process via the default OS handler mid-destructor.
        ArmSignals(signals, io_context);
        });
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    try {
        TalkMe::VoiceTrace::init();
        asio::io_context io_context;
        TalkMe::TalkMeServer server(io_context, 5555);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        ArmSignals(signals, io_context);

        // Cap the thread pool: the voice relay server is I/O-bound, not CPU-bound.
        // Beyond ~16 threads the added context-switch cost and m_RoomMutex
        // contention outweigh any throughput benefit. hardware_concurrency() can
        // return 128+ on high-core-count machines, which would spawn far more
        // threads than useful.
        constexpr unsigned int kMaxThreads = 16u;
        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        thread_count = std::min(thread_count, kMaxThreads);

        std::cout << "TalkMe Server running on 5555 with "
            << thread_count << " threads...\n";

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (unsigned int i = 0; i < thread_count; ++i)
            threads.emplace_back([&io_context] { io_context.run(); });
        for (auto& t : threads) t.join();
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
    }
    return 0;
}