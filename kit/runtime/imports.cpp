#include "imports.h"
#include "gdi32_internal.h"
#include "user32_internal.h"
#include "kernel32_internal.h"
#include "seh.h"
#include "profile.h"
#include "mods_seam.h"

// Defined in kernel32.cpp with the scheduler.
void sched_checkpoint();
#include "memory.h"
#include "../platform/ab_phase.h"

#include <map>
#include <atomic>
#include <strings.h>
#include <set>
#include <cctype>
#include <vector>
#include <string>
#include <algorithm>

namespace {

struct Tramp {
    std::string dll;
    std::string name;
    std::string desc; // "DLL!name", stable storage for imports_describe
    uint8_t argc = 0;
    void (*fn)(X86 *) = nullptr;
    // Entered without a scheduling checkpoint (see atomic_import).
    bool atomic = false;
};

// Imports whose callers have just set up state the call reads back, with no
// import in between: a thread switch at their entry checkpoint would let
// another thread replace it. Delphi's VCL sets its creation-control global and
// calls CreateWindowEx at once; the window procedure reads it on the first
// message.
bool atomic_import(const std::string &key) {
    return key == "user32.dll!CreateWindowExA" || key == "user32.dll!CreateWindowExW";
}

// Deliberately leaked: an atexit hook (RECOMP_IMPORT_STATS) reads these after
// static destructors would otherwise have run.
std::vector<Tramp> &tramps() {
    static auto *v = new std::vector<Tramp>();
    return *v;
}

// What imports_dispatch reads on every call, by trampoline index, in a table
// that never moves: tens of thousands of D3D9 and kernel32 calls a frame went
// through the vector above (a guarded static, a size computed by a division,
// 96-byte entries) several times each. Every write of a Tramp's fn, argc or
// atomic is mirrored here, and desc is the Tramp's own c_str(), refreshed when
// the vector moves its strings; the call count lives only here.
struct HotTramp {
    void (*fn)(X86 *) = nullptr;
    const char *desc = nullptr;
    uint32_t calls = 0;
    uint8_t argc = 0;
    bool atomic = false;
};
HotTramp g_hot[TRAMP_MAX];
uint32_t g_hot_count = 0; // == tramps().size()
void hot_sync(uint32_t idx) {
    const Tramp &t = tramps()[idx];
    g_hot[idx].fn = t.fn;
    g_hot[idx].desc = t.desc.c_str();
    g_hot[idx].argc = t.argc;
    g_hot[idx].atomic = t.atomic;
}
std::map<std::string, uint32_t> &index() {
    static auto *m = new std::map<std::string, uint32_t>();
    return *m;
}
std::map<std::string, ImportShim> &registry() {
    static auto *m = new std::map<std::string, ImportShim>();
    return *m;
}
std::set<std::string> &registered_dlls() {
    static auto *s = new std::set<std::string>();
    return *s;
}

struct DataImport {
    uint32_t size = 0;
    uint32_t addr = 0;
    std::string desc;
    void (*init)(uint32_t addr) = nullptr;
};
std::map<std::string, DataImport> &data_imports() {
    static auto *m = new std::map<std::string, DataImport>();
    return *m;
}

std::string lower_dll(const char *dll) {
    std::string k(dll ? dll : "");
    std::transform(k.begin(), k.end(), k.begin(),
                   [](unsigned char ch) { return (char)tolower(ch); });
    return k;
}

std::string key_of(const char *dll, const char *name) {
    std::string k = lower_dll(dll);
    k += '!';
    k += name ? name : "";
    return k;
}

// Stdcall argument counts of Win32 APIs that games import and no shim models
// yet (from the API signatures; ordinals as "ord<n>"). A logging-only stub
// then pops what its caller pushed, so the call fails visibly instead of
// leaving the guest stack drifting. Gothic 3 imports all of these.
struct KnownArgs {
    const char *dll, *name;
    uint8_t argc;
};
const KnownArgs known_args[] = {
    {"KERNEL32.dll", "DebugBreak", 0},
    {"KERNEL32.dll", "DeviceIoControl", 8},
    {"KERNEL32.dll", "EnumResourceLanguagesA", 5},
    {"KERNEL32.dll", "ExpandEnvironmentStringsA", 3},
    {"KERNEL32.dll", "FindFirstFileExA", 6},
    {"KERNEL32.dll", "FormatMessageA", 7},
    {"KERNEL32.dll", "GetConsoleMode", 2},
    {"KERNEL32.dll", "GetCurrentDirectoryW", 2},
    {"KERNEL32.dll", "GetDateFormatA", 6},
    {"KERNEL32.dll", "GetFileInformationByHandle", 2},
    {"KERNEL32.dll", "GetFileTime", 4},
    {"KERNEL32.dll", "GetTempFileNameA", 4},
    {"KERNEL32.dll", "GetTempFileNameW", 4},
    {"KERNEL32.dll", "GetTempPathA", 2},
    {"KERNEL32.dll", "GetThreadContext", 2},
    {"KERNEL32.dll", "GetTimeFormatA", 6},
    {"KERNEL32.dll", "GlobalAddAtomA", 1},
    {"KERNEL32.dll", "GlobalFindAtomA", 1},
    {"KERNEL32.dll", "GlobalFlags", 1},
    {"KERNEL32.dll", "GlobalGetAtomNameA", 3},
    {"KERNEL32.dll", "GlobalHandle", 1},
    {"KERNEL32.dll", "HeapValidate", 3},
    {"KERNEL32.dll", "HeapWalk", 2},
    {"KERNEL32.dll", "LockFile", 5},
    {"KERNEL32.dll", "MoveFileExA", 3},
    {"KERNEL32.dll", "MoveFileW", 2},
    {"KERNEL32.dll", "OpenEventA", 3},
    {"KERNEL32.dll", "OpenFile", 3},
    {"KERNEL32.dll", "OpenProcess", 3},
    {"KERNEL32.dll", "PeekNamedPipe", 6},
    {"KERNEL32.dll", "PulseEvent", 1},
    {"KERNEL32.dll", "ReadProcessMemory", 5},
    {"KERNEL32.dll", "TerminateThread", 2},
    {"KERNEL32.dll", "UnlockFile", 5},
    {"KERNEL32.dll", "WriteConsoleA", 5},
    {"KERNEL32.dll", "WriteConsoleW", 5},
    {"KERNEL32.dll", "WritePrivateProfileStringA", 4},
    {"USER32.dll", "CallWindowProcA", 5},
    {"USER32.dll", "CopyRect", 2},
    {"USER32.dll", "CreateDialogIndirectParamA", 5},
    {"USER32.dll", "DrawTextA", 5},
    {"USER32.dll", "DrawTextExA", 6},
    {"USER32.dll", "EndDialog", 2},
    {"USER32.dll", "GetCaretBlinkTime", 0},
    {"USER32.dll", "GetClassInfoA", 3},
    {"USER32.dll", "GetClassInfoExA", 3},
    {"USER32.dll", "GetClassLongA", 2},
    {"USER32.dll", "GetClassNameA", 3},
    {"USER32.dll", "GetDlgItem", 2},
    {"USER32.dll", "GetKeyNameTextA", 3},
    {"USER32.dll", "GetMenuCheckMarkDimensions", 0},
    {"USER32.dll", "GetNextDlgTabItem", 3},
    {"USER32.dll", "GetPropA", 2},
    {"USER32.dll", "GetWindowTextA", 3},
    {"USER32.dll", "GrayStringA", 9},
    {"USER32.dll", "LoadBitmapA", 2},
    {"USER32.dll", "ModifyMenuA", 5},
    {"USER32.dll", "MoveWindow", 6},
    {"USER32.dll", "PtInRect", 3},
    {"USER32.dll", "RemovePropA", 2},
    {"USER32.dll", "SendDlgItemMessageA", 5},
    {"USER32.dll", "SetKeyboardState", 1},
    {"USER32.dll", "SetLastErrorEx", 2},
    {"USER32.dll", "SetMenuItemBitmaps", 5},
    {"USER32.dll", "SetPropA", 3},
    {"USER32.dll", "SetWindowsHookExA", 4},
    {"USER32.dll", "TabbedTextOutA", 8},
    {"USER32.dll", "WinHelpA", 4},
    {"USER32.dll", "WindowFromDC", 1},
    {"USER32.dll", "keybd_event", 4},
    {"GDI32.dll", "Escape", 5},
    {"GDI32.dll", "ExtTextOutA", 8},
    {"GDI32.dll", "GetCharacterPlacementA", 6},
    {"GDI32.dll", "GetCharacterPlacementW", 6},
    {"GDI32.dll", "GetCurrentObject", 2},
    {"GDI32.dll", "GetFontLanguageInfo", 1},
    {"GDI32.dll", "GetGlyphIndicesA", 5},
    {"GDI32.dll", "GetGlyphIndicesW", 5},
    {"GDI32.dll", "GetGlyphOutlineA", 7},
    {"GDI32.dll", "GetGlyphOutlineW", 7},
    {"GDI32.dll", "GetOutlineTextMetricsA", 3},
    {"GDI32.dll", "OffsetViewportOrgEx", 4},
    {"GDI32.dll", "PtVisible", 3},
    {"GDI32.dll", "ScaleViewportExtEx", 6},
    {"GDI32.dll", "ScaleWindowExtEx", 6},
    {"GDI32.dll", "SetMapMode", 2},
    {"GDI32.dll", "SetTextAlign", 2},
    {"GDI32.dll", "SetViewportExtEx", 4},
    {"GDI32.dll", "SetWindowExtEx", 4},
    {"GDI32.dll", "TranslateCharsetInfo", 3},
    {"ADVAPI32.dll", "RegDeleteKeyA", 2},
    {"ADVAPI32.dll", "RegEnumKeyA", 4},
    {"ADVAPI32.dll", "RegQueryValueA", 4},
    {"COMDLG32.dll", "GetFileTitleA", 3},
    {"MSACM32.dll", "acmFormatSuggest", 5},
    {"MSACM32.dll", "acmStreamConvert", 3},
    {"MSACM32.dll", "acmStreamOpen", 8},
    {"MSACM32.dll", "acmStreamPrepareHeader", 3},
    {"MSACM32.dll", "acmStreamSize", 4},
    {"MSACM32.dll", "acmStreamUnprepareHeader", 3},
    {"OLEAUT32.dll", "ord2", 1},
    {"OLEAUT32.dll", "ord6", 1},
    {"OLEAUT32.dll", "ord8", 1},
    {"OLEAUT32.dll", "ord9", 1},
    {"OLEAUT32.dll", "ord12", 4},
    {"SETUPAPI.dll", "SetupDiDestroyDeviceInfoList", 1},
    {"SETUPAPI.dll", "SetupDiEnumDeviceInterfaces", 5},
    {"SETUPAPI.dll", "SetupDiGetClassDevsA", 4},
    {"SETUPAPI.dll", "SetupDiGetDeviceInterfaceDetailA", 6},
    {"SHLWAPI.dll", "PathFindExtensionA", 1},
    {"SHLWAPI.dll", "PathFindFileNameA", 1},
    {"SHLWAPI.dll", "PathIsUNCA", 1},
    {"SHLWAPI.dll", "PathStripToRootA", 1},
    {"WINMM.dll", "waveInAddBuffer", 3},
    {"WINMM.dll", "waveInClose", 1},
    {"WINMM.dll", "waveInGetDevCapsA", 3},
    {"WINMM.dll", "waveInGetDevCapsW", 3},
    {"WINMM.dll", "waveInGetNumDevs", 0},
    {"WINMM.dll", "waveInOpen", 6},
    {"WINMM.dll", "waveInPrepareHeader", 3},
    {"WINMM.dll", "waveInReset", 1},
    {"WINMM.dll", "waveInStart", 1},
    {"WINMM.dll", "waveInUnprepareHeader", 3},
    {"WINMM.dll", "waveOutGetDevCapsA", 3},
    {"WINMM.dll", "waveOutGetDevCapsW", 3},
    {"WINSPOOL.DRV", "DocumentPropertiesA", 6},
    {"WINSPOOL.DRV", "OpenPrinterA", 3},
    {"WS2_32.dll", "ord4", 3},
    {"WS2_32.dll", "ord9", 1},
    {"WS2_32.dll", "ord11", 1},
    {"WS2_32.dll", "ord19", 4},
    {"WS2_32.dll", "ord23", 3},
    {"WS2_32.dll", "ord115", 2},
    {"WSOCK32.dll", "ord1", 3},
    {"WSOCK32.dll", "ord2", 3},
    {"WSOCK32.dll", "ord3", 1},
    {"WSOCK32.dll", "ord4", 3},
    {"WSOCK32.dll", "ord8", 1},
    {"WSOCK32.dll", "ord9", 1},
    {"WSOCK32.dll", "ord10", 1},
    {"WSOCK32.dll", "ord12", 1},
    {"WSOCK32.dll", "ord13", 2},
    {"WSOCK32.dll", "ord16", 4},
    {"WSOCK32.dll", "ord18", 5},
    {"WSOCK32.dll", "ord19", 4},
    {"WSOCK32.dll", "ord23", 3},
    {"WSOCK32.dll", "ord51", 1},
    {"WSOCK32.dll", "ord103", 5}, // WSAAsyncGetHostByName(hwnd, msg, name, buf, buflen): 0, no request
    {"WSOCK32.dll", "ord108", 1}, // WSACancelAsyncRequest(handle)
    {"WSOCK32.dll", "ord111", 0},
    {"WSOCK32.dll", "ord115", 2},
    {"WSOCK32.dll", "ord116", 0},
    {"dbghelp.dll", "MiniDumpWriteDump", 7},
    {"imagehlp.dll", "StackWalk", 9},
    {"imagehlp.dll", "SymCleanup", 1},
    {"imagehlp.dll", "SymFunctionTableAccess", 2},
    {"imagehlp.dll", "SymGetLineFromAddr", 4},
    {"imagehlp.dll", "SymGetModuleBase", 2},
    {"imagehlp.dll", "SymGetModuleInfo", 3},
    {"imagehlp.dll", "SymGetOptions", 0},
    {"imagehlp.dll", "SymGetSymFromAddr", 4},
    {"imagehlp.dll", "SymInitialize", 3},
    {"imagehlp.dll", "SymLoadModule", 6},
    {"imagehlp.dll", "SymSetOptions", 1},
};
uint8_t known_stdcall_args(const char *dll, const char *name) {
    for (const KnownArgs &k : known_args)
        if (!strcmp(k.name, name) && lower_dll(k.dll) == lower_dll(dll))
            return k.argc;
    return ARGC_UNKNOWN;
}

// x86 stdcall exports may carry their stack byte count in _name@bytes.
// Only use this ABI evidence when no explicit signature was registered;
// fastcall (@name@bytes), C++ mangling and malformed suffixes remain unknown.
uint8_t decorated_stdcall_args(const char *name) {
    if (!name || name[0] != '_')
        return ARGC_UNKNOWN;
    const char *suffix = strrchr(name, '@');
    if (!suffix || suffix <= name + 1 || !suffix[1])
        return ARGC_UNKNOWN;
    unsigned bytes = 0;
    for (const char *p = suffix + 1; *p; ++p) {
        if (*p < '0' || *p > '9')
            return ARGC_UNKNOWN;
        bytes = bytes * 10 + unsigned(*p - '0');
        if (bytes > 4u * (ARGC_UNKNOWN - 1))
            return ARGC_UNKNOWN;
    }
    return bytes % 4 ? ARGC_UNKNOWN : uint8_t(bytes / 4);
}

} // namespace

void imports_register(const ImportShim *shims, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const ImportShim &s = shims[i];
        if (s.dll && *s.dll)
            registered_dlls().insert(lower_dll(s.dll));
        std::string k = key_of(s.dll, s.name);
        auto it = registry().find(k);
        if (it != registry().end() && it->second.fn && !s.fn)
            continue;
        registry()[k] = s;
        // If a trampoline already exists, upgrade it in place.
        auto ti = index().find(k);
        if (ti != index().end()) {
            Tramp &t = tramps()[ti->second];
            if (s.fn)
                t.fn = s.fn;
            t.argc = s.argc_stdcall;
            hot_sync(ti->second);
        }
    }
}

bool imports_has_dll(const char *dll_lower) {
    return registered_dlls().count(lower_dll(dll_lower)) != 0;
}

uint32_t imports_alloc_trampoline(const char *dll, const char *name, void (*fn)(X86 *),
                                  uint8_t argc_stdcall) {
    std::string k = key_of(dll, name);
    auto it = index().find(k);
    if (it != index().end()) {
        Tramp &t = tramps()[it->second];
        if (fn) {
            t.fn = fn;
            t.argc = argc_stdcall;
            hot_sync(it->second);
        }
        return TRAMP_BASE + TRAMP_STRIDE * it->second;
    }

    Tramp t;
    t.atomic = atomic_import(k);
    t.dll = dll ? dll : "?";
    t.name = name ? name : "?";
    t.desc = t.dll + "!" + t.name;
    t.fn = fn;
    t.argc = argc_stdcall;

    auto ri = registry().find(k);
    if (ri != registry().end()) {
        if (!t.fn)
            t.fn = ri->second.fn;
        if (!fn)
            t.argc = ri->second.argc_stdcall;
    }
    if (t.argc == ARGC_UNKNOWN)
        t.argc = decorated_stdcall_args(name);
    if (t.argc == ARGC_UNKNOWN)
        t.argc = known_stdcall_args(dll, name);

    uint32_t idx = (uint32_t)tramps().size();
    if (idx >= TRAMP_MAX) {
        LOGW("imports: trampoline space exhausted (%u entries) for %s", idx, t.desc.c_str());
        return 0;
    }
    const Tramp *before = tramps().data();
    tramps().push_back(t);
    // A reallocation moved every entry, and a short desc lives inside its entry.
    for (uint32_t i = tramps().data() == before ? idx : 0; i <= idx; ++i)
        hot_sync(i);
    g_hot_count = idx + 1;
    index()[k] = idx;
    return TRAMP_BASE + TRAMP_STRIDE * idx;
}

// The data imports D3DPopTB.exe has. weanetr.dll exports three GUIDs (16 bytes
// each) and options_to_parity_table (an int array; 4 KB of zeroed storage is
// what the plan specifies). Registering more is how a later task adds its own.
void imports_register_data(const char *dll, const char *name, uint32_t size, void (*init)(uint32_t addr)) {
    std::string k = key_of(dll, name);
    DataImport &d = data_imports()[k];
    if (d.size < size)
        d.size = size;
    if (d.desc.empty())
        d.desc = std::string(dll) + "!" + name;
    if (init)
        d.init = init;
}

static void register_default_data_imports() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register_data("weanetr.dll", "?BFAID_INet@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?BFAID_MODEM@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?BFSPGUID_MODEM@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?options_to_parity_table@@3PAHA", 4096);
}

uint32_t imports_alloc_data(const char *dll, const char *name) {
    register_default_data_imports();
    auto it = data_imports().find(key_of(dll, name));
    if (it == data_imports().end())
        return 0;
    DataImport &d = it->second;
    // mem_init() invalidates the heap, so a stale address is re-allocated.
    if (!d.addr || !heap_owns(d.addr)) {
        d.addr = heap_alloc(d.size, true);
        if (!d.addr)
            LOGW("imports: cannot allocate %u bytes for data import %s", d.size, d.desc.c_str());
        else
            LOGV("data import %s -> %u zeroed bytes at %08x", d.desc.c_str(), d.size, d.addr);
        if (d.addr && d.init)
            d.init(d.addr);
    }
    return d.addr;
}

uint32_t imports_data_address(const char *dll, const char *name) {
    auto it = data_imports().find(key_of(dll, name));
    return it == data_imports().end() ? 0 : it->second.addr;
}

uint32_t imports_data_count() {
    return (uint32_t)data_imports().size();
}

uint32_t imports_trampoline_for(const char *dll, const char *name) {
    auto it = index().find(key_of(dll, name));
    if (it == index().end())
        return 0;
    return TRAMP_BASE + TRAMP_STRIDE * it->second;
}

uint32_t imports_resolve(const char *dll, const char *name) {
    uint32_t a = imports_trampoline_for(dll, name);
    if (a)
        return a;
    auto ri = registry().find(key_of(dll, name));
    if (ri == registry().end())
        return 0;
    return imports_alloc_trampoline(ri->second.dll, ri->second.name, ri->second.fn,
                                    ri->second.argc_stdcall);
}

bool imports_serves_module(const char *dll) {
    if (!dll || !*dll)
        return false;
    // key_of applies the same DLL normalization as registration/resolution.
    // Including '!' makes the prefix match the complete module name only.
    std::string prefix = key_of(dll, nullptr);
    auto it = registry().lower_bound(prefix);
    return it != registry().end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

const char *imports_describe(uint32_t target) {
    if (!imports_is_trampoline(target))
        return nullptr;
    uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    if (idx >= tramps().size())
        return nullptr;
    return tramps()[idx].desc.c_str();
}

uint8_t imports_argc(uint32_t trampoline) {
    if (!imports_is_trampoline(trampoline))
        return 0;
    uint32_t idx = (trampoline - TRAMP_BASE) / TRAMP_STRIDE;
    return idx < tramps().size() ? tramps()[idx].argc : 0;
}

uint32_t imports_count() {
    return (uint32_t)tramps().size();
}

uint32_t imports_call_count(const char *dll, const char *name) {
    auto it = index().find(key_of(dll, name));
    if (it == index().end())
        return 0;
    return g_hot[it->second].calls;
}

void imports_dump_stats(FILE *out) {
    std::vector<uint32_t> v;
    for (uint32_t i = 0; i < g_hot_count; ++i)
        if (g_hot[i].calls)
            v.push_back(i);
    std::sort(v.begin(), v.end(), [](uint32_t a, uint32_t b) { return g_hot[a].calls > g_hot[b].calls; });
    fprintf(out, "import calls (%zu of %zu entries used):\n", v.size(), tramps().size());
    for (uint32_t i : v)
        fprintf(out, "  %8u  %-40s %s\n", g_hot[i].calls, tramps()[i].desc.c_str(),
                tramps()[i].fn ? "" : "(logging only)");
}

void imports_coverage(uint32_t *implemented, uint32_t *logging_only, uint32_t *unknown_argc) {
    uint32_t impl = 0, log_only = 0, unknown = 0;
    for (const Tramp &t : tramps()) {
        if (t.fn)
            ++impl;
        else
            ++log_only;
        if (t.argc == ARGC_UNKNOWN)
            ++unknown;
    }
    if (implemented)
        *implemented = impl;
    if (logging_only)
        *logging_only = log_only;
    if (unknown_argc)
        *unknown_argc = unknown;
}

void imports_dump_coverage(FILE *out) {
    std::map<std::string, std::pair<uint32_t, uint32_t>> per_dll; // implemented, logging-only
    std::vector<const Tramp *> log_only, unknown;
    for (const Tramp &t : tramps()) {
        auto &e = per_dll[t.dll];
        if (t.fn)
            ++e.first;
        else {
            ++e.second;
            log_only.push_back(&t);
        }
        if (t.argc == ARGC_UNKNOWN)
            unknown.push_back(&t);
    }
    fprintf(out, "import coverage by DLL (implemented / logging-only):\n");
    for (const auto &kv : per_dll)
        fprintf(out, "  %-16s %3u / %3u\n", kv.first.c_str(), kv.second.first, kv.second.second);
    fprintf(out, "logging-only imports (%zu):\n", log_only.size());
    for (const Tramp *t : log_only)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "imports with an unknown stdcall argument count (%zu):\n", unknown.size());
    for (const Tramp *t : unknown)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "data imports backed by guest storage (%zu):\n", data_imports().size());
    for (const auto &kv : data_imports())
        fprintf(out, "  %-44s %5u bytes at %08x\n", kv.second.desc.c_str(), kv.second.size,
                kv.second.addr);
}

namespace {
uint32_t calls_of(const Tramp *t) {
    return g_hot[t - tramps().data()].calls;
}
} // namespace

void imports_dump_report(FILE *out) {
    std::vector<const Tramp *> called, not_reached, log_only;
    for (const Tramp &t : tramps()) {
        if (!t.fn)
            log_only.push_back(&t);
        else if (calls_of(&t))
            called.push_back(&t);
        else
            not_reached.push_back(&t);
    }
    auto by_name = [](const Tramp *a, const Tramp *b) { return a->desc < b->desc; };
    std::sort(called.begin(), called.end(), by_name);
    std::sort(not_reached.begin(), not_reached.end(), by_name);
    std::sort(log_only.begin(), log_only.end(), by_name);

    fprintf(out, "implemented and called (%zu):\n", called.size());
    for (const Tramp *t : called)
        fprintf(out, "  %-44s %u calls\n", t->desc.c_str(), calls_of(t));
    fprintf(out, "implemented but not reached (%zu):\n", not_reached.size());
    for (const Tramp *t : not_reached)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "logging-only (%zu), of which called: ", log_only.size());
    uint32_t hit = 0;
    for (const Tramp *t : log_only)
        if (calls_of(t))
            ++hit;
    fprintf(out, "%u\n", hit);
    for (const Tramp *t : log_only)
        fprintf(out, "  %-44s %s\n", t->desc.c_str(), calls_of(t) ? "CALLED" : "");
}

namespace {
ImportReturnObserver g_return_observer = nullptr;
}

void imports_set_return_observer(ImportReturnObserver fn) {
    g_return_observer = fn;
}

namespace {
ImportCallObserver g_call_observer = nullptr;
}

void imports_set_call_observer(ImportCallObserver fn) {
    g_call_observer = fn;
}

ImportReturnObserver imports_set_return_observer_get(void) {
    return g_return_observer;
}

ImportCallObserver imports_set_call_observer_get(void) {
    return g_call_observer;
}

// Dispatch a guest import trampoline at a scheduler checkpoint.
// Copy dispatch metadata before calling a shim, then restore EIP/ESP according to its calling convention.
namespace {
uint64_t g_import_calls = 0;
} // namespace
uint64_t imports_call_count() {
    return g_import_calls;
}
void imports_call_leaves_surfaces() {
    --g_import_calls;
}

namespace {
// A call to a trampoline that was never allocated: no shim to say how many
// arguments to drop, but the return address must still be consumed or the
// guest stack is displaced from here on.
__attribute__((noinline)) bool dispatch_unallocated(X86 *c, uint32_t target) {
    LOGW("imports: call to unallocated trampoline %08x (ESP=%08x, return=%08x)", target,
         c->r[R_ESP], rd32(c->r[R_ESP]));
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
    set_eax(c, 0);
    return true;
}

// Every case imports_dispatch's own path leaves out: the verbose trace, a call
// observer, a logging-only stub and an unknown argument count. Out of line, so
// the common path keeps a small frame without these buffers.
__attribute__((noinline)) bool dispatch_full(X86 *c, uint32_t idx, bool trace) {
    // The shim may allocate trampolines (GetProcAddress, a COM shim, a guest
    // callback), which reallocates this vector, so nothing may hold a
    // reference to the element across the call.
    void (*fn)(X86 *) = g_hot[idx].fn;
    uint8_t argc = g_hot[idx].argc;
    // A guest exception can longjmp across fn(c). Keep no C++ owner live
    // across that call, and do not retain a pointer into the movable vector:
    // the label is read from the table again after the shim returns (the
    // entry at idx never changes; only the vector may move).
    // Fetched at each use: a shim or an observer may allocate a trampoline and move the table.
    auto label = [idx] { return g_hot[idx].desc; };
    ++g_hot[idx].calls;
    ++g_import_calls;

    uint32_t ret_addr = rd32(c->r[R_ESP]);
    if (trace) {
        log_msg(2, "-> %s (esp=%08x ret=%08x)", label(), c->r[R_ESP], ret_addr);
        recomp_seh_validate_chain(c, "import enter", label());
    }

    // The arguments as they are NOW, before the shim runs: a stdcall shim pops
    // them, so after the call they are gone and this is the only point they
    // can be read. Only meaningful for a known stdcall count; a cdecl or
    // unknown shim reports none rather than inventing them.
    bool skip = false;
    if (g_call_observer) {
        uint32_t args[8];
        uint32_t n = (argc == ARGC_CDECL || argc == ARGC_UNKNOWN) ? 0u : (uint32_t)argc;
        if (n > 8)
            n = 8;
        for (uint32_t i = 0; i < n; ++i)
            args[i] = rd32(c->r[R_ESP] + 4 + 4 * i);
        uint32_t result = 0;
        if (!g_call_observer(label(), args, n, &result)) {
            set_eax(c, result);
            skip = true;
        }
    }

    if (skip) {
        // The observer refused the call and has set the result itself.
    } else if (fn) {
        fn(c);
    } else {
        log_once(label(), "unimplemented import %s: returning 0", label());
        set_eax(c, 0);
    }
    // Read after the shim ran, so the pointer is into the table as it is now.
    if (g_return_observer)
        g_return_observer(label(), c->r[R_EAX]);
    if (trace)
        log_msg(2, "<- %s (eax=%08x)", label(), c->r[R_EAX]);

    uint32_t pop = 4;
    if (argc == ARGC_CDECL) {
        pop = 4;
    } else if (argc == ARGC_UNKNOWN) {
        pop = 4;
        char key[512 + 6];
        snprintf(key, sizeof key, "%s#argc", label());
        log_once(key,
                 "%s has an unknown stdcall argument count: not adjusting ESP, "
                 "the guest stack will drift if it is really stdcall",
                 label());
    } else {
        pop = 4 + 4u * argc;
    }
    c->r[R_ESP] += pop;
    c->eip = ret_addr;
    if (trace)
        recomp_seh_validate_chain(c, "import leave", label());
    return true;
}
} // namespace

// Free threads (kernel32.cpp, RECOMP_FREE_THREADS): what a shim called without the baton may do.
extern __thread bool recomp_t_free;
void sched_free_acquire();
void sched_free_release();
void sched_free_enter_critsec(uint32_t cs);
bool sched_free_try_critsec(uint32_t cs);
void sched_free_leave_critsec(uint32_t cs);
bool sched_free_sleep(uint32_t ms);

namespace {
enum FreeKind : uint8_t { FREE_UNKNOWN, FREE_BATON, FREE_DIRECT, FREE_SLEEP, FREE_CS_ENTER, FREE_CS_TRY, FREE_CS_LEAVE };
std::atomic<uint8_t> g_free_kind[TRAMP_MAX];

// By name, once per trampoline. FREE_DIRECT shims touch nothing but the guest words they are given,
// the caller's own TLS and the host clock; everything else keeps the baton.
FreeKind free_kind_of(const char *desc) {
    static const char *const direct[] = {
        "KERNEL32.dll!QueryPerformanceCounter", "KERNEL32.dll!QueryPerformanceFrequency",
        "KERNEL32.dll!GetCurrentThreadId",      "KERNEL32.dll!TlsGetValue",
        "KERNEL32.dll!TlsSetValue",             "KERNEL32.dll!InterlockedIncrement",
        "KERNEL32.dll!InterlockedDecrement",    "KERNEL32.dll!InterlockedExchange",
        "KERNEL32.dll!InterlockedCompareExchange", "KERNEL32.dll!GetLastError",
        "KERNEL32.dll!SetLastError",            "KERNEL32.dll!EncodePointer",
        "KERNEL32.dll!DecodePointer",           "WINMM.dll!timeGetTime"};
    if (!desc)
        return FREE_BATON;
    for (const char *d : direct)
        if (!strcasecmp(desc, d))
            return FREE_DIRECT;
    if (!strcasecmp(desc, "KERNEL32.dll!Sleep"))
        return FREE_SLEEP;
    if (!strcasecmp(desc, "KERNEL32.dll!EnterCriticalSection"))
        return FREE_CS_ENTER;
    if (!strcasecmp(desc, "KERNEL32.dll!TryEnterCriticalSection"))
        return FREE_CS_TRY;
    if (!strcasecmp(desc, "KERNEL32.dll!LeaveCriticalSection"))
        return FREE_CS_LEAVE;
    return FREE_BATON;
}

__attribute__((noinline)) bool free_dispatch(X86 *c, uint32_t target) {
    const uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    FreeKind kind = FREE_BATON;
    if (idx < g_hot_count && g_hot[idx].fn && g_hot[idx].argc != ARGC_UNKNOWN) {
        kind = (FreeKind)g_free_kind[idx].load(std::memory_order_relaxed);
        if (kind == FREE_UNKNOWN) {
            kind = free_kind_of(g_hot[idx].desc);
            g_free_kind[idx].store(kind, std::memory_order_relaxed);
            if (kind == FREE_BATON)
                LOGW("free thread: %s runs with the baton", g_hot[idx].desc ? g_hot[idx].desc : "?");
        }
    }
    const uint32_t ret_addr = rd32(c->r[R_ESP]);
    switch (kind) {
    case FREE_DIRECT:
        g_hot[idx].fn(c);
        break;
    case FREE_SLEEP:
        if (!sched_free_sleep(arg(c, 0)))
            goto baton;
        set_eax(c, 0);
        break;
    case FREE_CS_ENTER:
        sched_free_enter_critsec(arg(c, 0));
        set_eax(c, 0);
        break;
    case FREE_CS_TRY:
        set_eax(c, sched_free_try_critsec(arg(c, 0)) ? 1 : 0);
        break;
    case FREE_CS_LEAVE:
        sched_free_leave_critsec(arg(c, 0));
        set_eax(c, 0);
        break;
    default:
    baton: {
        // The shim runs as it would for a batoned thread; one that never returns (ExitThread) leaves
        // the baton held and recomp_t_free down, which is what the exit paths expect.
        sched_free_acquire();
        const bool r = imports_dispatch(c, target);
        sched_free_release();
        return r;
    }
    }
    const uint8_t argc = g_hot[idx].argc;
    c->r[R_ESP] += argc == ARGC_CDECL ? 4u : 4u + 4u * argc;
    c->eip = ret_addr;
    return true;
}
} // namespace

bool imports_dispatch(X86 *c, uint32_t target) {
    if (!imports_is_trampoline(target))
        return false;
    if (__builtin_expect(recomp_t_free, 0))
        return free_dispatch(c, target);
    // A bounded scheduling checkpoint. Cooperative threads must not depend on
    // the guest happening to call a blocking API: the frame limiter spins on
    // GetTickCount and would otherwise starve the service threads for as long
    // as it liked, expired waits and all. Every call into the runtime is a
    // checkpoint, rate limited to one yield per millisecond per thread, and it
    // runs before the shim so the guest state is a consistent call boundary -
    // except at an atomic import, and inside a sched_atomic_enter stretch.
    const uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    if (idx >= g_hot_count || !g_hot[idx].atomic)
        sched_checkpoint();
    if (idx >= g_hot_count)
        return dispatch_unallocated(c, target);
    // Test71: every graphics call passes here (tens of thousands per frame),
    // so the per-call work outside the shim is kept to what is used. The log
    // level is read once (memory.cpp caches it at first use and it never
    // changes); below 2, LOGV and the SEH chain check do nothing.
    static const bool trace = log_level() >= 2;
    // Read before the call: the shim may allocate a trampoline or upgrade this one, and the
    // entry it runs is the one it found. g_hot never moves, unlike the Tramp vector.
    void (*fn)(X86 *) = g_hot[idx].fn;
    const uint8_t argc = g_hot[idx].argc;
    if (__builtin_expect(trace || g_call_observer || !fn || argc == ARGC_UNKNOWN, 0))
        return dispatch_full(c, idx, trace);
    ++g_hot[idx].calls;
    ++g_import_calls;
    // A guest exception can longjmp across fn(c): nothing here owns anything.
    const uint32_t ret_addr = rd32(c->r[R_ESP]);
    fn(c);
    // Read after the shim ran, so the pointer is into the table as it is now.
    if (g_return_observer)
        g_return_observer(g_hot[idx].desc, c->r[R_EAX]);
    c->r[R_ESP] += argc == ARGC_CDECL ? 4u : 4u + 4u * argc;
    c->eip = ret_addr;
    return true;
}

// ---------------------------------------------------------------------------
// Calling guest code from a shim.
// ---------------------------------------------------------------------------
namespace {
struct Callback {
    X86 *cpu;
    jmp_buf env;
    uint32_t saved_esp, saved_eip, return_sp, profile_depth;
};
struct Callbacks {
    std::vector<Callback *> stack;
    ~Callbacks() {
        for (Callback *call : stack)
            delete call;
    }
};
thread_local Callbacks callbacks;
} // namespace

uint32_t recomp_callback_depth(void) {
    return (uint32_t)callbacks.stack.size();
}

void recomp_callback_truncate(uint32_t depth) {
    while (callbacks.stack.size() > depth) {
        delete callbacks.stack.back();
        callbacks.stack.pop_back();
    }
}

void recomp_callback_reset(X86 *c) {
    for (size_t i = callbacks.stack.size(); i-- > 0;)
        if (!c || callbacks.stack[i]->cpu == c) {
            delete callbacks.stack[i];
            callbacks.stack.erase(callbacks.stack.begin() + i);
        }
}

void recomp_callback_return(X86 *c) {
    if (callbacks.stack.empty())
        return;
    Callback *call = callbacks.stack.back();
    // Every translated return here is deeper on the host stack than this
    // live guest_call. Match the guest return slot as well: a completing SEH
    // landing may RET to an older callback's identical sentinel, in which
    // case intercept must finish that function through its SEH checkpoint.
    if (call->cpu != c || c->r[R_ESP] < call->return_sp + 4 || c->r[R_ESP] > call->saved_esp)
        return;
    mods_hooks_unwind_to_esp(call->return_sp);
    recomp_profile_truncate(call->profile_depth);
    longjmp(call->env, 1);
}

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) {
    Callback *call = new Callback{};
    call->cpu = c;
    call->saved_esp = c->r[R_ESP];
    call->saved_eip = c->eip;
    call->profile_depth = recomp_profile_depth();
    uint32_t esp = call->saved_esp;
    for (int i = nargs - 1; i >= 0; --i) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, GUEST_RETURN_SENTINEL);
    c->r[R_ESP] = esp;
    call->return_sp = esp;
    callbacks.stack.push_back(call);
    if (!setjmp(call->env))
        recomp_run(c, fn);
    recomp_seh_callback_leave(c, recomp_callback_depth());
    uint32_t eax = c->r[R_EAX];
    c->r[R_ESP] = call->saved_esp;
    c->eip = call->saved_eip;
    callbacks.stack.pop_back();
    delete call;
    return eax;
}

// ---------------------------------------------------------------------------
// Shim tables owned by this runtime.
// ---------------------------------------------------------------------------
void imports_init() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_kernel32_shims, g_kernel32_shim_count);
    kernel32_wide_register();
    oleaut32_register();
    misc_dlls_register();
    msvcrt_register();
    comctl32_register();
    media_foundation_register();
    imports_register(g_user32_shims, g_user32_shim_count);
    user32_wide_register();
    user32_vcl_register();
    imports_register(g_misc_shims, g_misc_shim_count);
    imports_register(g_gdi32_shims, g_gdi32_shim_count);
    extern void gdi_model_register();
    gdi_model_register();
    extern void gdi_bitmaps_register();
    gdi_bitmaps_register();
    gdi::register_draw();
    gdi::register_text();
    extern void msimg32_register();
    msimg32_register();
}
