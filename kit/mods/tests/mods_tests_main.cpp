// mods_tests_main.cpp - runs every registered suite. Headless: no window, no
// audio device, no application object. Fixture paths resolve from the kit's
// source folder (mods_test_source_path), so any working folder works.
#include "mods_tests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include "../../platform/os.h"
#include "../../runtime/x86.h"
#ifndef _WIN32
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#endif

namespace {
ModTestSuite *g_head = nullptr;
int g_checks = 0, g_failures = 0;
int g_skipped = 0;
const char *g_skip_reason = nullptr;

#ifndef _WIN32
// A suite that crashes fails; it does not take the run down with it.
//
// Run 31 found the binary dying with SIGBUS inside the Lua suites (a Populous
// address looked up on another game's image, then an unchecked -1 index into
// the hook table), and every suite after that one - hooks, events, loader - had
// silently never run while the totals still looked like a result. A fault on
// the main thread while a suite runs now jumps back to the runner, which
// records the suite as FAILED with the signal's name and goes on. A fault on
// any other thread, or outside a suite, still ends the process as before.
//
// After a crash the process state is whatever the suite left (a held lock, a
// half-reset registry), so a crash is always a failure to fix, never a pass;
// later suites run so the rest of the picture is not lost.
sigjmp_buf g_suite_jmp;
volatile sig_atomic_t g_in_suite = 0;
volatile sig_atomic_t g_crash_signal = 0;
pthread_t g_main_thread;
const int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};

void on_crash(int sig, siginfo_t *, void *) {
    if (g_in_suite && pthread_equal(pthread_self(), g_main_thread)) {
        g_in_suite = 0;
        g_crash_signal = sig;
        siglongjmp(g_suite_jmp, 1);
    }
    // Not a suite on the main thread: die of this signal, as before.
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_crash_handlers() {
    g_main_thread = pthread_self();
    // Its own stack, so a suite that overflows the main stack is caught too.
    static char alt[256 * 1024];
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = alt;
    ss.ss_size = sizeof alt;
    sigaltstack(&ss, nullptr);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : kCrashSignals)
        sigaction(sig, &sa, nullptr);
}

const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    default: return "a signal";
    }
}
#endif
} // namespace

bool mod_test_have_populous_image() {
    // Both of Populous's fixed routines are functions of this image.
    return recomp_index_of(0x004ec6f0u) >= 0 && recomp_index_of(0x0040c690u) >= 0;
}

void mod_test_skip(const char *reason) {
    if (!g_skip_reason)
        g_skip_reason = reason;
}

void mod_test_register(ModTestSuite *s) {
    s->next = g_head;
    g_head = s;
}
ModTestSuite *mod_test_suites() {
    return g_head;
}
void mod_test_pass() {
    ++g_checks;
}
int mod_test_failures() {
    return g_failures;
}
void mod_test_fail(const char *what, const char *file, int line) {
    ++g_checks;
    ++g_failures;
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
}

// Each suite name gets its OWN storage, and it is never rewritten.
//
// This used to build every answer in one static string and hand back a pointer
// into it, so the next call silently changed what every earlier caller was
// holding. Two suites keep the pointer rather than copying it, and after any
// later call their ROOT and PROFILE named whichever directory had been
// prepared most recently: the overlay suite wrote and unlinked inside the
// loader's tree, which made a plugin the loader had just installed vanish, and
// the settings suite read values out of the wrong profile. Which suites broke
// depended on suite order, so the same binary gave 0, 2, 4 or 10 failures on
// consecutive runs, and sometimes a segfault - the shared string reallocating
// between a short path and a long one, with a held pointer still aimed at the
// old buffer.
//
// A map keyed by suite name fixes it for every caller at once, including the
// ones that have not been written yet. The alternative - each caller copying
// into a string of its own - leaves the trap loaded for the next one.
const char *mod_test_dir(const char *suite) {
    static std::map<std::string, std::string> kept;
    std::string &path = kept[suite];
    const char *root = recomp_env("TEST_DIR");
    path = std::string(root && *root ? root : mods_test_build_path("recomp/mods-test").c_str()) +
           "/" + suite;
    std::string cmd = "rm -rf '" + path + "' && mkdir -p '" + path + "'";
    if (system(cmd.c_str()) != 0)
        fprintf(stderr, "mod_test_dir: cannot prepare %s\n", path.c_str());
    return path.c_str();
}

int main() {
    // Settings apply now persists immediately. Even suites which only exercise
    // the UI/API must have a scratch profile rather than the player's profile.
    os_setenv("RECOMP_PROFILE_DIR", mod_test_dir("default-profile"));
    // Line buffered, always. Redirected to a file or a pipe, stdout is block
    // buffered, and a suite that crashes takes every earlier suite's result
    // down with it - the run then looks as though nothing ran at all, which
    // is the opposite of what a crashing test run should tell you.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // RECOMP_MOD_TEST_ONLY=a,b runs only the suites whose name contains one
    // of the comma-separated parts, in the usual order.
    const char *only = recomp_env("MOD_TEST_ONLY");
    auto selected = [only](const char *name) {
        if (!only || !*only)
            return true;
        std::string list = only;
        for (size_t start = 0; start <= list.size();) {
            size_t end = list.find(',', start);
            if (end == std::string::npos)
                end = list.size();
            const std::string part = list.substr(start, end - start);
            if (!part.empty() && strstr(name, part.c_str()))
                return true;
            start = end + 1;
        }
        return false;
    };
#ifndef _WIN32
    install_crash_handlers();
#endif
    int ok = 0, failed = 0, crashed = 0;
    for (ModTestSuite *s = mod_test_suites(); s; s = s->next) {
        if (!selected(s->name))
            continue;
        int before = g_failures;
        g_skip_reason = nullptr;
        const char *crash = nullptr;
#ifndef _WIN32
        g_crash_signal = 0;
        if (sigsetjmp(g_suite_jmp, 1) == 0) {
            g_in_suite = 1;
            s->fn();
            g_in_suite = 0;
        } else {
            ++g_checks;
            ++g_failures;
            ++crashed;
            crash = signal_name(g_crash_signal);
            fprintf(stderr, "FAIL %s: crashed (%s)\n", s->name, crash);
        }
#else
        s->fn();
#endif
        if (crash) {
            ++failed;
            printf("%-34s FAILED (crashed: %s)\n", s->name, crash);
        } else if (g_failures != before) {
            ++failed;
            printf("%-34s FAILED\n", s->name);
        } else if (g_skip_reason) {
            ++g_skipped;
            printf("%-34s skipped (%s)\n", s->name, g_skip_reason);
        } else {
            ++ok;
            printf("%-34s ok\n", s->name);
        }
    }
    printf("\n%d suites: %d ok, %d failed (%d crashed), %d skipped\n", ok + failed + g_skipped, ok,
           failed, crashed, g_skipped);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all mod tests passed\n");
    return g_failures ? 1 : 0;
}
