// Snapshot of the D3D9 state at a draw, as text: the same code runs in the native host (d3d9.cpp, trace_hm_draws) and
// in the Wine-side proxy d3d9.dll (d3d9_trace_proxy.cpp) around the original SporeApp.exe, so the two logs can be
// compared line by line to find where the port feeds the GPU something different.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace d3d9trace {

inline uint32_t Fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}

template <typename Shader>
inline uint32_t ShaderHash(Shader* s) {
    if (!s) return 0;
    UINT n = 0;
    s->GetFunction(nullptr, &n);
    std::string b(n, '\0');
    if (n) s->GetFunction(&b[0], &n);
    return Fnv(b.data(), b.size());
}

inline void Add(std::string& out, const char* fmt, double a, double b, double c, double d) {
    char t[96];
    snprintf(t, sizeof t, fmt, a, b, c, d);
    out += t;
}

// Lines describing the state of `dev` for draw number `k`; `call` names the draw and its arguments.
inline std::string Snapshot(IDirect3DDevice9* dev, int k, const char* call) {
    std::string out;
    char t[256];
    snprintf(t, sizeof t, "draw %d %s\n", k, call);
    out += t;
    float c[64];
    memset(c, 0, sizeof c);
    dev->GetVertexShaderConstantF(0, c, 16);
    out += "  vs";
    for (int i = 0; i < 16; ++i) Add(out, " (%.6g %.6g %.6g %.6g)", c[4 * i], c[4 * i + 1], c[4 * i + 2], c[4 * i + 3]);
    out += "\n";
    memset(c, 0, sizeof c);
    dev->GetPixelShaderConstantF(0, c, 16);
    out += "  ps";
    for (int i = 0; i < 16; ++i) Add(out, " (%.6g %.6g %.6g %.6g)", c[4 * i], c[4 * i + 1], c[4 * i + 2], c[4 * i + 3]);
    out += "\n";
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);
    snprintf(t, sizeof t, "  vp %u %u %u %u %.3g %.3g  shaders vs %08x ps %08x\n", unsigned(vp.X), unsigned(vp.Y),
             unsigned(vp.Width), unsigned(vp.Height), vp.MinZ, vp.MaxZ, ShaderHash(vs), ShaderHash(ps));
    out += t;
    if (vs) vs->Release();
    if (ps) ps->Release();
    for (DWORD s = 0; s < 4; ++s) {
        IDirect3DBaseTexture9* tex = nullptr;
        if (FAILED(dev->GetTexture(s, &tex)) || !tex) continue;
        D3DSURFACE_DESC d{};
        if (tex->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d);
        DWORD lv = tex->GetLevelCount();
        tex->Release();
        DWORD st[13] = {};
        for (int i = 1; i <= 12; ++i) dev->GetSamplerState(s, D3DSAMPLERSTATETYPE(i), &st[i]);
        snprintf(t, sizeof t, "  t%u fmt %u %ux%u lv %u addr %u %u %u border %08x filt %u %u %u bias %08x maxmip %u\n",
                 unsigned(s), unsigned(d.Format), unsigned(d.Width), unsigned(d.Height), unsigned(lv), unsigned(st[1]),
                 unsigned(st[2]), unsigned(st[3]), unsigned(st[4]), unsigned(st[5]), unsigned(st[6]), unsigned(st[7]),
                 unsigned(st[8]), unsigned(st[9]));
        out += t;
    }
    static const D3DRENDERSTATETYPE rs[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
                                            D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_ALPHATESTENABLE,
                                            D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE,
                                            D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FILLMODE};
    out += "  rs";
    for (D3DRENDERSTATETYPE r : rs) {
        DWORD v = 0;
        dev->GetRenderState(r, &v);
        snprintf(t, sizeof t, " %u=%x", unsigned(r), unsigned(v));
        out += t;
    }
    out += "\n";
    return out;
}

// The vertices (stream 0, as floats) and indices a DrawIndexedPrimitive reads, straight from the buffers the GPU
// uses (read-only locks).
inline std::string Geometry(IDirect3DDevice9* dev, INT base, UINT min, UINT num, UINT start, UINT count) {
    std::string out;
    char t[64];
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (SUCCEEDED(dev->GetStreamSource(0, &vb, &off, &stride)) && vb && stride) {
        void* p = nullptr;
        if (SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_READONLY)) && p) {
            for (UINT v = 0; v < num && v < 12; ++v) {
                const float* f = reinterpret_cast<const float*>(static_cast<const char*>(p) + off +
                                                                size_t(base + min + v) * stride);
                out += "  v";
                for (UINT w = 0; w < stride / 4 && w < 16; ++w) {
                    snprintf(t, sizeof t, " %.5g", f[w]);
                    out += t;
                }
                out += "\n";
            }
            vb->Unlock();
        }
        vb->Release();
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(dev->GetIndices(&ib)) && ib) {
        D3DINDEXBUFFER_DESC d{};
        ib->GetDesc(&d);
        void* p = nullptr;
        if (SUCCEEDED(ib->Lock(0, 0, &p, D3DLOCK_READONLY)) && p) {
            out += "  i";
            for (UINT i = 0; i < count * 3 && i < 30; ++i) {
                unsigned v = d.Format == D3DFMT_INDEX32 ? static_cast<const uint32_t*>(p)[start + i]
                                                        : static_cast<const uint16_t*>(p)[start + i];
                snprintf(t, sizeof t, " %u", v);
                out += t;
            }
            out += "\n";
            ib->Unlock();
        }
        ib->Release();
    }
    return out;
}

// True when render target 0 is `w` x `h`.
inline bool TargetIs(IDirect3DDevice9* dev, UINT w, UINT h) {
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC d{};
    rt->GetDesc(&d);
    rt->Release();
    return d.Width == w && d.Height == h;
}

}  // namespace d3d9trace
