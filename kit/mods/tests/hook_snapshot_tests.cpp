// R3 run 31 O2 (NFSMW review/run29/opt-design.md): the published chain snapshots dispatch exactly as the
// locked per-call copy did. Every case runs twice, on the snapshot path and on the old copy (the A/B
// switch hookfast's OLD phase), and both must give the same log. Game-independent: the target is the first
// hookable function of whatever image the test binary carries, and every chain ends in a replace hook that
// returns for the function, so the function's own code never runs.
#include "mods_tests.h"
#include "../mods_internal.h"
#include "../../platform/ab_phase.h"
#include "../../runtime/loader.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"

#include <string.h>
#include <string>

namespace {

PopModApi g_snap_api[4];
std::string g_snap_log;
uint32_t g_snap_target = 0;
uint32_t g_snap_seen_size = 0;
bool g_snap_installed_e = false;

const PopModApi *snap_provider(uint32_t owner) {
    return owner < 4 ? &g_snap_api[owner] : nullptr;
}

bool snap_prepare() {
    sched_set_guest_thread(true);
    mods_set_context_provider(snap_provider);
    mods_hooks_reset();
    mem_init();
    const bool loaded = loader_load(nullptr);
    MOD_CHECK(loaded);
    const bool symbols = loaded && mods_symbols_load(nullptr);
    MOD_CHECK(symbols);
    if (!symbols)
        return false;
    for (uint32_t i = 0; i < 4; ++i) {
        memset(&g_snap_api[i], 0, sizeof g_snap_api[i]);
        g_snap_api[i].version = POP_MOD_API_VERSION;
        g_snap_api[i].size = (uint32_t)sizeof(PopModApi);
        g_snap_api[i].mod_index = i;
        g_snap_api[i].mod_id = "test.snapshot";
        mods_fill_hooks_api(&g_snap_api[i]);
        mods_hooks_set_load_order(i, i);
        mods_hooks_set_cpu_size(i, POP_CPU_V1_BASELINE_SIZE);
    }
    g_snap_target = 0;
    for (int32_t i = 0; i < recomp_func_count && !g_snap_target; ++i)
        if (mods_symbol_hookable(recomp_func_addrs[i]))
            g_snap_target = recomp_func_addrs[i];
    MOD_CHECK(g_snap_target != 0);
    g_snap_log.clear();
    return g_snap_target != 0;
}

// One call of the target as a guest caller returning to `ret` would make it.
uint32_t snap_call(uint32_t ret) {
    const int32_t index = recomp_index_of(g_snap_target);
    X86 c{};
    c.r[R_ESP] = 0x06010000u;
    wr32(c.r[R_ESP], ret);
    MOD_CHECK((*recomp_hooked[index]) != 0);
    if (!(*recomp_hooked[index]))
        return 0;
    recomp_hook_ptrs[index](&c, (uint32_t)index);
    return c.r[R_EAX];
}

void hook(uint32_t owner, int32_t mode, PopHookFn fn, uint32_t *id = nullptr, uint32_t return_pc = 0) {
    uint32_t local = 0;
    MOD_CHECK_EQ(mods_hook_install_ex(owner, g_snap_target, return_pc, fn, mode, 0, nullptr, id ? id : &local),
                 POP_OK);
}

#define LOG_HOOK(tag) [](const PopModApi *, pop_cpu_v1 *, PopHookInvocation *, void *) { g_snap_log += tag; }

void returner(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *, void *) {
    g_snap_log += "R";
    g_snap_seen_size = cpu->size;
    api->hook_return(api, cpu, 7, 0);
}

// The cases, on whichever path the A/B phase selects.
void snap_cases(const char *path) {
    if (!snap_prepare())
        return;
    hook(2, POP_HOOK_BEFORE, LOG_HOOK("b"));
    hook(1, POP_HOOK_BEFORE, LOG_HOOK("a"));
    hook(1, POP_HOOK_REPLACE, returner);
    uint32_t after_id = 0;
    hook(1, POP_HOOK_AFTER, LOG_HOOK("c"), &after_id);
    MOD_CHECK_EQ(snap_call(0x00401000u), 7u);
    MOD_CHECK_STR(g_snap_log.c_str(), "abRc");

    // An install made by a callback changes the next call, never the running one.
    g_snap_log.clear();
    g_snap_installed_e = false;
    hook(3, POP_HOOK_BEFORE, [](const PopModApi *, pop_cpu_v1 *, PopHookInvocation *, void *) {
        g_snap_log += "d";
        if (!g_snap_installed_e) {
            g_snap_installed_e = true;
            uint32_t id = 0;
            mods_hook_install_ex(3, g_snap_target, 0, LOG_HOOK("e"), POP_HOOK_BEFORE, 0, nullptr, &id);
        }
    });
    snap_call(0x00401000u);
    MOD_CHECK_STR(g_snap_log.c_str(), "abdRc");
    g_snap_log.clear();
    snap_call(0x00401000u);
    MOD_CHECK_STR(g_snap_log.c_str(), "abdeRc");

    // A removal is seen by the next call.
    g_snap_log.clear();
    MOD_CHECK_EQ(mods_hook_remove(1, after_id), POP_OK);
    snap_call(0x00401000u);
    MOD_CHECK_STR(g_snap_log.c_str(), "abdeR");

    // A hook for one caller runs for that caller only.
    hook(2, POP_HOOK_AFTER, LOG_HOOK("f"), nullptr, 0x00401000u);
    g_snap_log.clear();
    snap_call(0x00401000u);
    MOD_CHECK_STR(g_snap_log.c_str(), "abdeRf");
    g_snap_log.clear();
    snap_call(0x00402000u);
    MOD_CHECK_STR(g_snap_log.c_str(), "abdeR");

    // A size declared after the owner's hooks were published reaches the next call.
    MOD_CHECK_EQ(g_snap_seen_size, POP_CPU_V1_BASELINE_SIZE);
    mods_hooks_set_cpu_size(1, POP_CPU_V1_MIN_SIZE);
    snap_call(0x00401000u);
    MOD_CHECK_EQ(g_snap_seen_size, POP_CPU_V1_MIN_SIZE);

    // Removing every hook of the target clears it.
    mods_hooks_remove_all(1);
    mods_hooks_remove_all(2);
    mods_hooks_remove_all(3);
    MOD_CHECK_EQ((*recomp_hooked[recomp_index_of(g_snap_target)]), 0);
    printf("hook snapshot cases ran on the %s path\n", path);
    mods_hooks_reset();
    mods_set_context_provider(nullptr);
}

} // namespace

MOD_TEST_SUITE(hook_snapshot_dispatch_matches_the_locked_copy) {
    recomp_ab::old_phase.store(0, std::memory_order_relaxed);
    MOD_CHECK(!recomp_ab::old(recomp_ab::HOOKFAST));
    snap_cases("snapshot");
    recomp_ab::old_phase.store(1, std::memory_order_relaxed);
    MOD_CHECK(recomp_ab::old(recomp_ab::HOOKFAST));
    snap_cases("locked copy");
    recomp_ab::old_phase.store(0, std::memory_order_relaxed);
}
