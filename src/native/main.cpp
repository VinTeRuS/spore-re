// Native host entry point: replaces Windows' process startup for SporeApp.exe. Maps the exe into guest memory at its
// preferred base, gives the main thread its TEB and static TLS, runs the TLS callbacks and then the exe's own entry
// point (the MSVC CRT startup, which calls WinMain). From there the game drives itself: it creates its window,
// runs its message loop (user32.cpp turns SDL events into window messages) and ends through ExitProcess.
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>  // Android: main() becomes SDL_main, called by SDLActivity
#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <jni.h>
#endif

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"
#include "win.h"

namespace native {

const char* g_game_dir = nullptr;
const char* g_data_dir = nullptr;  // Android: the app's external files folder (logs, ini, saves)

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}  // namespace

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "sporere", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

void VulkanDriverAbandon();  // vulkan_driver.cpp
void VulkanDriverCrashed();

namespace {

// Crash report: the fault and the faulting code's library + offset go to the log (and logcat), then the previous
// handler (Android's debuggerd) takes over. Without it a crash on a user's phone just cuts the log off. Formatting
// is not strictly async-signal-safe, which is acceptable for a process that is dying anyway.
struct sigaction g_old_actions[32];

void CrashWrite(const char* text) {
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_ERROR, "sporere", text);
#else
    write(2, text, strlen(text));
    write(2, "\n", 1);
#endif
    if (g_log) {
        int fd = fileno(g_log);
        write(fd, text, strlen(text));
        write(fd, "\n", 1);
        fsync(fd);
    }
}

void DescribeAddress(char* out, size_t size, const char* what, uintptr_t address) {
    Dl_info info{};
    if (address && dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname) {
        const char* lib = strrchr(info.dli_fname, '/');
        snprintf(out, size, "crash: %s %#lx = %s+%#lx (%s)", what, (unsigned long)address, lib ? lib + 1 : info.dli_fname,
                 (unsigned long)(address - reinterpret_cast<uintptr_t>(info.dli_fbase)),
                 info.dli_sname ? info.dli_sname : "?");
    } else {
        snprintf(out, size, "crash: %s %#lx", what, (unsigned long)address);
    }
}

void OnCrash(int sig, siginfo_t* info, void* context) {
    VulkanDriverCrashed();  // a crash before the driver is confirmed: auto mode avoids Turnip next time
    char line[512];
    snprintf(line, sizeof(line), "crash: signal %d (%s) code %d addr %p on tid %d", sig, strsignal(sig), info->si_code,
             info->si_addr, int(gettid()));
    CrashWrite(line);
#if defined(__aarch64__)
    const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    DescribeAddress(line, sizeof(line), "pc", mc.pc);
    CrashWrite(line);
    DescribeAddress(line, sizeof(line), "lr", mc.regs[30]);
    CrashWrite(line);
#elif defined(__x86_64__)
    DescribeAddress(line, sizeof(line), "pc", static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
    CrashWrite(line);
#endif
    // The callers: for abort() the pc and lr above are both in libc, the caller that gave up is further up.
    struct Walk {
        char* line;
        size_t size;
        int depth;
    } walk = {line, sizeof(line), 0};
    _Unwind_Backtrace(
        [](_Unwind_Context* ctx, void* arg) -> _Unwind_Reason_Code {
            auto* w = static_cast<Walk*>(arg);
            uintptr_t pc = _Unwind_GetIP(ctx);
            if (!pc || w->depth >= 24) return _URC_END_OF_STACK;
            char what[16];
            snprintf(what, sizeof(what), "#%02d", w->depth++);
            DescribeAddress(w->line, w->size, what, pc);
            CrashWrite(w->line);
            return _URC_NO_REASON;
        },
        &walk);
    sigaction(sig, &g_old_actions[sig], nullptr);
    if (info->si_code <= 0) raise(sig);  // sent by kill/tgkill/abort: deliver again; a real fault re-runs on return
}

void InstallCrashHandler() {
    struct sigaction sa {};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, &g_old_actions[sig]);
}

}  // namespace

std::string TakeIoStats();   // kernel32.cpp
std::string ArenaUsage();    // memory.cpp

void Fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("FATAL: %s", buf);
    VulkanDriverAbandon();  // not the driver's fault: the next start keeps trying Turnip
    _exit(70);
}

// A start that cannot go on: tell the user and end the process. Returning from main would let SDL run main again in
// the same process, on top of this run's guest memory.
[[noreturn]] void StartupFailure(const char* what) {
    Log("startup failed: %s", what);
    VulkanDriverAbandon();
    std::string msg = std::string(what) + "\n\nDetails are in spore-native.log.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Spore", msg.c_str(), nullptr);
    _exit(1);
}

uint32_t HostWindow();  // user32.cpp
void InitClientSize(int window_w, int window_h);
void StartWatchdog(std::atomic<uint64_t>* frames);  // watchdog.cpp
extern std::atomic<uint64_t> g_frames;  // d3d9.cpp
void InitD3D9Bridge();  // d3d9.cpp
void SelectVulkanDriver();  // vulkan_driver.cpp
void VulkanDriverConfirm();
void InitInput();  // input.cpp
void ForwardInput(const SDL_Event& e);
void InjectKey(SDL_Scancode sc, bool down);
extern SDL_Window* g_sdl_window;

}  // namespace native


// ---- performance counters and frame limiter (read by the on-screen overlay through JNI) ----
namespace native {
std::atomic<float> g_fps{0}, g_frame_ms{0};
std::atomic<int> g_fps_limit{0};  // 0 = no limit (the swap interval still applies)

static long ResidentMiB() {
    long pages = 0, resident = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}
}  // namespace native

#ifdef __ANDROID__
extern "C" JNIEXPORT jfloat JNICALL Java_com_sporere_game_TouchControls_nativeFps(JNIEnv*, jclass) {
    return native::g_fps.load();
}
extern "C" JNIEXPORT jfloat JNICALL Java_com_sporere_game_TouchControls_nativeFrameMs(JNIEnv*, jclass) {
    return native::g_frame_ms.load();
}
extern "C" JNIEXPORT jint JNICALL Java_com_sporere_game_TouchControls_nativeMemoryMiB(JNIEnv*, jclass) {
    return jint(native::ResidentMiB());
}
extern "C" JNIEXPORT void JNICALL Java_com_sporere_game_TouchControls_nativeSetFpsLimit(JNIEnv*, jclass, jint fps) {
    native::g_fps_limit.store(fps);
    native::PerfHintTarget(fps);
}
#endif

// big.LITTLE: keep the engine's main thread on the fast cores (cpu_capacity >= half of the largest). The scheduler
// usually gets there by itself, but a frame-bound thread that waits on the GPU looks idle to it now and then and
// lands on a small core for a while. Setting big_cores (Android default 1).
static void PinToBigCores() {
#ifdef __ANDROID__
    // battery_saver: the scheduler picks the cores (the biggest ones draw several times the power per clock)
    if (!native::SettingInt("big_cores", 1) || native::SettingInt("battery_saver", 0)) return;
    int caps[64] = {}, max_cap = 0, n = 0;
    for (; n < 64; ++n) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", n);
        FILE* f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%d", &caps[n]) != 1) caps[n] = 0;
        fclose(f);
        if (caps[n] > max_cap) max_cap = caps[n];
    }
    if (n == 0 || max_cap == 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    int big = 0;
    for (int i = 0; i < n; ++i) {
        if (caps[i] * 2 >= max_cap) {
            CPU_SET(i, &set);
            ++big;
        }
    }
    if (big == 0 || big == n) return;  // not big.LITTLE
    int r = sched_setaffinity(0, sizeof(set), &set);
    native::Log("main thread pinned to %d of %d cores%s", big, n, r == 0 ? "" : " (failed)");
#endif
}

using namespace native;

#ifdef __ANDROID__
// DXVK and FAudio report errors on stderr, which goes nowhere in an app: forward it to logcat (tag sporere-err).
static void RedirectStderrToLogcat() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], 2);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([] {
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) > 0;) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_WARN, "sporere-err", line.c_str());
                    line.clear();
                } else {
                    line += buf[i];
                }
            }
        }
    }).detach();
}
#endif

// The port's credit. On Android the Java side hands over its own copy (SetupActivity.CREDIT); a build where either
// was removed or changed does not start.
constexpr const char* kCredit = "Spore Android port by VinTeRuS";

int main(int argc, char** argv) {
#ifdef __ANDROID__
    RedirectStderrToLogcat();
    setenv("DXVK_LOG_LEVEL", "info", 0);
    if (const char* credit = getenv("SPORERE_CREDIT"); !credit || strcmp(credit, kCredit) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kCredit,
                                 "This build of the Spore Android port was modified: the author's credit is missing.\n"
                                 "The port is made by VinTeRuS - patreon.com/cw/VinTeRuS",
                                 nullptr);
        return 1;
    }
#endif
    // The game root: the folder with SporebinEP1/, Data/, DataEP1/ (C:\Game for the guest).
    g_game_dir = argc > 1 ? argv[1] : getenv("SPORERE_GAME");
    if (!g_game_dir) {
        fprintf(stderr, "usage: %s <game directory>\n", argv[0]);
        return 2;
    }
    rename("spore-native.log", "spore-native.prev.log");  // the previous run's log survives one restart
    g_log = fopen("spore-native.log", "w");
    InstallCrashHandler();
    Log("%s - build %s %s", kCredit, __DATE__, __TIME__);
    InitGuestMemory();
    GuestThread* main_thread = AttachThread(1u << 20);  // the exe's SizeOfStackReserve
    InitD3D9Bridge();
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);  // DXVK native: the HWNDs we pass are SDL windows
    SelectVulkanDriver();
    if (SettingInt("present_stats", 0)) setenv("DXVK_PRESENT_STATS", "1", 0);
    if (std::string dc = Setting("dxvk_config", ""); !dc.empty()) {
        for (char& ch : dc) ch = ch == ',' ? ';' : ch;
        setenv("DXVK_CONFIG", dc.c_str(), 0);
    }
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) Fatal("SDL_Init: %s", SDL_GetError());
    // The window exists before the game asks for one: CreateWindowExW hands out this one (user32.cpp).
    g_sdl_window = SDL_CreateWindow("Spore - port by VinTeRuS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
#ifdef __ANDROID__
                                        | SDL_WINDOW_FULLSCREEN
#endif
    );
    if (!g_sdl_window) Fatal("SDL_CreateWindow: %s", SDL_GetError());
    int window_w, window_h;
    SDL_GetWindowSize(g_sdl_window, &window_w, &window_h);
    InitClientSize(window_w, window_h);

    std::string exe = std::string(g_game_dir) + "/SporebinEP1/SporeApp.exe";
    if (!LoadImage(exe.c_str(), &g_engine, kEngineBase)) StartupFailure("Cannot load SporebinEP1/SporeApp.exe.");
    g_engine_lo = g_engine.base;
    g_engine_hi = g_engine.base + g_engine.size;
    SetupStaticTls(main_thread);  // the main thread was attached before the exe (and its TLS template) existed

    StartWatchdog(&g_frames);  // counted by Present (d3d9.cpp)
    // TLS callbacks (DLL_PROCESS_ATTACH), as the Windows loader runs them before the entry point.
    for (uint32_t cb = g_static_tls.callbacks; cb && R32(cb); cb += 4) {
        Log("TLS callback %08x", R32(cb));
        GuestCall(R32(cb), {g_engine.base, 1, 0});
    }
    Log("entry point %08x", g_engine.entry);
    uint32_t code = GuestCall(g_engine.entry, {});
    // The CRT startup normally leaves through ExitProcess; returning means it gave up.
    Log("entry point returned %u", code);
    return int(code);
}
