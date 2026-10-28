/* native/tests/fsmatch_test.c - native/filesystem_match.c against the translated FUN_10da9e10.
 *
 * Build and run (from the checkout root, while build/recomp/gen still has the FileSystem.dll translation):
 *   NATIVE=filesystem_match.c sh native/tests/build.sh fsmatch aux-filesystem/chunk_10da8000.c
 *
 * FUN_10da9e10 (EAX = wanted name, EDX = entry name, AL = fits) runs on guest strings; g3fs_name_fits gets the same
 * bytes. FileSystem's _tolower 10db08d6 is a fake doing what the real one does in the C locale. Names are drawn so
 * that every path of the match runs: '*' and '?' in both names, runs of them, case differences, several dots, no dot,
 * empty halves, bytes from 0x80 up. Halves stay under 256 bytes (the original overruns its buffers past that). */
#include "difftest_rp.h"

void fn_10da9e10(X86 *c);
int g3fs_name_fits(const char *pattern, const char *name);

#define ESP0 0x0e800000u
#define WANT 0x03000000u
#define NAME 0x03001000u
#define RET_SITE 0x10da2a29u

void fn_10db08d6(X86 *c) { /* _tolower, cdecl: the C locale's answer */
    const uint32_t arg = rd32(c->r[R_ESP] + 4u);
    c->r[R_EAX] = arg;
    c->r[R_ECX] = arg - 0x41u;
    if (c->r[R_ECX] <= 0x19u)
        c->r[R_EAX] += 0x20u;
    c->eip = rd32(c->r[R_ESP]); /* RET */
    c->r[R_ESP] += 4u;
}
void g3_mods_control(const char *text) { /* g3_opt.c's console hook into native/mods.c, not linked here */
    (void)text;
}

static const char alphabet[] = "aAbBcC..**??xX_";

static unsigned make(char *s) {
    const unsigned n = dt_rand32() % 14u;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t r = dt_rand32();
        s[i] = r % 40u == 0 ? (char)(0x80u + r % 0x7fu) : alphabet[r % (sizeof alphabet - 1)];
    }
    s[n] = 0;
    return n;
}

/* A pattern made from a name: characters swapped for '?', runs replaced by '*', case flipped. */
static void derive(char *p, const char *name) {
    unsigned j = 0;
    for (unsigned i = 0; name[i] && j < 30; ++i) {
        const uint32_t r = dt_rand32() % 10u;
        if (r == 0)
            p[j++] = '?';
        else if (r == 1) {
            p[j++] = '*';
            i += dt_rand32() % 3u;
            if (!name[i])
                break;
        } else if (r == 2 && name[i] >= 'a' && name[i] <= 'z')
            p[j++] = (char)(name[i] - 32);
        else
            p[j++] = name[i];
    }
    if (dt_rand32() % 8u == 0)
        p[j++] = '*';
    p[j] = 0;
}

int main(void) {
    harness_mem();
    dt_load_pe("FileSystem.dll");
    unsigned fits[2] = {0, 0};
    char want[64], name[64];
    for (unsigned i = 0; i < 2000000u; ++i) {
        make(name);
        if (dt_rand32() % 3u)
            derive(want, name);
        else
            make(want);
        memcpy(g_mem + WANT, want, strlen(want) + 1);
        memcpy(g_mem + NAME, name, strlen(name) + 1);
        X86 c;
        memset(&c, 0, sizeof c);
        c.r[R_ESP] = ESP0 - 4u;
        wr32(ESP0 - 4u, RET_SITE);
        c.r[R_EAX] = WANT;
        c.r[R_EDX] = NAME;
        fn_10da9e10(&c);
        const int orig = (c.r[R_EAX] & 0xffu) != 0, mine = g3fs_name_fits(want, name) != 0;
        ++dt_cases;
        ++fits[orig];
        if (orig != mine) {
            if (dt_fail < 20)
                printf("differ: want \"%s\" name \"%s\": original %d, native %d\n", want, name, orig, mine);
            ++dt_fail;
        }
    }
    printf("fits: no %u, yes %u\n", fits[0], fits[1]);
    return dt_report("fsmatch_test");
}
