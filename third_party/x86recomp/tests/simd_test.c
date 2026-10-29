/* Checks runtime/runtime_simd.h against the host CPU's own MMX/SSE2 instructions on random and edge-case inputs
 * (x86-64 host). Build: cc -O1 -msse2 -o simd_test tests/simd_test.c -lm && ./simd_test */
#include "../runtime/runtime_simd.h"

#include <emmintrin.h>
#include <mmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <xmmintrin.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    /* every few values an edge case: lanes of 0, -1, the signed extremes */
    switch (rng % 16) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return 0x8000800080008000ull;
    case 3: return 0x7fff7fff7fff7fffull;
    case 4: return 0x8000000080000000ull;
    case 5: return 0x8080808080808080ull;
    default: return rng;
    }
}

static int failures;
static void check(const char* name, const void* got, const void* want, int n, const void* a, const void* b) {
    if (memcmp(got, want, n) == 0) return;
    if (failures++ < 20) {
        printf("FAIL %s n=%d\n  a=", name, n);
        for (int i = n - 1; i >= 0; --i) printf("%02x", ((const uint8_t*)a)[i]);
        printf("\n  b=");
        for (int i = n - 1; i >= 0; --i) printf("%02x", ((const uint8_t*)b)[i]);
        printf("\n  got=");
        for (int i = n - 1; i >= 0; --i) printf("%02x", ((const uint8_t*)got)[i]);
        printf("\n  want=");
        for (int i = n - 1; i >= 0; --i) printf("%02x", ((const uint8_t*)want)[i]);
        printf("\n");
    }
}

typedef void (*VFn)(uint8_t*, const uint8_t*, int);
typedef __m64 (*MmxFn)(__m64, __m64);
typedef __m128i (*SseFn)(__m128i, __m128i);

#define CASE(fn, mmx, sse) {#fn, fn, mmx, sse}
static __m64 m_madd(__m64 a, __m64 b) { return _mm_madd_pi16(a, b); }
static __m128i s_madd(__m128i a, __m128i b) { return _mm_madd_epi16(a, b); }
#define W(name, expr_m, expr_s)                                           \
    static __m64 m_##name(__m64 a, __m64 b) { return expr_m; }          \
    static __m128i s_##name(__m128i a, __m128i b) { return expr_s; }
W(paddb, _mm_add_pi8(a, b), _mm_add_epi8(a, b))
W(paddw, _mm_add_pi16(a, b), _mm_add_epi16(a, b))
W(paddd, _mm_add_pi32(a, b), _mm_add_epi32(a, b))
W(psubb, _mm_sub_pi8(a, b), _mm_sub_epi8(a, b))
W(psubw, _mm_sub_pi16(a, b), _mm_sub_epi16(a, b))
W(psubd, _mm_sub_pi32(a, b), _mm_sub_epi32(a, b))
W(paddsb, _mm_adds_pi8(a, b), _mm_adds_epi8(a, b))
W(paddsw, _mm_adds_pi16(a, b), _mm_adds_epi16(a, b))
W(paddusb, _mm_adds_pu8(a, b), _mm_adds_epu8(a, b))
W(paddusw, _mm_adds_pu16(a, b), _mm_adds_epu16(a, b))
W(psubsb, _mm_subs_pi8(a, b), _mm_subs_epi8(a, b))
W(psubsw, _mm_subs_pi16(a, b), _mm_subs_epi16(a, b))
W(psubusb, _mm_subs_pu8(a, b), _mm_subs_epu8(a, b))
W(psubusw, _mm_subs_pu16(a, b), _mm_subs_epu16(a, b))
W(pmullw, _mm_mullo_pi16(a, b), _mm_mullo_epi16(a, b))
W(pmulhw, _mm_mulhi_pi16(a, b), _mm_mulhi_epi16(a, b))
W(pmulhuw, _mm_mulhi_pu16(a, b), _mm_mulhi_epu16(a, b))
W(pmuludq, _mm_mul_su32(a, b), _mm_mul_epu32(a, b))
W(pcmpeqb, _mm_cmpeq_pi8(a, b), _mm_cmpeq_epi8(a, b))
W(pcmpeqw, _mm_cmpeq_pi16(a, b), _mm_cmpeq_epi16(a, b))
W(pcmpeqd, _mm_cmpeq_pi32(a, b), _mm_cmpeq_epi32(a, b))
W(pcmpgtb, _mm_cmpgt_pi8(a, b), _mm_cmpgt_epi8(a, b))
W(pcmpgtw, _mm_cmpgt_pi16(a, b), _mm_cmpgt_epi16(a, b))
W(pcmpgtd, _mm_cmpgt_pi32(a, b), _mm_cmpgt_epi32(a, b))
W(pminub, _mm_min_pu8(a, b), _mm_min_epu8(a, b))
W(pmaxub, _mm_max_pu8(a, b), _mm_max_epu8(a, b))
W(pminsw, _mm_min_pi16(a, b), _mm_min_epi16(a, b))
W(pmaxsw, _mm_max_pi16(a, b), _mm_max_epi16(a, b))
W(pavgb, _mm_avg_pu8(a, b), _mm_avg_epu8(a, b))
W(pavgw, _mm_avg_pu16(a, b), _mm_avg_epu16(a, b))
W(psadbw, _mm_sad_pu8(a, b), _mm_sad_epu8(a, b))
W(pand, _mm_and_si64(a, b), _mm_and_si128(a, b))
W(pandn, _mm_andnot_si64(a, b), _mm_andnot_si128(a, b))
W(por, _mm_or_si64(a, b), _mm_or_si128(a, b))
W(pxor, _mm_xor_si64(a, b), _mm_xor_si128(a, b))
W(packsswb, _mm_packs_pi16(a, b), _mm_packs_epi16(a, b))
W(packssdw, _mm_packs_pi32(a, b), _mm_packs_epi32(a, b))
W(packuswb, _mm_packs_pu16(a, b), _mm_packus_epi16(a, b))
W(punpcklbw, _mm_unpacklo_pi8(a, b), _mm_unpacklo_epi8(a, b))
W(punpcklwd, _mm_unpacklo_pi16(a, b), _mm_unpacklo_epi16(a, b))
W(punpckldq, _mm_unpacklo_pi32(a, b), _mm_unpacklo_epi32(a, b))
W(punpckhbw, _mm_unpackhi_pi8(a, b), _mm_unpackhi_epi8(a, b))
W(punpckhwd, _mm_unpackhi_pi16(a, b), _mm_unpackhi_epi16(a, b))
W(punpckhdq, _mm_unpackhi_pi32(a, b), _mm_unpackhi_epi32(a, b))
static __m128i s_paddq(__m128i a, __m128i b) { return _mm_add_epi64(a, b); }
static __m128i s_psubq(__m128i a, __m128i b) { return _mm_sub_epi64(a, b); }
static __m128i s_punpcklqdq(__m128i a, __m128i b) { return _mm_unpacklo_epi64(a, b); }
static __m128i s_punpckhqdq(__m128i a, __m128i b) { return _mm_unpackhi_epi64(a, b); }

static const struct { const char* name; VFn fn; MmxFn mmx; SseFn sse; } cases[] = {
    CASE(VPaddB, m_paddb, s_paddb), CASE(VPaddW, m_paddw, s_paddw), CASE(VPaddD, m_paddd, s_paddd),
    CASE(VPaddQ, NULL, s_paddq), CASE(VPsubB, m_psubb, s_psubb), CASE(VPsubW, m_psubw, s_psubw),
    CASE(VPsubD, m_psubd, s_psubd), CASE(VPsubQ, NULL, s_psubq),
    CASE(VPaddSB, m_paddsb, s_paddsb), CASE(VPaddSW, m_paddsw, s_paddsw), CASE(VPaddUSB, m_paddusb, s_paddusb),
    CASE(VPaddUSW, m_paddusw, s_paddusw), CASE(VPsubSB, m_psubsb, s_psubsb), CASE(VPsubSW, m_psubsw, s_psubsw),
    CASE(VPsubUSB, m_psubusb, s_psubusb), CASE(VPsubUSW, m_psubusw, s_psubusw),
    CASE(VPmullW, m_pmullw, s_pmullw), CASE(VPmulhW, m_pmulhw, s_pmulhw), CASE(VPmulhUW, m_pmulhuw, s_pmulhuw),
    CASE(VPmaddWD, m_madd, s_madd), CASE(VPmulUDQ, m_pmuludq, s_pmuludq),
    CASE(VPcmpeqB, m_pcmpeqb, s_pcmpeqb), CASE(VPcmpeqW, m_pcmpeqw, s_pcmpeqw), CASE(VPcmpeqD, m_pcmpeqd, s_pcmpeqd),
    CASE(VPcmpgtB, m_pcmpgtb, s_pcmpgtb), CASE(VPcmpgtW, m_pcmpgtw, s_pcmpgtw), CASE(VPcmpgtD, m_pcmpgtd, s_pcmpgtd),
    CASE(VPminUB, m_pminub, s_pminub), CASE(VPmaxUB, m_pmaxub, s_pmaxub), CASE(VPminSW, m_pminsw, s_pminsw),
    CASE(VPmaxSW, m_pmaxsw, s_pmaxsw), CASE(VPavgB, m_pavgb, s_pavgb), CASE(VPavgW, m_pavgw, s_pavgw),
    CASE(VPsadBW, m_psadbw, s_psadbw), CASE(VPand, m_pand, s_pand), CASE(VPandn, m_pandn, s_pandn),
    CASE(VPor, m_por, s_por), CASE(VPxor, m_pxor, s_pxor),
    CASE(VPacksSWB, m_packsswb, s_packsswb), CASE(VPacksSDW, m_packssdw, s_packssdw),
    CASE(VPackUSWB, m_packuswb, s_packuswb),
    CASE(VPunpcklBW, m_punpcklbw, s_punpcklbw), CASE(VPunpcklWD, m_punpcklwd, s_punpcklwd),
    CASE(VPunpcklDQ, m_punpckldq, s_punpckldq), CASE(VPunpcklQDQ, NULL, s_punpcklqdq),
    CASE(VPunpckhBW, m_punpckhbw, s_punpckhbw), CASE(VPunpckhWD, m_punpckhwd, s_punpckhwd),
    CASE(VPunpckhDQ, m_punpckhdq, s_punpckhdq), CASE(VPunpckhQDQ, NULL, s_punpckhqdq),
};

/* Intrinsics are inline-only: wrapped so the table can point at them. */
W(sh_VPsllW, _mm_sll_pi16(a, b), _mm_sll_epi16(a, b))
W(sh_VPsllD, _mm_sll_pi32(a, b), _mm_sll_epi32(a, b))
W(sh_VPsllQ, _mm_sll_si64(a, b), _mm_sll_epi64(a, b))
W(sh_VPsrlW, _mm_srl_pi16(a, b), _mm_srl_epi16(a, b))
W(sh_VPsrlD, _mm_srl_pi32(a, b), _mm_srl_epi32(a, b))
W(sh_VPsrlQ, _mm_srl_si64(a, b), _mm_srl_epi64(a, b))
W(sh_VPsraW, _mm_sra_pi16(a, b), _mm_sra_epi16(a, b))
W(sh_VPsraD, _mm_sra_pi32(a, b), _mm_sra_epi32(a, b))
typedef void (*VShift)(uint8_t*, uint64_t, int);
static const struct { const char* name; VShift fn; MmxFn mmx; SseFn sse; } shifts[] = {
    {"VPsllW", VPsllW, m_sh_VPsllW, s_sh_VPsllW},
    {"VPsllD", VPsllD, m_sh_VPsllD, s_sh_VPsllD},
    {"VPsllQ", VPsllQ, m_sh_VPsllQ, s_sh_VPsllQ},
    {"VPsrlW", VPsrlW, m_sh_VPsrlW, s_sh_VPsrlW},
    {"VPsrlD", VPsrlD, m_sh_VPsrlD, s_sh_VPsrlD},
    {"VPsrlQ", VPsrlQ, m_sh_VPsrlQ, s_sh_VPsrlQ},
    {"VPsraW", VPsraW, m_sh_VPsraW, s_sh_VPsraW},
    {"VPsraD", VPsraD, m_sh_VPsraD, s_sh_VPsraD},
};

int main(void) {
    const int iters = 200000;
    for (int it = 0; it < iters; ++it) {
        uint64_t a[2] = {next(), next()}, b[2] = {next(), next()};
        for (size_t k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
            uint8_t got[16], want[16];
            if (cases[k].mmx) {
                memcpy(got, a, 8);
                cases[k].fn(got, (const uint8_t*)b, 8);
                __m64 r = cases[k].mmx(_mm_cvtsi64_m64((long long)a[0]), _mm_cvtsi64_m64((long long)b[0]));
                uint64_t w = (uint64_t)_mm_cvtm64_si64(r);
                memcpy(want, &w, 8);
                check(cases[k].name, got, want, 8, a, b);
            }
            memcpy(got, a, 16);
            cases[k].fn(got, (const uint8_t*)b, 16);
            __m128i r = cases[k].sse(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b));
            _mm_storeu_si128((__m128i*)want, r);
            check(cases[k].name, got, want, 16, a, b);
        }
        uint64_t count = (it % 4 == 0) ? next() : next() % 70;  /* mostly in range, some huge */
        uint64_t cnt[2] = {count, 0};
        for (size_t k = 0; k < sizeof shifts / sizeof shifts[0]; ++k) {
            uint8_t got[16], want[16];
            memcpy(got, a, 8);
            shifts[k].fn(got, count, 8);
            uint64_t w = (uint64_t)_mm_cvtm64_si64(shifts[k].mmx(_mm_cvtsi64_m64((long long)a[0]),
                                                                  _mm_cvtsi64_m64((long long)count)));
            memcpy(want, &w, 8);
            check(shifts[k].name, got, want, 8, a, cnt);
            memcpy(got, a, 16);
            shifts[k].fn(got, count, 16);
            _mm_storeu_si128((__m128i*)want, shifts[k].sse(_mm_loadu_si128((const __m128i*)a),
                                                           _mm_loadu_si128((const __m128i*)cnt)));
            check(shifts[k].name, got, want, 16, a, cnt);
        }
        {
            uint8_t got[16], want[16];
            uint8_t imm = (uint8_t)next();
            memcpy(got, a, 8);
            VPshufW(got, (const uint8_t*)b, imm);
            uint64_t w;
            __m64 r;
            switch (imm) { /* _mm_shuffle_pi16 needs a constant: compare through the lane definition */
            default: {
                uint16_t lanes[4];
                for (int i = 0; i < 4; ++i) lanes[i] = (uint16_t)(b[0] >> (16 * ((imm >> (2 * i)) & 3)));
                memcpy(&w, lanes, 8);
            }
            }
            (void)r;
            memcpy(want, &w, 8);
            check("VPshufW", got, want, 8, a, b);
            uint32_t m8 = VPmovmskB((const uint8_t*)a, 8), m16 = VPmovmskB((const uint8_t*)a, 16);
            uint32_t h8 = (uint32_t)_mm_movemask_pi8(_mm_cvtsi64_m64((long long)a[0]));
            uint32_t h16 = (uint32_t)_mm_movemask_epi8(_mm_loadu_si128((const __m128i*)a));
            check("VPmovmskB", &m8, &h8, 4, a, b);
            check("VPmovmskB", &m16, &h16, 4, a, b);
            uint32_t sh = (uint32_t)(next() % 20);
            memcpy(got, a, 16);
            VPsrlDQ(got, sh, 16);
            memset(want, 0, 16);
            if (sh < 16) memcpy(want, (const uint8_t*)a + sh, 16 - sh);
            check("VPsrlDQ", got, want, 16, a, &sh);
        }
        {
            /* rcpps/rsqrtps: ours exact, hardware approximate; both must be within the documented bound */
            float x;
            uint32_t bits = (uint32_t)next() & 0x7fffffffu;
            memcpy(&x, &bits, 4);
            if (x > 1e-30f && x < 1e30f) {
                float hw_r = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x))), our_r = VRcp(x);
                float hw_s = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x))), our_s = VRsqrt(x);
                if (fabsf(hw_r - our_r) > 1.5f * 0x1p-12f * fabsf(our_r) ||
                    fabsf(hw_s - our_s) > 1.5f * 0x1p-12f * fabsf(our_s)) {
                    if (failures++ < 20) printf("FAIL rcp/rsqrt x=%g hw=%g/%g ours=%g/%g\n", x, hw_r, hw_s, our_r, our_s);
                }
            }
        }
    }
    _mm_empty();
    printf("%s: %d failures (%d iterations x %zu ops)\n", failures ? "FAILED" : "OK", failures, iters,
           sizeof cases / sizeof cases[0] + sizeof shifts / sizeof shifts[0] + 4);
    return failures != 0;
}
