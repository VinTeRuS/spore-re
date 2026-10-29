// Imports of SporeApp.exe with a small, self-contained implementation: winmm timer periods, the Steam API (the GOG build
// still links steam_api.dll but runs without Steam) and the D3DX math the game imports from d3dx9_27. Everything
// else missing logs and stops when called (imports.cpp), so real gaps show up in the log.
#include <cstring>

#include "win.h"

namespace native {

namespace {

// ---- winmm (timeGetTime: misc.cpp) ----
void w_timeBeginPeriod(Ctx* c) { Return(c, 0, 1); }  // TIMERR_NOERROR
void w_timeEndPeriod(Ctx* c) { Return(c, 0, 1); }

// ---- steam_api: no Steam; SteamApps() == nullptr is what the game sees without the client ----
void s_SteamAPI_Init(Ctx* c) { Return(c, 0); }
void s_SteamAPI_Shutdown(Ctx* c) { Return(c, 0); }
void s_SteamApps(Ctx* c) { Return(c, 0); }

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

const ImportDef kStubs[] = {
    {"winmm.dll", "timeBeginPeriod", w_timeBeginPeriod},
    {"winmm.dll", "timeEndPeriod", w_timeEndPeriod},
    {"steam_api.dll", "SteamAPI_Init", s_SteamAPI_Init},
    {"steam_api.dll", "SteamAPI_Shutdown", s_SteamAPI_Shutdown},
    {"steam_api.dll", "SteamApps", s_SteamApps},
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
