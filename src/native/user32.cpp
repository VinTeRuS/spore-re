// USER32 for the native host. The game's main window is the SDL window; other handles are fake values.
#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
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
extern std::atomic<int> g_cursor_off_x, g_cursor_off_y;  // input.cpp: where SetCursorPos put the game's cursor
void InjectKey(SDL_Scancode sc, bool down);
extern std::atomic<uint64_t> g_frames;  // d3d9.cpp: presented frames
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

// Window classes are found by name or by the atom RegisterClass returned ("#<atom>", as CreateWindowEx gets
// MAKEINTATOM(atom)); both keys point at the same WNDPROC. Called with g_mutex held.
uint32_t g_next_atom = 0xc001u;
uint32_t AddClass(const std::string& name, uint32_t proc) {
    uint32_t atom = g_next_atom++;
    g_classes[name] = proc;
    g_classes["#" + std::to_string(atom)] = proc;
    return atom;
}

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
    Return(c, AddClass(n, R32(wc + 8)), 1);
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
// Windows with an invalid region (InvalidateRect): PeekMessage hands out WM_PAINT for them when nothing else is
// queued, until BeginPaint/ValidateRect (or the dispatch, see DispatchMessageW) validates. Spore draws its frames
// from WM_PAINT and invalidates the window on every tick.
std::map<uint32_t, bool> g_paint_pending;  // under g_mutex
void SetPaintPending(uint32_t hwnd, bool pending) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_paint_pending[hwnd ? hwnd : kMainWindow] = pending;
}
// SPORERE_TRACE_MESSAGES=1: a histogram of dispatched messages every 5000 dispatches, and the PeekMessage calls.
const bool g_trace_messages = getenv("SPORERE_TRACE_MESSAGES") != nullptr;
std::map<uint32_t, uint32_t> g_msg_counts;
uint64_t g_peeks = 0, g_peek_hits = 0;
std::map<uint32_t, uint32_t> g_window_parent;

// Class and window names: an atom, or a UTF-16 string (win.h WideToUtf8).
std::string ClassName(uint32_t a) { return a < 0x10000 ? "#" + std::to_string(a) : WideToUtf8(a); }

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

// ignore_focus = 1 (SPORERE_IGNORE_FOCUS): the game never hears that its window lost focus, so it keeps running in
// the background (tests on a desktop that is in use; Spore pauses while inactive).
bool IgnoreFocus() {
    static const bool ignore = SettingInt("ignore_focus", 0) != 0;
    return ignore;
}

// SPORERE_INPUT="<frame>:<SDL key name>,<frame>:click:<x>,<y>,<frame>:text:<text>,...": scripted input for tests. A
// key is held for 5 frames ("Left Ctrl+Left Shift+C" holds them together); a click goes to client coordinates (left
// button down and up on consecutive frames); text arrives as typed characters (no commas in it).
struct Scripted {
    uint64_t frame;
    SDL_Scancode key;
    int x, y;  // click when key == SDL_SCANCODE_UNKNOWN and no keys/text
    std::vector<SDL_Scancode> keys;
    std::string text;
};
std::vector<Scripted> LoadScript() {
    std::vector<Scripted> out;
    const char* spec = getenv("SPORERE_INPUT");
    if (!spec) return out;
    std::string all(spec);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t colon = all.find(':', pos);
        if (colon == std::string::npos) break;
        uint64_t frame = strtoull(all.c_str() + pos, nullptr, 10);
        if (all.compare(colon + 1, 6, "click:") == 0) {
            int x = 0, y = 0;
            sscanf(all.c_str() + colon + 7, "%d,%d", &x, &y);
            out.push_back({frame, SDL_SCANCODE_UNKNOWN, x, y, {}, {}});
            size_t comma = all.find(',', all.find(',', colon + 7) + 1);
            pos = comma == std::string::npos ? all.size() : comma + 1;
        } else {
            size_t comma = all.find(',', colon);
            std::string key = all.substr(colon + 1, comma == std::string::npos ? std::string::npos : comma - colon - 1);
            pos = comma == std::string::npos ? all.size() : comma + 1;
            if (key.compare(0, 5, "text:") == 0) {
                out.push_back({frame, SDL_SCANCODE_UNKNOWN, -1, -1, {}, key.substr(5)});
            } else if (key.find('+') != std::string::npos) {
                Scripted s{frame, SDL_SCANCODE_UNKNOWN, -1, -1, {}, {}};
                for (size_t a = 0; a <= key.size();) {
                    size_t b = key.find('+', a);
                    if (b == std::string::npos) b = key.size();
                    s.keys.push_back(SDL_GetScancodeFromName(key.substr(a, b - a).c_str()));
                    a = b + 1;
                }
                out.push_back(s);
            } else {
                out.push_back({frame, SDL_GetScancodeFromName(key.c_str()), 0, 0, {}, {}});
            }
        }
    }
    return out;
}
// While a scripted click runs, the cursor and the left button read back as the script sets them (GetCursorPos,
// GetAsyncKeyState), not from the real mouse.
struct { bool active = false, down = false; int x = 0, y = 0; } g_script_mouse;
void RunScript() {
    static std::vector<Scripted> script = LoadScript();
    static uint64_t last = ~0ull;
    if (script.empty()) return;
    uint64_t frame = g_frames.load(std::memory_order_relaxed);
    if (frame == last) return;
    last = frame;
    for (const Scripted& s : script) {
        if (!s.keys.empty()) {
            // Modifiers first, the last key a frame later (games read the modifiers when the key arrives).
            for (size_t i = 0; i < s.keys.size(); ++i) {
                if (frame == s.frame + (i + 1 == s.keys.size() ? 2 : 0)) InjectKey(s.keys[i], true);
                if (frame == s.frame + 8) InjectKey(s.keys[i], false);
            }
        } else if (!s.text.empty()) {
            if (frame == s.frame) {
                SDL_Event e{};
                e.type = SDL_TEXTINPUT;
                snprintf(e.text.text, sizeof(e.text.text), "%s", s.text.c_str());
                ForwardInput(e);
            }
        } else if (s.key != SDL_SCANCODE_UNKNOWN) {
            if (frame == s.frame) InjectKey(s.key, true);
            if (frame == s.frame + 5) InjectKey(s.key, false);
        } else if (frame >= s.frame && frame <= s.frame + 30) {
            // Hover first, press 10 frames later, release 10 frames after that (the UI reacts per frame).
            uint32_t lp = uint32_t(s.y) << 16 | uint32_t(s.x & 0xffff);
            g_script_mouse.active = true;
            g_script_mouse.x = s.x;
            g_script_mouse.y = s.y;
            uint64_t t = frame - s.frame;
            g_script_mouse.down = t >= 10 && t < 20;
            if (t == 0 || t == 5) Post(kMainWindow, 0x0200, 0, lp);    // WM_MOUSEMOVE
            if (t == 10) Post(kMainWindow, 0x0201, 1, lp);             // WM_LBUTTONDOWN
            if (t == 20) Post(kMainWindow, 0x0202, 0, lp);             // WM_LBUTTONUP
        }
    }
}

// SDL events become posted messages; called from PeekMessage on the game's thread.
void PumpSdl() {
    RunScript();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || e.type == SDL_APP_TERMINATING) {
            Post(kMainWindow, kWmClose, 0, 0);
        } else if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                Post(kMainWindow, kWmActivateApp, 1, 0);
                Post(kMainWindow, kWmActivate, 1, 0);
                Post(kMainWindow, kWmSetFocus, 0, 0);
            } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !IgnoreFocus()) {
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
    Log("CreateWindowEx(%s, style %08x) -> %08x, wndproc %08x", cls.c_str(), style, hwnd, WindowProc(hwnd));
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
    std::string n = ClassName(R32(wc + 40));
    std::lock_guard<std::mutex> lock(g_mutex);
    Return(c, AddClass(n, R32(wc + 8)), 1);
}
U(RegisterClassA) {
    uint32_t wc = Arg(c, 0);  // WNDCLASSA: style, lpfnWndProc, ..., lpszClassName at 36
    uint32_t name = R32(wc + 36);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string n = name < 0x10000 ? "#" + std::to_string(name) : std::string(Str(name));
    Return(c, AddClass(n, R32(wc + 4)), 1);
}
U(CreateWindowExW) {
    uint32_t r = CreateWindowCommon(Arg(c, 0), ClassName(Arg(c, 1)), Arg(c, 2), Arg(c, 3), int32_t(Arg(c, 4)),
                                    int32_t(Arg(c, 5)), int32_t(Arg(c, 6)), int32_t(Arg(c, 7)), Arg(c, 8), Arg(c, 9),
                                    Arg(c, 10), Arg(c, 11));
    Return(c, r, 12);
}
// PeekMessage(MSG*, hwnd, min, max, remove): MSG = hwnd, message, wParam, lParam, time, pt.
void PeekCommon(Ctx* c) {
    PumpSdl();
    ++g_peeks;
    if (g_trace_messages && (g_peeks % 200000) == 0)
        Log("PeekMessage: %llu calls, %llu with a message, %zu timers, queue %zu", (unsigned long long)g_peeks,
            (unsigned long long)g_peek_hits, g_timers.size(), g_queue.size());
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
        ++g_peek_hits;
        Return(c, 1, 5);
        return;
    }
    uint32_t want = Arg(c, 1);
    bool paint_wanted = !(filter_lo || filter_hi) || (0x000f >= filter_lo && 0x000f <= filter_hi);
    for (auto& [hwnd, pending] : g_paint_pending) {
        if (!pending || !paint_wanted || (want && want != hwnd)) continue;
        W32(out, hwnd);
        W32(out + 4, 0x000f);  // WM_PAINT
        W32(out + 8, 0);
        W32(out + 12, 0);
        W32(out + 16, uint32_t(SDL_GetTicks()));
        W32(out + 20, 0);
        W32(out + 24, 0);
        ++g_peek_hits;
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
    if (g_trace_messages && ++g_msg_counts[msg] && (g_msg_counts[msg] % 5000) == 0) {
        std::string h;
        for (auto& [k, v] : g_msg_counts) h += " " + std::to_string(k) + ":" + std::to_string(v);
        Log("messages (peeks %llu, hits %llu):%s", (unsigned long long)g_peeks, (unsigned long long)g_peek_hits,
            h.c_str());
    }
    if (msg == kWmTimer && lparam) {
        GuestCall(lparam, {hwnd, msg, wparam, uint32_t(SDL_GetTicks())});  // TIMERPROC
        Return(c, 0, 1);
        return;
    }
    // A WM_PAINT the window procedure did not validate would come back at once on Windows too; validating on
    // dispatch keeps one frame per InvalidateRect instead of an endless loop.
    if (msg == 0x000f) SetPaintPending(hwnd, false);
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
    if (msg == 0x000f) SetPaintPending(hwnd, false);   // WM_PAINT: DefWindowProc validates
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
// Showing the main window activates it as on Windows (the SDL window had focus before the game's window existed,
// so its focus event went nowhere): WM_SHOWWINDOW, WM_ACTIVATEAPP, WM_ACTIVATE (WA_ACTIVE), WM_SETFOCUS, WM_SIZE and a
// WM_PAINT. Games pause their rendering while they believe they are in the background.
bool g_activated = false;
U(ShowWindow) {
    uint32_t hwnd = Arg(c, 0);
    bool show = Arg(c, 1) != 0;
    if (hwnd == kMainWindow) {
        Send(kMainWindow, kWmShowWindow, show ? 1 : 0, 0);
        if (show && !g_activated) {
            g_activated = true;
            Send(kMainWindow, kWmActivateApp, 1, 0);
            Send(kMainWindow, kWmActivate, 1, 0);
            Send(kMainWindow, kWmSetFocus, 0, 0);
            Send(kMainWindow, kWmSize, 0, uint32_t(g_height) << 16 | uint32_t(g_width));
            Post(kMainWindow, 0x000f, 0, 0);  // WM_PAINT
        }
    }
    Return(c, 0, 2);
}
U(UpdateWindow) { Return(c, 1, 1); }
U(SetWindowPos) { Return(c, 1, 7); }
U(MoveWindow) { Return(c, 1, 6); }
U(SetWindowTextW) {
    // The game's own title, with the port's credit after it.
    if (Arg(c, 0) == kMainWindow)
        SDL_SetWindowTitle(g_sdl_window, (WideToUtf8(Arg(c, 1)) + " - port by VinTeRuS").c_str());
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
// The cursor in the game's coordinates: SDL reports window pixels, the game lives in its client size (the render
// size, InitClientSize), which on a phone is much smaller than the screen. Window messages are scaled the same way
// (input.cpp PackPoint); the 3D world picks with GetCursorPos, so an unscaled position missed what was tapped.
U(GetCursorPos) {
    int x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    int ww = 1, wh = 1;
    SDL_GetWindowSize(g_sdl_window, &ww, &wh);
    x = x * g_width / (ww > 0 ? ww : 1) + g_cursor_off_x.load(std::memory_order_relaxed);
    y = y * g_height / (wh > 0 ? wh : 1) + g_cursor_off_y.load(std::memory_order_relaxed);
    if (g_script_mouse.active) x = g_script_mouse.x, y = g_script_mouse.y;
    W32(Arg(c, 0), uint32_t(x));
    W32(Arg(c, 0) + 4, uint32_t(y));
    Return(c, 1, 1);
}
// Moves only the game's view of the cursor (input.cpp g_cursor_off_*): no SDL warp, see there.
U(SetCursorPos) {
    int x = 0, y = 0, ww = 1, wh = 1;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(g_sdl_window, &ww, &wh);
    g_cursor_off_x = int32_t(Arg(c, 0)) - x * g_width / (ww > 0 ? ww : 1);
    g_cursor_off_y = int32_t(Arg(c, 1)) - y * g_height / (wh > 0 ? wh : 1);
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
    SetPaintPending(Arg(c, 0), false);
    uint32_t ps = Arg(c, 1);  // PAINTSTRUCT: hdc, fErase, rcPaint, ...
    memset(Ptr<void>(ps), 0, 64);
    W32(ps, 0x00050000u);
    W32(ps + 16, uint32_t(g_width));
    W32(ps + 20, uint32_t(g_height));
    Return(c, 0x00050000u, 2);
}
U(EndPaint) { Return(c, 1, 2); }  // BeginPaint validated already
U(ValidateRect) {
    SetPaintPending(Arg(c, 0), false);
    Return(c, 1, 2);
}
U(InvalidateRect) {
    SetPaintPending(Arg(c, 0), true);
    Return(c, 1, 3);
}
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
U(GetKeyState) {
    bool down = Arg(c, 0) == 1 && g_script_mouse.active ? g_script_mouse.down : VkDown(Arg(c, 0));
    Return(c, down ? 0xff80u : 0u, 1);
}
U(GetAsyncKeyState) {
    bool down = Arg(c, 0) == 1 && g_script_mouse.active ? g_script_mouse.down : VkDown(Arg(c, 0));
    Return(c, down ? 0x8000u : 0u, 1);
}
U(GetKeyboardState) {
    for (uint32_t vk = 0; vk < 256; ++vk) W8(Arg(c, 0) + vk, VkDown(vk) ? 0x80 : 0);
    Return(c, 1, 1);
}

// wsprintfW: %s is a UTF-16 string here (%S and %hs narrow), %c a UTF-16 unit; numbers as in printf. The result is
// written as UTF-16 (at most 1024 units, wsprintf's own limit).
std::string WideFormat(uint32_t fmt, uint32_t args) {
    std::string f = WideToUtf8(fmt), out;
    for (size_t i = 0; i < f.size();) {
        if (f[i] != '%') {
            out += f[i++];
            continue;
        }
        if (i + 1 < f.size() && f[i + 1] == '%') {
            out += '%';
            i += 2;
            continue;
        }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < f.size() && strchr("-+ #0", f[j])) spec += f[j++];
        while (j < f.size() && isdigit((unsigned char)f[j])) spec += f[j++];
        if (j < f.size() && f[j] == '.') {
            spec += f[j++];
            while (j < f.size() && isdigit((unsigned char)f[j])) spec += f[j++];
        }
        bool narrow = false;
        if (j < f.size() && (f[j] == 'l' || f[j] == 'h')) narrow = f[j++] == 'h';
        if (j + 2 < f.size() && f.compare(j, 3, "I64") == 0) {
            j += 3;
            char conv = f[j++];
            uint64_t v = uint64_t(R32(args)) | uint64_t(R32(args + 4)) << 32;
            args += 8;
            char buf[64];
            snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)v);
            out += buf;
            i = j;
            continue;
        }
        char conv = j < f.size() ? f[j++] : 's';
        char buf[512];
        if (conv == 's' || conv == 'S') {
            uint32_t p = R32(args);
            args += 4;
            std::string v = !p ? "(null)" : (conv == 'S' || narrow) ? std::string(Str(p)) : WideToUtf8(p);
            snprintf(buf, sizeof buf, (spec + "s").c_str(), v.c_str());
            out += buf;
        } else if (conv == 'c' || conv == 'C') {
            uint32_t ch = R32(args) & 0xffff;
            args += 4;
            uint32_t tmp = HeapAlloc(4);
            W16(tmp, uint16_t(ch));
            W16(tmp + 2, 0);
            out += WideToUtf8(tmp);
            HeapFree(tmp);
        } else {
            snprintf(buf, sizeof buf, (spec + conv).c_str(), R32(args));
            args += 4;
            out += buf;
        }
        i = j;
    }
    return out;
}
U(wsprintfW) {
    std::string s = WideFormat(Arg(c, 1), c->esp + 12);
    Return(c, PutWideString(Arg(c, 0), 1025, s));  // cdecl
}
U(EnumDisplayDevicesA) {
    uint32_t index = Arg(c, 1), dd = Arg(c, 2);  // DISPLAY_DEVICEA: cb, DeviceName[32], DeviceString[128], ...
    if (index != 0 || !dd) {
        Return(c, 0, 4);
        return;
    }
    uint32_t cb = R32(dd);
    memset(Ptr<uint8_t>(dd) + 4, 0, cb > 4 ? cb - 4 : 0);
    strcpy(Str(dd + 4), "\\\\.\\DISPLAY1");
    strcpy(Str(dd + 36), "Vulkan (DXVK)");
    W32(dd + 164, 0x5);  // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE
    Return(c, 1, 4);
}

// TrackMouseEvent (looked up with GetProcAddress): accepted; the cursor never leaves a full-screen window, so
// WM_MOUSELEAVE/WM_MOUSEHOVER never come.
U(TrackMouseEvent) { Return(c, 1, 1); }

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
    E(ChangeClipboardChain), E(GetKeyState), E(GetAsyncKeyState), E(GetKeyboardState), E(wsprintfW),
    E(EnumDisplayDevicesA), E(TrackMouseEvent),
    {"user32.dll", "SetWindowLongW", u_SetWindowLongA}, {"user32.dll", "GetWindowLongW", u_GetWindowLongA},
};
#undef E
ImportRegistrar g_user32(kUser32);

}  // namespace

uint32_t HostWindow() { return kMainWindow; }

void PostWindowMessage(uint32_t msg, uint32_t wparam, uint32_t lparam) { Post(kMainWindow, msg, wparam, lparam); }

// The engine renders at the size of the window's client area. Setting render_height (default 600 on Android, 0 = the
// real window size elsewhere) makes it render smaller, keeping the window's aspect ratio; DXVK scales the frame to
// the window when presenting.
void InitClientSize(int window_w, int window_h) {
#ifdef __ANDROID__
    int rh = SettingInt("render_height", 600);  // Spore's smallest supported height: the largest UI on a phone
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
