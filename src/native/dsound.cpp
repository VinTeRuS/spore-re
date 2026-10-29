// DirectSound for the native host, on SDL audio.
//
// SporeApp.exe gets IDirectSound from dsound.dll #1 (DirectSoundCreate), creates a primary buffer for the output format
// and secondary buffers that it fills with Lock/Unlock and plays (one-shot or looping). Secondary buffers keep their
// samples in guest memory; the SDL audio callback mixes every playing buffer into the device format, with volume,
// pan and frequency, and moves its play cursor. Position notifications (IDirectSoundNotify) signal the game's events
// when the cursor passes their offsets, which is how streamed music and speech refill their buffers.
//
// Guest objects are 16-byte blocks: [0] guest vtable (import thunks), [4] index into g_buffers (0 = IDirectSound).
#include <SDL2/SDL.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "win.h"

namespace native {

namespace {

constexpr uint32_t kDS_OK = 0, kE_NoInterface = 0x80004002u, kE_Pointer = 0x80004003u,
                   kDSERR_InvalidParam = 0x80070057u;
constexpr uint32_t kDsbcapsPrimary = 0x1, kDsbplayLooping = 0x1;
constexpr uint32_t kDsbstatusPlaying = 0x1, kDsbstatusLooping = 0x4;
constexpr uint32_t kNotifyStop = 0xffffffffu;  // DSBPN_OFFSETSTOP

struct Guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
    bool operator==(const Guid& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
// IID_IDirectSoundNotify {b0210783-89cd-11d0-af08-00a0c925cd16}
constexpr Guid kIidNotify = {0xb0210783, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
// IID_IDirectSoundBuffer {279afa85-4981-11ce-a521-0020af0be560}, IID_IDirectSoundBuffer8 {6825a449-...}
constexpr Guid kIidBuffer = {0x279afa85, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
constexpr Guid kIidBuffer8 = {0x6825a449, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};

struct Notify {
    uint32_t offset, event;
};

struct Buffer {
    bool primary = false;
    uint32_t data = 0, bytes = 0;  // guest memory
    uint32_t rate = 22050, channels = 1, bits = 16;
    std::atomic<bool> playing{false}, looping{false};
    std::atomic<uint32_t> frequency{0};  // 0: the format's rate
    std::atomic<int32_t> volume{0}, pan{0};  // hundredths of dB
    double pos = 0;                          // play cursor in bytes (fractional for resampling); g_mix_mutex
    std::vector<Notify> notifies;            // g_mix_mutex
    int refs = 1;
    uint32_t guest = 0, notify_guest = 0;
};

std::mutex g_mix_mutex;
std::vector<std::unique_ptr<Buffer>> g_buffers;  // index 0 unused (the IDirectSound object)
uint32_t g_vtable_ds = 0, g_vtable_buf = 0, g_vtable_notify = 0;
SDL_AudioDeviceID g_device = 0;
int g_out_rate = 48000;

Buffer* Buf(uint32_t guest) {
    uint32_t i = R32(guest + 4);
    std::lock_guard<std::mutex> lock(g_mix_mutex);
    return i < g_buffers.size() ? g_buffers[i].get() : nullptr;
}

float Gain(int32_t hundredths_db) {
    if (hundredths_db <= -10000) return 0.0f;
    return std::pow(10.0f, float(hundredths_db) / 2000.0f);
}

// One sample of a buffer at byte offset `off` (frame-aligned), channel `ch`, as -1..1.
float Sample(const Buffer& b, uint32_t off, uint32_t ch) {
    if (b.channels == 1) ch = 0;
    if (b.bits == 8) return (float(R8(b.data + off + ch)) - 128.0f) / 128.0f;
    return float(int16_t(R16(b.data + off + 2 * ch))) / 32768.0f;
}

// Fires the notifications between two cursor positions (exclusive from, inclusive to), wrapping at the end.
void FireBetween(const Buffer& b, uint32_t from, uint32_t to, bool wrapped) {
    for (const Notify& n : b.notifies) {
        if (n.offset == kNotifyStop) continue;
        bool hit = wrapped ? (n.offset > from || n.offset <= to) : (n.offset > from && n.offset <= to);
        if (hit) SignalEventHandle(n.event);
    }
}

void Mix(void*, Uint8* stream, int len) {
    int frames = len / int(2 * sizeof(float));
    float* out = reinterpret_cast<float*>(stream);
    memset(out, 0, size_t(len));
    std::lock_guard<std::mutex> lock(g_mix_mutex);
    for (auto& bp : g_buffers) {
        Buffer* b = bp.get();
        if (!b || b->primary || !b->playing || !b->bytes) continue;
        uint32_t frame_bytes = b->channels * (b->bits / 8);
        uint32_t rate = b->frequency ? b->frequency.load() : b->rate;
        double step = double(rate) / double(g_out_rate) * frame_bytes;
        float gain = Gain(b->volume);
        int32_t pan = b->pan;
        float gl = gain * (pan > 0 ? Gain(-pan) : 1.0f), gr = gain * (pan < 0 ? Gain(pan) : 1.0f);
        uint32_t start = uint32_t(b->pos);
        bool wrapped = false;
        for (int i = 0; i < frames; ++i) {
            uint32_t off = uint32_t(b->pos) / frame_bytes * frame_bytes;
            out[2 * i] += Sample(*b, off, 0) * gl;
            out[2 * i + 1] += Sample(*b, off, 1) * gr;
            b->pos += step;
            if (b->pos >= b->bytes) {
                if (!b->looping) {
                    b->playing = false;
                    b->pos = 0;
                    for (const Notify& n : b->notifies)
                        if (n.offset == kNotifyStop) SignalEventHandle(n.event);
                    break;
                }
                b->pos -= b->bytes;
                wrapped = true;
            }
        }
        if (!b->notifies.empty()) FireBetween(*b, start, uint32_t(b->pos), wrapped);
    }
    for (int i = 0; i < 2 * frames; ++i) out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
}

void OpenDevice() {
    if (g_device) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        Log("dsound: SDL audio: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = Mix;
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_device) {
        Log("dsound: no audio device (%s); running silent", SDL_GetError());
        return;
    }
    g_out_rate = have.freq;
    Log("dsound: SDL audio %d Hz, %d frames per callback", have.freq, have.samples);
    SDL_PauseAudioDevice(g_device, 0);
}

uint32_t NewGuest(uint32_t vtable, uint32_t index) {
    uint32_t g = HeapAlloc(16);
    W32(g, vtable);
    W32(g + 4, index);
    return g;
}

#define M(name) void name(Ctx* c)

// ---- IUnknown (shared) ----
M(QueryInterface) {
    uint32_t self = Arg(c, 0), riid = Arg(c, 1), out = Arg(c, 2);
    if (!out) {
        Return(c, kE_Pointer, 3);
        return;
    }
    const Guid& iid = *Ptr<Guid>(riid);
    uint32_t index = R32(self + 4);
    if (index && iid == kIidNotify) {
        Buffer* b = Buf(self);
        if (!b->notify_guest) b->notify_guest = NewGuest(g_vtable_notify, index);
        W32(out, b->notify_guest);
        Return(c, kDS_OK, 3);
        return;
    }
    if (index && (iid == kIidBuffer || iid == kIidBuffer8)) {
        W32(out, self);
        Return(c, kDS_OK, 3);
        return;
    }
    Log("dsound: QueryInterface({%08x-%04x-...}) on %s: not supported", iid.d1, iid.d2, index ? "buffer" : "device");
    W32(out, 0);
    Return(c, kE_NoInterface, 3);
}
M(AddRef) {
    if (Buffer* b = R32(Arg(c, 0) + 4) ? Buf(Arg(c, 0)) : nullptr) ++b->refs;
    Return(c, 1, 1);
}
M(Release) {
    uint32_t index = R32(Arg(c, 0) + 4);
    if (index) {
        std::lock_guard<std::mutex> lock(g_mix_mutex);
        Buffer* b = index < g_buffers.size() ? g_buffers[index].get() : nullptr;
        if (b && --b->refs == 0) {
            if (b->data) HeapFree(b->data);
            g_buffers[index].reset();
        }
    }
    Return(c, 0, 1);
}

// ---- IDirectSound ----
// CreateSoundBuffer(this, const DSBUFFERDESC* desc, IDirectSoundBuffer** out, IUnknown* outer)
// DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat (WAVEFORMATEX: tag, channels(16),
// rate(32), avg bytes(32), block align(16), bits(16), cbSize(16)).
M(CreateSoundBuffer) {
    uint32_t desc = Arg(c, 1), out = Arg(c, 2);
    if (!desc || !out) {
        Return(c, kDSERR_InvalidParam, 4);
        return;
    }
    auto b = std::make_unique<Buffer>();
    uint32_t flags = R32(desc + 4);
    b->primary = flags & kDsbcapsPrimary;
    if (uint32_t wfx = R32(desc + 16)) {
        b->channels = R16(wfx + 2);
        b->rate = R32(wfx + 4);
        b->bits = R16(wfx + 14);
    }
    if (!b->primary) {
        b->bytes = R32(desc + 8);
        b->data = HeapAlloc(b->bytes ? b->bytes : 1);
        memset(Ptr<void>(b->data), b->bits == 8 ? 0x80 : 0, b->bytes);
    }
    OpenDevice();
    uint32_t index;
    {
        std::lock_guard<std::mutex> lock(g_mix_mutex);
        index = uint32_t(g_buffers.size());
        g_buffers.push_back(std::move(b));
    }
    uint32_t g = NewGuest(g_vtable_buf, index);
    g_buffers[index]->guest = g;
    W32(out, g);
    Return(c, kDS_OK, 4);
}
M(GetCaps) {
    uint32_t caps = Arg(c, 1);  // DSCAPS: dwSize first; report a capable software device (all zero is acceptable)
    uint32_t size = R32(caps);
    memset(Ptr<uint8_t>(caps) + 4, 0, size > 4 ? size - 4 : 0);
    W32(caps + 4, 0x00000f00u);  // DSCAPS_SECONDARY{MONO,STEREO,8BIT,16BIT}
    W32(caps + 8, 100);          // dwMinSecondarySampleRate
    W32(caps + 12, 200000);      // dwMaxSecondarySampleRate
    Return(c, kDS_OK, 2);
}
M(DuplicateSoundBuffer) {
    Log("dsound: DuplicateSoundBuffer not supported");
    Return(c, kDSERR_InvalidParam, 3);
}
M(SetCooperativeLevel) { Return(c, kDS_OK, 3); }
M(Compact) { Return(c, kDS_OK, 1); }
M(GetSpeakerConfig) {
    W32(Arg(c, 1), 0x00000004u);  // DSSPEAKER_STEREO
    Return(c, kDS_OK, 2);
}
M(SetSpeakerConfig) { Return(c, kDS_OK, 2); }
M(DsInitialize) { Return(c, kDS_OK, 2); }

// ---- IDirectSoundBuffer ----
M(BufGetCaps) {
    Buffer* b = Buf(Arg(c, 0));
    uint32_t caps = Arg(c, 1);  // DSBCAPS: dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead
    W32(caps + 4, b->primary ? kDsbcapsPrimary : 0x8u);  // DSBCAPS_STATIC-free software buffer
    W32(caps + 8, b->bytes);
    W32(caps + 12, 0);
    W32(caps + 16, 0);
    Return(c, kDS_OK, 2);
}
M(GetCurrentPosition) {
    Buffer* b = Buf(Arg(c, 0));
    uint32_t play = 0, write = 0;
    if (b && b->bytes) {
        std::lock_guard<std::mutex> lock(g_mix_mutex);
        uint32_t frame_bytes = b->channels * (b->bits / 8);
        play = uint32_t(b->pos) / frame_bytes * frame_bytes;
        // The write cursor leads by about one mixer period (the part already handed to SDL).
        uint32_t lead = uint32_t(uint64_t(b->rate) * frame_bytes * 1024 / uint32_t(g_out_rate)) / frame_bytes * frame_bytes;
        write = (play + lead) % b->bytes;
    }
    if (Arg(c, 1)) W32(Arg(c, 1), play);
    if (Arg(c, 2)) W32(Arg(c, 2), write);
    Return(c, kDS_OK, 3);
}
M(GetFormat) {
    Buffer* b = Buf(Arg(c, 0));
    uint32_t wfx = Arg(c, 1), size = Arg(c, 2);
    if (wfx && size >= 16) {
        W16(wfx, 1);  // WAVE_FORMAT_PCM
        W16(wfx + 2, uint16_t(b->channels));
        W32(wfx + 4, b->rate);
        W32(wfx + 8, b->rate * b->channels * b->bits / 8);
        W16(wfx + 12, uint16_t(b->channels * b->bits / 8));
        W16(wfx + 14, uint16_t(b->bits));
        if (size >= 18) W16(wfx + 16, 0);
    }
    if (Arg(c, 3)) W32(Arg(c, 3), 18);
    Return(c, kDS_OK, 4);
}
M(GetVolume) {
    W32(Arg(c, 1), uint32_t(Buf(Arg(c, 0))->volume.load()));
    Return(c, kDS_OK, 2);
}
M(GetPan) {
    W32(Arg(c, 1), uint32_t(Buf(Arg(c, 0))->pan.load()));
    Return(c, kDS_OK, 2);
}
M(GetFrequency) {
    Buffer* b = Buf(Arg(c, 0));
    W32(Arg(c, 1), b->frequency ? b->frequency.load() : b->rate);
    Return(c, kDS_OK, 2);
}
M(GetStatus) {
    Buffer* b = Buf(Arg(c, 0));
    uint32_t s = (b->playing ? kDsbstatusPlaying : 0) | (b->playing && b->looping ? kDsbstatusLooping : 0);
    W32(Arg(c, 1), b->primary ? kDsbstatusPlaying | kDsbstatusLooping : s);
    Return(c, kDS_OK, 2);
}
M(BufInitialize) { Return(c, kDS_OK, 3); }
// Lock(this, offset, bytes, void** p1, DWORD* n1, void** p2, DWORD* n2, flags): the region wraps at the end.
M(Lock) {
    Buffer* b = Buf(Arg(c, 0));
    uint32_t offset = Arg(c, 1), bytes = Arg(c, 2), flags = Arg(c, 7);
    if (!b || !b->bytes) {
        Return(c, kDSERR_InvalidParam, 8);
        return;
    }
    if (flags & 0x1) {  // DSBLOCK_FROMWRITECURSOR
        std::lock_guard<std::mutex> lock(g_mix_mutex);
        offset = uint32_t(b->pos);
    }
    if (flags & 0x2) bytes = b->bytes;  // DSBLOCK_ENTIREBUFFER
    offset %= b->bytes;
    if (bytes > b->bytes) bytes = b->bytes;
    uint32_t first = std::min(bytes, b->bytes - offset);
    W32(Arg(c, 3), b->data + offset);
    W32(Arg(c, 4), first);
    if (Arg(c, 5)) W32(Arg(c, 5), bytes > first ? b->data : 0);
    if (Arg(c, 6)) W32(Arg(c, 6), bytes - first);
    Return(c, kDS_OK, 8);
}
M(Play) {
    Buffer* b = Buf(Arg(c, 0));
    if (b) {
        b->looping = (Arg(c, 3) & kDsbplayLooping) != 0;
        b->playing = true;
    }
    Return(c, kDS_OK, 4);
}
M(SetCurrentPosition) {
    Buffer* b = Buf(Arg(c, 0));
    if (b && b->bytes) {
        std::lock_guard<std::mutex> lock(g_mix_mutex);
        b->pos = Arg(c, 1) % b->bytes;
    }
    Return(c, kDS_OK, 2);
}
M(SetFormat) {
    Buffer* b = Buf(Arg(c, 0));
    if (uint32_t wfx = Arg(c, 1)) {
        b->channels = R16(wfx + 2);
        b->rate = R32(wfx + 4);
        b->bits = R16(wfx + 14);
    }
    Return(c, kDS_OK, 2);
}
M(SetVolume) {
    Buf(Arg(c, 0))->volume = int32_t(Arg(c, 1));
    Return(c, kDS_OK, 2);
}
M(SetPan) {
    Buf(Arg(c, 0))->pan = int32_t(Arg(c, 1));
    Return(c, kDS_OK, 2);
}
M(SetFrequency) {
    Buf(Arg(c, 0))->frequency = Arg(c, 1);  // DSBFREQUENCY_ORIGINAL (0) restores the format's rate
    Return(c, kDS_OK, 2);
}
M(Stop) {
    if (Buffer* b = Buf(Arg(c, 0))) b->playing = false;
    Return(c, kDS_OK, 1);
}
M(Unlock) { Return(c, kDS_OK, 5); }
M(Restore) { Return(c, kDS_OK, 1); }

// ---- IDirectSoundNotify ----
// SetNotificationPositions(this, count, const DSBPOSITIONNOTIFY* positions): {dwOffset, hEventNotify} pairs.
M(SetNotificationPositions) {
    Buffer* b = Buf(Arg(c, 0));
    uint32_t n = Arg(c, 1), p = Arg(c, 2);
    std::lock_guard<std::mutex> lock(g_mix_mutex);
    b->notifies.clear();
    for (uint32_t i = 0; i < n; ++i) b->notifies.push_back({R32(p + 8 * i), R32(p + 8 * i + 4)});
    Return(c, kDS_OK, 3);
}

#undef M

const ImportFn kDsMethods[] = {
    QueryInterface, AddRef, Release, CreateSoundBuffer, GetCaps, DuplicateSoundBuffer, SetCooperativeLevel,
    Compact, GetSpeakerConfig, SetSpeakerConfig, DsInitialize,
};
const ImportFn kBufMethods[] = {
    QueryInterface, AddRef, Release, BufGetCaps, GetCurrentPosition, GetFormat, GetVolume, GetPan, GetFrequency,
    GetStatus, BufInitialize, Lock, Play, SetCurrentPosition, SetFormat, SetVolume, SetPan, SetFrequency, Stop,
    Unlock, Restore,
};
const ImportFn kNotifyMethods[] = {QueryInterface, AddRef, Release, SetNotificationPositions};

std::vector<ImportDef> g_defs;

uint32_t BuildVtable(const char* prefix, const ImportFn* fns, size_t n) {
    uint32_t vt = HeapAlloc(uint32_t(4 * n));
    for (size_t k = 0; k < n; ++k) {
        std::string name = std::string(prefix) + "#" + std::to_string(k);
        g_defs.push_back({"dsound.com", strdup(name.c_str()), fns[k]});
        RegisterImports(&g_defs.back(), 1);
        W32(vt + uint32_t(4 * k), ImportThunk("dsound.com", g_defs.back().name));
    }
    return vt;
}

void Init() {
    static std::once_flag once;
    std::call_once(once, [] {
        constexpr size_t kD = sizeof(kDsMethods) / sizeof(ImportFn), kB = sizeof(kBufMethods) / sizeof(ImportFn),
                         kN = sizeof(kNotifyMethods) / sizeof(ImportFn);
        g_defs.reserve(kD + kB + kN);  // ImportDef pointers must stay valid
        g_vtable_ds = BuildVtable("IDirectSound", kDsMethods, kD);
        g_vtable_buf = BuildVtable("IDirectSoundBuffer", kBufMethods, kB);
        g_vtable_notify = BuildVtable("IDirectSoundNotify", kNotifyMethods, kN);
        g_buffers.emplace_back();  // index 0: the device object
    });
}

// DirectSoundCreate(const GUID* device, IDirectSound** out, IUnknown* outer)
void DirectSoundCreate(Ctx* c) {
    if (!Arg(c, 1)) {
        Return(c, kDSERR_InvalidParam, 3);
        return;
    }
    if (SettingInt("audio", 1) == 0) {  // audio = 0: no device, the game runs silent
        W32(Arg(c, 1), 0);
        Return(c, 0x88780078u, 3);  // DSERR_NODRIVER
        return;
    }
    Init();
    W32(Arg(c, 1), NewGuest(g_vtable_ds, 0));
    Return(c, kDS_OK, 3);
}
// GetDeviceID(const GUID* src, GUID* dest): the game asks for the default playback device's GUID first and only
// creates the device when that succeeds. One device: a fixed GUID.
void GetDeviceID(Ctx* c) {
    static const Guid kDevice = {0x53505245, 0x0001, 0x0001, {'S', 'D', 'L', 'A', 'U', 'D', 'I', 'O'}};
    if (!Arg(c, 1)) {
        Return(c, kDSERR_InvalidParam, 2);
        return;
    }
    memcpy(Ptr<void>(Arg(c, 1)), &kDevice, sizeof(kDevice));
    Return(c, kDS_OK, 2);
}

const ImportDef kDsound[] = {
    {"dsound.dll", "#1", DirectSoundCreate},
    {"dsound.dll", "#9", GetDeviceID},
};
ImportRegistrar g_dsound(kDsound);

}  // namespace

}  // namespace native
