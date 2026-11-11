// d3d9.dll for the original SporeApp.exe under Wine: loads the real d3d9 (DXVK, system32) and logs the state of the
// first draws into 512x512 render targets (planet height maps) with d3d9_trace.h, the same text the native host
// writes (trace_hm_draws), for a line-by-line comparison. Debugging tool only; put it next to SporeApp.exe of a test
// folder. D3D9TRACE_DRAWS sets how many draws (default 40), output goes to d3d9trace.log in the working directory.
//   i686-w64-mingw32-g++ -O1 -shared -static -o d3d9.dll d3d9_trace_proxy.cpp d3d9_trace_proxy.def
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdlib>

#include "d3d9_trace.h"

namespace {

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using DrawIndexedFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

CreateDeviceFn g_create_device = nullptr;
DrawIndexedFn g_draw_indexed = nullptr;
FILE* g_log = nullptr;
int g_limit = 40, g_count = 0;

void Patch(void** vtable, int index, void* fn, void** old) {
    DWORD prot;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &prot);
    if (old && !*old) *old = vtable[index];
    vtable[index] = fn;
    VirtualProtect(&vtable[index], sizeof(void*), prot, &prot);
}

HRESULT STDMETHODCALLTYPE DrawIndexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT min, UINT num,
                                      UINT start, UINT count) {
    if (g_count < g_limit && d3d9trace::TargetIs(dev, 512, 512)) {
        char call[128];
        snprintf(call, sizeof call, "DrawIndexedPrimitive %d %d %u %u %u %u", int(type), base, min, num, start, count);
        std::string s = d3d9trace::Snapshot(dev, g_count++, call) + d3d9trace::Geometry(dev, base, min, num, start, count);
        if (!g_log) g_log = fopen("d3d9trace.log", "w");
        if (g_log) {
            fputs(s.c_str(), g_log);
            fflush(g_log);
        }
    }
    return g_draw_indexed(dev, type, base, min, num, start, count);
}

using GetRtdFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
GetRtdFn g_get_rtd = nullptr;
int g_rtd = 0;

// The same files as the native host's dump_rtd: rtd_<n>_<w>x<h>_<format>.raw, rows packed (A8R8G8B8 only here).
HRESULT STDMETHODCALLTYPE GetRtd(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst) {
    HRESULT hr = g_get_rtd(dev, src, dst);
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && g_rtd < 60 && SUCCEEDED(dst->GetDesc(&d)) && d.Format == D3DFMT_A8R8G8B8 &&
        SUCCEEDED(dst->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        char name[96];
        snprintf(name, sizeof name, "rtd_%03d_%ux%u_%u.raw", g_rtd++, d.Width, d.Height, unsigned(d.Format));
        if (FILE* f = fopen(name, "wb")) {
            for (UINT y = 0; y < d.Height; ++y)
                fwrite(static_cast<const char*>(lr.pBits) + y * lr.Pitch, 1, d.Width * 4, f);
            fclose(f);
        }
        dst->UnlockRect();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                       D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = g_create_device(d3d, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        Patch(*reinterpret_cast<void***>(*out), 82, (void*)DrawIndexed, (void**)&g_draw_indexed);
        Patch(*reinterpret_cast<void***>(*out), 32, (void*)GetRtd, (void**)&g_get_rtd);
    }
    return hr;
}

}  // namespace

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (const char* n = getenv("D3D9TRACE_DRAWS")) g_limit = atoi(n);
    static HMODULE real = LoadLibraryA("C:\\windows\\system32\\d3d9.dll");
    auto create = real ? reinterpret_cast<CreateFn>(GetProcAddress(real, "Direct3DCreate9")) : nullptr;
    IDirect3D9* d3d = create ? create(sdk) : nullptr;
    if (d3d) Patch(*reinterpret_cast<void***>(d3d), 16, (void*)CreateDevice, (void**)&g_create_device);
    return d3d;
}
