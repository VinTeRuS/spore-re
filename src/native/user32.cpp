// USER32 for the native host. The game's main window is the SDL window; other handles are fake values.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "win.h"

namespace native {

std::string GuestFormat(const char* fmt, uint32_t args);  // msvcrt.cpp: printf with guest varargs
void ForwardInput(const SDL_Event& e);  // input.cpp
void InitInput();
bool VkDown(uint32_t vk);
extern SDL_Window* g_sdl_window;

namespace {

constexpr uint32_t kMainWindow = 0x00010010u;
constexpr uint32_t kDesktopWindow = 0x00010004u;
uint32_t g_next_window = 0x00010020u;
int g_width = 1280, g_height = 720;  // client size = render resolution (InitClientSize)

std::mutex g_mutex;
std::map<std::pair<uint32_t, int32_t>, uint32_t> g_window_longs;
std::map<std::string, uint32_t> g_classes;  // class name -> WNDPROC
std::map<uint32_t, uint32_t> g_window_proc;

#define U(name) void u_##name(Ctx* c)

U(GetDesktopWindow) { Return(c, kDesktopWindow); }
U(GetForegroundWindow) { Return(c, kMainWindow); }
U(GetClientRect) {
    uint32_t r = Arg(c, 1);
    W32(r, 0);
    W32(r + 4, 0);
    W32(r + 8, uint32_t(g_width));
    W32(r + 12, uint32_t(g_height));
    Return(c, 1, 2);
}
U(GetWindowRect) {
    uint32_t r = Arg(c, 1);
    W32(r, 0);
    W32(r + 4, 0);
    W32(r + 8, uint32_t(g_width));
    W32(r + 12, uint32_t(g_height));
    Return(c, 1, 2);
}
U(GetWindowLongA) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int32_t index = int32_t(Arg(c, 1));
    uint32_t v = g_window_longs[{Arg(c, 0), index}];
    if (index == -4 && !v) v = g_window_proc[Arg(c, 0)];  // GWL_WNDPROC
    Return(c, v, 2);
}
U(SetWindowLongA) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int32_t index = int32_t(Arg(c, 1));
    auto& slot = g_window_longs[{Arg(c, 0), index}];
    uint32_t old = slot;
    if (index == -4) {
        old = g_window_proc[Arg(c, 0)];
        g_window_proc[Arg(c, 0)] = Arg(c, 2);
    }
    slot = Arg(c, 2);
    Return(c, old, 3);
}
U(MapVirtualKeyExA) {
    // MAPVK_VK_TO_VSC for the common keys is enough for the key binding names.
    Return(c, Arg(c, 0) & 0xff, 3);
}
U(GetKeyboardLayout) { Return(c, 0x04090409u, 1); }
U(RegisterClassExA) {
    uint32_t wc = Arg(c, 0);
    uint32_t name = R32(wc + 40);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string n = name < 0x10000 ? "#" + std::to_string(name) : std::string(Str(name));
    g_classes[n] = R32(wc + 8);
    Return(c, 0xc000u + uint32_t(g_classes.size()), 1);
}
U(UnregisterClassA) { Return(c, 1, 2); }
U(CreateWindowExA) {
    uint32_t cls = Arg(c, 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string n = cls < 0x10000 ? "#" + std::to_string(cls) : std::string(Str(cls));
    uint32_t h = g_next_window;
    g_next_window += 4;
    g_window_proc[h] = g_classes[n];
    Log("CreateWindowExA(%s) -> %08x", n.c_str(), h);
    Return(c, h, 12);
}
U(DestroyWindow) { Return(c, 1, 1); }
U(wvsprintfA) {
    std::string s = GuestFormat(Str(Arg(c, 1)), Arg(c, 2));
    strcpy(Str(Arg(c, 0)), s.c_str());
    Return(c, uint32_t(s.size()), 3);
}
U(wsprintfA) {
    // cdecl variadic
    std::string s = GuestFormat(Str(Arg(c, 1)), c->esp + 12);
    strcpy(Str(Arg(c, 0)), s.c_str());
    Return(c, uint32_t(s.size()));
}
U(MessageBoxA) {
    Log("MessageBox [%s]: %s", Arg(c, 2) ? Str(Arg(c, 2)) : "", Arg(c, 1) ? Str(Arg(c, 1)) : "");
    Return(c, 1, 4);  // IDOK
}
U(CharLowerBuffA) {
    char* s = Str(Arg(c, 0));
    for (uint32_t i = 0; i < Arg(c, 1); ++i) s[i] = char(tolower((unsigned char)s[i]));
    Return(c, Arg(c, 1), 2);
}
U(CallWindowProcA) { Return(c, GuestCall(Arg(c, 0), {Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4)}), 5); }
U(DefWindowProcA) { Return(c, 0, 4); }
U(ShowCursor) { Return(c, Arg(c, 0) ? 0 : 0xffffffffu, 1); }
U(GetSystemMetrics) {
    int32_t i = int32_t(Arg(c, 0));
    uint32_t v = i == 0 ? uint32_t(g_width) : i == 1 ? uint32_t(g_height) : 0;  // SM_CXSCREEN / SM_CYSCREEN
    Return(c, v, 1);
}

// ---- windows and the message queue (SporeApp.exe: its own window, PeekMessage loop) ----
// The first top-level window the game creates is the SDL window (HostWindow); its window procedure gets the input
// that input.cpp posts, the timers and the quit/activation messages.
constexpr uint32_t kWmCreate = 0x0001, kWmDestroy = 0x0002, kWmSize = 0x0005, kWmActivate = 0x0006,
                   kWmSetFocus = 0x0007, kWmKillFocus = 0x0008, kWmClose = 0x0010, kWmQuit = 0x0012,
                   kWmShowWindow = 0x0018, kWmActivateApp = 0x001c, kWmNcCreate = 0x0081, kWmTimer = 0x0113;

struct Msg {
    uint32_t hwnd, message, wparam, lparam;
};
std::deque<Msg> g_queue;  // under g_mutex
bool g_main_created = false;
struct Timer {
    uint32_t hwnd, id, period_ms, callback;
    std::chrono::steady_clock::time_point due;
};
std::vector<Timer> g_timers;  // under g_mutex
std::map<uint32_t, uint32_t> g_window_parent;

std::string WideToUtf8(uint32_t a) {
    std::string out;
    if (!a) return out;
    if (a < 0x10000) return "#" + std::to_string(a);  // atom
    for (uint32_t p = a;; p += 2) {
        uint32_t ch = R16(p);
        if (!ch) break;
        if (ch >= 0xd800 && ch < 0xdc00) {
            uint32_t lo = R16(p + 2);
            if (lo >= 0xdc00 && lo < 0xe000) {
                ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00);
                p += 2;
            }
        }
        if (ch < 0x80) {
            out += char(ch);
        } else if (ch < 0x800) {
            out += char(0xc0 | ch >> 6);
            out += char(0x80 | (ch & 0x3f));
        } else if (ch < 0x10000) {
            out += char(0xe0 | ch >> 12);
            out += char(0x80 | ((ch >> 6) & 0x3f));
            out += char(0x80 | (ch & 0x3f));
        } else {
            out += char(0xf0 | ch >> 18);
            out += char(0x80 | ((ch >> 12) & 0x3f));
            out += char(0x80 | ((ch >> 6) & 0x3f));
            out += char(0x80 | (ch & 0x3f));
        }
    }
    return out;
}

uint32_t WindowProc(uint32_t hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_window_proc.find(hwnd);
    return it == g_window_proc.end() ? 0 : it->second;
}

// Sends a message to a window procedure on the calling thread (SendMessage semantics).
uint32_t Send(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    uint32_t proc = WindowProc(hwnd);
    return proc ? GuestCall(proc, {hwnd, msg, wparam, lparam}) : 0;
}

void Post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.push_back({hwnd, msg, wparam, lparam});
}

// SDL events become posted messages; called from PeekMessage on the game's thread.
void PumpSdl() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || e.type == SDL_APP_TERMINATING) {
            Post(kMainWindow, kWmClose, 0, 0);
        } else if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                Post(kMainWindow, kWmActivateApp, 1, 0);
                Post(kMainWindow, kWmActivate, 1, 0);
                Post(kMainWindow, kWmSetFocus, 0, 0);
            } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                Post(kMainWindow, kWmActivate, 0, 0);
                Post(kMainWindow, kWmActivateApp, 0, 0);
                Post(kMainWindow, kWmKillFocus, 0, 0);
            }
        } else {
            ForwardInput(e);
        }
    }
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Timer& t : g_timers) {
        if (now >= t.due) {
            g_queue.push_back({t.hwnd, kWmTimer, t.id, t.callback});
            t.due = now + std::chrono::milliseconds(t.period_ms ? t.period_ms : 1);
        }
    }
}

uint32_t CreateWindowCommon(uint32_t exstyle, const std::string& cls, uint32_t name, uint32_t style, int32_t x,
                            int32_t y, int32_t w, int32_t h, uint32_t parent, uint32_t menu, uint32_t inst,
                            uint32_t param) {
    uint32_t hwnd;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        bool child = (style & 0x40000000u) != 0;  // WS_CHILD
        if (!child && !g_main_created) {
            hwnd = kMainWindow;
            g_main_created = true;
        } else {
            hwnd = g_next_window;
            g_next_window += 4;
        }
        g_window_proc[hwnd] = g_classes[cls];
        g_window_parent[hwnd] = parent;
    }
    Log("CreateWindowEx(%s, style %08x) -> %08x", cls.c_str(), style, hwnd);
    // CREATESTRUCT for WM_NCCREATE/WM_CREATE: games keep `this` from lpCreateParams.
    uint32_t cs = HeapAlloc(48);
    W32(cs + 0, param);
    W32(cs + 4, inst);
    W32(cs + 8, menu);
    W32(cs + 12, parent);
    W32(cs + 16, uint32_t(h));
    W32(cs + 20, uint32_t(w));
    W32(cs + 24, uint32_t(y));
    W32(cs + 28, uint32_t(x));
    W32(cs + 32, style);
    W32(cs + 36, name);
    W32(cs + 40, 0);
    W32(cs + 44, exstyle);
    if (!Send(hwnd, kWmNcCreate, 0, cs) && WindowProc(hwnd)) {
        Log("CreateWindowEx: WM_NCCREATE refused");
    }
    Send(hwnd, kWmCreate, 0, cs);
    HeapFree(cs);
    if (hwnd == kMainWindow) {
        Send(hwnd, kWmSize, 0, uint32_t(g_height) << 16 | uint32_t(g_width));
        InitInput();
    }
    return hwnd;
}

U(RegisterClassExW) {
    uint32_t wc = Arg(c, 0);
    std::string n = WideToUtf8(R32(wc + 40));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_classes[n] = R32(wc + 8);
    Return(c, 0xc000u + uint32_t(g_classes.size()), 1);
}
U(RegisterClassA) {
    uint32_t wc = Arg(c, 0);  // WNDCLASSA: style, lpfnWndProc, ..., lpszClassName at 36
    uint32_t name = R32(wc + 36);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string n = name < 0x10000 ? "#" + std::to_string(name) : std::string(Str(name));
    g_classes[n] = R32(wc + 4);
    Return(c, 0xc000u + uint32_t(g_classes.size()), 1);
}
U(CreateWindowExW) {
    uint32_t r = CreateWindowCommon(Arg(c, 0), WideToUtf8(Arg(c, 1)), Arg(c, 2), Arg(c, 3), int32_t(Arg(c, 4)),
                                    int32_t(Arg(c, 5)), int32_t(Arg(c, 6)), int32_t(Arg(c, 7)), Arg(c, 8), Arg(c, 9),
                                    Arg(c, 10), Arg(c, 11));
    Return(c, r, 12);
}
// PeekMessage(MSG*, hwnd, min, max, remove): MSG = hwnd, message, wParam, lParam, time, pt.
void PeekCommon(Ctx* c) {
    PumpSdl();
    uint32_t out = Arg(c, 0), filter_lo = Arg(c, 2), filter_hi = Arg(c, 3);
    bool remove = (Arg(c, 4) & 1) != 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if ((filter_lo || filter_hi) && (it->message < filter_lo || it->message > filter_hi)) continue;
        W32(out, it->hwnd);
        W32(out + 4, it->message);
        W32(out + 8, it->wparam);
        W32(out + 12, it->lparam);
        W32(out + 16, uint32_t(SDL_GetTicks()));
        W32(out + 20, 0);
        W32(out + 24, 0);
        if (remove) g_queue.erase(it);
        Return(c, 1, 5);
        return;
    }
    Return(c, 0, 5);
}
U(PeekMessageW) { PeekCommon(c); }
U(PeekMessageA) { PeekCommon(c); }
U(TranslateMessage) { Return(c, 0, 1); }  // WM_CHAR comes from SDL text input (input.cpp)
U(DispatchMessageW) {
    uint32_t m = Arg(c, 0);
    uint32_t hwnd = R32(m), msg = R32(m + 4), wparam = R32(m + 8), lparam = R32(m + 12);
    if (msg == kWmTimer && lparam) {
        GuestCall(lparam, {hwnd, msg, wparam, uint32_t(SDL_GetTicks())});  // TIMERPROC
        Return(c, 0, 1);
        return;
    }
    Return(c, Send(hwnd, msg, wparam, lparam), 1);
}
U(PostMessageA) {
    Post(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
    Return(c, 1, 4);
}
U(SendMessageA) { Return(c, Send(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3)), 4); }
U(PostQuitMessage) {
    Post(0, kWmQuit, Arg(c, 0), 0);
    Return(c, 0, 1);
}
U(DefWindowProcW) {
    uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1);
    uint32_t r = 0;
    if (msg == kWmNcCreate) r = 1;  // creation goes on
    if (msg == kWmClose) Send(hwnd, kWmDestroy, 0, 0);  // DestroyWindow
    Return(c, r, 4);
}
U(SetTimer) {
    uint32_t hwnd = Arg(c, 0), id = Arg(c, 1), ms = Arg(c, 2), cb = Arg(c, 3);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!hwnd && !id) id = 0x7000u + uint32_t(g_timers.size());
    for (Timer& t : g_timers) {
        if (t.hwnd == hwnd && t.id == id) {
            t.period_ms = ms;
            t.callback = cb;
            Return(c, id, 4);
            return;
        }
    }
    g_timers.push_back({hwnd, id, ms, cb, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)});
    Return(c, id, 4);
}
U(KillTimer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it) {
        if (it->hwnd == Arg(c, 0) && it->id == Arg(c, 1)) {
            g_timers.erase(it);
            break;
        }
    }
    Return(c, 1, 2);
}
U(ShowWindow) {
    if (Arg(c, 0) == kMainWindow) Send(kMainWindow, kWmShowWindow, Arg(c, 1) ? 1 : 0, 0);
    Return(c, 0, 2);
}
U(UpdateWindow) { Return(c, 1, 1); }
U(SetWindowPos) { Return(c, 1, 7); }
U(MoveWindow) { Return(c, 1, 6); }
U(SetWindowTextW) {
    if (Arg(c, 0) == kMainWindow) SDL_SetWindowTitle(g_sdl_window, WideToUtf8(Arg(c, 1)).c_str());
    Return(c, 1, 2);
}
U(SetFocus) { Return(c, kMainWindow, 1); }
U(SetActiveWindow) { Return(c, kMainWindow, 1); }
U(GetActiveWindow) { Return(c, kMainWindow); }
U(IsIconic) { Return(c, 0, 1); }
U(IsZoomed) { Return(c, 0, 1); }
U(IsWindowVisible) { Return(c, 1, 1); }
U(GetAncestor) {
    uint32_t hwnd = Arg(c, 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    while (g_window_parent.count(hwnd) && g_window_parent[hwnd]) hwnd = g_window_parent[hwnd];
    Return(c, hwnd, 2);
}
U(GetWindowThreadProcessId) {
    if (Arg(c, 1)) W32(Arg(c, 1), 0x1000u);  // GetCurrentProcessId's value (kernel32.cpp)
    Return(c, CurrentThread()->id, 2);
}
U(AdjustWindowRectEx) { Return(c, 1, 4); }  // borderless: the client area is the window
U(ClientToScreen) { Return(c, 1, 2); }
U(ScreenToClient) { Return(c, 1, 2); }
U(GetCursorPos) {
    int x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    W32(Arg(c, 0), uint32_t(x));
    W32(Arg(c, 0) + 4, uint32_t(y));
    Return(c, 1, 1);
}
U(SetCursorPos) {
    SDL_WarpMouseInWindow(g_sdl_window, int(Arg(c, 0)), int(Arg(c, 1)));
    Return(c, 1, 2);
}
U(SetCursor) { Return(c, 0, 1); }
U(LoadCursorA) { Return(c, 0x00020000u | (Arg(c, 1) & 0xffff), 2); }
U(LoadIconA) { Return(c, 0x00030000u | (Arg(c, 1) & 0xffff), 2); }
U(LoadImageW) { Return(c, 0x00040000u, 6); }
U(DestroyCursor) { Return(c, 1, 1); }
U(GetIconInfo) { Return(c, 0, 2); }
U(SetCapture) { Return(c, 0, 1); }
U(ReleaseCapture) { Return(c, 1); }
U(GetDoubleClickTime) { Return(c, 500); }
U(GetMenu) { Return(c, 0, 1); }
U(BeginPaint) {
    uint32_t ps = Arg(c, 1);  // PAINTSTRUCT: hdc, fErase, rcPaint, ...
    memset(Ptr<void>(ps), 0, 64);
    W32(ps, 0x00050000u);
    W32(ps + 16, uint32_t(g_width));
    W32(ps + 20, uint32_t(g_height));
    Return(c, 0x00050000u, 2);
}
U(EndPaint) { Return(c, 1, 2); }
U(ValidateRect) { Return(c, 1, 2); }
U(InvalidateRect) { Return(c, 1, 3); }
U(GetDC) { Return(c, 0x00050000u, 1); }
U(ReleaseDC) { Return(c, 1, 2); }
U(FillRect) { Return(c, 1, 3); }
U(IntersectRect) {
    uint32_t d = Arg(c, 0), a = Arg(c, 1), b = Arg(c, 2);
    int32_t l = std::max(int32_t(R32(a)), int32_t(R32(b))), t = std::max(int32_t(R32(a + 4)), int32_t(R32(b + 4)));
    int32_t r = std::min(int32_t(R32(a + 8)), int32_t(R32(b + 8))), bo = std::min(int32_t(R32(a + 12)), int32_t(R32(b + 12)));
    bool ok = l < r && t < bo;
    W32(d, ok ? uint32_t(l) : 0);
    W32(d + 4, ok ? uint32_t(t) : 0);
    W32(d + 8, ok ? uint32_t(r) : 0);
    W32(d + 12, ok ? uint32_t(bo) : 0);
    Return(c, ok ? 1 : 0, 3);
}
constexpr uint32_t kMonitor = 0x00060000u;
U(MonitorFromWindow) { Return(c, kMonitor, 2); }
U(GetMonitorInfoA) {
    uint32_t mi = Arg(c, 1);  // MONITORINFO: cbSize, rcMonitor, rcWork, dwFlags
    for (int k : {4, 20}) {
        W32(mi + k, 0);
        W32(mi + k + 4, 0);
        W32(mi + k + 8, uint32_t(g_width));
        W32(mi + k + 12, uint32_t(g_height));
    }
    W32(mi + 36, 1);  // MONITORINFOF_PRIMARY
    Return(c, 1, 2);
}
U(EnumDisplayMonitors) {
    uint32_t rect = HeapAlloc(16);
    W32(rect, 0);
    W32(rect + 4, 0);
    W32(rect + 8, uint32_t(g_width));
    W32(rect + 12, uint32_t(g_height));
    GuestCall(Arg(c, 2), {kMonitor, 0x00050000u, rect, Arg(c, 3)});
    HeapFree(rect);
    Return(c, 1, 4);
}
U(SystemParametersInfoA) {
    uint32_t action = Arg(c, 0), pv = Arg(c, 2);
    if (action == 0x30 && pv) {  // SPI_GETWORKAREA
        W32(pv, 0);
        W32(pv + 4, 0);
        W32(pv + 8, uint32_t(g_width));
        W32(pv + 12, uint32_t(g_height));
    } else if (pv && (action == 0x3a || action == 0x34 || action == 0x32)) {
        // SPI_GETSTICKYKEYS / TOGGLEKEYS / FILTERKEYS: cbSize, dwFlags = 0
        W32(pv + 4, 0);
    }
    Return(c, 1, 4);
}
U(GetProcessWindowStation) { Return(c, 0x00070000u); }
U(GetUserObjectInformationW) {
    // UOI_FLAGS: WSF_VISIBLE, so the game does not think it runs as a service.
    if (Arg(c, 1) == 1 && Arg(c, 2) && Arg(c, 3) >= 12) {
        W32(Arg(c, 2), 0);
        W32(Arg(c, 2) + 4, 0);
        W32(Arg(c, 2) + 8, 1);
    }
    if (Arg(c, 4)) W32(Arg(c, 4), 12);
    Return(c, 1, 5);
}
U(SetWindowsHookExA) { Return(c, 0x00080000u, 4); }
U(UnhookWindowsHookEx) { Return(c, 1, 1); }
U(CallNextHookEx) { Return(c, 0, 4); }
U(MessageBoxW) {
    Log("MessageBox [%s]: %s", WideToUtf8(Arg(c, 2)).c_str(), WideToUtf8(Arg(c, 1)).c_str());
    Return(c, 1, 4);
}
U(IsDlgButtonChecked) { Return(c, 0, 2); }
U(CheckDlgButton) { Return(c, 1, 3); }
// Clipboard: none (OpenClipboard fails, so the game never gets further).
U(OpenClipboard) { Return(c, 0, 1); }
U(CloseClipboard) { Return(c, 1); }
U(EmptyClipboard) { Return(c, 0); }
U(GetClipboardData) { Return(c, 0, 1); }
U(SetClipboardData) { Return(c, 0, 2); }
U(IsClipboardFormatAvailable) { Return(c, 0, 1); }
U(SetClipboardViewer) { Return(c, 0, 1); }
U(ChangeClipboardChain) { Return(c, 0, 2); }
// Key state for VK codes from SDL's keyboard (scancodes through input.cpp's table).
U(GetKeyState) { Return(c, VkDown(Arg(c, 0)) ? 0xff80u : 0u, 1); }
U(GetAsyncKeyState) { Return(c, VkDown(Arg(c, 0)) ? 0x8000u : 0u, 1); }
U(GetKeyboardState) {
    for (uint32_t vk = 0; vk < 256; ++vk) W8(Arg(c, 0) + vk, VkDown(vk) ? 0x80 : 0);
    Return(c, 1, 1);
}

#undef U

#define E(name) {"user32.dll", #name, u_##name}
const ImportDef kUser32[] = {
    E(GetDesktopWindow), E(GetForegroundWindow), E(GetClientRect), E(GetWindowRect), E(GetWindowLongA),
    E(SetWindowLongA), E(MapVirtualKeyExA), E(GetKeyboardLayout), E(RegisterClassExA), E(UnregisterClassA),
    E(CreateWindowExA), E(DestroyWindow), E(wvsprintfA), E(wsprintfA), E(MessageBoxA), E(CharLowerBuffA),
    E(CallWindowProcA), E(DefWindowProcA), E(ShowCursor), E(GetSystemMetrics),
    E(RegisterClassExW), E(RegisterClassA), E(CreateWindowExW), E(PeekMessageW), E(PeekMessageA), E(TranslateMessage),
    E(DispatchMessageW), E(PostMessageA), E(SendMessageA), E(PostQuitMessage), E(DefWindowProcW), E(SetTimer),
    E(KillTimer), E(ShowWindow), E(UpdateWindow), E(SetWindowPos), E(MoveWindow), E(SetWindowTextW), E(SetFocus),
    E(SetActiveWindow), E(GetActiveWindow), E(IsIconic), E(IsZoomed), E(IsWindowVisible), E(GetAncestor),
    E(GetWindowThreadProcessId), E(AdjustWindowRectEx), E(ClientToScreen), E(ScreenToClient), E(GetCursorPos),
    E(SetCursorPos), E(SetCursor), E(LoadCursorA), E(LoadIconA), E(LoadImageW), E(DestroyCursor), E(GetIconInfo),
    E(SetCapture), E(ReleaseCapture), E(GetDoubleClickTime), E(GetMenu), E(BeginPaint), E(EndPaint), E(ValidateRect),
    E(InvalidateRect), E(GetDC), E(ReleaseDC), E(FillRect), E(IntersectRect), E(MonitorFromWindow),
    E(GetMonitorInfoA), E(EnumDisplayMonitors), E(SystemParametersInfoA), E(GetProcessWindowStation),
    E(GetUserObjectInformationW), E(SetWindowsHookExA), E(UnhookWindowsHookEx), E(CallNextHookEx), E(MessageBoxW),
    E(IsDlgButtonChecked), E(CheckDlgButton), E(OpenClipboard), E(CloseClipboard), E(EmptyClipboard),
    E(GetClipboardData), E(SetClipboardData), E(IsClipboardFormatAvailable), E(SetClipboardViewer),
    E(ChangeClipboardChain), E(GetKeyState), E(GetAsyncKeyState), E(GetKeyboardState),
    {"user32.dll", "SetWindowLongW", u_SetWindowLongA}, {"user32.dll", "GetWindowLongW", u_GetWindowLongA},
};
#undef E
ImportRegistrar g_user32(kUser32);

}  // namespace

uint32_t HostWindow() { return kMainWindow; }

void PostWindowMessage(uint32_t msg, uint32_t wparam, uint32_t lparam) { Post(kMainWindow, msg, wparam, lparam); }

// The engine renders at the size of the window's client area. Setting render_height (default 720 on Android, 0 = the
// real window size elsewhere) makes it render smaller, keeping the window's aspect ratio; DXVK scales the frame to
// the window when presenting.
void InitClientSize(int window_w, int window_h) {
#ifdef __ANDROID__
    int rh = SettingInt("render_height", 720);
#else
    int rh = SettingInt("render_height", 0);
#endif
    if (rh > 0 && window_h > 0) {
        g_height = rh;
        g_width = (rh * window_w / window_h + 4) & ~7;
    } else {
        g_width = window_w;
        g_height = window_h;
    }
    Log("render size %dx%d (window %dx%d)", g_width, g_height, window_w, window_h);
}

void SetClientSize(int w, int h) {
    g_width = w;
    g_height = h;
}

void ClientSize(int* w, int* h) {
    *w = g_width;
    *h = g_height;
}

}  // namespace native
