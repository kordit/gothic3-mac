// msvcrt.dll for a module that imports the system C runtime instead of linking
// its own (Gothic 3's d3dx9_40.dll, 97 imports). The C locale, one heap shared
// with the rest of the guest, no console: stdout and stderr go to the log, and
// the guest's FILE table names only those three streams. setjmp and longjmp
// are the translator's intrinsics (runtime/intrinsics.h).
//
// C++ exception dispatch (__CxxFrameHandler) is not modelled here. A game
// points it at a statically linked CRT's handler in one of its own modules
// (game.toml import_forwards); without that, a C++ exception passing through
// the module is unhandled and the runtime says so.
#include "imports.h"
#include "intrinsics.h"
#include "memory.h"
#include "seh.h"
#include "win32.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {
const char *const DLL = "msvcrt.dll";

double arg_double(X86 *c, int i) {
    uint64_t bits = (uint64_t)arg(c, i) | ((uint64_t)arg(c, i + 1) << 32);
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}
void wr_double(uint32_t a, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    wr32(a, (uint32_t)bits);
    wr32(a + 4, (uint32_t)(bits >> 32));
}
// A double result comes back in ST(0), as from the x86 CRT.
void ret_double(X86 *c, double v) {
    fpush(c, v);
}

// One guest word or string the runtime owns; re-made after mem_init().
uint32_t kit_block(uint32_t &slot, uint32_t size) {
    if (!slot || !heap_owns(slot))
        slot = heap_alloc_kit(size, true);
    return slot;
}
uint32_t kit_string(uint32_t &slot, const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!slot || !heap_owns(slot)) {
        slot = heap_alloc_kit(n, true);
        if (slot)
            memcpy(gm_ptr(slot), s, n);
    }
    return slot;
}

uint32_t g_errno;
void set_errno(int value) {
    if (uint32_t at = kit_block(g_errno, 4))
        wr32(at, (uint32_t)value);
}
const int CRT_EBADF = 9, CRT_ERANGE = 34, CRT_EILSEQ = 42;

void exit_process(X86 *c, uint32_t code) {
    guest_call(c, imports_resolve("KERNEL32.dll", "ExitProcess"), code);
}

// ---------------------------------------------------------------- memory ---
uint32_t crt_alloc(uint32_t size, bool zero) {
    return heap_alloc(size ? size : 1, win32_heap_zeroes(size, zero));
}
void malloc_(X86 *c) {
    set_eax(c, crt_alloc(arg(c, 0), false));
}
void calloc_(X86 *c) {
    uint64_t total = (uint64_t)arg(c, 0) * arg(c, 1);
    set_eax(c, total > 0xffffffffu ? 0 : crt_alloc((uint32_t)total, true));
}
void free_(X86 *c) {
    if (uint32_t p = arg(c, 0))
        heap_free(p);
}
void realloc_(X86 *c) {
    uint32_t p = arg(c, 0), size = arg(c, 1);
    if (!p) {
        set_eax(c, crt_alloc(size, false));
    } else if (!size) {
        heap_free(p);
        set_eax(c, 0);
    } else {
        set_eax(c, heap_realloc(p, size, win32_heap_zeroes(size, false)));
    }
}
void strdup_(X86 *c) {
    uint32_t s = arg(c, 0);
    if (!s) {
        set_eax(c, 0);
        return;
    }
    std::string text = gm_str(s);
    uint32_t p = crt_alloc((uint32_t)text.size() + 1, false);
    if (p)
        memcpy(gm_ptr(p), text.c_str(), text.size() + 1);
    set_eax(c, p);
}

// --------------------------------------------------------------- strings ---
// memcpy is memmove in msvcrt, and code that overlaps its copies relies on it.
void memmove_(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), n = arg(c, 2);
    if (n && gm_valid(dst, n) && gm_valid(src, n))
        memmove(gm_ptr(dst), gm_ptr(src), n);
    set_eax(c, dst);
}
void memset_(X86 *c) {
    uint32_t dst = arg(c, 0), n = arg(c, 2);
    if (n && gm_valid(dst, n))
        memset(gm_ptr(dst), (int)arg(c, 1), n);
    set_eax(c, dst);
}
void strchr_(X86 *c) {
    uint32_t s = arg(c, 0);
    uint8_t ch = (uint8_t)arg(c, 1);
    for (;; ++s) {
        uint8_t b = rd8(s);
        if (b == ch) {
            set_eax(c, s);
            return;
        }
        if (!b)
            break;
    }
    set_eax(c, 0);
}
void strrchr_(X86 *c) {
    uint32_t s = arg(c, 0), found = 0;
    uint8_t ch = (uint8_t)arg(c, 1);
    for (;; ++s) {
        uint8_t b = rd8(s);
        if (b == ch)
            found = s;
        if (!b)
            break;
    }
    set_eax(c, found);
}
int ascii_lower(int b) {
    return b >= 'A' && b <= 'Z' ? b + 32 : b;
}
int compare(uint32_t a, uint32_t b, uint32_t n, bool fold) {
    for (uint32_t i = 0; i < n; ++i) {
        int x = rd8(a + i), y = rd8(b + i);
        if (fold) {
            x = ascii_lower(x);
            y = ascii_lower(y);
        }
        if (x != y)
            return x - y;
        if (!x)
            break;
    }
    return 0;
}
void strncmp_(X86 *c) {
    set_eax(c, (uint32_t)compare(arg(c, 0), arg(c, 1), arg(c, 2), false));
}
void stricmp_(X86 *c) {
    set_eax(c, (uint32_t)compare(arg(c, 0), arg(c, 1), 0xffffffffu, true));
}
void strnicmp_(X86 *c) {
    set_eax(c, (uint32_t)compare(arg(c, 0), arg(c, 1), arg(c, 2), true));
}

// ------------------------------------------------------- characters (C) ---
// The C locale: classification and case mapping know ASCII only.
int ch_arg(X86 *c) {
    return (int)arg(c, 0);
}
bool ascii(int ch) {
    return ch >= 0 && ch < 128;
}
void tolower_(X86 *c) {
    set_eax(c, (uint32_t)ascii_lower(ch_arg(c)));
}
void toupper_(X86 *c) {
    int ch = ch_arg(c);
    set_eax(c, (uint32_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
}
void isalnum_(X86 *c) {
    set_eax(c, ascii(ch_arg(c)) && isalnum(ch_arg(c)) ? 1 : 0);
}
void isalpha_(X86 *c) {
    set_eax(c, ascii(ch_arg(c)) && isalpha(ch_arg(c)) ? 1 : 0);
}
void isdigit_(X86 *c) {
    set_eax(c, ascii(ch_arg(c)) && isdigit(ch_arg(c)) ? 1 : 0);
}
void isspace_(X86 *c) {
    set_eax(c, ascii(ch_arg(c)) && isspace(ch_arg(c)) ? 1 : 0);
}
void isxdigit_(X86 *c) {
    set_eax(c, ascii(ch_arg(c)) && isxdigit(ch_arg(c)) ? 1 : 0);
}
// Code page 1252 has no lead bytes.
void isleadbyte_(X86 *c) {
    set_eax(c, 0);
}
// Wide classification over Latin-1, which is what the C locale's conversions produce.
uint32_t wch_arg(X86 *c) {
    return arg(c, 0) & 0xffff;
}
void iswspace_(X86 *c) {
    uint32_t w = wch_arg(c);
    set_eax(c, (w < 128 && isspace((int)w)) || w == 0xa0 ? 1 : 0);
}
bool latin1_alpha(uint32_t w) {
    return (w < 128 && isalpha((int)w)) || w == 0xaa || w == 0xb5 || w == 0xba ||
           (w >= 0xc0 && w <= 0xff && w != 0xd7 && w != 0xf7);
}
void iswalpha_(X86 *c) {
    set_eax(c, latin1_alpha(wch_arg(c)) ? 1 : 0);
}
void iswdigit_(X86 *c) {
    uint32_t w = wch_arg(c);
    set_eax(c, w >= '0' && w <= '9' ? 1 : 0);
}
void iswpunct_(X86 *c) {
    uint32_t w = wch_arg(c);
    bool punct = (w < 128 && ispunct((int)w)) || (w >= 0xa1 && w <= 0xbf && !latin1_alpha(w)) || w == 0xd7 ||
                 w == 0xf7;
    set_eax(c, punct ? 1 : 0);
}
void wctomb_(X86 *c) {
    uint32_t out = arg(c, 0), w = arg(c, 1) & 0xffff;
    if (!out) {
        set_eax(c, 0); // no shift states
        return;
    }
    if (w > 0xff) {
        set_errno(CRT_EILSEQ);
        set_eax(c, 0xffffffffu);
        return;
    }
    wr8(out, (uint8_t)w);
    set_eax(c, 1);
}
void wcstombs_(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), n = arg(c, 2);
    uint32_t count = 0;
    for (;; src += 2) {
        uint32_t w = rd16(src);
        if (w > 0xff) {
            set_errno(CRT_EILSEQ);
            set_eax(c, 0xffffffffu);
            return;
        }
        if (dst) {
            if (count >= n)
                break;
            wr8(dst + count, (uint8_t)w);
        }
        if (!w)
            break;
        ++count;
    }
    set_eax(c, count);
}

// ------------------------------------------------------------- numbers ---
void atoi_(X86 *c) {
    set_eax(c, (uint32_t)(int32_t)strtol(gm_str(arg(c, 0)).c_str(), nullptr, 10));
}
void atof_(X86 *c) {
    ret_double(c, strtod(gm_str(arg(c, 0)).c_str(), nullptr));
}
void strtod_(X86 *c) {
    uint32_t s = arg(c, 0), end = arg(c, 1);
    std::string text = gm_str(s);
    char *stop = nullptr;
    double v = strtod(text.c_str(), &stop);
    if (end)
        wr32(end, s + (uint32_t)(stop - text.c_str()));
    if (std::isinf(v) || (v != 0 && std::fabs(v) < 2.2250738585072014e-308))
        set_errno(CRT_ERANGE);
    ret_double(c, v);
}
void itoa_into(X86 *c, uint32_t value, bool is_signed) {
    uint32_t out = arg(c, 1), radix = arg(c, 2);
    std::string text;
    if (radix < 2 || radix > 36) {
        set_eax(c, out);
        return;
    }
    bool negative = is_signed && radix == 10 && (int32_t)value < 0;
    uint32_t v = negative ? 0u - value : value;
    do {
        uint32_t d = v % radix;
        text.insert(text.begin(), (char)(d < 10 ? '0' + d : 'a' + d - 10));
        v /= radix;
    } while (v);
    if (negative)
        text.insert(text.begin(), '-');
    memcpy(gm_ptr(out), text.c_str(), text.size() + 1);
    set_eax(c, out);
}
void itoa_(X86 *c) {
    itoa_into(c, arg(c, 0), true);
}
void ultoa_(X86 *c) {
    itoa_into(c, arg(c, 0), false);
}

// The CRT's own generator, so a seeded sequence is the one the game saw.
uint32_t g_rand_seed = 1;
void srand_(X86 *c) {
    g_rand_seed = arg(c, 0);
}
void rand_(X86 *c) {
    g_rand_seed = g_rand_seed * 214013u + 2531011u;
    set_eax(c, (g_rand_seed >> 16) & 0x7fff);
}

// ----------------------------------------------------------------- math ---
void floor_(X86 *c) {
    ret_double(c, std::floor(arg_double(c, 0)));
}
void ceil_(X86 *c) {
    ret_double(c, std::ceil(arg_double(c, 0)));
}
void ldexp_(X86 *c) {
    ret_double(c, std::ldexp(arg_double(c, 0), (int)arg(c, 2)));
}
void frexp_(X86 *c) {
    int e = 0;
    double m = std::frexp(arg_double(c, 0), &e);
    if (uint32_t at = arg(c, 2))
        wr32(at, (uint32_t)e);
    ret_double(c, m);
}
void modf_(X86 *c) {
    double whole = 0;
    double frac = std::modf(arg_double(c, 0), &whole);
    if (uint32_t at = arg(c, 2))
        wr_double(at, whole);
    ret_double(c, frac);
}
void isnan_(X86 *c) {
    set_eax(c, std::isnan(arg_double(c, 0)) ? 1 : 0);
}
void finite_(X86 *c) {
    set_eax(c, std::isfinite(arg_double(c, 0)) ? 1 : 0);
}
void fpclass_(X86 *c) {
    double v = arg_double(c, 0);
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    bool negative = bits >> 63;
    uint32_t cls;
    switch (std::fpclassify(v)) {
    case FP_NAN:
        cls = (bits >> 51) & 1 ? 0x2 : 0x1; // _FPCLASS_QNAN : _FPCLASS_SNAN
        break;
    case FP_INFINITE:
        cls = negative ? 0x4 : 0x200;
        break;
    case FP_SUBNORMAL:
        cls = negative ? 0x10 : 0x80;
        break;
    case FP_ZERO:
        cls = negative ? 0x20 : 0x40;
        break;
    default:
        cls = negative ? 0x8 : 0x100;
    }
    set_eax(c, cls);
}
// The x87 status and control words in the CRT's abstract form. Translated code
// computes in doubles whatever the precision control says, so the word is
// remembered and reported, not applied.
uint32_t g_control = 0x0009001f; // _PC_53 | _RC_NEAR | every exception masked
void controlfp_(X86 *c) {
    uint32_t value = arg(c, 0), mask = arg(c, 1) & 0x000f031fu;
    g_control = (g_control & ~mask) | (value & mask);
    set_eax(c, g_control);
}
void clearfp_(X86 *c) {
    uint32_t sw = c->fpu_sw, old = 0;
    // x87 IE ZE OE UE PE DE -> _SW_INVALID ZERODIVIDE OVERFLOW UNDERFLOW INEXACT DENORMAL
    if (sw & 0x01) old |= 0x10;
    if (sw & 0x04) old |= 0x08;
    if (sw & 0x08) old |= 0x04;
    if (sw & 0x10) old |= 0x02;
    if (sw & 0x20) old |= 0x01;
    if (sw & 0x02) old |= 0x80000;
    c->fpu_sw = (uint16_t)(sw & ~0x80ffu);
    set_eax(c, old);
}
// The _CI functions take their operands on the x87 stack: x in ST(0), or for
// the two-operand ones the first in ST(1) and the second in ST(0), and leave
// the result in ST(0).
template <double (*F)(double)> void ci1(X86 *c) {
    double x = fpop(c);
    fpush(c, F(x));
}
template <double (*F)(double, double)> void ci2(X86 *c) {
    double y = fpop(c);
    double x = fpop(c);
    fpush(c, F(x, y));
}
double d_sin(double x) { return std::sin(x); }
double d_cos(double x) { return std::cos(x); }
double d_tan(double x) { return std::tan(x); }
double d_asin(double x) { return std::asin(x); }
double d_acos(double x) { return std::acos(x); }
double d_atan(double x) { return std::atan(x); }
double d_sinh(double x) { return std::sinh(x); }
double d_cosh(double x) { return std::cosh(x); }
double d_tanh(double x) { return std::tanh(x); }
double d_exp(double x) { return std::exp(x); }
double d_log(double x) { return std::log(x); }
double d_sqrt(double x) { return std::sqrt(x); }
double d_pow(double x, double y) { return std::pow(x, y); }
double d_atan2(double y, double x) { return std::atan2(y, x); }
double d_fmod(double x, double y) { return std::fmod(x, y); }

// ------------------------------------------------------------ formatting ---
// printf and scanf over guest memory: each conversion is handed to the host's
// own with the same flags, width and precision, its operands read from (or
// stored to) the guest. Length modifiers are the CRT's: h, l, ll, I64, I32,
// L, w.
struct Spec {
    std::string flags;
    int width = -1, precision = -1;
    bool star_width = false, star_precision = false, suppress = false;
    int size = 0; // 0 int, 1 short, 2 long, 3 64-bit, 4 long double, 5 wide
    char type = 0;
    std::string scanset;
};

// Reads one conversion after a '%'; returns the index past it.
size_t parse_spec(const std::string &f, size_t i, Spec &s, bool scan) {
    if (scan && i < f.size() && f[i] == '*') {
        s.suppress = true;
        ++i;
    }
    while (!scan && i < f.size() && strchr("-+ #0", f[i]))
        s.flags += f[i++];
    if (!scan && i < f.size() && f[i] == '*') {
        s.star_width = true;
        ++i;
    } else if (i < f.size() && isdigit((unsigned char)f[i])) {
        s.width = 0;
        while (i < f.size() && isdigit((unsigned char)f[i]))
            s.width = s.width * 10 + (f[i++] - '0');
    }
    if (!scan && i < f.size() && f[i] == '.') {
        ++i;
        s.precision = 0;
        if (i < f.size() && f[i] == '*') {
            s.star_precision = true;
            ++i;
        } else {
            while (i < f.size() && isdigit((unsigned char)f[i]))
                s.precision = s.precision * 10 + (f[i++] - '0');
        }
    }
    if (f.compare(i, 3, "I64") == 0) {
        s.size = 3;
        i += 3;
    } else if (f.compare(i, 3, "I32") == 0) {
        s.size = 2;
        i += 3;
    } else if (f.compare(i, 2, "ll") == 0) {
        s.size = 3;
        i += 2;
    } else if (i < f.size() && strchr("hlLw", f[i])) {
        s.size = f[i] == 'h' ? 1 : f[i] == 'l' ? 2 : f[i] == 'L' ? 4 : 5;
        ++i;
    }
    if (i < f.size())
        s.type = f[i++];
    if (scan && s.type == '[') {
        size_t start = i;
        if (i < f.size() && f[i] == '^')
            ++i;
        if (i < f.size() && f[i] == ']')
            ++i;
        while (i < f.size() && f[i] != ']')
            ++i;
        s.scanset = "[" + f.substr(start, i - start) + "]";
        if (i < f.size())
            ++i;
    }
    return i;
}

std::string guest_wide(uint32_t a, int limit) {
    std::string out;
    for (int n = 0; limit < 0 || n < limit; ++n, a += 2) {
        uint32_t w = rd16(a);
        if (!w)
            break;
        out += (char)(w > 0xff ? '?' : w);
    }
    return out;
}

std::string format_guest(const std::string &f, uint32_t ap) {
    std::string out;
    auto next = [&ap]() {
        uint32_t v = rd32(ap);
        ap += 4;
        return v;
    };
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
        Spec s;
        i = parse_spec(f, i + 1, s, false);
        if (s.star_width) {
            s.width = (int32_t)next();
            if (s.width < 0) {
                s.flags += '-';
                s.width = -s.width;
            }
        }
        if (s.star_precision)
            s.precision = (int32_t)next();
        std::string host = "%" + s.flags;
        if (s.width >= 0)
            host += std::to_string(s.width);
        if (s.precision >= 0)
            host += "." + std::to_string(s.precision);
        char buf[512];
        int n = 0;
        switch (s.type) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            bool is_signed = s.type == 'd' || s.type == 'i';
            if (s.size == 3) {
                uint64_t lo = next();
                uint64_t v = lo | ((uint64_t)next() << 32);
                host += std::string("ll") + s.type;
                n = is_signed ? snprintf(buf, sizeof buf, host.c_str(), (long long)v)
                              : snprintf(buf, sizeof buf, host.c_str(), (unsigned long long)v);
            } else {
                uint32_t v = next();
                if (s.size == 1)
                    v = is_signed ? (uint32_t)(int32_t)(int16_t)v : v & 0xffff;
                host += s.type;
                n = is_signed ? snprintf(buf, sizeof buf, host.c_str(), (int)v)
                              : snprintf(buf, sizeof buf, host.c_str(), (unsigned)v);
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A': {
            uint64_t lo = next();
            uint64_t bits = lo | ((uint64_t)next() << 32);
            double v;
            memcpy(&v, &bits, sizeof v);
            host += s.type;
            n = snprintf(buf, sizeof buf, host.c_str(), v);
            break;
        }
        case 'c': case 'C': {
            uint32_t v = next();
            char ch = (char)((s.type == 'C' || s.size == 2 || s.size == 5) && (v & 0xffff) > 0xff ? '?' : v);
            host += 'c';
            n = snprintf(buf, sizeof buf, host.c_str(), ch);
            break;
        }
        case 's': case 'S': {
            uint32_t p = next();
            bool wide = s.type == 'S' ? s.size != 1 : (s.size == 2 || s.size == 5);
            std::string text = !p ? "(null)" : wide ? guest_wide(p, s.precision) : gm_str(p);
            host += 's';
            std::vector<char> big(text.size() + (size_t)std::max(s.width, 0) + 1);
            n = snprintf(big.data(), big.size(), host.c_str(), text.c_str());
            out.append(big.data(), (size_t)std::max(n, 0));
            continue;
        }
        case 'p':
            n = snprintf(buf, sizeof buf, "%08X", next());
            break;
        case 'n': {
            uint32_t at = next();
            if (at)
                wr32(at, (uint32_t)out.size());
            continue;
        }
        default:
            // An unknown conversion is printed as written, like the CRT.
            out += s.type;
            continue;
        }
        out.append(buf, (size_t)std::min(std::max(n, 0), (int)sizeof buf - 1));
    }
    return out;
}

// _snprintf and _vsnprintf: at most `count` characters, terminated only when
// they fit; the length, or -1 when they did not.
uint32_t put_bounded(uint32_t dst, uint32_t count, const std::string &text) {
    uint32_t n = (uint32_t)std::min<size_t>(text.size(), count);
    if (n)
        memcpy(gm_ptr(dst), text.data(), n);
    if (text.size() < count)
        wr8(dst + n, 0);
    return text.size() <= count ? (uint32_t)text.size() : 0xffffffffu;
}
void snprintf_(X86 *c) {
    uint32_t esp = c->r[R_ESP];
    set_eax(c, put_bounded(arg(c, 0), arg(c, 1), format_guest(gm_str(arg(c, 2)), esp + 16)));
}
void vsnprintf_(X86 *c) {
    set_eax(c, put_bounded(arg(c, 0), arg(c, 1), format_guest(gm_str(arg(c, 2)), arg(c, 3))));
}

// sscanf: the count of stored conversions, or EOF when the input ran out
// before the first one.
int scan_guest(const std::string &input, const std::string &f, uint32_t ap) {
    auto next = [&ap]() {
        uint32_t v = rd32(ap);
        ap += 4;
        return v;
    };
    size_t in = 0;
    int stored = 0;
    bool converted = false;
    for (size_t i = 0; i < f.size();) {
        unsigned char fc = (unsigned char)f[i];
        if (isspace(fc)) {
            while (in < input.size() && isspace((unsigned char)input[in]))
                ++in;
            ++i;
            continue;
        }
        if (fc != '%' || (i + 1 < f.size() && f[i + 1] == '%')) {
            if (fc == '%')
                ++i;
            if (in >= input.size())
                return converted ? stored : -1;
            if (input[in] != f[i])
                return stored;
            ++in;
            ++i;
            continue;
        }
        Spec s;
        i = parse_spec(f, i + 1, s, true);
        if (s.type == 'n') {
            if (!s.suppress)
                wr32(next(), (uint32_t)in);
            continue;
        }
        if (s.type != 'c' && s.type != '[')
            while (in < input.size() && isspace((unsigned char)input[in]))
                ++in;
        if (in >= input.size())
            return converted ? stored : -1;
        std::string width = s.width > 0 ? std::to_string(s.width) : "";
        const char *rest = input.c_str() + in;
        int used = 0, got = 0;
        switch (s.type) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            std::string host = "%" + width + "ll" + (s.type == 'X' ? 'x' : s.type) + "%n";
            long long v = 0;
            got = sscanf(rest, host.c_str(), &v, &used);
            if (got == 1 && !s.suppress) {
                uint32_t at = next();
                if (s.size == 3) {
                    wr32(at, (uint32_t)v);
                    wr32(at + 4, (uint32_t)((unsigned long long)v >> 32));
                } else if (s.size == 1) {
                    wr16(at, (uint16_t)v);
                } else {
                    wr32(at, (uint32_t)v);
                }
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            std::string host = "%" + width + "lf%n";
            double v = 0;
            got = sscanf(rest, host.c_str(), &v, &used);
            if (got == 1 && !s.suppress) {
                uint32_t at = next();
                if (s.size == 2 || s.size == 4)
                    wr_double(at, v);
                else {
                    float fv = (float)v;
                    uint32_t bits;
                    memcpy(&bits, &fv, sizeof bits);
                    wr32(at, bits);
                }
            }
            break;
        }
        case 'c': case 's': case '[': {
            std::string spec = s.type == '[' ? s.scanset : std::string(1, s.type);
            if (s.type == 'c' && width.empty())
                width = "1";
            std::vector<char> text(input.size() - in + 2);
            std::string host = "%" + width + spec + "%n";
            got = sscanf(rest, host.c_str(), text.data(), &used);
            if (got == 1 && !s.suppress) {
                uint32_t at = next();
                size_t n = s.type == 'c' ? (size_t)used : strlen(text.data());
                memcpy(gm_ptr(at), text.data(), n);
                if (s.type != 'c')
                    wr8(at + (uint32_t)n, 0);
            }
            break;
        }
        default:
            return stored;
        }
        if (got != 1)
            return got < 0 && !converted ? -1 : stored;
        converted = true;
        in += (size_t)used;
        if (!s.suppress)
            ++stored;
    }
    return stored;
}
void sscanf_(X86 *c) {
    uint32_t esp = c->r[R_ESP];
    set_eax(c, (uint32_t)scan_guest(gm_str(arg(c, 0)), gm_str(arg(c, 1)), esp + 12));
}

// ------------------------------------------------------------------ qsort ---
// Heapsort in place, the guest comparator deciding: no guest memory beyond the
// array, and n log n calls however the input is ordered.
void qsort_(X86 *c) {
    uint32_t base = arg(c, 0), n = arg(c, 1), width = arg(c, 2), cmp = arg(c, 3);
    if (n < 2 || !width || !gm_valid(base, n * width))
        return;
    std::vector<uint8_t> tmp(width);
    auto at = [&](uint32_t i) { return base + i * width; };
    auto less = [&](uint32_t a, uint32_t b) { return (int32_t)guest_call(c, cmp, at(a), at(b)) < 0; };
    auto swap = [&](uint32_t a, uint32_t b) {
        memcpy(tmp.data(), gm_ptr(at(a)), width);
        memcpy(gm_ptr(at(a)), gm_ptr(at(b)), width);
        memcpy(gm_ptr(at(b)), tmp.data(), width);
    };
    auto sift = [&](uint32_t root, uint32_t end) {
        for (;;) {
            uint32_t child = 2 * root + 1;
            if (child >= end)
                return;
            if (child + 1 < end && less(child, child + 1))
                ++child;
            if (!less(root, child))
                return;
            swap(root, child);
            root = child;
        }
    };
    for (uint32_t i = n / 2; i-- > 0;)
        sift(i, n);
    for (uint32_t end = n - 1; end > 0; --end) {
        swap(0, end);
        sift(0, end);
    }
}

// ------------------------------------------------- startup and shutdown ---
void initterm_(X86 *c) {
    for (uint32_t p = arg(c, 0), end = arg(c, 1); p < end; p += 4)
        if (uint32_t fn = rd32(p))
            guest_call(c, fn);
}
// A DLL's own atexit table: *begin and *end bound the entries, and the block
// grows as the CRT's does.
void dllonexit_(X86 *c) {
    uint32_t fn = arg(c, 0), pbegin = arg(c, 1), pend = arg(c, 2);
    uint32_t begin = rd32(pbegin), end = rd32(pend);
    uint32_t used = begin ? end - begin : 0;
    uint32_t capacity = begin && heap_owns(begin) ? heap_size(begin) : 0;
    if (used + 4 > capacity) {
        uint32_t grown = std::max<uint32_t>(capacity * 2, 32 * 4);
        uint32_t block = begin ? heap_realloc(begin, grown) : heap_alloc(grown, true);
        if (!block) {
            set_eax(c, 0);
            return;
        }
        begin = block;
    }
    wr32(begin + used, fn);
    wr32(pbegin, begin);
    wr32(pend, begin + used + 4);
    set_eax(c, fn);
}
// The process's table; an EXE linked against msvcrt calls it through exit().
std::vector<uint32_t> g_onexit;
void onexit_(X86 *c) {
    g_onexit.push_back(arg(c, 0));
    set_eax(c, arg(c, 0));
}
void exit_(X86 *c) {
    uint32_t code = arg(c, 0);
    while (!g_onexit.empty()) {
        uint32_t fn = g_onexit.back();
        g_onexit.pop_back();
        guest_call(c, fn);
    }
    exit_process(c, code);
}
void abort_(X86 *c) {
    LOGW("msvcrt: abort() called");
    exit_process(c, 3);
}
void amsg_exit_(X86 *c) {
    LOGW("msvcrt: _amsg_exit(%u), a C runtime error", arg(c, 0));
    exit_process(c, 255);
}
void purecall_(X86 *c) {
    LOGW("msvcrt: pure virtual function call");
    exit_process(c, 255);
}
void terminate_(X86 *c) {
    LOGW("msvcrt: terminate() called");
    exit_process(c, 3);
}
// The guest runs one thread at a time; the CRT's locks have nothing to order.
void nothing(X86 *) {}
// _XcptFilter decides for a thread's top-level __except: not ours to handle.
void xcpt_filter_(X86 *c) {
    set_eax(c, 0); // EXCEPTION_CONTINUE_SEARCH
}
void type_info_dtor_(X86 *) {}

uint32_t g_c_locale;
void setlocale_(X86 *c) {
    uint32_t name = arg(c, 1);
    std::string wanted = name ? gm_str(name) : "C";
    if (wanted.empty() || wanted == "C")
        set_eax(c, kit_string(g_c_locale, "C"));
    else
        set_eax(c, 0); // only the C locale exists
}
void errno_(X86 *c) {
    set_eax(c, kit_block(g_errno, 4));
}

// C++ exceptions are raised as MSVC raises them: code E06D7363 with the magic
// number, the object and its throw information.
uint32_t g_throw_args;
void cxx_throw_(X86 *c) {
    uint32_t args = kit_block(g_throw_args, 12);
    wr32(args, 0x19930520);
    wr32(args + 4, arg(c, 0));
    wr32(args + 8, arg(c, 1));
    recomp_seh_raise(c, 0xE06D7363, 1 /* EXCEPTION_NONCONTINUABLE */, 3, args);
}
void cxx_frame_handler_(X86 *c) {
    if (log_once("msvcrt-cxx-frame-handler", "msvcrt: __CxxFrameHandler is not modelled; the C++ exception "
                                             "passes through (game.toml import_forwards can name a CRT's)"))
        LOGW("msvcrt: FuncInfo %08x, frame %08x", c->r[R_EAX], arg(c, 1));
    set_eax(c, 1); // ExceptionContinueSearch
}
// setjmp itself never reaches here: the translator takes it at the call
// site (runtime/intrinsics.h), where the host jmp_buf stays live. This shim
// answers a call it did not see, as the intrinsic's fallback does.
void setjmp3_(X86 *c) {
    recomp_setjmp(c);
}
void longjmp_(X86 *c) {
    recomp_longjmp(c);
}

// ------------------------------------------------------------ time, temp ---
struct tm local_now() {
    time_t now = time(nullptr);
    struct tm tm_now {};
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    return tm_now;
}
void strtime_(X86 *c) {
    struct tm tm_now = local_now();
    char text[16];
    snprintf(text, sizeof text, "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    memcpy(gm_ptr(arg(c, 0)), text, 9);
    set_eax(c, arg(c, 0));
}
void strdate_(X86 *c) {
    struct tm tm_now = local_now();
    char text[16];
    snprintf(text, sizeof text, "%02d/%02d/%02d", tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_year % 100);
    memcpy(gm_ptr(arg(c, 0)), text, 9);
    set_eax(c, arg(c, 0));
}
uint32_t g_tempnam_serial;
void tempnam_(X86 *c) {
    uint32_t dir = arg(c, 0), prefix = arg(c, 1);
    std::string path = dir ? gm_str(dir) : "C:\\TEMP";
    if (path.empty())
        path = "C:\\TEMP";
    if (path.back() != '\\' && path.back() != '/')
        path += '\\';
    path += (prefix ? gm_str(prefix) : "") + std::to_string(++g_tempnam_serial);
    uint32_t out = crt_alloc((uint32_t)path.size() + 1, false);
    if (out)
        memcpy(gm_ptr(out), path.c_str(), path.size() + 1);
    set_eax(c, out);
}

// ---------------------------------------------------------------- stdio ---
// FILE is 32 bytes: _ptr, _cnt, _base, _flag, _file, ... The three standard
// streams are there with no buffer and no mode, so the module's own buffered
// writers find them closed; _write and fwrite on 1 and 2 reach the log.
const uint32_t FILE_SIZE = 32, IOB_COUNT = 20;
void iob_init(uint32_t iob) {
    for (uint32_t i = 0; i < 3; ++i)
        wr32(iob + i * FILE_SIZE + 16, i);
}
// __pioinfo: 64 pointers to blocks of 32 ioinfo records (0x24 bytes each, the
// handle then the flags); the first block, for descriptors 0-31, follows them.
const uint32_t IOINFO_SIZE = 0x24, PIOINFO_SIZE = 64 * 4 + 32 * IOINFO_SIZE;
void pioinfo_init(uint32_t table) {
    uint32_t block = table + 64 * 4;
    wr32(table, block);
    for (uint32_t fd = 0; fd < 32; ++fd)
        wr32(block + fd * IOINFO_SIZE, 0xffffffffu);
    for (uint32_t fd = 0; fd < 3; ++fd)
        wr8(block + fd * IOINFO_SIZE + 4, 0x81); // FOPEN | FTEXT
}
void badioinfo_init(uint32_t info) {
    wr32(info, 0xffffffffu);
}
void log_output(uint32_t fd, uint32_t buf, uint32_t n) {
    if (!n || !gm_valid(buf, n))
        return;
    std::string text(reinterpret_cast<const char *>(gm_ptr(buf)), n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    LOGW("msvcrt: %s: %s", fd == 1 ? "stdout" : "stderr", text.c_str());
}
void write_(X86 *c) {
    uint32_t fd = arg(c, 0), buf = arg(c, 1), n = arg(c, 2);
    if (fd != 1 && fd != 2) {
        set_errno(CRT_EBADF);
        set_eax(c, 0xffffffffu);
        return;
    }
    log_output(fd, buf, n);
    set_eax(c, n);
}
void isatty_(X86 *c) {
    set_eax(c, 0);
}
void lseeki64_(X86 *c) {
    set_errno(CRT_EBADF);
    set_eax64(c, ~0ull);
}
void fileno_(X86 *c) {
    uint32_t stream = arg(c, 0);
    set_eax(c, stream ? rd32(stream + 16) : 0xffffffffu);
}
void fread_(X86 *c) {
    set_eax(c, 0);
}
void fwrite_(X86 *c) {
    uint32_t buf = arg(c, 0), size = arg(c, 1), count = arg(c, 2), stream = arg(c, 3);
    uint32_t fd = stream ? rd32(stream + 16) : 0xffffffffu;
    if (fd != 1 && fd != 2) {
        set_errno(CRT_EBADF);
        set_eax(c, 0);
        return;
    }
    log_output(fd, buf, size * count);
    set_eax(c, count);
}
void fflush_(X86 *c) {
    set_eax(c, 0);
}

const ImportShim shims[] = {
    {DLL, "malloc", ARGC_CDECL, malloc_},
    {DLL, "calloc", ARGC_CDECL, calloc_},
    {DLL, "realloc", ARGC_CDECL, realloc_},
    {DLL, "free", ARGC_CDECL, free_},
    {DLL, "??2@YAPAXI@Z", ARGC_CDECL, malloc_},
    {DLL, "??3@YAXPAX@Z", ARGC_CDECL, free_},
    {DLL, "_strdup", ARGC_CDECL, strdup_},
    {DLL, "memcpy", ARGC_CDECL, memmove_},
    {DLL, "memmove", ARGC_CDECL, memmove_},
    {DLL, "memset", ARGC_CDECL, memset_},
    {DLL, "strchr", ARGC_CDECL, strchr_},
    {DLL, "strrchr", ARGC_CDECL, strrchr_},
    {DLL, "strncmp", ARGC_CDECL, strncmp_},
    {DLL, "_stricmp", ARGC_CDECL, stricmp_},
    {DLL, "_strnicmp", ARGC_CDECL, strnicmp_},
    {DLL, "tolower", ARGC_CDECL, tolower_},
    {DLL, "toupper", ARGC_CDECL, toupper_},
    {DLL, "isalnum", ARGC_CDECL, isalnum_},
    {DLL, "isalpha", ARGC_CDECL, isalpha_},
    {DLL, "isdigit", ARGC_CDECL, isdigit_},
    {DLL, "isspace", ARGC_CDECL, isspace_},
    {DLL, "isxdigit", ARGC_CDECL, isxdigit_},
    {DLL, "isleadbyte", ARGC_CDECL, isleadbyte_},
    {DLL, "iswspace", ARGC_CDECL, iswspace_},
    {DLL, "iswalpha", ARGC_CDECL, iswalpha_},
    {DLL, "iswdigit", ARGC_CDECL, iswdigit_},
    {DLL, "iswpunct", ARGC_CDECL, iswpunct_},
    {DLL, "wctomb", ARGC_CDECL, wctomb_},
    {DLL, "wcstombs", ARGC_CDECL, wcstombs_},
    {DLL, "atoi", ARGC_CDECL, atoi_},
    {DLL, "atol", ARGC_CDECL, atoi_},
    {DLL, "atof", ARGC_CDECL, atof_},
    {DLL, "strtod", ARGC_CDECL, strtod_},
    {DLL, "_itoa", ARGC_CDECL, itoa_},
    {DLL, "_ultoa", ARGC_CDECL, ultoa_},
    {DLL, "rand", ARGC_CDECL, rand_},
    {DLL, "srand", ARGC_CDECL, srand_},
    {DLL, "floor", ARGC_CDECL, floor_},
    {DLL, "ceil", ARGC_CDECL, ceil_},
    {DLL, "ldexp", ARGC_CDECL, ldexp_},
    {DLL, "frexp", ARGC_CDECL, frexp_},
    {DLL, "modf", ARGC_CDECL, modf_},
    {DLL, "_isnan", ARGC_CDECL, isnan_},
    {DLL, "_finite", ARGC_CDECL, finite_},
    {DLL, "_fpclass", ARGC_CDECL, fpclass_},
    {DLL, "_controlfp", ARGC_CDECL, controlfp_},
    {DLL, "_clearfp", ARGC_CDECL, clearfp_},
    {DLL, "_CIsin", ARGC_CDECL, ci1<d_sin>},
    {DLL, "_CIcos", ARGC_CDECL, ci1<d_cos>},
    {DLL, "_CItan", ARGC_CDECL, ci1<d_tan>},
    {DLL, "_CIasin", ARGC_CDECL, ci1<d_asin>},
    {DLL, "_CIacos", ARGC_CDECL, ci1<d_acos>},
    {DLL, "_CIatan", ARGC_CDECL, ci1<d_atan>},
    {DLL, "_CIsinh", ARGC_CDECL, ci1<d_sinh>},
    {DLL, "_CIcosh", ARGC_CDECL, ci1<d_cosh>},
    {DLL, "_CItanh", ARGC_CDECL, ci1<d_tanh>},
    {DLL, "_CIexp", ARGC_CDECL, ci1<d_exp>},
    {DLL, "_CIlog", ARGC_CDECL, ci1<d_log>},
    {DLL, "_CIsqrt", ARGC_CDECL, ci1<d_sqrt>},
    {DLL, "_CIpow", ARGC_CDECL, ci2<d_pow>},
    {DLL, "_CIatan2", ARGC_CDECL, ci2<d_atan2>},
    {DLL, "_CIfmod", ARGC_CDECL, ci2<d_fmod>},
    {DLL, "_snprintf", ARGC_CDECL, snprintf_},
    {DLL, "_vsnprintf", ARGC_CDECL, vsnprintf_},
    {DLL, "sscanf", ARGC_CDECL, sscanf_},
    {DLL, "qsort", ARGC_CDECL, qsort_},
    {DLL, "_initterm", ARGC_CDECL, initterm_},
    {DLL, "__dllonexit", ARGC_CDECL, dllonexit_},
    {DLL, "_onexit", ARGC_CDECL, onexit_},
    {DLL, "exit", ARGC_CDECL, exit_},
    {DLL, "abort", ARGC_CDECL, abort_},
    {DLL, "_amsg_exit", ARGC_CDECL, amsg_exit_},
    {DLL, "_purecall", ARGC_CDECL, purecall_},
    {DLL, "?terminate@@YAXXZ", ARGC_CDECL, terminate_},
    {DLL, "_lock", ARGC_CDECL, nothing},
    {DLL, "_unlock", ARGC_CDECL, nothing},
    {DLL, "_XcptFilter", ARGC_CDECL, xcpt_filter_},
    {DLL, "??1type_info@@UAE@XZ", 0, type_info_dtor_},
    {DLL, "setlocale", ARGC_CDECL, setlocale_},
    {DLL, "_errno", ARGC_CDECL, errno_},
    {DLL, "_CxxThrowException", 2, cxx_throw_},
    {DLL, "__CxxFrameHandler", ARGC_CDECL, cxx_frame_handler_},
    {DLL, "_setjmp3", ARGC_CDECL, setjmp3_},
    {DLL, "longjmp", ARGC_CDECL, longjmp_},
    {DLL, "_strtime", ARGC_CDECL, strtime_},
    {DLL, "_strdate", ARGC_CDECL, strdate_},
    {DLL, "_tempnam", ARGC_CDECL, tempnam_},
    {DLL, "_write", ARGC_CDECL, write_},
    {DLL, "_isatty", ARGC_CDECL, isatty_},
    {DLL, "_lseeki64", ARGC_CDECL, lseeki64_},
    {DLL, "_fileno", ARGC_CDECL, fileno_},
    {DLL, "fread", ARGC_CDECL, fread_},
    {DLL, "fwrite", ARGC_CDECL, fwrite_},
    {DLL, "fflush", ARGC_CDECL, fflush_},
};
} // namespace

void msvcrt_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    imports_register_data(DLL, "_iob", FILE_SIZE * IOB_COUNT, iob_init);
    imports_register_data(DLL, "__pioinfo", PIOINFO_SIZE, pioinfo_init);
    imports_register_data(DLL, "__badioinfo", IOINFO_SIZE, badioinfo_init);
}
