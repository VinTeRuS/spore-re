// Sampling CPU profiler for finding what is slow on a phone (setting cpu_profile = 1). ITIMER_PROF fires SIGPROF
// every millisecond of process CPU time on whichever thread is running; the handler records the interrupted program
// counter in a lock-free ring. Every 30 s a reporter thread logs the 200 most frequent addresses as offsets into
// libmain.so (64-byte buckets), with their share of the samples. The APK's library is stripped: the offsets are
// turned into function names on the desktop with the unstripped build (llvm-addr2line), and summed per function.
#include <dlfcn.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

#include "guest.h"

namespace native {

namespace {

constexpr uint32_t kRing = 1u << 16;
std::atomic<uint32_t> g_head{0};
uintptr_t g_ring[kRing];
uintptr_t g_caller[kRing];  // return address at the sample (LR on arm64, [rsp] on x86-64): who called a leaf like memcpy
uintptr_t g_base = 0;

void OnProf(int, siginfo_t*, void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    uintptr_t pc = uc->uc_mcontext.pc, caller = uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t caller = *reinterpret_cast<const uintptr_t*>(uc->uc_mcontext.gregs[REG_RSP]);
#else
    uintptr_t pc = 0, caller = 0;
#endif
    uint32_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    g_ring[i & (kRing - 1)] = pc;
    g_caller[i & (kRing - 1)] = caller;
}

void Report() {
    uint32_t seen = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        uint32_t head = g_head.load(std::memory_order_relaxed);
        uint32_t n = std::min(head - seen, kRing);
        std::unordered_map<uintptr_t, uint32_t> hist;
        std::unordered_map<std::string, uint32_t> libs;  // other modules (DXVK, the GPU driver, libc), by name
        std::unordered_map<uintptr_t, uint32_t> callers;  // libmain code that called into them (64-byte buckets)
        uint32_t outside = 0;
        for (uint32_t k = head - n; k != head; ++k) {
            uintptr_t pc = g_ring[k & (kRing - 1)];
            Dl_info info;
            if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fbase &&
                reinterpret_cast<uintptr_t>(info.dli_fbase) != g_base) {
                const char* name = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
                std::string key = name ? name + 1 : (info.dli_fname ? info.dli_fname : "?");
                if (info.dli_sname) key += std::string("!") + info.dli_sname;  // e.g. libc.so!memcpy
                ++libs[key];
                ++outside;
                uintptr_t from = g_caller[k & (kRing - 1)];
                Dl_info ci;
                if (dladdr(reinterpret_cast<void*>(from), &ci) && reinterpret_cast<uintptr_t>(ci.dli_fbase) == g_base)
                    ++callers[(from - g_base) & ~uintptr_t(63)];
                continue;
            }
            if (pc < g_base) {
                ++libs["(unknown)"];
                ++outside;
                continue;
            }
            ++hist[(pc - g_base) & ~uintptr_t(63)];
        }
        seen = head;
        std::vector<std::pair<uint32_t, std::string>> by_lib;
        for (auto& [name, count] : libs) by_lib.push_back({count, name});
        std::sort(by_lib.rbegin(), by_lib.rend());
        std::string libline;
        if (by_lib.size() > 40) by_lib.resize(40);
        for (auto& [count, name] : by_lib) {
            char item[96];
            snprintf(item, sizeof item, " %s:%u", name.c_str(), count);
            libline += item;
        }
        Log("profile: other modules:%s", libline.c_str());
        std::vector<std::pair<uint32_t, uintptr_t>> top_callers;
        for (auto& [off, count] : callers) top_callers.push_back({count, off});
        std::sort(top_callers.rbegin(), top_callers.rend());
        if (top_callers.size() > 40) top_callers.resize(40);
        std::string callline;
        for (auto& [count, off] : top_callers) {
            char item[48];
            snprintf(item, sizeof item, " %lx:%u", (unsigned long)off, count);
            callline += item;
        }
        Log("profile: libmain callers of other modules:%s", callline.c_str());
        std::vector<std::pair<uint32_t, uintptr_t>> top;
        for (auto& [off, count] : hist) top.push_back({count, off});
        std::sort(top.rbegin(), top.rend());
        if (top.size() > 200) top.resize(200);
        Log("profile: %u samples (%u in other modules), libmain top buckets:", n, outside);
        std::string line;
        for (auto& [count, off] : top) {
            char item[48];
            snprintf(item, sizeof item, " %lx:%u", (unsigned long)off, count);
            line += item;
            if (line.size() > 900) {
                Log("profile:%s", line.c_str());
                line.clear();
            }
        }
        if (!line.empty()) Log("profile:%s", line.c_str());
    }
}

}  // namespace

void StartProfiler() {
    if (!SettingInt("cpu_profile", 0)) return;
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&StartProfiler), &info) && info.dli_fbase)
        g_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    struct sigaction sa {};
    sa.sa_sigaction = OnProf;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    itimerval t {};
    t.it_interval.tv_usec = 1000;
    t.it_value.tv_usec = 1000;
    setitimer(ITIMER_PROF, &t, nullptr);
    std::thread(Report).detach();
    Log("profiler: sampling every 1 ms of CPU time, module base %lx", (unsigned long)g_base);
}

}  // namespace native
