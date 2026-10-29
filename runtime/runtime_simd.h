/* Packed integer (MMX, SSE2) and approximate float helpers for recompiled code (recomp.py simd()). A vector is n
 * bytes (8 for mm, 16 for xmm) handled as little-endian lanes; every helper takes the destination register's bytes
 * and a copy of the source, so dst may alias src. Cross-lane operations (packs, unpacks, byte shifts) work on the
 * whole vector exactly as the instruction does for its width. */
#ifndef X86RECOMP_RUNTIME_SIMD_H
#define X86RECOMP_RUNTIME_SIMD_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#else
#include <time.h>
#endif

#define VLANE_GET(T, p, i) ({ T v_; memcpy(&v_, (const uint8_t*)(p) + (i) * sizeof(T), sizeof(T)); v_; })
#define VLANE_SET(T, p, i, v) do { T v_ = (T)(v); memcpy((uint8_t*)(p) + (i) * sizeof(T), &v_, sizeof(T)); } while (0)

static inline int32_t VSat(int64_t v, int64_t lo, int64_t hi) { return (int32_t)(v < lo ? lo : v > hi ? hi : v); }

/* Lane-wise binary operation: r = f(a, b) over lanes of type T (a = destination, b = source). */
#define VBIN(name, T, expr)                                                                  \
    static inline void name(uint8_t* d, const uint8_t* s, int n) {                          \
        for (int i = 0; i < n / (int)sizeof(T); ++i) {                                      \
            T a = VLANE_GET(T, d, i), b = VLANE_GET(T, s, i);                               \
            VLANE_SET(T, d, i, (expr));                                                     \
        }                                                                                   \
    }
VBIN(VPaddB, uint8_t, a + b)
VBIN(VPaddW, uint16_t, a + b)
VBIN(VPaddD, uint32_t, a + b)
VBIN(VPaddQ, uint64_t, a + b)
VBIN(VPsubB, uint8_t, a - b)
VBIN(VPsubW, uint16_t, a - b)
VBIN(VPsubD, uint32_t, a - b)
VBIN(VPsubQ, uint64_t, a - b)
VBIN(VPaddSB, int8_t, VSat((int64_t)a + b, -128, 127))
VBIN(VPaddSW, int16_t, VSat((int64_t)a + b, -32768, 32767))
VBIN(VPaddUSB, uint8_t, VSat((int64_t)a + b, 0, 255))
VBIN(VPaddUSW, uint16_t, VSat((int64_t)a + b, 0, 65535))
VBIN(VPsubSB, int8_t, VSat((int64_t)a - b, -128, 127))
VBIN(VPsubSW, int16_t, VSat((int64_t)a - b, -32768, 32767))
VBIN(VPsubUSB, uint8_t, VSat((int64_t)a - b, 0, 255))
VBIN(VPsubUSW, uint16_t, VSat((int64_t)a - b, 0, 65535))
VBIN(VPmullW, int16_t, (int16_t)((int32_t)a * b))
VBIN(VPmulhW, int16_t, (int16_t)(((int32_t)a * b) >> 16))
VBIN(VPmulhUW, uint16_t, (uint16_t)(((uint32_t)a * b) >> 16))
VBIN(VPcmpeqB, uint8_t, a == b ? 0xffu : 0u)
VBIN(VPcmpeqW, uint16_t, a == b ? 0xffffu : 0u)
VBIN(VPcmpeqD, uint32_t, a == b ? 0xffffffffu : 0u)
VBIN(VPcmpgtB, int8_t, a > b ? -1 : 0)
VBIN(VPcmpgtW, int16_t, a > b ? -1 : 0)
VBIN(VPcmpgtD, int32_t, a > b ? -1 : 0)
VBIN(VPminUB, uint8_t, a < b ? a : b)
VBIN(VPmaxUB, uint8_t, a > b ? a : b)
VBIN(VPminSW, int16_t, a < b ? a : b)
VBIN(VPmaxSW, int16_t, a > b ? a : b)
VBIN(VPavgB, uint8_t, ((uint32_t)a + b + 1u) >> 1)
VBIN(VPavgW, uint16_t, ((uint32_t)a + b + 1u) >> 1)
VBIN(VPand, uint64_t, a & b)
VBIN(VPandn, uint64_t, ~a & b)
VBIN(VPor, uint64_t, a | b)
VBIN(VPxor, uint64_t, a ^ b)

static inline void VPmaddWD(uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n / 4; ++i) {
        int32_t r = (int32_t)VLANE_GET(int16_t, d, 2 * i) * VLANE_GET(int16_t, s, 2 * i) +
                    (int32_t)VLANE_GET(int16_t, d, 2 * i + 1) * VLANE_GET(int16_t, s, 2 * i + 1);
        VLANE_SET(int32_t, d, i, r);  /* only 0x8000 * 0x8000 * 2 wraps, as on hardware */
    }
}

static inline void VPmulUDQ(uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n / 8; ++i)
        VLANE_SET(uint64_t, d, i, (uint64_t)VLANE_GET(uint32_t, d, 2 * i) * VLANE_GET(uint32_t, s, 2 * i));
}

static inline void VPsadBW(uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n / 8; ++i) {
        uint32_t sum = 0;
        for (int k = 0; k < 8; ++k) {
            int v = (int)d[8 * i + k] - (int)s[8 * i + k];
            sum += (uint32_t)(v < 0 ? -v : v);
        }
        VLANE_SET(uint64_t, d, i, sum);
    }
}

/* Shifts by a count (immediate or the source's low 64 bits): logical shifts past the lane width clear it,
 * arithmetic ones fill it with the sign. */
#define VSHIFT(name, T, bits, expr_in, expr_out)                                            \
    static inline void name(uint8_t* d, uint64_t count, int n) {                            \
        for (int i = 0; i < n / (int)sizeof(T); ++i) {                                      \
            T a = VLANE_GET(T, d, i);                                                       \
            VLANE_SET(T, d, i, count < (bits) ? (expr_in) : (expr_out));                    \
        }                                                                                   \
    }
VSHIFT(VPsllW, uint16_t, 16, a << count, 0)
VSHIFT(VPsllD, uint32_t, 32, a << count, 0)
VSHIFT(VPsllQ, uint64_t, 64, a << count, 0)
VSHIFT(VPsrlW, uint16_t, 16, a >> count, 0)
VSHIFT(VPsrlD, uint32_t, 32, a >> count, 0)
VSHIFT(VPsrlQ, uint64_t, 64, a >> count, 0)
VSHIFT(VPsraW, int16_t, 16, a >> count, a >> 15)
VSHIFT(VPsraD, int32_t, 32, a >> count, a >> 31)

static inline void VPsrlDQ(uint8_t* d, uint64_t count, int n) {
    uint8_t t[16] = {0};
    if (count < (uint64_t)n) memcpy(t, d + count, n - count);
    memcpy(d, t, n);
}
static inline void VPsllDQ(uint8_t* d, uint64_t count, int n) {
    uint8_t t[16] = {0};
    if (count < (uint64_t)n) memcpy(t + count, d, n - count);
    memcpy(d, t, n);
}

/* Packs: destination lanes saturate into the low half of the result, source lanes into the high half. */
#define VPACK(name, TI, TO, lo, hi)                                                         \
    static inline void name(uint8_t* d, const uint8_t* s, int n) {                          \
        uint8_t r[16];                                                                      \
        int m = n / (int)sizeof(TI);                                                        \
        for (int i = 0; i < m; ++i) {                                                       \
            VLANE_SET(TO, r, i, VSat(VLANE_GET(TI, d, i), lo, hi));                         \
            VLANE_SET(TO, r, m + i, VSat(VLANE_GET(TI, s, i), lo, hi));                     \
        }                                                                                   \
        memcpy(d, r, n);                                                                    \
    }
VPACK(VPacksSWB, int16_t, int8_t, -128, 127)
VPACK(VPacksSDW, int32_t, int16_t, -32768, 32767)
VPACK(VPackUSWB, int16_t, uint8_t, 0, 255)

/* Unpacks: interleave the low (or high) halves' lanes of destination and source. */
#define VUNPACK(name, T, high)                                                              \
    static inline void name(uint8_t* d, const uint8_t* s, int n) {                          \
        uint8_t r[16];                                                                      \
        int m = n / (int)sizeof(T) / 2, o = (high) ? m : 0;                                 \
        for (int i = 0; i < m; ++i) {                                                       \
            VLANE_SET(T, r, 2 * i, VLANE_GET(T, d, o + i));                                 \
            VLANE_SET(T, r, 2 * i + 1, VLANE_GET(T, s, o + i));                             \
        }                                                                                   \
        memcpy(d, r, n);                                                                    \
    }
VUNPACK(VPunpcklBW, uint8_t, 0)
VUNPACK(VPunpcklWD, uint16_t, 0)
VUNPACK(VPunpcklDQ, uint32_t, 0)
VUNPACK(VPunpcklQDQ, uint64_t, 0)
VUNPACK(VPunpckhBW, uint8_t, 1)
VUNPACK(VPunpckhWD, uint16_t, 1)
VUNPACK(VPunpckhDQ, uint32_t, 1)
VUNPACK(VPunpckhQDQ, uint64_t, 1)

static inline uint32_t VPmovmskB(const uint8_t* s, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; ++i) m |= (uint32_t)(s[i] >> 7) << i;
    return m;
}

static inline void VPshufW(uint8_t* d, const uint8_t* s, uint8_t imm) {
    uint8_t r[8];
    for (int i = 0; i < 4; ++i) VLANE_SET(uint16_t, r, i, VLANE_GET(uint16_t, s, (imm >> (2 * i)) & 3));
    memcpy(d, r, 8);
}

/* rcpps/rsqrtps: hardware returns a vendor-specific approximation (relative error <= 1.5 * 2^-12); the exact
 * result is within that bound, so code that accepts the hardware's answer accepts this one too. */
static inline float VRcp(float a) { return 1.0f / a; }
static inline float VRsqrt(float a) { return 1.0f / sqrtf(a); }

/* rdtsc: the host's cycle counter where there is one, otherwise nanoseconds of the monotonic clock (games use
 * it for relative timings and calibrate it against QueryPerformanceCounter or timeGetTime). */
static inline uint64_t RecompRdtsc(void) {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

#endif
