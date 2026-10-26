/* native/console.c - the game's own console, driven from the trace control file.
 *
 * G3CMD=<command> in RECOMP_TRACE_CONTROL (read once a second, applied when the file changes)
 * runs <command> as if typed into Gothic 3's console: eCConsole::OnExecute(command, output)
 * (Engine 102c6bd0) on the console eCConsole::AccessConsole (102c6040) returns, with the two
 * bCStrings built by SharedBase's bCString(char const *) (100029c0) and freed by ~bCString
 * (10001a00). The command and its output go to stderr as "g3cmd>" and "g3cmd<" lines. Several
 * G3CMD lines run in file order. Only a line that was not in the file's previous text runs, so
 * changing another key (G3OPT) runs nothing again; to repeat a command, change its line, e.g. a
 * " #2" at its end, which is cut off before it runs.
 *
 * The control is applied by the host's pump while it holds the baton, inside a guest call of
 * the game's own thread (guest_current_context): the guest stack below ESP is free, the
 * strings live there, and every register comes back as it was. */
#include "chunk.h"

#include <stdio.h>
#include <string.h>

/* kit/runtime (C++): win32.h guest_current_context, guest.h guest_call. */
X86 *guest_current_context(void) __asm__("__Z21guest_current_contextv");
uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");

#define CON_ACCESS 0x102c6040u    /* Engine eCConsole::AccessConsole, stdcall, no arguments */
#define CON_ON_EXECUTE 0x102c6bd0u /* Engine eCConsole::OnExecute(bCString const &, bCString &), thiscall */
#define STR_CTOR 0x100029c0u       /* SharedBase bCString::bCString(char const *), thiscall */
#define STR_DTOR 0x10001a00u       /* SharedBase bCString::~bCString(void), thiscall */

#define CON_SCRATCH 0x800u /* bytes of guest stack below ESP for the strings */
#define CON_TEXT_MAX 0x700u

static uint32_t con_thiscall(X86 *c, uint32_t fn, uint32_t self, const uint32_t *args, int n) {
    c->r[R_ECX] = self;
    return guest_call(c, fn, args, n);
}

/* Copies the guest bCString at `obj` into out (its text pointer is the first field). */
static void con_read(uint32_t obj, char *out, size_t size) {
    uint32_t p = rd32(obj);
    size_t i = 0;
    if (p)
        for (; i + 1 < size; ++i) {
            const char ch = (char)g_mem[p + i];
            if (!ch)
                break;
            out[i] = ch;
        }
    out[i] = 0;
}

static void con_run(const char *cmd, size_t len) {
    X86 *c = guest_current_context();
    if (!c) {
        fprintf(stderr, "g3cmd> no guest context, not run: %.*s\n", (int)len, cmd);
        return;
    }
    if (len > CON_TEXT_MAX - 1)
        len = CON_TEXT_MAX - 1;
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    const uint32_t eip = c->eip;
    const uint32_t base = (c->r[R_ESP] - CON_SCRATCH) & ~15u;
    const uint32_t text = base, in_str = base + CON_TEXT_MAX, out_str = in_str + 16u;
    memcpy(g_mem + text, cmd, len);
    g_mem[text + len] = 0;
    wr32(in_str, 0);
    wr32(out_str, 0);
    c->r[R_ESP] = base - 16u;

    const uint32_t console = guest_call(c, CON_ACCESS, 0, 0);
    char out[2048];
    if (!console) {
        snprintf(out, sizeof out, "(no console: eCConsole::ms_pThis is 0)");
    } else {
        con_thiscall(c, STR_CTOR, in_str, &text, 1);
        con_thiscall(c, STR_CTOR, out_str, &text, 1); /* any text; OnExecute assigns it */
        const uint32_t args[2] = {in_str, out_str};
        const uint32_t ok = con_thiscall(c, CON_ON_EXECUTE, console, args, 2) & 0xffu;
        char result[1800];
        con_read(out_str, result, sizeof result);
        snprintf(out, sizeof out, "%s%s%s", ok ? "ok" : "failed", result[0] ? ": " : "", result);
        con_thiscall(c, STR_DTOR, out_str, 0, 0);
        con_thiscall(c, STR_DTOR, in_str, 0, 0);
    }
    memcpy(c->r, saved, sizeof saved);
    c->eip = eip;
    fprintf(stderr, "g3cmd> %.*s\ng3cmd< %s\n", (int)len, cmd, out);
}

static char con_prev[8192]; /* the control text of the previous call */

/* Whether `line` (n bytes, without its newline) is a whole line of con_prev. */
static int con_was_there(const char *line, size_t n) {
    for (const char *q = con_prev; (q = strstr(q, "G3CMD=")) != 0; q += 6)
        if ((q == con_prev || q[-1] == '\n') && !strncmp(q, line, n) &&
            (q[n] == '\n' || q[n] == '\r' || q[n] == 0))
            return 1;
    return 0;
}

/* Every new "G3CMD=" line of the control text, in order (native/g3_opt.c calls this). */
void g3_console_control(const char *text) {
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        while (n && p[n - 1] == '\r')
            --n;
        if (n > 6 && !strncmp(p, "G3CMD=", 6) && !con_was_there(p, n)) {
            size_t len = n - 6;
            const char *hash = memchr(p + 6, '#', len);
            if (hash && hash > p + 6 && hash[-1] == ' ')
                len = (size_t)(hash - (p + 6));
            while (len && p[6 + len - 1] == ' ')
                --len;
            if (len)
                con_run(p + 6, len);
        }
        p = eol ? eol + 1 : 0;
    }
    snprintf(con_prev, sizeof con_prev, "%s", text ? text : "");
}
