#include "call_counter.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace CallCounter
{
    void Report(uint64_t frames)
    {
        if (frames == 0)
            return;
        std::vector<std::pair<uint32_t, uint32_t>> hits; // calls, address
        for (uint32_t i = 0; i < g_callCounterCount; ++i)
        {
            const uint32_t n = g_callCounters[i].exchange(0, std::memory_order_relaxed);
            if (n)
                hits.emplace_back(n, g_callCounterAddresses[i]);
        }
        std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::printf("---- calls per frame over %llu frames (%zu functions hit) ----\n", (unsigned long long)frames, hits.size());
        for (size_t i = 0; i < hits.size() && i < 60; ++i)
            std::printf("  sub_%08X  %9.2f\n", hits[i].second, double(hits[i].first) / double(frames));
        // Every function hit in the game's graphics wrapper + Direct3D (the renderer's API surface).
        std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.second < b.second; });
        std::printf("---- graphics range 0x822A0000-0x82304000 (by address) ----\n");
        for (const auto& [n, addr] : hits)
            if (addr >= 0x822A0000 && addr < 0x82304000)
                std::printf("  sub_%08X  %9.2f\n", addr, double(n) / double(frames));
        std::printf("----\n");
        std::fflush(stdout);
    }
}

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace CallCounter
{
    // PES_CALL_WATCH=82279888,823C3058,... : logs the first call of each listed function and,
    // every 30 s, the running totals of those that were hit (counters run in every build).
    void StartWatch()
    {
        const char* env = std::getenv("PES_CALL_WATCH");
        if (!env || !*env)
            return;
        std::vector<std::pair<uint32_t, uint32_t>> watch; // address, counter index
        std::string s = env;
        for (size_t pos = 0; pos < s.size();)
        {
            size_t end = s.find(',', pos);
            if (end == std::string::npos) end = s.size();
            const uint32_t addr = uint32_t(std::strtoul(s.substr(pos, end - pos).c_str(), nullptr, 16));
            for (uint32_t i = 0; i < g_callCounterCount; ++i)
                if (g_callCounterAddresses[i] == addr) { watch.emplace_back(addr, i); break; }
            pos = end + 1;
        }
        std::printf("[watch] %zu functions watched\n", watch.size());
        std::thread([watch]
        {
            std::vector<uint64_t> totals(watch.size(), 0);
            for (uint32_t tick = 1;; ++tick)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                for (size_t k = 0; k < watch.size(); ++k)
                {
                    const uint32_t n = g_callCounters[watch[k].second].load(std::memory_order_relaxed);
                    if (n && totals[k] == 0)
                        std::printf("[watch] sub_%08X first called (t=%us)\n", watch[k].first, tick);
                    totals[k] = n;
                }
                if (tick % 30 == 0)
                {
                    std::string line = "[watch] totals:";
                    for (size_t k = 0; k < watch.size(); ++k)
                        if (totals[k]) { char b[40]; std::snprintf(b, sizeof(b), " %08X=%llu", watch[k].first, (unsigned long long)totals[k]); line += b; }
                    std::printf("%s\n", line.c_str());
                }
                std::fflush(stdout);
            }
        }).detach();
    }
}
