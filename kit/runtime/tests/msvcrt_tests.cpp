// msvcrt_tests.cpp - the msvcrt.dll stand-ins (runtime/msvcrt.cpp) called as a
// module calls them: cdecl arguments on the guest stack, doubles back in ST(0).
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

static uint32_t call(X86 *c, const char *name, const std::vector<uint32_t> &args) {
    uint32_t tramp = imports_resolve("msvcrt.dll", name);
    if (!check(tramp != 0, "msvcrt.dll!%s is served", name))
        return 0;
    uint32_t before = c->r[R_ESP], esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    uint32_t expected = imports_argc(tramp) == ARGC_CDECL ? before - uint32_t(args.size()) * 4
                                                          : before - uint32_t(args.size()) * 4 + imports_argc(tramp) * 4;
    check(c->r[R_ESP] == expected, "%s leaves ESP where its convention says", name);
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}

static uint32_t g_scratch;
static uint32_t put(const void *data, uint32_t n) {
    uint32_t a = heap_alloc(n + 16, true);
    memcpy(gm_ptr(a), data, n);
    return a;
}
static uint32_t str(const char *s) {
    return put(s, (uint32_t)strlen(s) + 1);
}
static std::vector<uint32_t> dbl(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return {(uint32_t)bits, (uint32_t)(bits >> 32)};
}
static double rd_double(uint32_t a) {
    uint64_t bits = rd32(a) | ((uint64_t)rd32(a + 4) << 32);
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

// A guest comparator for qsort: int elements, ascending.
static uint32_t g_compares;
static void compare_ints(X86 *c) {
    ++g_compares;
    int32_t a = (int32_t)rd32(arg(c, 0)), b = (int32_t)rd32(arg(c, 1));
    set_eax(c, (uint32_t)(a < b ? -1 : a > b ? 1 : 0));
}
static uint32_t g_init_calls;
static void count_init(X86 *c) {
    ++g_init_calls;
    set_eax(c, 0);
}
static const ImportShim test_shims[] = {
    {"msvcrt_tests.dll", "compare_ints", ARGC_CDECL, compare_ints},
    {"msvcrt_tests.dll", "count_init", ARGC_CDECL, count_init},
};

static void test_strings(X86 *c) {
    printf("== strings and memory\n");
    uint32_t buf = str("abcdef");
    call(c, "memcpy", {buf + 2, buf, 4});
    check(gm_str(buf) == "ababcd", "memcpy copies overlapping ranges as memmove does");
    uint32_t s = str("a/b/c");
    check(call(c, "strchr", {s, '/'}) == s + 1 && call(c, "strrchr", {s, '/'}) == s + 3, "strchr, strrchr");
    check(call(c, "strchr", {s, 0}) == s + 5, "strchr finds the terminator");
    check(call(c, "_stricmp", {str("Shader"), str("sHADER")}) == 0, "_stricmp folds ASCII case");
    check((int32_t)call(c, "_strnicmp", {str("abcX"), str("ABCy"), 4}) < 0, "_strnicmp compares after folding");
    uint32_t dup = call(c, "_strdup", {str("copy me")});
    check(dup && gm_str(dup) == "copy me" && heap_owns(dup), "_strdup allocates on the guest heap");
    call(c, "free", {dup});
    uint32_t p = call(c, "calloc", {4, 8});
    check(p && rd32(p + 28) == 0, "calloc zeroes");
    p = call(c, "realloc", {p, 4096});
    check(p && heap_size(p) == 4096, "realloc grows");
    check(call(c, "realloc", {p, 0}) == 0, "realloc to 0 frees");
    uint32_t out = heap_alloc(16, true);
    check(gm_str(call(c, "_itoa", {(uint32_t)-42, out, 10})) == "-42", "_itoa signed in base 10");
    check(gm_str(call(c, "_ultoa", {0xdeadbeef, out, 16})) == "deadbeef", "_ultoa base 16");
    uint16_t wide[] = {'h', 0xe9, 0};
    check(call(c, "wcstombs", {out, put(wide, sizeof wide), 16}) == 2 && rd8(out + 1) == 0xe9,
          "wcstombs converts Latin-1");
    check(call(c, "toupper", {'q'}) == 'Q' && call(c, "isalpha", {0xe9}) == 0 && call(c, "iswalpha", {0xe9}) == 1,
          "C-locale classification");
}

static void test_numbers(X86 *c) {
    printf("== numbers and the x87 stack\n");
    uint32_t end = heap_alloc(4, true), text = str("  2.5e3xyz");
    unsigned top = c->fpu_top;
    call(c, "strtod", {text, end});
    check(fpop(c) == 2500.0 && rd32(end) == text + 7, "strtod returns in ST(0) and sets the end pointer");
    check(c->fpu_top == top, "the x87 stack is balanced again");
    call(c, "floor", dbl(-1.5));
    check(fpop(c) == -2.0, "floor");
    fpush(c, 2.0);
    fpush(c, 10.0);
    call(c, "_CIpow", {});
    check(fpop(c) == 1024.0 && c->fpu_top == top, "_CIpow takes x in ST(1), y in ST(0)");
    fpush(c, 1.0);
    fpush(c, -1.0);
    call(c, "_CIatan2", {});
    check(std::fabs(fpop(c) - 2.356194490192345) < 1e-12, "_CIatan2 is atan2(ST(1), ST(0))");
    fpush(c, 0.0);
    call(c, "_CIcos", {});
    check(fpop(c) == 1.0, "_CIcos");
    uint32_t exponent = heap_alloc(4, true);
    std::vector<uint32_t> a = dbl(48.0);
    a.push_back(exponent);
    call(c, "frexp", a);
    check(fpop(c) == 0.75 && rd32(exponent) == 6, "frexp");
    uint32_t whole = heap_alloc(8, true);
    a = dbl(-3.25);
    a.push_back(whole);
    call(c, "modf", a);
    check(fpop(c) == -0.25 && rd_double(whole) == -3.0, "modf");
    check(call(c, "_fpclass", dbl(-0.0)) == 0x20 && call(c, "_isnan", dbl(NAN)) == 1, "_fpclass, _isnan");
    call(c, "srand", {1});
    uint32_t r1 = call(c, "rand", {}), r2 = call(c, "rand", {});
    check(r1 == 41 && r2 == 18467, "rand is the CRT's generator (41, 18467 after srand(1))");
    check(call(c, "_controlfp", {0, 0}) == 0x0009001f, "_controlfp reports the CRT's default word");
}

static void test_formatting(X86 *c) {
    printf("== _snprintf, _vsnprintf, sscanf\n");
    uint32_t out = heap_alloc(64, true);
    std::vector<uint32_t> a = {out, 64, str("%d|%5s|%-3c|%.2f|%I64d|%x|%ls")};
    a.push_back((uint32_t)-7);
    a.push_back(str("ab"));
    a.push_back('z');
    for (uint32_t w : dbl(3.14159))
        a.push_back(w);
    a.push_back(0x00000001);
    a.push_back(0x00000002); // 0x200000001
    a.push_back(255);
    uint16_t wide[] = {'w', 'i', 0};
    a.push_back(put(wide, sizeof wide));
    uint32_t n = call(c, "_snprintf", a);
    std::string want = "-7|   ab|z  |3.14|8589934593|ff|wi";
    check(gm_str(out) == want && n == want.size(), "_snprintf: %s", gm_str(out).c_str());
    uint32_t va = heap_alloc(8, true);
    wr32(va, str("tail"));
    memset(gm_ptr(out), 'X', 16);
    check(call(c, "_vsnprintf", {out, 4, str("%s!"), va}) == 0xffffffffu && rd8(out + 4) == 'X',
          "_vsnprintf truncates without a terminator and returns -1");
    uint32_t i = heap_alloc(4, true), f = heap_alloc(4, true), s = heap_alloc(16, true), d = heap_alloc(8, true);
    uint32_t got = call(c, "sscanf", {str(" 12 , 0.5 name 2.25"), str("%d , %f %s %lf"), i, f, s, d});
    float fv;
    uint32_t fbits = rd32(f);
    memcpy(&fv, &fbits, sizeof fv);
    check(got == 4 && rd32(i) == 12 && fv == 0.5f && gm_str(s) == "name" && rd_double(d) == 2.25,
          "sscanf %%d %%f %%s %%lf");
    check(call(c, "sscanf", {str("  "), str("%d"), i}) == (uint32_t)-1, "sscanf reports EOF on empty input");
    check(call(c, "sscanf", {str("v 1"), str("v %*d %d"), i}) == 0,
          "sscanf: a suppressed conversion counts as one, so running out after it is 0, not EOF");
    check(call(c, "sscanf", {str("x"), str("%d"), i}) == 0, "sscanf stops at a mismatch");
}

static void test_runtime(X86 *c) {
    printf("== qsort, _initterm, __dllonexit, stdio data\n");
    int32_t values[] = {5, -3, 9, 0, 9, 1, -8, 4};
    uint32_t base = put(values, sizeof values);
    g_compares = 0;
    call(c, "qsort", {base, 8, 4, imports_resolve("msvcrt_tests.dll", "compare_ints")});
    bool sorted = true;
    for (uint32_t k = 1; k < 8; ++k)
        sorted &= (int32_t)rd32(base + 4 * (k - 1)) <= (int32_t)rd32(base + 4 * k);
    check(sorted && g_compares > 0, "qsort sorts through the guest comparator (%u calls)", g_compares);
    uint32_t init = imports_resolve("msvcrt_tests.dll", "count_init");
    uint32_t table[] = {init, 0, init};
    uint32_t t = put(table, sizeof table);
    g_init_calls = 0;
    call(c, "_initterm", {t, t + 12});
    check(g_init_calls == 2, "_initterm calls each entry and skips null ones");
    uint32_t pbegin = heap_alloc(4, true), pend = heap_alloc(4, true);
    for (uint32_t k = 0; k < 40; ++k)
        call(c, "__dllonexit", {0x1000 + k, pbegin, pend});
    check(rd32(pend) - rd32(pbegin) == 160 && rd32(rd32(pbegin) + 39 * 4) == 0x1000 + 39,
          "__dllonexit grows the module's table past 32 entries");
    uint32_t iob = imports_alloc_data("msvcrt.dll", "_iob");
    check(iob && call(c, "_fileno", {iob + 2 * 32}) == 2, "_iob[2] is descriptor 2");
    uint32_t pio = imports_alloc_data("msvcrt.dll", "__pioinfo");
    check(pio && rd32(pio) == pio + 256 && rd8(rd32(pio) + 0x24 + 4) == 0x81,
          "__pioinfo's first block is in place and descriptor 1 is open");
    check(call(c, "_write", {2, str("to the log\n"), 11}) == 11 && call(c, "_write", {5, str("x"), 1}) == 0xffffffffu,
          "_write reaches the log for stdout/stderr only");
    uint32_t errno_at = call(c, "_errno", {});
    check(errno_at && rd32(errno_at) == 9, "_errno holds EBADF");
    check(gm_str(call(c, "setlocale", {0, str("")})) == "C" && call(c, "setlocale", {0, str("German")}) == 0,
          "setlocale knows the C locale only");
}

int main() {
    mem_init();
    imports_init();
    imports_register(test_shims, sizeof test_shims / sizeof test_shims[0]);
    X86 c;
    loader_init_context(&c);
    test_strings(&c);
    test_numbers(&c);
    test_formatting(&c);
    test_runtime(&c);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
