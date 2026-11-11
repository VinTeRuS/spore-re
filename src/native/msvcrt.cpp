// MSVCR90 for the native host: the C runtime functions SporeApp.exe imports. Guest pointers are host
// pointers (flat guest memory), so most functions forward to the host C library directly. Variadic functions read
// their arguments from the guest stack with the 32-bit MSVC layout.
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "win.h"

namespace native {

// ---------------- guest printf/scanf ----------------
// Formats `fmt` with arguments starting at guest address `args` (each int/pointer 4 bytes, double/__int64 8).
std::string GuestFormat(const char* fmt, uint32_t args) {
    std::string out;
    char buf[512];
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        if (p[1] == '%') {
            out += '%';
            p += 2;
            continue;
        }
        std::string spec = "%";
        ++p;
        while (strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') {
            spec += std::to_string(int32_t(R32(args)));
            args += 4;
            ++p;
        }
        while (isdigit((unsigned char)*p)) spec += *p++;
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                spec += std::to_string(int32_t(R32(args)));
                args += 4;
                ++p;
            }
            while (isdigit((unsigned char)*p)) spec += *p++;
        }
        int size = 4;  // argument size: 4, 8, or 2 (h)
        bool wide = false;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { size = 8; p += 3; }
        else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
        else if (p[0] == 'l' && p[1] == 'l') { size = 8; p += 2; }
        else if (*p == 'l') { wide = true; ++p; }
        else if (*p == 'h') { ++p; }
        else if (*p == 'L') { ++p; }
        else if (*p == 'w') { wide = true; ++p; }
        char conv = *p ? *p++ : 0;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
                if (size == 8) {
                    spec += "ll";
                    spec += conv;
                    snprintf(buf, sizeof(buf), spec.c_str(), (long long)R64(args));
                    args += 8;
                } else {
                    spec += conv;
                    if (conv == 'd' || conv == 'i') snprintf(buf, sizeof(buf), spec.c_str(), int32_t(R32(args)));
                    else snprintf(buf, sizeof(buf), spec.c_str(), R32(args));
                    args += 4;
                }
                out += buf;
                break;
            case 'c': case 'C':
                spec += 'c';
                snprintf(buf, sizeof(buf), spec.c_str(), int(R32(args) & 0xff));
                args += 4;
                out += buf;
                break;
            case 'p':
                snprintf(buf, sizeof(buf), "%08X", R32(args));
                args += 4;
                out += buf;
                break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
                spec += conv;
                snprintf(buf, sizeof(buf), spec.c_str(), RF64(args));
                args += 8;
                out += buf;
                break;
            case 's': case 'S': {
                uint32_t s = R32(args);
                args += 4;
                std::string v;
                if (!s) v = "(null)";
                else if (wide || conv == 'S')
                    for (uint32_t k = 0; R16(s + 2 * k); ++k) v += char(R16(s + 2 * k));
                else v = Str(s);
                spec += 's';
                std::vector<char> big(v.size() + 512);
                snprintf(big.data(), big.size(), spec.c_str(), v.c_str());
                out += big.data();
                break;
            }
            case 'n':
                W32(R32(args), uint32_t(out.size()));
                args += 4;
                break;
            default:
                out += spec;
                if (conv) out += conv;
        }
    }
    return out;
}

namespace {

// Scans `str` per `fmt`, storing through guest pointers starting at guest address `args`. `secure` (the _s
// variants) passes a buffer size after each %s/%c/%[ pointer.
int GuestScan(const char* str, const char* fmt, uint32_t args, bool secure) {
    int assigned = 0;
    const char* s = str;
    for (const char* p = fmt; *p;) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*s)) ++s;
            ++p;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') ++p;
            if (*s != *p) break;
            ++s;
            ++p;
            continue;
        }
        const char* start = p++;
        bool suppress = false;
        if (*p == '*') { suppress = true; ++p; }
        while (isdigit((unsigned char)*p)) ++p;
        std::string len;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len = "ll"; p += 3; }
        while (*p == 'l' || *p == 'h' || *p == 'L') len += *p++;
        char conv = *p;
        // MSVC sizes: long is 32 bits (the host's is 64), long double is a double.
        if (len == "l" && strchr("diouxXn", conv)) len.clear();
        if (len == "L" && strchr("eEfgGa", conv)) len = "l";
        // "%<*><width><length>" with the adjusted length modifier
        std::string spec = "%";
        for (const char* q = start + 1; *q == '*' || isdigit((unsigned char)*q); ++q) spec += *q;
        spec += len;
        if (conv == '[') {
            const char* e = p + 1;
            if (*e == '^') ++e;
            if (*e == ']') ++e;
            while (*e && *e != ']') ++e;
            spec.append(p, e + 1);
            p = *e ? e + 1 : e;
        } else {
            spec += conv;
            ++p;
        }
        if (strchr("sc[", conv) && len == "l") {
            Log("scanf: wide conversion %s not supported", spec.c_str());
            break;
        }
        spec = "%" + spec.substr(1);
        // Host sscanf on one conversion; %n reports how far it read.
        int consumed = -1, r;
        std::string one = spec + "%n";
        if (suppress) {
            r = sscanf(s, one.c_str(), &consumed);
        } else {
            void* target = Ptr<void>(R32(args));
            args += 4;
            if (secure && strchr("sc[", conv)) args += 4;
            if (conv == 'p') {
                unsigned v;
                r = sscanf(s, (spec.substr(0, spec.size() - 1) + "x%n").c_str(), &v, &consumed);
                if (r == 1) *static_cast<uint32_t*>(target) = v;
            } else {
                r = sscanf(s, one.c_str(), target, &consumed);
            }
            if (r == 1) ++assigned;
        }
        if (consumed < 0) break;
        s += consumed;
    }
    return assigned ? assigned : (*str ? 0 : -1);
}

// ---------------- FILE ----------------
// Guest FILE objects are 32-byte blocks; the host FILE* is kept in a side table. __iob_func's array covers
// stdin/stdout/stderr.
std::mutex g_files_mutex;
std::map<uint32_t, FILE*> g_files;
uint32_t g_iob = 0;

uint32_t Iob() {
    std::lock_guard<std::mutex> lock(g_files_mutex);
    if (!g_iob) {
        g_iob = HeapAlloc(3 * 32);
        memset(Ptr<void>(g_iob), 0, 96);
        g_files[g_iob] = stdin;
        g_files[g_iob + 32] = stdout;
        g_files[g_iob + 64] = stderr;
    }
    return g_iob;
}

FILE* HostFile(uint32_t f) {
    Iob();
    std::lock_guard<std::mutex> lock(g_files_mutex);
    auto it = g_files.find(f);
    return it == g_files.end() ? nullptr : it->second;
}

uint32_t OpenGuestFile(const char* name, const char* mode) {
    std::string m;
    for (const char* p = mode; *p; ++p)
        if (strchr("rwab+", *p)) m += *p;
    std::string host = HostPath(name);
    FILE* f = fopen(host.c_str(), m.c_str());
    if (!f) return 0;
    uint32_t g = HeapAlloc(32);
    memset(Ptr<void>(g), 0, 32);
    std::lock_guard<std::mutex> lock(g_files_mutex);
    g_files[g] = f;
    return g;
}

// ---------------- misc state ----------------
thread_local uint32_t tl_errno_addr = 0;
uint32_t g_rand_state = 1;
std::mutex g_crt_locks[64];
std::vector<uint32_t> g_onexit;
uint32_t g_tm = 0, g_lconv = 0;
thread_local uint32_t tl_strtok = 0;

int32_t& Errno() {
    if (!tl_errno_addr) tl_errno_addr = HeapAlloc(4);
    return *Ptr<int32_t>(tl_errno_addr);
}

// x87 stack access for the _CI* functions (arguments and result on the FPU stack).
double St(Ctx* c, int i) { return c->st[(c->st_top + i) & 7]; }
// trace_ci = N: log the first N x87 CRT helper calls (_CIsin, _CIpow, ...) with their operands, for debugging.
void CiTrace(const char* name, double x, double y, double r, Ctx* c) {
    static const int limit = SettingInt("trace_ci", 0);
    static std::atomic<int> n{0};
    if (limit > 0 && n.fetch_add(1) < limit)
        Log("ci %s(%g, %g) = %g  top %u ret %08x", name, x, y, r, c->st_top & 7, R32(c->esp));
}
void CiUnaryN(Ctx* c, double (*f)(double), const char* name) {
    double x = St(c, 0);
    c->st[c->st_top & 7] = f(x);
    CiTrace(name, x, 0, c->st[c->st_top & 7], c);
    c->ret_target = Pop32(c);
}
#define CiUnary(c, f) CiUnaryN(c, f, __func__)
void CiBinaryN(Ctx* c, double (*f)(double, double), const char* name) {
    double y = St(c, 0), x = St(c, 1);
    c->st_top = (c->st_top + 1) & 7;  // pop
    c->st[c->st_top] = f(x, y);
    CiTrace(name, x, y, c->st[c->st_top], c);
    c->ret_target = Pop32(c);
}
#define CiBinary(c, f) CiBinaryN(c, f, __func__)

uint32_t WriteTm(const struct tm& t) {
    if (!g_tm) g_tm = HeapAlloc(36);
    int32_t v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    memcpy(Ptr<void>(g_tm), v, 36);
    return g_tm;
}

thread_local uint32_t tl_qsort_cmp = 0;
int QsortThunk(const void* a, const void* b) {
    return int32_t(GuestCall(tl_qsort_cmp, {Addr(a), Addr(b)}));
}

#define C(name) void m_##name(Ctx* c)

// memory and strings
C(memset) { Return(c, Addr(memset(Ptr<void>(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2)))); }
C(memcpy) { Return(c, Addr(memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2)))); }
C(memmove) { Return(c, Addr(memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2)))); }
C(memmove_s) {
    memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 2)), Arg(c, 3) < Arg(c, 1) ? Arg(c, 3) : Arg(c, 1));
    Return(c, 0);
}
C(memcpy_s) {
    memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 2)), Arg(c, 3) < Arg(c, 1) ? Arg(c, 3) : Arg(c, 1));
    Return(c, 0);
}
C(memchr) { Return(c, Addr(memchr(Ptr<void>(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2)))); }
C(strchr) { Return(c, Addr(strchr(Str(Arg(c, 0)), int(Arg(c, 1))))); }
C(strrchr) { Return(c, Addr(strrchr(Str(Arg(c, 0)), int(Arg(c, 1))))); }
C(strstr) { Return(c, Addr(strstr(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strpbrk) { Return(c, Addr(strpbrk(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strspn) { Return(c, uint32_t(strspn(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strcspn) { Return(c, uint32_t(strcspn(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strncmp) { Return(c, uint32_t(strncmp(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strcoll) { Return(c, uint32_t(strcmp(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(_stricmp) { Return(c, uint32_t(strcasecmp(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(_strnicmp) { Return(c, uint32_t(strncasecmp(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strncpy) { Return(c, Addr(strncpy(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strncat) { Return(c, Addr(strncat(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strnlen) { Return(c, uint32_t(strnlen(Str(Arg(c, 0)), Arg(c, 1)))); }
C(strcpy_s) {
    char* d = Str(Arg(c, 0));
    uint32_t size = Arg(c, 1);
    const char* s = Str(Arg(c, 2));
    size_t n = strlen(s);
    if (n >= size) n = size - 1;
    memcpy(d, s, n);
    d[n] = 0;
    Return(c, 0);
}
C(strcat_s) {
    char* d = Str(Arg(c, 0));
    uint32_t size = Arg(c, 1);
    size_t have = strnlen(d, size);
    const char* s = Str(Arg(c, 2));
    size_t n = strlen(s);
    if (have + n >= size) n = size - have - 1;
    memcpy(d + have, s, n);
    d[have + n] = 0;
    Return(c, 0);
}
C(strncpy_s) {
    char* d = Str(Arg(c, 0));
    uint32_t size = Arg(c, 1), count = Arg(c, 3);
    const char* s = Str(Arg(c, 2));
    size_t n = strnlen(s, count == 0xffffffffu ? size : count);
    if (n >= size) n = size - 1;
    memcpy(d, s, n);
    d[n] = 0;
    Return(c, 0);
}
C(strtok) {
    uint32_t s = Arg(c, 0);
    const char* delim = Str(Arg(c, 1));
    if (!s) s = tl_strtok;
    if (!s) {
        Return(c, 0);
        return;
    }
    s += uint32_t(strspn(Str(s), delim));
    if (!R8(s)) {
        tl_strtok = 0;
        Return(c, 0);
        return;
    }
    uint32_t e = s + uint32_t(strcspn(Str(s), delim));
    if (R8(e)) {
        W8(e, 0);
        tl_strtok = e + 1;
    } else {
        tl_strtok = 0;
    }
    Return(c, s);
}
C(_strupr) {
    for (char* p = Str(Arg(c, 0)); *p; ++p) *p = char(toupper((unsigned char)*p));
    Return(c, Arg(c, 0));
}
C(_strlwr) {
    for (char* p = Str(Arg(c, 0)); *p; ++p) *p = char(tolower((unsigned char)*p));
    Return(c, Arg(c, 0));
}
C(strerror) {
    static uint32_t buf = 0;
    if (!buf) buf = HeapAlloc(128);
    snprintf(Str(buf), 128, "%s", strerror(int(Arg(c, 0))));
    Return(c, buf);
}
C(_itoa_s) {
    uint32_t radix = Arg(c, 3);
    int32_t v = int32_t(Arg(c, 0));
    char tmp[40];
    if (radix == 10) snprintf(tmp, sizeof(tmp), "%d", v);
    else if (radix == 16) snprintf(tmp, sizeof(tmp), "%x", uint32_t(v));
    else {
        uint32_t u = uint32_t(v);
        char r[40];
        int n = 0;
        do { r[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[u % radix]; u /= radix; } while (u);
        for (int i = 0; i < n; ++i) tmp[i] = r[n - 1 - i];
        tmp[n] = 0;
    }
    strncpy(Str(Arg(c, 1)), tmp, Arg(c, 2));
    Return(c, 0);
}

// character classes (C locale)
C(toupper) { Return(c, uint32_t(toupper(int(Arg(c, 0))))); }
C(tolower) { Return(c, uint32_t(tolower(int(Arg(c, 0))))); }
C(isspace) { Return(c, isspace(int(Arg(c, 0) & 0xff)) ? 8 : 0); }
C(isdigit) { Return(c, isdigit(int(Arg(c, 0) & 0xff)) ? 4 : 0); }
C(isxdigit) { Return(c, isxdigit(int(Arg(c, 0) & 0xff)) ? 0x80 : 0); }
C(isalpha) { Return(c, isalpha(int(Arg(c, 0) & 0xff)) ? 0x103 : 0); }
C(isalnum) { Return(c, isalnum(int(Arg(c, 0) & 0xff)) ? 0x107 : 0); }
C(islower) { Return(c, islower(int(Arg(c, 0) & 0xff)) ? 2 : 0); }
C(isupper) { Return(c, isupper(int(Arg(c, 0) & 0xff)) ? 1 : 0); }
C(ispunct) { Return(c, ispunct(int(Arg(c, 0) & 0xff)) ? 0x10 : 0); }
C(iscntrl) { Return(c, iscntrl(int(Arg(c, 0) & 0xff)) ? 0x20 : 0); }
C(iswalnum) { Return(c, iswalnum(wint_t(Arg(c, 0) & 0xffff)) ? 1 : 0); }
C(iswspace) { Return(c, iswspace(wint_t(Arg(c, 0) & 0xffff)) ? 1 : 0); }
C(towupper) { Return(c, uint32_t(towupper(wint_t(Arg(c, 0) & 0xffff))) & 0xffff); }
C(towlower) { Return(c, uint32_t(towlower(wint_t(Arg(c, 0) & 0xffff))) & 0xffff); }

// wide strings (16-bit wchar_t)
uint32_t WLen(uint32_t s) {
    uint32_t n = 0;
    while (R16(s + 2 * n)) ++n;
    return n;
}
C(wcscpy_s) {
    uint32_t d = Arg(c, 0), size = Arg(c, 1), s = Arg(c, 2), n = WLen(s);
    if (n >= size) n = size - 1;
    memmove(Ptr<void>(d), Ptr<void>(s), 2 * n);
    W16(d + 2 * n, 0);
    Return(c, 0);
}
C(wcscat_s) {
    uint32_t d = Arg(c, 0), size = Arg(c, 1), s = Arg(c, 2), have = WLen(d), n = WLen(s);
    if (have + n >= size) n = size - have - 1;
    memmove(Ptr<void>(d + 2 * have), Ptr<void>(s), 2 * n);
    W16(d + 2 * (have + n), 0);
    Return(c, 0);
}
C(wcsstr) {
    uint32_t h = Arg(c, 0), n = Arg(c, 1), nl = WLen(n);
    for (uint32_t p = h;; p += 2) {
        if (memcmp(Ptr<void>(p), Ptr<void>(n), 2 * nl) == 0) {
            Return(c, p);
            return;
        }
        if (!R16(p)) break;
    }
    Return(c, 0);
}
C(wcsrtombs_s) {
    uint32_t ret = Arg(c, 0), dst = Arg(c, 1), size = Arg(c, 2), srcp = Arg(c, 3), count = Arg(c, 4);
    uint32_t s = R32(srcp), n = 0;
    while (R16(s + 2 * n) && n < count && (!dst || n + 1 < size)) {
        if (dst) W8(dst + n, uint8_t(R16(s + 2 * n)));
        ++n;
    }
    if (dst) W8(dst + n, 0);
    if (ret) W32(ret, n + 1);
    Return(c, 0);
}
C(swscanf) {
    // Narrow both strings (the engine only scans ASCII content).
    std::string s, f;
    for (uint32_t p = Arg(c, 0); R16(p); p += 2) s += char(R16(p));
    for (uint32_t p = Arg(c, 1); R16(p); p += 2) f += char(R16(p));
    Return(c, uint32_t(GuestScan(s.c_str(), f.c_str(), c->esp + 12, false)));
}

// conversions
C(atoi) { Return(c, uint32_t(atoi(Str(Arg(c, 0))))); }
C(atof) { ReturnF(c, atof(Str(Arg(c, 0)))); }
C(strtod) {
    char* end;
    double v = strtod(Str(Arg(c, 0)), &end);
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    ReturnF(c, v);
}
C(strtol) {
    char* end;
    long v = strtol(Str(Arg(c, 0)), &end, int(Arg(c, 2)));
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    Return(c, uint32_t(int32_t(v)));
}
C(strtoul) {
    char* end;
    unsigned long v = strtoul(Str(Arg(c, 0)), &end, int(Arg(c, 2)));
    if (v > UINT32_MAX) v = UINT32_MAX;
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    Return(c, uint32_t(v));
}
C(_strtoui64) {
    char* end;
    unsigned long long v = strtoull(Str(Arg(c, 0)), &end, int(Arg(c, 2)));
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    c->edx = uint32_t(v >> 32);
    Return(c, uint32_t(v));
}

// formatted I/O
void PutFormatted(uint32_t dst, uint32_t size, const std::string& s, bool truncate_ok) {
    (void)truncate_ok;
    if (!size) return;
    size_t n = s.size() < size - 1 ? s.size() : size - 1;
    memcpy(Str(dst), s.data(), n);
    Str(dst)[n] = 0;
}
C(sprintf) {
    std::string s = GuestFormat(Str(Arg(c, 1)), c->esp + 12);
    memcpy(Str(Arg(c, 0)), s.c_str(), s.size() + 1);
    Return(c, uint32_t(s.size()));
}
C(sprintf_s) {
    std::string s = GuestFormat(Str(Arg(c, 2)), c->esp + 16);
    PutFormatted(Arg(c, 0), Arg(c, 1), s, false);
    Return(c, uint32_t(s.size()));
}
C(_snprintf) {
    std::string s = GuestFormat(Str(Arg(c, 2)), c->esp + 16);
    uint32_t size = Arg(c, 1);
    // MSVC _snprintf: no terminator when the result fills the buffer, -1 when it does not fit.
    memcpy(Str(Arg(c, 0)), s.data(), s.size() < size ? s.size() : size);
    if (s.size() < size) Str(Arg(c, 0))[s.size()] = 0;
    Return(c, s.size() <= size ? uint32_t(s.size()) : 0xffffffffu);
}
C(_snprintf_s) {
    std::string s = GuestFormat(Str(Arg(c, 3)), c->esp + 20);
    PutFormatted(Arg(c, 0), Arg(c, 1), s, true);
    Return(c, s.size() < Arg(c, 1) ? uint32_t(s.size()) : 0xffffffffu);
}
C(_vsnprintf) {
    std::string s = GuestFormat(Str(Arg(c, 2)), Arg(c, 3));
    uint32_t size = Arg(c, 1);
    memcpy(Str(Arg(c, 0)), s.data(), s.size() < size ? s.size() : size);
    if (s.size() < size) Str(Arg(c, 0))[s.size()] = 0;
    Return(c, s.size() <= size ? uint32_t(s.size()) : 0xffffffffu);
}
C(_vsnprintf_s) {
    std::string s = GuestFormat(Str(Arg(c, 3)), Arg(c, 4));
    PutFormatted(Arg(c, 0), Arg(c, 1), s, true);
    Return(c, s.size() < Arg(c, 1) ? uint32_t(s.size()) : 0xffffffffu);
}
C(sscanf) { Return(c, uint32_t(GuestScan(Str(Arg(c, 0)), Str(Arg(c, 1)), c->esp + 12, false))); }
C(sscanf_s) { Return(c, uint32_t(GuestScan(Str(Arg(c, 0)), Str(Arg(c, 1)), c->esp + 12, true))); }
C(fprintf) {
    std::string s = GuestFormat(Str(Arg(c, 1)), c->esp + 12);
    FILE* f = HostFile(Arg(c, 0));
    if (f) fwrite(s.data(), 1, s.size(), f);
    Return(c, uint32_t(s.size()));
}
C(_set_printf_count_output) { Return(c, 1); }

// FILE
C(__iob_func) { Return(c, Iob()); }
C(fopen) { Return(c, OpenGuestFile(Str(Arg(c, 0)), Str(Arg(c, 1)))); }
C(fopen_s) {
    uint32_t f = OpenGuestFile(Str(Arg(c, 1)), Str(Arg(c, 2)));
    W32(Arg(c, 0), f);
    Return(c, f ? 0 : 2);
}
C(fclose) {
    FILE* f = HostFile(Arg(c, 0));
    if (f && f != stdin && f != stdout && f != stderr) {
        fclose(f);
        std::lock_guard<std::mutex> lock(g_files_mutex);
        g_files.erase(Arg(c, 0));
    }
    Return(c, 0);
}
C(fread) {
    FILE* f = HostFile(Arg(c, 3));
    Return(c, f ? uint32_t(fread(Ptr<void>(Arg(c, 0)), Arg(c, 1), Arg(c, 2), f)) : 0);
}
C(fwrite) {
    FILE* f = HostFile(Arg(c, 3));
    Return(c, f ? uint32_t(fwrite(Ptr<void>(Arg(c, 0)), Arg(c, 1), Arg(c, 2), f)) : 0);
}
C(fseek) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(fseek(f, int32_t(Arg(c, 1)), int(Arg(c, 2)))) : 0xffffffffu);
}
C(ftell) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(ftell(f)) : 0xffffffffu);
}
C(rewind) {
    if (FILE* f = HostFile(Arg(c, 0))) rewind(f);
    Return(c, 0);
}
C(fflush) {
    if (FILE* f = HostFile(Arg(c, 0))) fflush(f);
    Return(c, 0);
}
C(feof) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(feof(f)) : 1);
}
C(ferror) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(ferror(f)) : 1);
}
C(fgets) {
    FILE* f = HostFile(Arg(c, 2));
    Return(c, f && fgets(Str(Arg(c, 0)), int(Arg(c, 1)), f) ? Arg(c, 0) : 0);
}
C(getc) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(getc(f)) : 0xffffffffu);
}
C(ungetc) {
    FILE* f = HostFile(Arg(c, 1));
    Return(c, f ? uint32_t(ungetc(int(Arg(c, 0)), f)) : 0xffffffffu);
}
C(fputs) {
    FILE* f = HostFile(Arg(c, 1));
    Return(c, f ? uint32_t(fputs(Str(Arg(c, 0)), f)) : 0xffffffffu);
}
C(fputc) {
    FILE* f = HostFile(Arg(c, 1));
    Return(c, f ? uint32_t(fputc(int(Arg(c, 0)), f)) : 0xffffffffu);
}
C(_stat64) {
    struct stat st;
    if (stat(HostPath(Str(Arg(c, 0))).c_str(), &st) != 0) {
        Errno() = 2;
        Return(c, 0xffffffffu);
        return;
    }
    uint32_t b = Arg(c, 1);
    memset(Ptr<void>(b), 0, 56);
    uint16_t mode = (S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | 0x100 | ((st.st_mode & S_IWUSR) ? 0x80 : 0);
    W16(b + 6, mode);
    W16(b + 8, 1);
    W64(b + 24, uint64_t(st.st_size));
    W64(b + 32, uint64_t(st.st_atime));
    W64(b + 40, uint64_t(st.st_mtime));
    W64(b + 48, uint64_t(st.st_ctime));
    Return(c, 0);
}

// memory
C(malloc) { Return(c, HeapAlloc(Arg(c, 0))); }
C(_malloc_crt) { Return(c, HeapAlloc(Arg(c, 0))); }
C(free) {
    HeapFree(Arg(c, 0));
    Return(c, 0);
}
C(realloc) { Return(c, HeapRealloc(Arg(c, 0), Arg(c, 1))); }

// math (cdecl, double arguments on the stack, result in st0)
C(floor) { ReturnF(c, floor(ArgD(c, 0))); }
C(ceil) { ReturnF(c, ceil(ArgD(c, 0))); }
C(ldexp) { ReturnF(c, ldexp(ArgD(c, 0), int32_t(Arg(c, 2)))); }
C(frexp) {
    int e;
    double m = frexp(ArgD(c, 0), &e);
    W32(Arg(c, 2), uint32_t(e));
    ReturnF(c, m);
}
C(modf) {
    double ip;
    double f = modf(ArgD(c, 0), &ip);
    WF64(Arg(c, 2), ip);
    ReturnF(c, f);
}
C(_isnan) { Return(c, std::isnan(ArgD(c, 0)) ? 1 : 0); }
// _CI*: arguments on the x87 stack
C(_CIsqrt) { CiUnary(c, [](double x) { return std::sqrt(x); }); }
C(_CIcos) { CiUnary(c, [](double x) { return std::cos(x); }); }
C(_CIasin) { CiUnary(c, [](double x) { return std::asin(x); }); }
C(_CIacos) { CiUnary(c, [](double x) { return std::acos(x); }); }
C(_CItanh) { CiUnary(c, [](double x) { return std::tanh(x); }); }
C(_CIsinh) { CiUnary(c, [](double x) { return std::sinh(x); }); }
C(_CIcosh) { CiUnary(c, [](double x) { return std::cosh(x); }); }
C(_CIpow) { CiBinary(c, [](double x, double y) { return std::pow(x, y); }); }
C(_CIfmod) { CiBinary(c, [](double x, double y) { return std::fmod(x, y); }); }

C(rand) {
    g_rand_state = g_rand_state * 214013u + 2531011u;
    Return(c, (g_rand_state >> 16) & 0x7fff);
}
C(srand) {
    g_rand_state = Arg(c, 0);
    Return(c, 0);
}
C(qsort) {
    uint32_t saved = tl_qsort_cmp;
    tl_qsort_cmp = Arg(c, 3);
    qsort(Ptr<void>(Arg(c, 0)), Arg(c, 1), Arg(c, 2), QsortThunk);
    tl_qsort_cmp = saved;
    Return(c, 0);
}

// time
C(_localtime64) {
    time_t t = time_t(R64(Arg(c, 0)));
    struct tm tm;
    localtime_r(&t, &tm);
    Return(c, WriteTm(tm));
}
C(_gmtime64) {
    time_t t = time_t(R64(Arg(c, 0)));
    struct tm tm;
    gmtime_r(&t, &tm);
    Return(c, WriteTm(tm));
}
C(_ftime64) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t b = Arg(c, 0);
    W64(b, uint64_t(tv.tv_sec));
    W16(b + 8, uint16_t(tv.tv_usec / 1000));
    W16(b + 10, 0);
    W16(b + 12, 0);
    Return(c, 0);
}

// runtime support
C(_initterm) {
    for (uint32_t p = Arg(c, 0); p < Arg(c, 1); p += 4) {
        if (uint32_t f = R32(p)) GuestCall(f, {});
    }
    Return(c, 0);
}
C(_initterm_e) {
    for (uint32_t p = Arg(c, 0); p < Arg(c, 1); p += 4) {
        if (uint32_t f = R32(p)) {
            if (uint32_t r = GuestCall(f, {})) {
                Return(c, r);
                return;
            }
        }
    }
    Return(c, 0);
}
C(_onexit) {
    g_onexit.push_back(Arg(c, 0));
    Return(c, Arg(c, 0));
}
C(__dllonexit) {
    g_onexit.push_back(Arg(c, 0));
    Return(c, Arg(c, 0));
}
C(_lock) {
    g_crt_locks[Arg(c, 0) & 63].lock();
    Return(c, 0);
}
C(_unlock) {
    g_crt_locks[Arg(c, 0) & 63].unlock();
    Return(c, 0);
}
C(_encode_pointer) { Return(c, Arg(c, 0)); }
C(_decode_pointer) { Return(c, Arg(c, 0)); }
C(_encoded_null) { Return(c, 0); }
C(_crt_debugger_hook) { Return(c, 0); }
C(__clean_type_info_names_internal) { Return(c, 0); }
C(_set_purecall_handler) { Return(c, 0); }
C(_errno) {
    Errno();
    Return(c, tl_errno_addr);
}
C(_controlfp_s) {
    // Precision and rounding go to the guest x87 control word, which the recompiled x87 code honours.
    uint32_t cur = Arg(c, 0), value = Arg(c, 1), mask = Arg(c, 2);
    uint16_t cw = c->fpu_cw;
    if (mask & 0x300) {  // _MCW_RC
        static const uint16_t rc[4] = {0, 1, 2, 3};
        cw = uint16_t((cw & ~0x0c00) | (rc[(value >> 8) & 3] << 10));
    }
    if (mask & 0x30000) {  // _MCW_PC: _PC_64 0, _PC_53 0x10000, _PC_24 0x20000
        static const uint16_t pc[4] = {3, 2, 0, 3};
        cw = uint16_t((cw & ~0x0300) | (pc[(value >> 16) & 3] << 8));
    }
    c->fpu_cw = cw;
    if (cur) {
        static const uint32_t pc_back[4] = {0x20000, 0, 0x10000, 0};
        W32(cur, ((uint32_t(cw >> 10) & 3) << 8) | pc_back[(cw >> 8) & 3] | 0x9001f);
    }
    Return(c, 0);
}
C(localeconv) {
    if (!g_lconv) {
        g_lconv = HeapAlloc(64);
        memset(Ptr<void>(g_lconv), 0, 64);
        uint32_t dot = GuestStrdup("."), empty = GuestStrdup("");
        W32(g_lconv, dot);
        for (int i = 1; i < 10; ++i) W32(g_lconv + 4 * i, empty);
    }
    Return(c, g_lconv);
}
C(exit) {
    Log("exit(%d)", int32_t(Arg(c, 0)));
    _exit(int(Arg(c, 0)));
}
C(_exit) { _exit(int(Arg(c, 0))); }
C(abort) { Fatal("abort() from %08x", R32(c->esp)); }
C(_amsg_exit) { Fatal("_amsg_exit(%u)", Arg(c, 0)); }
C(_purecall) { Fatal("pure virtual call from %08x", R32(c->esp)); }
C(terminate) { Fatal("terminate() from %08x", R32(c->esp)); }
// Printable guest string at a (or "" when it does not look like one), for exception diagnostics.
std::string MaybeString(uint32_t a) {
    if (a < 0x10000 || a >= 0x80000000u) return "";
    std::string out;
    for (uint32_t i = 0; i < 160; ++i) {
        uint8_t ch = R8(a + i);
        if (!ch) break;
        if (ch < 0x20 && ch != '\n' && ch != '\t') return "";
        out += char(ch);
    }
    return out.size() >= 3 ? out : "";
}
C(_CxxThrowException) {
    // Not supported yet: log what was thrown (the object's dwords and any strings they point to) before stopping.
    uint32_t obj = Arg(c, 0);
    Log("C++ exception from %08x: object %08x, throw info %08x", R32(c->esp), obj, Arg(c, 1));
    for (uint32_t i = 0; obj && i < 16; ++i) {
        uint32_t v = R32(obj + 4 * i);
        std::string str = MaybeString(v);
        if (str.empty()) {  // one level deeper: a pointer to a structure holding a string pointer
            for (uint32_t k = 0; k < 4 && str.empty(); ++k) str = v >= 0x10000 && v < 0x80000000u ? MaybeString(R32(v + 4 * k)) : "";
        }
        Log("  +%02x: %08x %s", 4 * i, v, str.c_str());
    }
    Fatal("C++ exception thrown from %08x (object %08x, type %08x): not supported yet", R32(c->esp), obj, Arg(c, 1));
}
C(__CppXcptFilter) { Return(c, 0); }
C(_except_handler4_common) { Return(c, 1); }  // ExceptionContinueSearch

// ---- MSVC RTTI ----
// Complete object locator (vtable[-1]): signature, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor.
// Hierarchy: signature, attributes, numBaseClasses, pBaseClassArray. Base class descriptor: pTypeDescriptor,
// numContainedBases, mdisp, pdisp, vdisp, attributes. Type descriptor: vftable, spare, decorated name at +8.
bool SameType(uint32_t a, uint32_t b) { return a == b || strcmp(Str(a + 8), Str(b + 8)) == 0; }
uint32_t CompleteObject(uint32_t obj, uint32_t* col_out) {
    uint32_t col = R32(R32(obj) - 4);
    if (col_out) *col_out = col;
    return obj - R32(col + 4);
}
C(__RTtypeid) {
    uint32_t obj = Arg(c, 0);
    if (!obj) Fatal("typeid of a null pointer from %08x", R32(c->esp));
    Return(c, R32(R32(R32(obj) - 4) + 12));
}
C(__RTDynamicCast) {
    uint32_t obj = Arg(c, 0), target = Arg(c, 3), is_ref = Arg(c, 4);
    if (!obj) {
        Return(c, 0);
        return;
    }
    uint32_t col;
    uint32_t complete = CompleteObject(obj, &col);
    uint32_t chd = R32(col + 16);
    uint32_t n = R32(chd + 8), bases = R32(chd + 12);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t bcd = R32(bases + 4 * i);
        if (!SameType(R32(bcd), target)) continue;
        int32_t mdisp = int32_t(R32(bcd + 8)), pdisp = int32_t(R32(bcd + 12)), vdisp = int32_t(R32(bcd + 16));
        int32_t off = mdisp;
        if (pdisp >= 0) off += pdisp + int32_t(R32(R32(complete + uint32_t(pdisp)) + uint32_t(vdisp)));
        Return(c, complete + uint32_t(off));
        return;
    }
    if (is_ref) Fatal("dynamic_cast<&> failed (bad_cast) from %08x", R32(c->esp));
    Return(c, 0);
}
// type_info members (thiscall: this in ecx)
C(type_info_eq) { Return(c, SameType(c->ecx, Arg(c, 0)) ? 1 : 0, 1); }
C(type_info_before) { Return(c, strcmp(Str(c->ecx + 8), Str(Arg(c, 0) + 8)) < 0 ? 1 : 0, 1); }
C(type_info_name) { Return(c, c->ecx + 8, 1); }
C(type_info_dtor) { Return(c, 0); }



// ---- MSVCR90 additions for SporeApp.exe: CRT startup, wide strings, aligned heap, threads ----
C(__set_app_type) { Return(c, 0); }
C(__setusermatherr) { Return(c, 0); }
C(_configthreadlocale) { Return(c, 2); }  // _DISABLE_PER_THREAD_LOCALE: one locale for the process
C(_invoke_watson) { Fatal("_invoke_watson from %08x (invalid CRT parameter)", R32(c->esp)); }
C(_XcptFilter) { Return(c, 0); }  // EXCEPTION_CONTINUE_SEARCH
C(_cexit) {
    for (auto it = g_onexit.rbegin(); it != g_onexit.rend(); ++it) GuestCall(*it, {});
    g_onexit.clear();
    Return(c, 0);
}
uint32_t g_fmode = 0, g_commode = 0;
C(__p__fmode) {
    if (!g_fmode) g_fmode = HeapAlloc(4), W32(g_fmode, 0);
    Return(c, g_fmode);
}
C(__p__commode) {
    if (!g_commode) g_commode = HeapAlloc(4), W32(g_commode, 0);
    Return(c, g_commode);
}
// __getmainargs(int* argc, char*** argv, char*** env, int expand, _startupinfo*)
C(__getmainargs) {
    static uint32_t argv = 0, envp = 0;
    if (!argv) {
        argv = HeapAlloc(8);
        W32(argv, GuestStrdup("C:\\Game\\SporebinEP1\\SporeApp.exe"));
        W32(argv + 4, 0);
        envp = HeapAlloc(4);
        W32(envp, 0);
    }
    W32(Arg(c, 0), 1);
    W32(Arg(c, 1), argv);
    W32(Arg(c, 2), envp);
    Return(c, 0);
}
C(_ismbblead) { Return(c, 0); }  // single-byte code page
C(_setmbcp) { Return(c, 0); }
C(setlocale) {
    static uint32_t name = GuestStrdup("C");
    Return(c, name);
}
C(signal) { Return(c, 0); }  // SIG_DFL as the previous handler
C(_getch) { Return(c, 0x0d); }
C(getenv) { Return(c, 0); }
C(tmpfile) { Return(c, 0); }
C(_getdrive) { Return(c, 3); }  // C:
C(_control87) {
    // Only the precision/rounding bits matter to recompiled x87 code (c->fpu_cw); exceptions stay masked.
    uint32_t value = Arg(c, 0), mask = Arg(c, 1);
    uint32_t cw = c->fpu_cw;
    // _MCW_RC 0x300 <-> RC bits 10-11, _MCW_PC 0x30000 <-> PC bits 8-9 (_PC_24 0x20000 = single, _PC_53 0x10000).
    uint32_t rc = (cw >> 10) & 3, pc = (cw >> 8) & 3;
    uint32_t cur = (rc << 8) | (pc == 0 ? 0x20000u : pc == 2 ? 0x10000u : 0u) | 0x9001fu;
    uint32_t next = (cur & ~mask) | (value & mask);
    rc = (next >> 8) & 3;
    pc = (next & 0x30000u) == 0x20000u ? 0 : (next & 0x30000u) == 0x10000u ? 2 : 3;
    c->fpu_cw = uint16_t((cw & ~0x0f00u) | (rc << 10) | (pc << 8));
    Return(c, next);
}
C(_controlfp) { m__control87(c); }
C(strcmp) { Return(c, uint32_t(strcmp(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(isprint) { Return(c, Arg(c, 0) >= 0x20 && Arg(c, 0) < 0x7f ? 1 : 0); }
C(atol) { Return(c, uint32_t(atoi(Str(Arg(c, 0))))); }
C(vsprintf) {
    std::string s = GuestFormat(Str(Arg(c, 1)), Arg(c, 2));
    memcpy(Str(Arg(c, 0)), s.c_str(), s.size() + 1);
    Return(c, uint32_t(s.size()));
}
C(vfprintf) {
    std::string s = GuestFormat(Str(Arg(c, 1)), Arg(c, 2));
    Log("vfprintf: %s", s.c_str());
    Return(c, uint32_t(s.size()));
}
C(printf) {
    std::string s = GuestFormat(Str(Arg(c, 0)), c->esp + 8);
    Log("printf: %s", s.c_str());
    Return(c, uint32_t(s.size()));
}
std::string IntToBase(uint64_t v, unsigned radix, bool negative) {
    std::string r;
    do {
        unsigned d = unsigned(v % radix);
        r += char(d < 10 ? '0' + d : 'a' + d - 10);
        v /= radix;
    } while (v);
    if (negative) r += '-';
    return std::string(r.rbegin(), r.rend());
}
C(_ltoa) {
    int32_t v = int32_t(Arg(c, 0));
    unsigned radix = Arg(c, 2);
    bool neg = radix == 10 && v < 0;
    std::string r = IntToBase(neg ? uint64_t(-int64_t(v)) : uint64_t(uint32_t(v)), radix, neg);
    strcpy(Str(Arg(c, 1)), r.c_str());
    Return(c, Arg(c, 1));
}
C(_ultoa) {
    std::string r = IntToBase(Arg(c, 0), Arg(c, 2), false);
    strcpy(Str(Arg(c, 1)), r.c_str());
    Return(c, Arg(c, 1));
}
C(_ltow) {
    int32_t v = int32_t(Arg(c, 0));
    unsigned radix = Arg(c, 2);
    bool neg = radix == 10 && v < 0;
    PutWideString(Arg(c, 1), 34, IntToBase(neg ? uint64_t(-int64_t(v)) : uint64_t(uint32_t(v)), radix, neg));
    Return(c, Arg(c, 1));
}
C(_ultow) {
    PutWideString(Arg(c, 1), 34, IntToBase(Arg(c, 0), Arg(c, 2), false));
    Return(c, Arg(c, 1));
}
C(_finite) {
    double v = RF64(c->esp + 4);
    Return(c, std::isfinite(v) ? 1 : 0);
}
C(_CIsin) { CiUnary(c, [](double x) { return std::sin(x); }); }
C(_CIlog) { c->st[c->st_top] = log(c->st[c->st_top]); c->ret_target = Pop32(c); }
// _ecvt/_fcvt(double, digits, int* decpt, int* sign): digits without the point, in a per-thread buffer.
std::string Cvt(double v, int digits, bool fixed, int* decpt, int* sign) {
    *sign = std::signbit(v) ? 1 : 0;
    v = fabs(v);
    char buf[512];
    if (fixed) snprintf(buf, sizeof buf, "%.*f", digits < 0 ? 0 : digits, v);
    else snprintf(buf, sizeof buf, "%.*e", digits > 0 ? digits - 1 : 0, v);
    std::string mant;
    int exp10 = 0;
    if (fixed) {
        std::string t(buf);
        size_t dot = t.find('.');
        std::string ip = dot == std::string::npos ? t : t.substr(0, dot);
        std::string fp = dot == std::string::npos ? "" : t.substr(dot + 1);
        size_t nz = ip.find_first_not_of('0');
        ip = nz == std::string::npos ? "" : ip.substr(nz);
        mant = ip + fp;
        exp10 = int(ip.size());
    } else {
        std::string t(buf);
        size_t e = t.find('e');
        for (char ch : t.substr(0, e)) if (ch != '.') mant += ch;
        exp10 = atoi(t.c_str() + e + 1) + 1;
    }
    *decpt = v == 0 ? 0 : exp10;
    return mant;
}
C(_ecvt) {
    static thread_local uint32_t buf = 0;
    if (!buf) buf = HeapAlloc(512);
    int decpt, sign;
    std::string m = Cvt(RF64(c->esp + 4), int32_t(Arg(c, 2)), false, &decpt, &sign);
    strcpy(Str(buf), m.c_str());
    W32(Arg(c, 3), uint32_t(decpt));
    W32(Arg(c, 4), uint32_t(sign));
    Return(c, buf);
}
C(_fcvt) {
    static thread_local uint32_t buf = 0;
    if (!buf) buf = HeapAlloc(512);
    int decpt, sign;
    std::string m = Cvt(RF64(c->esp + 4), int32_t(Arg(c, 2)), true, &decpt, &sign);
    strcpy(Str(buf), m.c_str());
    W32(Arg(c, 3), uint32_t(decpt));
    W32(Arg(c, 4), uint32_t(sign));
    Return(c, buf);
}
// Aligned heap: the block's start (for free) sits in the dword before the aligned pointer.
C(_aligned_malloc) {
    uint32_t size = Arg(c, 0), align = Arg(c, 1) < 4 ? 4 : Arg(c, 1);
    uint32_t raw = HeapAlloc(size + align + 4);
    if (!raw) {
        Return(c, 0);
        return;
    }
    uint32_t p = (raw + 4 + align - 1) & ~(align - 1);
    W32(p - 4, raw);
    Return(c, p);
}
C(_aligned_free) {
    if (Arg(c, 0)) HeapFree(R32(Arg(c, 0) - 4));
    Return(c, 0);
}
C(_aligned_realloc) {
    uint32_t old = Arg(c, 0), size = Arg(c, 1), align = Arg(c, 2) < 4 ? 4 : Arg(c, 2);
    if (!old) {
        uint32_t raw = HeapAlloc(size + align + 4), p = (raw + 4 + align - 1) & ~(align - 1);
        W32(p - 4, raw);
        Return(c, p);
        return;
    }
    uint32_t raw_old = R32(old - 4);
    uint32_t avail = HeapSize(raw_old) - (old - raw_old);
    uint32_t raw = HeapAlloc(size + align + 4), p = (raw + 4 + align - 1) & ~(align - 1);
    W32(p - 4, raw);
    memcpy(Ptr<void>(p), Ptr<void>(old), avail < size ? avail : size);
    HeapFree(raw_old);
    Return(c, p);
}
C(_beginthreadex) { BeginThreadEx(c); }
C(_time64) {
    uint64_t t = uint64_t(time(nullptr));
    if (Arg(c, 0)) W64(Arg(c, 0), t);
    c->edx = uint32_t(t >> 32);
    Return(c, uint32_t(t));
}
C(_time32) {
    uint32_t t = uint32_t(time(nullptr));
    if (Arg(c, 0)) W32(Arg(c, 0), t);
    Return(c, t);
}
struct tm ReadTm(uint32_t a) {
    struct tm t {};
    t.tm_sec = int(R32(a)); t.tm_min = int(R32(a + 4)); t.tm_hour = int(R32(a + 8)); t.tm_mday = int(R32(a + 12));
    t.tm_mon = int(R32(a + 16)); t.tm_year = int(R32(a + 20)); t.tm_wday = int(R32(a + 24));
    t.tm_yday = int(R32(a + 28)); t.tm_isdst = int(R32(a + 32));
    return t;
}
C(_mktime64) {
    struct tm t = ReadTm(Arg(c, 0));
    uint64_t v = uint64_t(int64_t(mktime(&t)));
    c->edx = uint32_t(v >> 32);
    Return(c, uint32_t(v));
}
C(strftime) {
    struct tm t = ReadTm(Arg(c, 3));
    char buf[1024];
    size_t n = strftime(buf, sizeof buf < Arg(c, 1) ? sizeof buf : Arg(c, 1), Str(Arg(c, 2)), &t);
    memcpy(Str(Arg(c, 0)), buf, n + 1);
    Return(c, uint32_t(n));
}
// ---- stat ----
void WriteStat64i32(uint32_t b, const struct stat& st) {
    // struct _stat64i32: dev, ino(16), mode(16), nlink(16), uid, gid, rdev, size (32), atime/mtime/ctime (64)
    memset(Ptr<void>(b), 0, 48);
    uint16_t mode = (S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | 0x100 | ((st.st_mode & S_IWUSR) ? 0x80 : 0);
    W16(b + 6, mode);
    W16(b + 8, 1);
    W32(b + 20, uint32_t(st.st_size));
    W64(b + 24, uint64_t(st.st_atime));
    W64(b + 32, uint64_t(st.st_mtime));
    W64(b + 40, uint64_t(st.st_ctime));
}
void StatCommon(Ctx* c, const std::string& guest_path) {
    struct stat st;
    if (stat(HostPath(guest_path.c_str()).c_str(), &st) != 0) {
        Errno() = 2;
        Return(c, 0xffffffffu);
        return;
    }
    WriteStat64i32(Arg(c, 1), st);
    Return(c, 0);
}
C(_stat64i32) { StatCommon(c, Str(Arg(c, 0))); }
C(_wstat64i32) { StatCommon(c, WideToUtf8(Arg(c, 0))); }
C(_wgetcwd) {
    uint32_t buf = Arg(c, 0), size = Arg(c, 1);
    if (!buf) buf = HeapAlloc(size = 520);
    PutWideString(buf, size, GuestCurrentDir());
    Return(c, buf);
}
C(_wgetdcwd) {
    uint32_t buf = Arg(c, 1), size = Arg(c, 2);
    if (!buf) buf = HeapAlloc(size = 520);
    PutWideString(buf, size, GuestCurrentDir());
    Return(c, buf);
}
C(_searchenv) { Str(Arg(c, 2))[0] = 0; Return(c, 0); }
C(_wsearchenv) { W16(Arg(c, 2), 0); Return(c, 0); }
C(_spawnv) {
    Log("_spawnv(%s): not supported", Str(Arg(c, 1)));
    Errno() = 2;
    Return(c, 0xffffffffu);
}
C(_wspawnv) {
    Log("_wspawnv(%s): not supported", WideToUtf8(Arg(c, 1)).c_str());
    Errno() = 2;
    Return(c, 0xffffffffu);
}
// ---- wide strings (UTF-16 in guest memory) ----
C(wcschr) {
    uint32_t s = Arg(c, 0);
    uint16_t ch = uint16_t(Arg(c, 1));
    for (;; s += 2) {
        uint16_t v = R16(s);
        if (v == ch) { Return(c, s); return; }
        if (!v) { Return(c, 0); return; }
    }
}
C(wcsrchr) {
    uint32_t s = Arg(c, 0), last = 0;
    uint16_t ch = uint16_t(Arg(c, 1));
    for (;; s += 2) {
        uint16_t v = R16(s);
        if (v == ch) last = s;
        if (!v) break;
    }
    Return(c, last);
}
C(wcscspn) {
    uint32_t s = Arg(c, 0), set = Arg(c, 1), n = 0, m = WLen(set);
    for (;; ++n) {
        uint16_t v = R16(s + 2 * n);
        if (!v) break;
        bool hit = false;
        for (uint32_t k = 0; k < m && !hit; ++k) hit = R16(set + 2 * k) == v;
        if (hit) break;
    }
    Return(c, n);
}
C(wcsncpy) {
    uint32_t d = Arg(c, 0), s = Arg(c, 1), n = Arg(c, 2), i = 0;
    for (; i < n && R16(s + 2 * i); ++i) W16(d + 2 * i, R16(s + 2 * i));
    for (; i < n; ++i) W16(d + 2 * i, 0);
    Return(c, d);
}
C(wcsncat) {
    uint32_t d = Arg(c, 0), s = Arg(c, 1), n = Arg(c, 2), e = d + 2 * WLen(d), i = 0;
    for (; i < n && R16(s + 2 * i); ++i) W16(e + 2 * i, R16(s + 2 * i));
    W16(e + 2 * i, 0);
    Return(c, d);
}
int WCompare(uint32_t a, uint32_t b, uint32_t n, bool icase) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = R16(a + 2 * i), y = R16(b + 2 * i);
        if (icase) x = towlower(x), y = towlower(y);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
C(wcsncmp) { Return(c, uint32_t(WCompare(Arg(c, 0), Arg(c, 1), Arg(c, 2), false))); }
C(_wcsicmp) { Return(c, uint32_t(WCompare(Arg(c, 0), Arg(c, 1), 0xffffffffu, true))); }
C(_wcsnicmp) { Return(c, uint32_t(WCompare(Arg(c, 0), Arg(c, 1), Arg(c, 2), true))); }
C(_wcslwr) {
    for (uint32_t s = Arg(c, 0); R16(s); s += 2) W16(s, uint16_t(towlower(R16(s))));
    Return(c, Arg(c, 0));
}
C(iswctype) {
    uint32_t ch = Arg(c, 0) & 0xffff, mask = Arg(c, 1) & 0xffff;
    uint32_t r = 0;
    if ((mask & 0x1) && iswupper(ch)) r |= 0x1;      // _UPPER
    if ((mask & 0x2) && iswlower(ch)) r |= 0x2;      // _LOWER
    if ((mask & 0x4) && iswdigit(ch)) r |= 0x4;      // _DIGIT
    if ((mask & 0x8) && iswspace(ch)) r |= 0x8;      // _SPACE
    if ((mask & 0x10) && iswpunct(ch)) r |= 0x10;    // _PUNCT
    if ((mask & 0x20) && iswcntrl(ch)) r |= 0x20;    // _CONTROL
    if ((mask & 0x40) && ch == ' ') r |= 0x40;       // _BLANK
    if ((mask & 0x80) && iswxdigit(ch)) r |= 0x80;   // _HEX
    if ((mask & 0x100) && iswalpha(ch)) r |= 0x100;  // _ALPHA
    Return(c, r);
}
long WideToLong(Ctx* c, int base_arg, bool is_unsigned, uint32_t* end_out) {
    std::string s = WideToUtf8(Arg(c, 0));
    char* end;
    long v = is_unsigned ? long(strtoul(s.c_str(), &end, base_arg >= 0 ? int(Arg(c, base_arg)) : 10))
                         : strtol(s.c_str(), &end, base_arg >= 0 ? int(Arg(c, base_arg)) : 10);
    // The prefix consumed is ASCII here, so the byte count is the UTF-16 unit count.
    if (end_out) *end_out = Arg(c, 0) + 2 * uint32_t(end - s.c_str());
    return v;
}
C(wcstol) {
    uint32_t end;
    long v = WideToLong(c, 2, false, &end);
    if (Arg(c, 1)) W32(Arg(c, 1), end);
    Return(c, uint32_t(int32_t(v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : v)));
}
C(wcstoul) {
    uint32_t end;
    long v = WideToLong(c, 2, true, &end);
    if (Arg(c, 1)) W32(Arg(c, 1), end);
    Return(c, uint32_t(v));
}
C(_wtol) { Return(c, uint32_t(int32_t(WideToLong(c, -1, false, nullptr)))); }
C(wcstod) {
    std::string s = WideToUtf8(Arg(c, 0));
    char* end;
    double v = strtod(s.c_str(), &end);
    if (Arg(c, 1)) W32(Arg(c, 1), Arg(c, 0) + 2 * uint32_t(end - s.c_str()));
    ReturnF(c, v);
}
// C++ exceptions: not supported yet (see _CxxThrowException); a frame handler only sees SEH unwinds.
C(__CxxFrameHandler3) { Return(c, 1); }  // ExceptionContinueSearch
C(_except_handler3) { Return(c, 1); }

#undef C

#define E(name) {"msvcr90.dll", #name, m_##name}
const ImportDef kMsvcr80[] = {
    E(__set_app_type), E(__setusermatherr), E(_configthreadlocale), E(_invoke_watson), E(_XcptFilter), E(_cexit), E(__p__fmode), E(__p__commode), E(__getmainargs), E(_ismbblead), E(_setmbcp), E(setlocale), E(signal), E(_getch), E(getenv), E(tmpfile), E(_getdrive), E(_control87), E(_controlfp), E(strcmp), E(isprint), E(atol), E(vsprintf), E(vfprintf), E(printf), E(_ltoa), E(_ultoa), E(_ltow), E(_ultow), E(_finite), E(_CIsin), E(_CIlog), E(_ecvt), E(_fcvt), E(_aligned_malloc), E(_aligned_free), E(_aligned_realloc), E(_beginthreadex), E(_time64), E(_time32), E(_mktime64), E(strftime), E(_stat64i32), E(_wstat64i32), E(_wgetcwd), E(_wgetdcwd), E(_searchenv), E(_wsearchenv), E(_spawnv), E(_wspawnv), E(wcschr), E(wcsrchr), E(wcscspn), E(wcsncpy), E(wcsncat), E(wcsncmp), E(_wcsicmp), E(_wcsnicmp), E(_wcslwr), E(iswctype), E(wcstol), E(wcstoul), E(_wtol), E(wcstod), E(__CxxFrameHandler3), E(_except_handler3),
    E(memset), E(memcpy), E(memmove), E(memmove_s), E(memcpy_s), E(memchr), E(strchr), E(strrchr), E(strstr),
    E(strpbrk), E(strspn), E(strcspn), E(strncmp), E(strcoll), E(_stricmp), E(_strnicmp), E(strncpy), E(strncat),
    E(strnlen), E(strcpy_s), E(strcat_s), E(strncpy_s), E(strtok), E(_strupr), E(_strlwr), E(strerror),
    E(_itoa_s), E(toupper), E(tolower), E(isspace), E(isdigit), E(isxdigit), E(isalpha), E(isalnum), E(islower),
    E(isupper), E(ispunct), E(iscntrl), E(iswalnum), E(iswspace), E(towupper), E(towlower), E(wcscpy_s),
    E(wcscat_s), E(wcsstr), E(wcsrtombs_s), E(swscanf), E(atoi), E(atof), E(strtod), E(strtol), E(strtoul),
    E(_strtoui64), E(sprintf), E(sprintf_s), E(_snprintf), E(_snprintf_s), E(_vsnprintf), E(_vsnprintf_s),
    E(sscanf), E(sscanf_s), E(fprintf), E(_set_printf_count_output), E(__iob_func), E(fopen), E(fopen_s),
    E(fclose), E(fread), E(fwrite), E(fseek), E(ftell), E(rewind), E(fflush), E(feof), E(ferror), E(fgets),
    E(getc), E(ungetc), E(fputs), E(fputc), E(_stat64), E(malloc), E(_malloc_crt), E(free), E(realloc),
    E(floor), E(ceil), E(ldexp), E(frexp), E(modf), E(_isnan), E(_CIsqrt), E(_CIcos), E(_CIasin), E(_CIacos),
    E(_CItanh), E(_CIsinh), E(_CIcosh), E(_CIpow), E(_CIfmod), E(rand), E(srand), E(qsort), E(_localtime64),
    E(_gmtime64), E(_ftime64), E(_initterm), E(_initterm_e), E(_onexit), E(__dllonexit), E(_lock), E(_unlock),
    E(_encode_pointer), E(_decode_pointer), E(_encoded_null), E(_crt_debugger_hook),
    E(__clean_type_info_names_internal), E(_set_purecall_handler), E(_errno), E(_controlfp_s), E(localeconv),
    E(exit), E(_exit), E(abort), E(_amsg_exit), E(_purecall), E(_CxxThrowException), E(__CppXcptFilter),
    E(_except_handler4_common), E(__RTtypeid), E(__RTDynamicCast),
    {"msvcr90.dll", "?terminate@@YAXXZ", m_terminate},
    {"msvcr90.dll", "??8type_info@@QBE_NABV0@@Z", m_type_info_eq},
    {"msvcr90.dll", "?before@type_info@@QBEHABV1@@Z", m_type_info_before},
    {"msvcr90.dll", "?_name_internal_method@type_info@@QBEPBDPAU__type_info_node@@@Z", m_type_info_name},
    {"msvcr90.dll", "?_type_info_dtor_internal_method@type_info@@QAEXXZ", m_type_info_dtor},
};
#undef E
ImportRegistrar g_msvcr80(kMsvcr80);

}  // namespace

}  // namespace native
