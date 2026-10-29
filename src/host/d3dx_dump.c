/* d3dx9_27 proxy for the Wine test run: dumps every HLSL source the game passes to D3DXCompileShader (with entry,
 * profile, flags, defines and the reference bytecode from the real d3dx9_27, renamed d3dx9_27_real.dll) into
 * SPORERE_SHADER_DUMP (default: shaders\ in the working directory). The other imports are forwarded in d3dx_dump.def. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef struct { const char* Name; const char* Definition; } D3DXMACRO;
typedef struct Buffer Buffer;
typedef struct {
    HRESULT(WINAPI* QueryInterface)(Buffer*, const void*, void**);
    ULONG(WINAPI* AddRef)(Buffer*);
    ULONG(WINAPI* Release)(Buffer*);
    void*(WINAPI* GetBufferPointer)(Buffer*);
    DWORD(WINAPI* GetBufferSize)(Buffer*);
} BufferVtbl;
struct Buffer { const BufferVtbl* v; };

typedef HRESULT(WINAPI* CompileFn)(const char*, UINT, const D3DXMACRO*, void*, const char*, const char*, DWORD,
                                   Buffer**, Buffer**, void**);
static CompileFn real_compile;
static CRITICAL_SECTION lock;
static char dir[MAX_PATH];

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
    for (const unsigned char* b = p; n--; b++) h = (h ^ *b) * 0x100000001b3ull;
    return h;
}

static void write_file(const char* name, const char* ext, const void* p, size_t n) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s.%s", dir, name, ext);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
}

HRESULT WINAPI D3DXCompileShader(const char* src, UINT len, const D3DXMACRO* defines, void* include, const char* entry,
                                 const char* profile, DWORD flags, Buffer** shader, Buffer** errors, void** constants) {
    HRESULT hr = real_compile(src, len, defines, include, entry, profile, flags, shader, errors, constants);
    uint64_t h = fnv(0xcbf29ce484222325ull, src, len);
    h = fnv(h, entry ? entry : "", entry ? strlen(entry) : 0);
    h = fnv(h, profile ? profile : "", profile ? strlen(profile) : 0);
    for (const D3DXMACRO* d = defines; d && d->Name; d++) {
        h = fnv(h, d->Name, strlen(d->Name));
        if (d->Definition) h = fnv(h, d->Definition, strlen(d->Definition));
    }
    char name[32];
    snprintf(name, sizeof name, "%016llx", (unsigned long long)h);
    EnterCriticalSection(&lock);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s.hlsl", dir, name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        write_file(name, "hlsl", src, len);
        char meta[4096];
        int n = snprintf(meta, sizeof meta, "entry=%s\nprofile=%s\nflags=0x%lx\ninclude=%d\nhr=0x%lx\n",
                         entry ? entry : "", profile ? profile : "", (unsigned long)flags, include != NULL,
                         (unsigned long)hr);
        for (const D3DXMACRO* d = defines; d && d->Name && n < (int)sizeof meta - 256; d++)
            n += snprintf(meta + n, sizeof meta - n, "define=%s=%s\n", d->Name, d->Definition ? d->Definition : "");
        write_file(name, "meta", meta, n);
        if (SUCCEEDED(hr) && shader && *shader)
            write_file(name, "d3dbc", (*shader)->v->GetBufferPointer(*shader), (*shader)->v->GetBufferSize(*shader));
        if (errors && *errors)
            write_file(name, "err", (*errors)->v->GetBufferPointer(*errors), (*errors)->v->GetBufferSize(*errors));
    }
    LeaveCriticalSection(&lock);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void* reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&lock);
        if (!GetEnvironmentVariableA("SPORERE_SHADER_DUMP", dir, sizeof dir)) strcpy(dir, "shaders");
        CreateDirectoryA(dir, NULL);
        HMODULE real = LoadLibraryA("d3dx9_27_real.dll");
        real_compile = real ? (CompileFn)GetProcAddress(real, "D3DXCompileShader") : NULL;
        if (!real_compile) return FALSE;
    }
    return TRUE;
}
