// Imports of SporeApp.exe with a small, self-contained implementation: winmm timer periods, the Steam API (the GOG build
// still links steam_api.dll but runs without Steam), the D3DX math the game imports from d3dx9_27, and an offline
// Winsock. Everything
// else missing logs and stops when called (imports.cpp), so real gaps show up in the log.
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "win.h"

namespace native {

uint32_t NewDirectInput8();  // dinput.cpp
void ClientSize(int* w, int* h);  // user32.cpp


namespace {

// ---- winmm (timeGetTime: misc.cpp) ----
void w_timeBeginPeriod(Ctx* c) { Return(c, 0, 1); }  // TIMERR_NOERROR
void w_timeEndPeriod(Ctx* c) { Return(c, 0, 1); }

// ---- steam_api: what GOG's replacement steam_api.dll does: SteamAPI_Init succeeds and SteamApps() is a static
// ISteamApps whose answers come from no Steam client at all (owned, DLC installed, not low violence). The game quits
// right after a failed SteamAPI_Init. Methods are thiscall; their slots are import thunks of made-up names. ----
void s_SteamAPI_Init(Ctx* c) { Return(c, 1); }
void s_SteamAPI_Shutdown(Ctx* c) { Return(c, 0); }
void a_True0(Ctx* c) { Return(c, 1); }
void a_False0(Ctx* c) { Return(c, 0); }
void a_True1(Ctx* c) { Return(c, 1, 1); }
void a_Language(Ctx* c) {
    static uint32_t english = GuestStrdup("english"), russian = GuestStrdup("russian");
    static const bool ru = Setting("locale", "ru-ru").rfind("ru", 0) == 0;
    Return(c, ru ? russian : english);
}
void s_SteamApps(Ctx* c) {
    static uint32_t object = 0;
    if (!object) {
        // ISteamApps (v002): BIsSubscribed, BIsLowViolence, BIsCybercafe, BIsVACBanned, GetCurrentGameLanguage,
        // GetAvailableGameLanguages, BIsSubscribedApp(appid), BIsDlcInstalled(appid)
        static const char* methods[] = {"ISteamApps::BIsSubscribed", "ISteamApps::BIsLowViolence",
                                        "ISteamApps::BIsCybercafe", "ISteamApps::BIsVACBanned",
                                        "ISteamApps::GetCurrentGameLanguage", "ISteamApps::GetAvailableGameLanguages",
                                        "ISteamApps::BIsSubscribedApp", "ISteamApps::BIsDlcInstalled"};
        uint32_t vtable = HeapAlloc(4 * 8);
        for (int i = 0; i < 8; ++i) W32(vtable + 4 * uint32_t(i), ImportThunk("steam_api.dll", methods[i]));
        object = HeapAlloc(4);
        W32(object, vtable);
    }
    Return(c, object);
}

// ---- d3dx9_27 math (D3DXMATRIX: row-major float[16], D3DXVECTOR3/4) ----
void LoadMatrix(uint32_t a, float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = RF32(a + 4 * i);
}
void StoreMatrix(uint32_t a, const float m[16]) {
    for (int i = 0; i < 16; ++i) WF32(a + 4 * i, m[i]);
}
void Multiply(const float a[16], const float b[16], float r[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[4 * i + k] * b[4 * k + j];
            r[4 * i + j] = s;
        }
}
void Transpose(const float a[16], float r[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[4 * j + i] = a[4 * i + j];
}
// D3DXMatrixMultiply(out, m1, m2) -> out; out may alias an input.
void d_D3DXMatrixMultiply(Ctx* c) {
    float a[16], b[16], r[16];
    LoadMatrix(Arg(c, 1), a);
    LoadMatrix(Arg(c, 2), b);
    Multiply(a, b, r);
    StoreMatrix(Arg(c, 0), r);
    Return(c, Arg(c, 0), 3);
}
void d_D3DXMatrixMultiplyTranspose(Ctx* c) {
    float a[16], b[16], m[16], r[16];
    LoadMatrix(Arg(c, 1), a);
    LoadMatrix(Arg(c, 2), b);
    Multiply(a, b, m);
    Transpose(m, r);
    StoreMatrix(Arg(c, 0), r);
    Return(c, Arg(c, 0), 3);
}
void d_D3DXMatrixTranspose(Ctx* c) {
    float a[16], r[16];
    LoadMatrix(Arg(c, 1), a);
    Transpose(a, r);
    StoreMatrix(Arg(c, 0), r);
    Return(c, Arg(c, 0), 2);
}
// D3DXVec3Transform(D3DXVECTOR4* out, const D3DXVECTOR3* v, const D3DXMATRIX* m): (x, y, z, 1) * m.
void d_D3DXVec3Transform(Ctx* c) {
    float m[16], v[3] = {RF32(Arg(c, 1)), RF32(Arg(c, 1) + 4), RF32(Arg(c, 1) + 8)};
    LoadMatrix(Arg(c, 2), m);
    for (int j = 0; j < 4; ++j) WF32(Arg(c, 0) + 4 * j, v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + m[12 + j]);
    Return(c, Arg(c, 0), 3);
}
// D3DXVec3TransformNormal(D3DXVECTOR3* out, v, m): (x, y, z, 0) * m.
void d_D3DXVec3TransformNormal(Ctx* c) {
    float m[16], v[3] = {RF32(Arg(c, 1)), RF32(Arg(c, 1) + 4), RF32(Arg(c, 1) + 8)};
    LoadMatrix(Arg(c, 2), m);
    float r[3];
    for (int j = 0; j < 3; ++j) r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j];
    for (int j = 0; j < 3; ++j) WF32(Arg(c, 0) + 4 * j, r[j]);
    Return(c, Arg(c, 0), 3);
}
void d_D3DXCheckVersion(Ctx* c) { Return(c, 1, 2); }
// Profiles for the device DXVK exposes (shader model 3).
void d_D3DXGetVertexShaderProfile(Ctx* c) {
    static uint32_t s = GuestStrdup("vs_3_0");
    Return(c, s, 1);
}
void d_D3DXGetPixelShaderProfile(Ctx* c) {
    static uint32_t s = GuestStrdup("ps_3_0");
    Return(c, s, 1);
}
// The game ships its shaders compiled (Spore_Graphics.package); two Wine runs never reached the compiler. Fail
// loudly enough to notice if some path does.
void d_D3DXCompileShader(Ctx* c) {
    Log("D3DXCompileShader(entry %s, profile %s): not supported", Arg(c, 4) ? Str(Arg(c, 4)) : "",
        Arg(c, 5) ? Str(Arg(c, 5)) : "");
    if (Arg(c, 7)) W32(Arg(c, 7), 0);
    if (Arg(c, 8)) W32(Arg(c, 8), 0);
    if (Arg(c, 9)) W32(Arg(c, 9), 0);
    Return(c, 0x80004005u, 10);  // E_FAIL
}


// ---- Winsock (wsock32 by ordinal, ws2_32): started, but no network; every socket call fails as on a machine
// without a connection, which the game's online features (Sporepedia, sharing) handle on their own ----
constexpr uint32_t kWsaENetDown = 10050, kSocketError = 0xffffffffu;
uint32_t g_wsa_error = 0;
void ws_WSAStartup(Ctx* c) {
    uint32_t data = Arg(c, 1);  // WSADATA: wVersion, wHighVersion, szDescription[257], szSystemStatus[129], ...
    memset(Ptr<void>(data), 0, 400);
    W16(data, uint16_t(Arg(c, 0)));
    W16(data + 2, 0x0202);
    strcpy(Str(data + 4), "native host: offline");
    Return(c, 0, 2);
}
void ws_WSACleanup(Ctx* c) { Return(c, 0); }
void ws_WSAGetLastError(Ctx* c) { Return(c, g_wsa_error); }
void ws_WSASetLastError(Ctx* c) {
    g_wsa_error = Arg(c, 0);
    Return(c, 0, 1);
}
template <int Args>
void ws_Fail(Ctx* c) {
    g_wsa_error = kWsaENetDown;
    Return(c, kSocketError, Args);
}
template <int Args>
void ws_Null(Ctx* c) {
    g_wsa_error = kWsaENetDown;
    Return(c, 0, Args);
}
void ws_inet_addr(Ctx* c) {
    unsigned a, b, d, e;
    const char* s = Str(Arg(c, 0));
    uint32_t r = 0xffffffffu;  // INADDR_NONE
    if (s && sscanf(s, "%u.%u.%u.%u", &a, &b, &d, &e) == 4) r = a | b << 8 | d << 16 | e << 24;
    Return(c, r, 1);
}
void ws_WSAFDIsSet(Ctx* c) { Return(c, 0, 2); }



// ---- shell32: known folders under C:\Users\Player (native_c next to the game, kernel32.cpp HostRootFor), apart
// from the Windows install's own saves ----
void sh_SHGetFolderPathW(Ctx* c) {
    uint32_t csidl = Arg(c, 1) & 0xff, out = Arg(c, 4);
    const char* path = nullptr;
    switch (csidl) {
        case 0x05: path = "C:\\Users\\Player\\Documents"; break;              // CSIDL_PERSONAL
        case 0x1a: path = "C:\\Users\\Player\\AppData\\Roaming"; break;       // CSIDL_APPDATA
        case 0x1c: path = "C:\\Users\\Player\\AppData\\Local"; break;         // CSIDL_LOCAL_APPDATA
        case 0x23: path = "C:\\ProgramData"; break;                           // CSIDL_COMMON_APPDATA
        case 0x2e: path = "C:\\Users\\Public\\Documents"; break;              // CSIDL_COMMON_DOCUMENTS
        case 0x10: path = "C:\\Users\\Player\\Desktop"; break;                // CSIDL_DESKTOPDIRECTORY
        case 0x27: path = "C:\\Users\\Player\\Pictures"; break;               // CSIDL_MYPICTURES
        case 0x0d: path = "C:\\Users\\Player\\Music"; break;                  // CSIDL_MYMUSIC
        case 0x0e: path = "C:\\Users\\Player\\Videos"; break;                 // CSIDL_MYVIDEO
    }
    if (!path) {
        Log("SHGetFolderPathW(csidl %02x): unknown folder", csidl);
        Return(c, 0x80004005u, 5);
        return;
    }
    // Create it (and its parents) on the host so the game finds it as on a fresh Windows profile.
    std::string host = HostPath(path);
    for (size_t i = 1; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '/') mkdir(host.substr(0, i).c_str(), 0755);
    }
    PutWideString(out, 260, path);
    Return(c, 0, 5);
}
void sh_DragQueryFileW(Ctx* c) { Return(c, 0, 4); }
void sh_ShellExecuteW(Ctx* c) {
    Log("ShellExecuteW(%s): not supported", WideToUtf8(Arg(c, 2)).c_str());
    Return(c, 2, 6);  // <= 32: failed (ERROR_FILE_NOT_FOUND)
}


// ---- dinput8 (dinput.cpp) ----
uint32_t NewDirectInputObject() { return NewDirectInput8(); }
void di_DirectInput8Create(Ctx* c) {
    if (!Arg(c, 3)) {
        Return(c, 0x80004003u, 5);  // E_POINTER
        return;
    }
    W32(Arg(c, 3), NewDirectInputObject());
    Return(c, 0, 5);
}


// ---- gdi32: the window class brush and display queries; there is no GDI drawing ----
void g_CreateSolidBrush(Ctx* c) { Return(c, 0x00110000u, 1); }
void g_DeleteObject(Ctx* c) { Return(c, 1, 1); }
void g_DeleteDC(Ctx* c) { Return(c, 1, 1); }
void g_CreateDCA(Ctx* c) { Return(c, 0x00050001u, 4); }
void g_ExtEscape(Ctx* c) { Return(c, 0, 6); }
void g_GetDeviceCaps(Ctx* c) {
    int w, h;
    ClientSize(&w, &h);
    uint32_t v = 0;
    switch (Arg(c, 1)) {
        case 8: v = uint32_t(w); break;     // HORZRES
        case 10: v = uint32_t(h); break;    // VERTRES
        case 12: v = 32; break;             // BITSPIXEL
        case 14: v = 1; break;              // PLANES
        case 88: case 90: v = 96; break;    // LOGPIXELSX/Y
        case 116: v = 60; break;            // VREFRESH
    }
    Return(c, v, 2);
}

// ---- ole32: OLE for drag and drop of creations onto the window, which a phone does not have ----
void o_OleInitialize(Ctx* c) { Return(c, 0, 1); }
void o_OleUninitialize(Ctx* c) { Return(c, 0); }
void o_RegisterDragDrop(Ctx* c) { Return(c, 0, 2); }
void o_RevokeDragDrop(Ctx* c) { Return(c, 0, 1); }
void o_CoLockObjectExternal(Ctx* c) { Return(c, 0, 3); }
void o_ReleaseStgMedium(Ctx* c) { Return(c, 0, 1); }

// ---- version: no version resources (callers fall back to their own build number) ----
void v_Fail1(Ctx* c) { Return(c, 0, 2); }  // GetFileVersionInfoSize(name, handle*)
void v_Fail4(Ctx* c) { Return(c, 0, 4); }  // GetFileVersionInfo(name, handle, len, data) / VerQueryValue

// ---- imm32: no input method editor (text comes in as WM_CHAR) ----
void i_Zero1(Ctx* c) { Return(c, 0, 1); }
void i_Zero2(Ctx* c) { Return(c, 0, 2); }
void i_Zero3(Ctx* c) { Return(c, 0, 3); }
void i_Zero4(Ctx* c) { Return(c, 0, 4); }
void i_Zero6(Ctx* c) { Return(c, 0, 6); }
void i_One1(Ctx* c) { Return(c, 1, 1); }
void i_One2(Ctx* c) { Return(c, 1, 2); }

// ---- usp10: one script item for the whole run, line breaks after white space ----
// ScriptItemize(chars, n, max_items, control, state, SCRIPT_ITEM* items, int* count); SCRIPT_ITEM is
// {int iCharPos; SCRIPT_ANALYSIS a (4 bytes)}, followed by a sentinel item at n.
void u_ScriptItemize(Ctx* c) {
    uint32_t n = Arg(c, 1), max_items = Arg(c, 2), items = Arg(c, 5), count = Arg(c, 6);
    if (max_items < 2) {
        Return(c, 0x8007000eu, 7);  // E_OUTOFMEMORY
        return;
    }
    W32(items, 0);
    W32(items + 4, 0);
    W32(items + 8, n);
    W32(items + 12, 0);
    if (count) W32(count, 1);
    Return(c, 0, 7);
}
// ScriptBreak(chars, n, analysis, SCRIPT_LOGATTR* attrs): fSoftBreak 1, fWhiteSpace 2, fCharStop 4, fWordStop 8.
void u_ScriptBreak(Ctx* c) {
    uint32_t chars = Arg(c, 0), n = Arg(c, 1), attrs = Arg(c, 3);
    bool prev_space = false;
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t ch = R16(chars + 2 * i);
        bool space = ch == ' ' || ch == '\t' || ch == 0x3000 || ch == 0xa0;
        uint8_t a = 4;
        if (space) a |= 2;
        if (i > 0 && prev_space && !space) a |= 1 | 8;
        if (i == 0) a |= 8;
        W8(attrs + i, a);
        prev_space = space;
    }
    Return(c, 0, 4);
}

// ---- psapi ----
void p_GetModuleInformation(Ctx* c) {
    uint32_t info = Arg(c, 2);  // MODULEINFO: lpBaseOfDll, SizeOfImage, EntryPoint
    W32(info, g_engine.base);
    W32(info + 4, g_engine.size);
    W32(info + 8, g_engine.entry);
    Return(c, 1, 4);
}

// ---- dbghelp: no symbols (the crash reporter writes addresses only) ----
void dh_SymInitialize(Ctx* c) { Return(c, 0, 3); }
void dh_SymSetOptions(Ctx* c) { Return(c, Arg(c, 0), 1); }
void dh_SymCleanup(Ctx* c) { Return(c, 1, 1); }
void dh_SymLoadModule64(Ctx* c) { Return(c, 0, 8); }  // hProcess, hFile, name, module, base (64), size
void dh_SymGetSymFromAddr64(Ctx* c) { Return(c, 0, 5); }  // hProcess, address (64), displacement*, symbol*
void dh_SymGetLineFromAddr64(Ctx* c) { Return(c, 0, 5); }
void dh_UnDecorateSymbolName(Ctx* c) {
    uint32_t name = Arg(c, 0), out = Arg(c, 1), cap = Arg(c, 2);
    std::string n = Str(name);
    if (cap == 0) {
        Return(c, 0, 4);
        return;
    }
    if (n.size() >= cap) n.resize(cap - 1);
    strcpy(Str(out), n.c_str());
    Return(c, uint32_t(n.size()), 4);
}

const ImportDef kStubs[] = {
    {"gdi32.dll", "CreateSolidBrush", g_CreateSolidBrush}, {"gdi32.dll", "DeleteObject", g_DeleteObject},
    {"gdi32.dll", "DeleteDC", g_DeleteDC}, {"gdi32.dll", "CreateDCA", g_CreateDCA},
    {"gdi32.dll", "ExtEscape", g_ExtEscape}, {"gdi32.dll", "GetDeviceCaps", g_GetDeviceCaps},
    {"ole32.dll", "OleInitialize", o_OleInitialize}, {"ole32.dll", "OleUninitialize", o_OleUninitialize},
    {"ole32.dll", "RegisterDragDrop", o_RegisterDragDrop}, {"ole32.dll", "RevokeDragDrop", o_RevokeDragDrop},
    {"ole32.dll", "CoLockObjectExternal", o_CoLockObjectExternal},
    {"ole32.dll", "ReleaseStgMedium", o_ReleaseStgMedium},
    {"version.dll", "GetFileVersionInfoSizeA", v_Fail1}, {"version.dll", "GetFileVersionInfoA", v_Fail4},
    {"version.dll", "GetFileVersionInfoW", v_Fail4}, {"version.dll", "VerQueryValueA", v_Fail4},
    {"version.dll", "VerQueryValueW", v_Fail4},
    {"imm32.dll", "ImmGetContext", i_Zero1}, {"imm32.dll", "ImmReleaseContext", i_One2},
    {"imm32.dll", "ImmDisableTextFrameService", i_One1}, {"imm32.dll", "ImmGetCompositionStringW", i_Zero4},
    {"imm32.dll", "ImmSetCompositionStringW", i_Zero6}, {"imm32.dll", "ImmNotifyIME", i_Zero4},
    {"imm32.dll", "ImmSetOpenStatus", i_Zero2}, {"imm32.dll", "ImmGetOpenStatus", i_Zero1},
    {"imm32.dll", "ImmSimulateHotKey", i_Zero2}, {"imm32.dll", "ImmGetConversionStatus", i_Zero3},
    {"imm32.dll", "ImmSetConversionStatus", i_Zero3}, {"imm32.dll", "ImmGetCandidateListW", i_Zero4},
    {"usp10.dll", "ScriptItemize", u_ScriptItemize}, {"usp10.dll", "ScriptBreak", u_ScriptBreak},
    {"psapi.dll", "GetModuleInformation", p_GetModuleInformation},
    {"dbghelp.dll", "SymInitialize", dh_SymInitialize}, {"dbghelp.dll", "SymSetOptions", dh_SymSetOptions},
    {"dbghelp.dll", "SymCleanup", dh_SymCleanup}, {"dbghelp.dll", "SymLoadModule64", dh_SymLoadModule64},
    {"dbghelp.dll", "SymGetSymFromAddr64", dh_SymGetSymFromAddr64},
    {"dbghelp.dll", "SymGetLineFromAddr64", dh_SymGetLineFromAddr64},
    {"dbghelp.dll", "UnDecorateSymbolName", dh_UnDecorateSymbolName},
    {"dinput8.dll", "DirectInput8Create", di_DirectInput8Create},
    {"shell32.dll", "SHGetFolderPathW", sh_SHGetFolderPathW},
    {"shell32.dll", "DragQueryFileW", sh_DragQueryFileW},
    {"shell32.dll", "ShellExecuteW", sh_ShellExecuteW},
    {"wsock32.dll", "#115", ws_WSAStartup}, {"wsock32.dll", "#116", ws_WSACleanup},
    {"wsock32.dll", "#111", ws_WSAGetLastError}, {"ws2_32.dll", "#112", ws_WSASetLastError},
    {"wsock32.dll", "#23", ws_Fail<3>},   // socket
    {"ws2_32.dll", "WSASocketA", ws_Fail<6>},
    {"ws2_32.dll", "WSAIoctl", ws_Fail<9>},
    {"wsock32.dll", "#3", ws_Fail<1>},    // closesocket
    {"wsock32.dll", "#2", ws_Fail<3>},    // bind
    {"wsock32.dll", "#4", ws_Fail<3>},    // connect
    {"wsock32.dll", "#13", ws_Fail<2>},   // listen
    {"wsock32.dll", "#1", ws_Fail<3>},    // accept
    {"wsock32.dll", "#19", ws_Fail<4>},   // send
    {"wsock32.dll", "#20", ws_Fail<6>},   // sendto
    {"wsock32.dll", "#16", ws_Fail<4>},   // recv
    {"wsock32.dll", "#17", ws_Fail<6>},   // recvfrom
    {"wsock32.dll", "#18", ws_Fail<5>},   // select
    {"wsock32.dll", "#12", ws_Fail<3>},   // ioctlsocket
    {"wsock32.dll", "#22", ws_Fail<2>},   // shutdown
    {"wsock32.dll", "#21", ws_Fail<5>},   // setsockopt
    {"wsock32.dll", "#7", ws_Fail<5>},    // getsockopt
    {"wsock32.dll", "#6", ws_Fail<3>},    // getsockname
    {"wsock32.dll", "#5", ws_Fail<3>},    // getpeername
    {"wsock32.dll", "#52", ws_Null<1>},   // gethostbyname
    {"wsock32.dll", "#10", ws_inet_addr},
    {"wsock32.dll", "#151", ws_WSAFDIsSet},
    {"winmm.dll", "timeBeginPeriod", w_timeBeginPeriod},
    {"winmm.dll", "timeEndPeriod", w_timeEndPeriod},
    {"steam_api.dll", "SteamAPI_Init", s_SteamAPI_Init},
    {"steam_api.dll", "SteamAPI_Shutdown", s_SteamAPI_Shutdown},
    {"steam_api.dll", "SteamApps", s_SteamApps},
    {"steam_api.dll", "ISteamApps::BIsSubscribed", a_True0},
    {"steam_api.dll", "ISteamApps::BIsLowViolence", a_False0},
    {"steam_api.dll", "ISteamApps::BIsCybercafe", a_False0},
    {"steam_api.dll", "ISteamApps::BIsVACBanned", a_False0},
    {"steam_api.dll", "ISteamApps::GetCurrentGameLanguage", a_Language},
    {"steam_api.dll", "ISteamApps::GetAvailableGameLanguages", a_Language},
    {"steam_api.dll", "ISteamApps::BIsSubscribedApp", a_True1},
    {"steam_api.dll", "ISteamApps::BIsDlcInstalled", a_True1},
    {"d3dx9_27.dll", "D3DXMatrixMultiply", d_D3DXMatrixMultiply},
    {"d3dx9_27.dll", "D3DXMatrixMultiplyTranspose", d_D3DXMatrixMultiplyTranspose},
    {"d3dx9_27.dll", "D3DXMatrixTranspose", d_D3DXMatrixTranspose},
    {"d3dx9_27.dll", "D3DXVec3Transform", d_D3DXVec3Transform},
    {"d3dx9_27.dll", "D3DXVec3TransformNormal", d_D3DXVec3TransformNormal},
    {"d3dx9_27.dll", "D3DXCheckVersion", d_D3DXCheckVersion},
    {"d3dx9_27.dll", "D3DXGetVertexShaderProfile", d_D3DXGetVertexShaderProfile},
    {"d3dx9_27.dll", "D3DXGetPixelShaderProfile", d_D3DXGetPixelShaderProfile},
    {"d3dx9_27.dll", "D3DXCompileShader", d_D3DXCompileShader},
};
ImportRegistrar g_stubs(kStubs);

}  // namespace

}  // namespace native
