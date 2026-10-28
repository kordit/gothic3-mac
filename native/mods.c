/* native/mods.c - the script mods of the Poprawki set (docs/mods/plan-poprawki.md) as native code.
 *
 * On Windows each one is a Script_*.dll built on the Gothic 3 SDK that patches Engine and Game
 * code in memory (VirtualProtect). The port runs translated C, so such a patch would land on
 * bytes nothing executes; each is redone here as a g3n_ replacement (native/g3_overrides.h) that
 * acts by default (session 12, the player's choice; G3_NATIVE_MODS=listed limits them to
 * <profile>/active-mods.json, the launcher's contract in tools/g3mods.py, =none turns them off),
 * decided once at the first call. Otherwise the translated original runs as is.
 *
 * Font Scale (docs/mods/dll-fontscale.md): before gCGUIEntityRenderer::DrawFocusNames
 * (Game 10989810), when the viewport's height differs from the last seen (1080 at start), a new
 * focus-name font of round(height * 24 / 1080) pixels goes to the renderer's +0x70, built the way
 * the mod builds it: the engine setup's font name (+0x1e4) and weight (bold +0x1e8 -> 800, else
 * 400), eCGUIAdmin's first virtual method.
 *
 * Subtitles (docs/mods/dll-subtitles.md): gCScriptProcessingUnit::sAIOutputInstr (Game 10a793e0)
 * sets a line's time limit at pu+0x9c to (length of the sample name + 10) * 100 ms; the mod sets
 * it from the words of the text shown, max(words, 4) * 250 / 500 / 1000 ms (subtitles.ini
 * DisplayTime Short / Normal / Long; Infinite never ends a line by time). Voiced lines end with
 * their sound, so only unvoiced ones change. The mod hooks the two calls in Script_Game, which
 * this recognises by the return address: _AI_Say_Ex (10ff9478, text id at the caller's
 * ESI+0x160) and SaySound (1106a447, the sample name). The original runs, then the time is
 * written over its own on the path that sets it (stop byte not 1, arg and the speaker set),
 * where nothing reads it before the return.
 *
 * G3Fixes (docs/mods/dll-g3fixes.md): the hooks of its table, each as the mod does it, at
 * function starts here and inside functions as game.toml native_sites (native_site_<at>), with
 * G3Fixes.ini as the player has it.
 *
 * Extended Frying (docs/mods/dll-extendedfrying.md), while active-mods.json lists it: the frying
 * page lists and fries any item for which a template <name with _Raw as _Fried>, <name>_Fried or
 * <name>_Fried_Pan exists (Immersive World's deer, pig, camel, chicken, sheep and fish), not only
 * It_Meat_Raw and It_Meatbug_Raw. The mod's three hooks and its NOP patch sit inside two small
 * functions, so both are replaced whole from their start: the list filter (Game 109b2280, slot 67
 * of gCHUDListCtrlInvTransforms) and OnPlayerFryStack (Script_Game 1101a740).
 *
 * G3LOC=<id> in the trace control file (RECOMP_TRACE_CONTROL) prints the string table's entry
 * <id> in the current language as "g3loc<" lines: whether strings.mod is what the game reads. */
#include "chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* kit/runtime (C++): win32.h guest_current_context, guest.h guest_call, layout.h host_profile_dir. */
X86 *guest_current_context(void) __asm__("__Z21guest_current_contextv");
uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");
const char *host_profile_dir(void);

void fn_10989810(X86 *c); /* Game gCGUIEntityRenderer::DrawFocusNames */
void fn_10a793e0(X86 *c); /* Game gCScriptProcessingUnit::sAIOutputInstr */
void fn_10f4b230(X86 *c); /* Script Entity::EndTransform */
void fn_10ffcdd0(X86 *c); /* Script_Game _AI_UseInventoryItem */
void fn_10fddb80(X86 *c); /* Script_Game IsDeadlyDamage */
void fn_10fdd530(X86 *c); /* Script_Game CanBurn */
void fn_10fdd7b0(X86 *c); /* Script_Game CanFreeze */
void fn_1105c3c0(X86 *c); /* Script_Game Respawn */
void fn_10fce570(X86 *c); /* Script_Game ZS_ObserveSuspect */
void fn_10fc2f50(X86 *c); /* Script_Game OnFollowPlayer */
void fn_10fc3d60(X86 *c); /* Script_Game OnGuidePlayer */
void fn_102c6bd0(X86 *c); /* Engine eCConsole::OnExecute */
void fn_10dd4680(X86 *c); /* GFC CFFGFCWnd window thunk */
void fn_10276490(X86 *c); /* Engine eCApplication::Process */
void fn_10a5f180(X86 *c); /* Game gCPlayerMemory_PS::CalcMissingValue */
void fn_109b2280(X86 *c); /* Game gCHUDListCtrlInvTransforms slot 67, the transform list's filter */
void fn_1101a740(X86 *c); /* Script_Game OnPlayerFryStack */

/* SharedBase */
#define STR_CTOR 0x100029c0u       /* bCString::bCString(char const *) */
#define STR_DEFAULT 0x10002a50u    /* bCString::bCString(void) */
#define STR_ASSIGN 0x10005040u     /* bCString::operator=(bCString const &) */
#define STR_DTOR 0x10001a00u       /* bCString::~bCString(void) */
#define USTR_CTOR 0x100059a0u      /* bCUnicodeString::bCUnicodeString(char const *) */
#define USTR_WORDS 0x100081a0u     /* bCUnicodeString::CountWords(bCUnicodeString const &) const */
#define USTR_DTOR 0x10005ca0u      /* bCUnicodeString::~bCUnicodeString(void) */
/* Engine */
#define MODULES_GET 0x1027d520u    /* eCModuleAdmin::GetInstance, stdcall */
#define MODULES_FIND 0x1027d570u   /* eCModuleAdmin::FindModule(bCString const &) const */
#define GFX_ADMIN_VTABLE 0x10689e1cu /* eCGfxAdmin::`vftable' */
#define GFX_MIXER 0x10316f30u      /* eCGfxAdmin::GetAccessToMixer(void) */
#define MIXER_VIEWPORT 0x102e8b70u /* eCGfxMixerAdmin::GetViewport(eSGfxViewport &) const */
#define APP_GET 0x1026ba90u        /* eCApplication::GetInstance, stdcall */
#define APP_SETUP 0x10274db0u      /* eCApplication::GetSetupEngineDesc(void) const */
#define GUI_GET 0x1033f210u        /* eCGUIModule::GetInstance, stdcall */
#define GUI_ADMIN 0x103557e0u      /* eCGUIModule::GetAdmin(void) */
#define LOC_CTOR 0x1050e5a0u       /* eCLocString::eCLocString(bCString const &) */
#define LOC_TEXT 0x1037fb80u       /* eCLocString::operator bCUnicodeString const(void) const */
#define LOC_DTOR 0x1050e5f0u       /* eCLocString::~eCLocString(void) */
/* Script_Game: return addresses of the two calls the Subtitles mod hooks */
#define RET_SAY 0x10ff9478u
#define RET_SAY_SOUND 0x1106a447u

enum { MOD_FONT_SCALE = 1, MOD_SUBTITLES = 2, MOD_G3FIXES = 4, MOD_EXTENDED_FRYING = 8 };

static int mods_read;
static uint32_t mods_on;
static void g3f_read_ini(const char *profile);
static int sub_mode = 1; /* subtitles.ini DisplayTime: 0 Short, 1 Normal (the mod's default), 2 Long, 3 Infinite */

static char *read_file(const char *path, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char *buf = malloc(cap + 1);
    size_t n = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    if (buf)
        buf[n] = 0;
    return buf;
}

/* The DisplayTime value of <profile>/ini/subtitles.ini, which tools/g3mods.py links from the mod. */
static int read_display_time(const char *profile) {
    char path[1024];
    snprintf(path, sizeof path, "%s/ini/subtitles.ini", profile);
    char *text = read_file(path, 1 << 16);
    int mode = 1;
    for (const char *p = text; p && (p = strstr(p, "DisplayTime")) != 0; p += 11) {
        if (p != text && p[-1] != '\n')
            continue; /* the comment line names it too */
        const char *v = p + 11;
        while (*v == ' ' || *v == '=')
            ++v;
        static const char *const names[] = {"Short", "Normal", "Long", "Infinite"};
        for (int i = 0; i < 4; ++i)
            if (!strncasecmp(v, names[i], strlen(names[i])))
                mode = i;
        break;
    }
    free(text);
    return mode;
}

static uint32_t mods(void) {
    if (mods_read)
        return mods_on;
    mods_read = 1;
    const char *dir = host_profile_dir();
    char profile[1024];
    snprintf(profile, sizeof profile, "%s", dir ? dir : "");
    char path[1100];
    snprintf(path, sizeof path, "%s/active-mods.json", profile);
    /* Session 12, the player: the mods redone here run by default, whatever the launcher listed (it
       blocks DLL mods, so they never reached active-mods.json). G3_NATIVE_MODS=none turns both off,
       G3_NATIVE_MODS=listed keeps them to active-mods.json as before. */
    const char *pick = getenv("G3_NATIVE_MODS");
    if (pick && !strcmp(pick, "none"))
        mods_on = 0;
    else if (pick && !strcmp(pick, "listed")) {
        char *json = read_file(path, 1 << 20);
        if (json) {
            /* The ids as mod.json names them; a folder path never has a quote beside them. */
            if (strstr(json, "\"Font Scale\""))
                mods_on |= MOD_FONT_SCALE;
            if (strstr(json, "\"Subtitles\""))
                mods_on |= MOD_SUBTITLES;
            free(json);
        }
    } else
        mods_on = MOD_FONT_SCALE | MOD_SUBTITLES;
    /* G3Fixes changes the game's rules (docs/mods/dll-g3fixes.md), so it stays with the player's
       set: on only while active-mods.json lists it, and off with G3_NATIVE_MODS=none. */
    if (!(pick && !strcmp(pick, "none"))) {
        char *json = read_file(path, 1 << 20);
        if (json && strstr(json, "\"G3Fixes\""))
            mods_on |= MOD_G3FIXES;
        /* Extended Frying too: a rule of the game, and Immersive World's dependency. */
        if (json && strstr(json, "\"Extended Frying\""))
            mods_on |= MOD_EXTENDED_FRYING;
        free(json);
    }
    if (mods_on & MOD_SUBTITLES)
        sub_mode = read_display_time(profile);
    fprintf(stderr, "g3mods: native mods from %s: font scale %s, subtitles %s (display time %d), extended frying %s\n",
            path, (mods_on & MOD_FONT_SCALE) ? "on" : "off", (mods_on & MOD_SUBTITLES) ? "on" : "off", sub_mode,
            (mods_on & MOD_EXTENDED_FRYING) ? "on" : "off");
    if (mods_on & MOD_G3FIXES)
        g3f_read_ini(profile);
    return mods_on;
}

/* Guest calls from a replacement: the whole guest CPU state (registers, flags, x87, SSE) as it was
   afterwards, scratch below ESP. X86 holds nothing but that state (kit/runtime/x86.h). */
typedef struct {
    X86 x;
} Saved;

static uint32_t scratch(X86 *c, Saved *s, uint32_t size) {
    s->x = *c;
    const uint32_t base = (c->r[R_ESP] - size) & ~15u;
    memset(g_mem + base, 0, size);
    c->r[R_ESP] = base - 16u;
    return base;
}

static void restore(X86 *c, const Saved *s) {
    *c = s->x;
}

static uint32_t thiscall(X86 *c, uint32_t fn, uint32_t self, const uint32_t *args, int n) {
    c->r[R_ECX] = self;
    return guest_call(c, fn, args, n);
}

static uint32_t thiscall1(X86 *c, uint32_t fn, uint32_t self, uint32_t a0) {
    return thiscall(c, fn, self, &a0, 1);
}

static void put_text(uint32_t at, const char *s) {
    memcpy(g_mem + at, s, strlen(s) + 1);
}

/* ---------------------------------------------------------------- Font Scale -- */

static int32_t fs_height = 1080; /* the mod's starting value: at 1080 the game keeps its own font */
static uint32_t fs_gfx_admin;
static int fs_no_admin;

static void font_scale(X86 *c, uint32_t renderer) {
    Saved s;
    const uint32_t base = scratch(c, &s, 0x100);
    const uint32_t name = base, view = base + 0x20, desc = base + 0x40, text = base + 0x80;
    if (!fs_gfx_admin && !fs_no_admin) {
        put_text(text, "eCGfxAdmin");
        thiscall1(c, STR_CTOR, name, text);
        const uint32_t modules = guest_call(c, MODULES_GET, 0, 0);
        const uint32_t found = thiscall1(c, MODULES_FIND, modules, name);
        thiscall(c, STR_DTOR, name, 0, 0);
        if (found && rd32(found) == GFX_ADMIN_VTABLE)
            fs_gfx_admin = found;
        else {
            fs_no_admin = 1;
            fprintf(stderr, "g3mods: font scale: no eCGfxAdmin module (found %#x), off\n", found);
        }
    }
    if (fs_gfx_admin) {
        const uint32_t mixer = thiscall(c, GFX_MIXER, fs_gfx_admin, 0, 0);
        thiscall1(c, MIXER_VIEWPORT, mixer, view);
        const int32_t height = (int32_t)(rd32(view + 0xc) - rd32(view + 4));
        if (height != fs_height) {
            fs_height = height;
            /* As the mod: the height as unsigned, to double, to float, times 1/1080 and 24 in single
               precision, rounded half away from zero by +-0.49999997 and truncated. */
            float v = (float)(double)(uint32_t)height;
            v = v * 0.0009259259095415473f;
            v = v * 24.0f;
            const int32_t size = (int32_t)(v >= 0.0f ? v + 0.49999997f : v - 0.49999997f);
            const uint32_t app = guest_call(c, APP_GET, 0, 0);
            const uint32_t setup = thiscall(c, APP_SETUP, app, 0, 0);
            thiscall(c, STR_DEFAULT, desc, 0, 0);
            thiscall1(c, STR_ASSIGN, desc, setup + 0x1e4);
            wr32(desc + 0x4, (uint32_t)size);
            wr32(desc + 0x8, 0);
            wr32(desc + 0xc, g_mem[setup + 0x1e8] ? 800u : 400u);
            wr32(desc + 0x10, 1);
            g_mem[desc + 0x14] = 0;
            const uint32_t gui = guest_call(c, GUI_GET, 0, 0);
            const uint32_t admin = thiscall(c, GUI_ADMIN, gui, 0, 0);
            const uint32_t font = thiscall1(c, rd32(rd32(admin)), admin, desc);
            wr32(renderer + 0x70, font);
            thiscall(c, STR_DTOR, desc, 0, 0);
            fprintf(stderr, "g3mods: font scale: viewport height %d, focus name font %d px (%#x)\n", height,
                    size, font);
        }
    }
    restore(c, &s);
}

void g3n_10989810(X86 *c) {
    if (mods() & MOD_FONT_SCALE)
        font_scale(c, c->r[R_ECX]);
    fn_10989810(c);
}

/* ----------------------------------------------------------------- Subtitles -- */

/* The words of the localized text `id` (a guest bCString), as bCUnicodeString::CountWords(" ")
   counts them. */
static uint32_t text_words(X86 *c, uint32_t id) {
    Saved s;
    const uint32_t base = scratch(c, &s, 0x60);
    const uint32_t loc = base, text = base + 0x10, sep = base + 0x20, space = base + 0x30;
    thiscall1(c, LOC_CTOR, loc, id);
    thiscall1(c, LOC_TEXT, loc, text);
    put_text(space, " ");
    thiscall1(c, USTR_CTOR, sep, space);
    const uint32_t words = thiscall1(c, USTR_WORDS, text, sep);
    thiscall(c, USTR_DTOR, sep, 0, 0);
    thiscall(c, USTR_DTOR, text, 0, 0);
    thiscall(c, LOC_DTOR, loc, 0, 0);
    restore(c, &s);
    return words;
}

static int sub_logged;

void g3n_10a793e0(X86 *c) {
    const uint32_t sp = c->r[R_ESP];
    const uint32_t ret = rd32(sp), arg = rd32(sp + 4), pu = rd32(sp + 8);
    if (!(mods() & MOD_SUBTITLES) || g_mem[sp + 0xc] == 1 || !arg || !rd32(arg) ||
        (ret != RET_SAY && ret != RET_SAY_SOUND)) {
        fn_10a793e0(c);
        return;
    }
    float limit;
    if (sub_mode == 3) {
        limit = __builtin_inff();
    } else {
        static const float per_word[3] = {250.0f, 500.0f, 1000.0f};
        uint32_t words = text_words(c, ret == RET_SAY ? c->r[R_ESI] + 0x160 : arg + 4);
        if ((int32_t)words < 4)
            words = 4;
        limit = (float)(int32_t)words * per_word[sub_mode];
    }
    fn_10a793e0(c);
    uint32_t bits;
    memcpy(&bits, &limit, sizeof bits);
    wr32(pu + 0x9c, bits);
    if (sub_logged < 5) {
        ++sub_logged;
        fprintf(stderr, "g3mods: subtitles: %s line, %.0f ms\n", ret == RET_SAY ? "say" : "say sound",
                (double)limit);
    }
}

/* ------------------------------------------------------------------ G3Fixes -- */

/* Script_G3Fixes.dll (docs/mods/dll-g3fixes.md; hook numbers are its table's). Each g3n_ below runs
   the translated original unchanged unless G3Fixes is in active-mods.json; G3Fixes.ini comes from
   <profile>/ini, which tools/g3mods.py links from the mod. Script entities are Entity wrappers of
   0xa8 bytes built in guest scratch memory and destroyed before the replacement returns. */

/* Script.dll */
#define ENT_NONE 0x10f96d08u          /* Entity::None (data) */
#define ENT_CTOR 0x10f439f0u          /* Entity::Entity(void) */
#define ENT_CTOR_EC 0x10f43bd0u       /* Entity::Entity(eCEntity const *) */
#define ENT_COPY 0x10f43a90u          /* Entity::Entity(Entity const &) */
#define ENT_DTOR 0x10f31390u          /* Entity::~Entity(void) */
#define ENT_ASSIGN 0x10f43e60u        /* Entity::operator=(Entity const &) */
#define ENT_ATTACH 0x10f43e80u        /* Entity::AttachTo(eCEntity *) */
#define ENT_EQ 0x10f43e20u            /* Entity::operator==(Entity const &) const */
#define ENT_NE 0x10f43e40u            /* Entity::operator!=(Entity const &) const */
#define ENT_PLAYER 0x10f442e0u        /* Entity::GetPlayer(void), stdcall, by value */
#define ENT_NAME 0x10f45810u          /* Entity::GetName(void) const, bCString by value */
#define ENT_FREE_LOS 0x10f48e20u      /* Entity::IsFreeLOS(Entity const &, bool) const */
#define ENT_IS_ITEM 0x10f448c0u       /* Entity::IsItem(void) const */
#define ENT_IS_NPC 0x10f448e0u        /* Entity::IsNPC(void) const */
#define ENT_KILL 0x10f4aed0u          /* Entity::Kill(void) */
#define ENT_ENTITIES 0x10f445e0u      /* Entity::GetEntities(void), stdcall, bTObjArray<Entity> by value */
#define ENT_SORT 0x10f44880u          /* Entity::SortEntityListByDistanceTo(bTObjArray<Entity> &, Entity const &), stdcall */
#define ENT_BY_DISTANCE 0x10f44d90u   /* Entity::GetEntitiesByDistance(void) const, by value */
#define ENT_DISTANCE 0x10f45540u      /* Entity::GetDistanceTo(Entity const &) const, float in ST0 */
#define ENT_MOVE_MODE 0x10f493a0u     /* Entity::GetMovementMode(void) const */
#define ENT_SET_MOVE_MODE 0x10f493b0u /* Entity::SetMovementMode(gECharMovementMode) */
#define ENT_SPAWN_POSE 0x10f4a180u    /* Entity::FindSpawnPose(bCMatrix &, Entity const &, bool, unsigned short) const */
#define ENT_MOVE_TO 0x10f45600u       /* Entity::MoveTo(bCMatrix const &) */
#define ENT_TELEPORT 0x10f45640u      /* Entity::Teleport(Entity const &) */
#define ENT_EVADE 0x10f48870u         /* Entity::GetEvadeDestination(Entity const &, gEDirection) const, bCVector by value */
#define ENT_GOTO 0x10f49840u          /* Entity::StartGoto(bCVector const &, gEWalkMode) */
#define NPC_HAS_STATUS 0x10f3b880u    /* PSNpc::HasStatusEffects(int) const */
#define NPC_ENABLE_STATUS 0x10f3b960u /* PSNpc::EnableStatusEffects(int, bool) */
#define NPC_SPECIES 0x10f3a1f0u       /* PSNpc::operator gESpecies */
#define NPC_SET_TARGET 0x10f3b570u    /* PSNpc::SetCurrentTarget(Entity const &) */
#define NPC_TARGET 0x10f3b4b0u        /* PSNpc::GetCurrentTarget(void) const, by value */
#define NPC_COMBAT_STATE 0x10f3ad70u  /* PSNpc::operator CombatState */
#define NPC_ATTACK_REASON 0x10f3a790u /* PSNpc::operator gEAttackReason */
#define NPC_ATTITUDE 0x10f3a510u      /* PSNpc::operator AttitudeToPlayer2 */
#define ROUTINE_FULL_STOP 0x10f43140u /* PSRoutine::FullStop(void) */
#define ROUTINE_SET_TASK 0x10f42f70u  /* PSRoutine::SetTask(bCString), by value */
#define ROUTINE_ACTION 0x10f42af0u    /* PSRoutine::operator gEAction */
#define ROUTINE_LAST_TASK 0x10f42ea0u /* PSRoutine::GetLastTask(void) const, by value */
#define INTER_OWNER 0x10f361d0u       /* PSInteraction::GetOwner(void) const, by value */
#define INTER_USER 0x10f36230u        /* PSInteraction::GetUser(void) const, by value */
#define INTER_SET_USER 0x10f36270u    /* PSInteraction::SetUser(Entity const &) */
#define INTER_USE_TYPE 0x10f35ed0u    /* PSInteraction::operator gEUseType */
#define INTER_USE_FUNC 0x10f35f70u    /* PSInteraction::operator PropertyUseFunc, bCString by value */
#define NAV_VALID 0x10f31330u         /* PSNavigation::IsValid(void) const */
#define NAV_SET_DEST 0x10f39a00u      /* PSNavigation::SetCurrentDestinationPoint(Entity &) */
#define INV_TEMPLATE_ITEM 0x10f36fb0u /* PSInventory::GetTemplateItem(int) const, by value */
#define INV_SKILL_ACTIVE 0x10f370f0u  /* PSInventory::IsSkillActive(Template const &) const */
#define DAMAGE_TYPE 0x10f33870u       /* PSDamage::operator gEDamageType */
#define DAMAGE_MULT 0x10f339f0u       /* PSDamage::operator DamageHitMultiplier, float in ST0 */
#define ITEM_QUALITY 0x10f33510u      /* PSItem::GetQuality(void) const */
#define PROJ_PATH 0x10f41af0u         /* PSProjectile::operator gEProjectilePath */
#define PARTY_LEADER 0x10f3f3e0u      /* PSParty::GetPartyLeader(void) const, by value */
#define PARTY_TYPE 0x10f3f1a0u        /* PSParty::operator gEPartyMemberType */
#define TEMPLATE_CTOR 0x10f4b970u     /* Template::Template(char const *) */
#define TEMPLATE_DTOR 0x10f4b9c0u     /* Template::~Template(void) */
/* Entity's property sets, as the mod reaches them */
#define E_ROUTINE 0x04u
#define E_INTERACTION 0x1cu
#define E_INVENTORY 0x24u
#define E_ITEM 0x28u
#define E_NAVIGATION 0x30u
#define E_NPC 0x34u
#define E_DAMAGE 0x48u
#define E_PARTY 0x58u
#define E_PROJECTILE 0x70u
/* Script_Game: predicates on an Entity by value (RET 0xa8) the mod calls by address */
#define SG_IS_SPELL 0x10fdaeb0u
#define SG_IS_SPELL_PROJECTILE 0x10fdaf20u
#define SG_IS_PROJECTILE 0x10fdaf70u
/* Game */
#define SPU_SELF 0x10a7f090u          /* gCScriptProcessingUnit::GetSelfEntity(void) */
#define SPU_OTHER 0x10a7f0f0u         /* gCScriptProcessingUnit::GetOtherEntity(void) */
#define CALL_SCRIPT 0x10a70350u       /* gCScriptAdmin::CallScriptFromScript(bCString const &, void *, void *, int) */
#define PM_ATTRIBUTE 0x10a5e9a0u      /* gCPlayerMemory_PS::GetAttribute(bCString const &) const */
#define SESSION_GET 0x10a82f80u       /* gCSession::GetSession, stdcall */
#define SESSION_PAUSED 0x10911d60u    /* gCSession::IsPaused(void) const */
#define SESSION_GUI 0x10a84470u       /* gCSession::GetGUIManager(void) const */
#define INFO_MANAGER 0x10adecb0u      /* gCInfoManager_PS::GetInstance, stdcall */
#define GUI_MENU_OPEN 0x1098c2c0u     /* gCGUIManager::IsMenuOpen(void) const */
#define GUI_PAGE_OPEN 0x1098b4c0u     /* gCGUIManager::IsAnyPageOpen(void) const */
/* Engine */
#define SCENE_BY_NAME 0x10493a10u     /* eCSceneAdmin::GetEntityByName(bCString const &) */
#define SCENE_ENTITIES 0x104931e0u    /* eCSceneAdmin::GetAccessToEntityAdmin(void) */
#define ENTITIES_ROI 0x10287e50u      /* eCEntityAdmin::GetROISphere(void) */
#define APP_CONSOLE 0x10275980u       /* eCApplication::GetConsole(void) */
#define WND_ACTIVE 0x103211f0u        /* eCWnd::IsActive(void) const */
#define APP_KEYBOARD 0x10275920u      /* eCApplication::GetKeyboard(void) */
#define KEY_PRESSED 0x1037f750u       /* eCKeyboard::KeyPressed(eEKeyboardStateOffset) const */
/* SharedBase */
#define STR_EMPTY 0x100052f0u         /* bCString::IsEmpty(void) const */
#define STR_COUNT_WORDS 0x10004f60u   /* bCString::CountWords(bCString const &) const */
#define STR_GET_WORD 0x10004c40u      /* bCString::GetWord(int, bCString const &, bCString &, bool, bool) const */
#define STR_COMPARE_FAST 0x10001ae0u  /* bCString::CompareFast(bCString const &) const */
#define STR_EQ_CHARS 0x100053a0u      /* bCString::operator==(char const *) const */
#define SPHERE_RADIUS 0x10035020u     /* bCSphere::GetRadius(void) const, float in ST0 */
#define MATRIX_CTOR 0x1000ff70u       /* bCMatrix::bCMatrix(void) */
#define MATRIX_DTOR 0x1001b3d0u       /* bCMatrix::~bCMatrix, bCVector::~bCVector (one empty body) */
#define MEM_ADMIN 0x1000c800u         /* bCMemoryAdmin::GetInstance, stdcall */
#define MEM_FREE 0x1000c9e0u          /* bCMemoryAdmin::Free(void *) */

static struct {
    uint32_t hotkey;                    /* CompanionAutoDefendHotkey, eEKeyboardStateOffset (a DIK code) */
    int teleport_far, block_respawn, herd_unity;
    int32_t quick_freeze, quick_burn, melee_freeze, melee_freeze_power, melee_burn, melee_burn_power;
    int skills_mode, equipment_mode;    /* 0 Disabled, 1 DontAddEquipmentBonus, 2 AddEquipmentBonus */
    int32_t companion_size, transform_size;                   /* [CompanionIcon], [TransformationIcon]: pixels */
    float companion_x, companion_y, transform_x, transform_y; /* and percent of the window */
} g3f_ini = {0x28, 1, 0, 0, 10, 10, 30, 35, 30, 30, 0, 0, 16, 24, 98.5f, 2.5f, 0.5f, 6.0f}; /* the mod's .data without an ini */

static int g3f_defend;       /* 0x1006eece: companions and guides attack whoever attacks the party */
static int g3f_key_held;     /* 0x1006eed9 */

static int g3f(void) {
    return (mods() & MOD_G3FIXES) != 0;
}

/* One line per hook the first time it acts: an indirect call through a script table reached it. */
static void g3f_first(int hook, const char *what) {
    static uint32_t seen;
    if (seen & (1u << hook))
        return;
    seen |= 1u << hook;
    fprintf(stderr, "g3mods: g3fixes: hook %d (%s) first call\n", hook, what);
}

static const struct {
    const char *name;
    uint8_t dik;
} g3f_keys[] = {
    {"ESCAPE", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07},
    {"7", 0x08}, {"8", 0x09}, {"9", 0x0a}, {"0", 0x0b}, {"MINUS", 0x0c}, {"EQUALS", 0x0d},
    {"BACK", 0x0e}, {"TAB", 0x0f}, {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14},
    {"Y", 0x15}, {"U", 0x16}, {"I", 0x17}, {"O", 0x18}, {"P", 0x19}, {"LBRACKET", 0x1a},
    {"RBRACKET", 0x1b}, {"RETURN", 0x1c}, {"LCONTROL", 0x1d}, {"A", 0x1e}, {"S", 0x1f}, {"D", 0x20},
    {"F", 0x21}, {"G", 0x22}, {"H", 0x23}, {"J", 0x24}, {"K", 0x25}, {"L", 0x26}, {"SEMICOLON", 0x27},
    {"APOSTROPHE", 0x28}, {"GRAVE", 0x29}, {"LSHIFT", 0x2a}, {"BACKSLASH", 0x2b}, {"Z", 0x2c},
    {"X", 0x2d}, {"C", 0x2e}, {"V", 0x2f}, {"B", 0x30}, {"N", 0x31}, {"M", 0x32}, {"COMMA", 0x33},
    {"PERIOD", 0x34}, {"SLASH", 0x35}, {"RSHIFT", 0x36}, {"MULTIPLY", 0x37}, {"LMENU", 0x38},
    {"SPACE", 0x39}, {"CAPITAL", 0x3a}, {"F1", 0x3b}, {"F2", 0x3c}, {"F3", 0x3d}, {"F4", 0x3e},
    {"F5", 0x3f}, {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44},
    {"NUMLOCK", 0x45}, {"SCROLL", 0x46}, {"NUMPAD7", 0x47}, {"NUMPAD8", 0x48}, {"NUMPAD9", 0x49},
    {"SUBTRACT", 0x4a}, {"NUMPAD4", 0x4b}, {"NUMPAD5", 0x4c}, {"NUMPAD6", 0x4d}, {"ADD", 0x4e},
    {"NUMPAD1", 0x4f}, {"NUMPAD2", 0x50}, {"NUMPAD3", 0x51}, {"NUMPAD0", 0x52}, {"DECIMAL", 0x53},
    {"F11", 0x57}, {"F12", 0x58}, {"RCONTROL", 0x9d}, {"DIVIDE", 0xb5}, {"RMENU", 0xb8},
    {"HOME", 0xc7}, {"UP", 0xc8}, {"PRIOR", 0xc9}, {"LEFT", 0xcb}, {"RIGHT", 0xcd}, {"END", 0xcf},
    {"DOWN", 0xd0}, {"NEXT", 0xd1}, {"INSERT", 0xd2}, {"DELETE", 0xd3},
};

/* The value after `key=` at the start of a line of `text`, up to the line's end, or 0. */
static const char *ini_value(const char *text, const char *key, char *out, size_t cap) {
    const size_t n = strlen(key);
    for (const char *p = text; p && (p = strstr(p, key)) != 0; p += n) {
        if ((p != text && p[-1] != '\n') || p[n] != '=')
            continue;
        const char *v = p + n + 1;
        size_t i = 0;
        while (v[i] && v[i] != '\r' && v[i] != '\n' && v[i] != ' ' && v[i] != ';' && i + 1 < cap)
            out[i] = v[i], ++i;
        out[i] = 0;
        return out;
    }
    return 0;
}

static void ini_bool(const char *text, const char *key, int *dst) {
    char v[64];
    if (ini_value(text, key, v, sizeof v))
        *dst = !strcasecmp(v, "true") || !strcmp(v, "1");
}

static void ini_int(const char *text, const char *key, int32_t *dst) {
    char v[64];
    if (ini_value(text, key, v, sizeof v))
        *dst = (int32_t)strtol(v, 0, 10);
}

static void ini_float(const char *text, const char *key, float *dst) {
    char v[64];
    if (ini_value(text, key, v, sizeof v))
        *dst = strtof(v, 0);
}

static void ini_mode(const char *text, const char *key, int *dst) {
    static const char *const modes[] = {"Disabled", "DontAddEquipmentBonus", "AddEquipmentBonus"};
    char v[64];
    if (ini_value(text, key, v, sizeof v))
        for (int i = 0; i < 3; ++i)
            if (!strcasecmp(v, modes[i]))
                *dst = i;
}

static void g3f_read_ini(const char *profile) {
    char path[1100];
    snprintf(path, sizeof path, "%s/ini/G3Fixes.ini", profile);
    char *text = read_file(path, 1 << 16);
    if (text) {
        char v[64];
        if (ini_value(text, "CompanionAutoDefendHotkey", v, sizeof v))
            for (size_t i = 0; i < sizeof g3f_keys / sizeof g3f_keys[0]; ++i)
                if (!strcasecmp(v, g3f_keys[i].name))
                    g3f_ini.hotkey = g3f_keys[i].dik;
        ini_bool(text, "TeleportCompanionTooFarAway", &g3f_ini.teleport_far);
        ini_bool(text, "BlockMonsterRespawn", &g3f_ini.block_respawn);
        ini_bool(text, "HerdUnity", &g3f_ini.herd_unity);
        ini_int(text, "QuickCastFreezeChancePercent", &g3f_ini.quick_freeze);
        ini_int(text, "QuickCastBurnChancePercent", &g3f_ini.quick_burn);
        ini_int(text, "MeleeFreezeChancePercent", &g3f_ini.melee_freeze);
        ini_int(text, "MeleeFreezeChancePercentPowerAttack", &g3f_ini.melee_freeze_power);
        ini_int(text, "MeleeBurnChancePercent", &g3f_ini.melee_burn);
        ini_int(text, "MeleeBurnChancePercentPowerAttack", &g3f_ini.melee_burn_power);
        ini_mode(text, "AttributeRequirementFixModeSkills", &g3f_ini.skills_mode);
        ini_mode(text, "AttributeRequirementFixModeEquipment", &g3f_ini.equipment_mode);
        ini_int(text, "CompanionIconSize", &g3f_ini.companion_size);
        ini_float(text, "CompanionIconPosTopX", &g3f_ini.companion_x);
        ini_float(text, "CompanionIconPosTopY", &g3f_ini.companion_y);
        ini_int(text, "TransformationIconSize", &g3f_ini.transform_size);
        ini_float(text, "TransformationIconPosTopX", &g3f_ini.transform_x);
        ini_float(text, "TransformationIconPosTopY", &g3f_ini.transform_y);
        free(text);
    }
    fprintf(stderr,
            "g3mods: g3fixes from %s%s: hotkey %#x, teleport %d, block respawn %d, herd %d, chances %d/%d/%d/%d/%d/%d, "
            "skills %d, equipment %d\n",
            path, text ? "" : " (missing, the mod's defaults)", g3f_ini.hotkey, g3f_ini.teleport_far,
            g3f_ini.block_respawn, g3f_ini.herd_unity, g3f_ini.melee_freeze, g3f_ini.melee_freeze_power,
            g3f_ini.melee_burn, g3f_ini.melee_burn_power, g3f_ini.quick_freeze, g3f_ini.quick_burn,
            g3f_ini.skills_mode, g3f_ini.equipment_mode);
}

/* The mod's random numbers: std::mt19937 with its default seed (0x1006bc60) and 0..100 inclusive
   by Lemire's multiply and reject (0x100018d0), so a run draws what the mod draws. */
static uint32_t mt[624];
static int mt_index = -1;

static uint32_t mt_next(void) {
    if (mt_index < 0) {
        mt[0] = 5489u;
        for (int i = 1; i < 624; ++i)
            mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + (uint32_t)i;
        mt_index = 624;
    }
    if (mt_index >= 624) {
        for (int i = 0; i < 624; ++i) {
            const uint32_t y = (mt[i] & 0x80000000u) | (mt[(i + 1) % 624] & 0x7fffffffu);
            mt[i] = mt[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        mt_index = 0;
    }
    uint32_t y = mt[mt_index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return y ^ (y >> 18);
}

static int32_t g3f_rand101(void) {
    uint64_t m = (uint64_t)mt_next() * 101u;
    if ((uint32_t)m < 101u)
        while ((uint32_t)m < 68u) /* 2^32 mod 101 */
            m = (uint64_t)mt_next() * 101u;
    return (int32_t)(m >> 32);
}

/* Guest scratch memory for one replacement, handed out from the bottom up below the caller's ESP.
   Guest calls made meanwhile push below it. */
typedef struct {
    Saved s;
    uint32_t at, end;
} Frame;

static void frame_open(X86 *c, Frame *f, uint32_t size) {
    f->at = scratch(c, &f->s, size);
    f->end = f->at + size;
}

static uint32_t frame_alloc(Frame *f, uint32_t size) {
    const uint32_t a = f->at;
    f->at += (size + 15u) & ~15u;
    if (f->at > f->end) {
        fprintf(stderr, "g3mods: g3fixes: scratch frame too small\n");
        abort();
    }
    return a;
}

static uint32_t frame_text(Frame *f, const char *s) {
    const uint32_t a = frame_alloc(f, (uint32_t)strlen(s) + 1);
    put_text(a, s);
    return a;
}

static uint32_t call0(X86 *c, uint32_t fn, uint32_t self) {
    return thiscall(c, fn, self, 0, 0);
}

static uint32_t call2(X86 *c, uint32_t fn, uint32_t self, uint32_t a0, uint32_t a1) {
    const uint32_t a[2] = {a0, a1};
    return thiscall(c, fn, self, a, 2);
}

static int test0(X86 *c, uint32_t fn, uint32_t self) {
    return (call0(c, fn, self) & 0xffu) != 0;
}

static int test1(X86 *c, uint32_t fn, uint32_t self, uint32_t a0) {
    return (thiscall1(c, fn, self, a0) & 0xffu) != 0;
}

/* A float a guest function returns in ST0. */
static float st0_float(X86 *c) {
    return (float)fpop(c);
}

static uint32_t str_new(X86 *c, Frame *f, const char *text) {
    const uint32_t s = frame_alloc(f, 4);
    thiscall1(c, STR_CTOR, s, frame_text(f, text));
    return s;
}

static void str_free(X86 *c, uint32_t s) {
    call0(c, STR_DTOR, s);
}

static uint32_t ent_new(X86 *c, Frame *f) {
    const uint32_t e = frame_alloc(f, 0xa8);
    call0(c, ENT_CTOR, e);
    return e;
}

static uint32_t ent_of(X86 *c, Frame *f, uint32_t ec) {
    const uint32_t e = frame_alloc(f, 0xa8);
    thiscall1(c, ENT_CTOR_EC, e, ec);
    return e;
}

static uint32_t ent_copy(X86 *c, Frame *f, uint32_t from) {
    const uint32_t e = frame_alloc(f, 0xa8);
    thiscall1(c, ENT_COPY, e, from);
    return e;
}

/* An Entity a method of `self` returns by value. */
static uint32_t ent_from(X86 *c, Frame *f, uint32_t fn, uint32_t self) {
    const uint32_t e = frame_alloc(f, 0xa8);
    thiscall1(c, fn, self, e);
    return e;
}

static uint32_t ent_player(X86 *c, Frame *f) {
    const uint32_t e = frame_alloc(f, 0xa8);
    guest_call(c, ENT_PLAYER, &e, 1);
    return e;
}

static void ent_free(X86 *c, uint32_t e) {
    call0(c, ENT_DTOR, e);
}

static int ent_eq(X86 *c, uint32_t a, uint32_t b) {
    return test1(c, ENT_EQ, a, b);
}

/* A script's self or other: the argument when given, else the processing unit's (mod 0x10004370). */
static uint32_t script_entity(X86 *c, Frame *f, uint32_t spu, uint32_t given, uint32_t getter) {
    const uint32_t e = ent_new(c, f);
    if (given)
        thiscall1(c, ENT_ASSIGN, e, given);
    else
        thiscall1(c, ENT_ATTACH, e, call0(c, getter, spu));
    return e;
}

/* A Script_Game predicate on an Entity by value: the copy goes where the callee's argument is,
   and the callee destroys it. */
static int ent_pred(X86 *c, uint32_t fn, uint32_t e) {
    const uint32_t esp = c->r[R_ESP];
    c->r[R_ESP] = esp - 0xa8u;
    thiscall1(c, ENT_COPY, c->r[R_ESP], e);
    const int r = (guest_call(c, fn, 0, 0) & 0xffu) != 0;
    c->r[R_ESP] = esp;
    return r;
}

/* A method taking one bCString by value (the callee destroys it). */
static void call_str_arg(X86 *c, Frame *f, uint32_t fn, uint32_t self, const char *text) {
    const uint32_t chars = frame_text(f, text);
    const uint32_t esp = c->r[R_ESP];
    c->r[R_ESP] = esp - 4u;
    thiscall1(c, STR_CTOR, c->r[R_ESP], chars);
    c->r[R_ECX] = self;
    guest_call(c, fn, 0, 0);
    c->r[R_ESP] = esp;
}

static uint32_t find_module(X86 *c, Frame *f, const char *name) {
    const uint32_t s = str_new(c, f, name);
    const uint32_t found = thiscall1(c, MODULES_FIND, guest_call(c, MODULES_GET, 0, 0), s);
    str_free(c, s);
    return found;
}

/* gCScriptAdmin::CallScriptFromScript(name, self, other, arg), as the mod calls scripts by name;
   `name` a guest bCString. */
static uint32_t script_named(X86 *c, Frame *f, uint32_t name, uint32_t self, uint32_t other, uint32_t arg) {
    static uint32_t admin;
    if (!admin)
        admin = find_module(c, f, "gCScriptAdmin");
    if (!admin)
        return 0;
    const uint32_t a[4] = {name, self, other, arg};
    return thiscall(c, CALL_SCRIPT, admin, a, 4);
}

static uint32_t script(X86 *c, Frame *f, const char *name, uint32_t self, uint32_t other, uint32_t arg) {
    const uint32_t s = str_new(c, f, name);
    const uint32_t r = script_named(c, f, s, self, other, arg);
    str_free(c, s);
    return r;
}

/* Who dealt the damage: the damager's owner, or the damager itself when it has none and can move
   (not in the original game). */
static uint32_t damage_owner(X86 *c, Frame *f, uint32_t damager) {
    const uint32_t owner = ent_from(c, f, INTER_OWNER, damager + E_INTERACTION);
    if (ent_eq(c, owner, ENT_NONE) && test0(c, NAV_VALID, damager + E_NAVIGATION))
        thiscall1(c, ENT_ASSIGN, owner, damager);
    return owner;
}

/* GetAttitude either way round gives Hostile (4) or Panic (6). */
static int hostile_either(X86 *c, Frame *f, uint32_t self, uint32_t owner) {
    uint32_t a = script(c, f, "GetAttitude", self, owner, 0);
    if (a == 4 || script(c, f, "GetAttitude", self, owner, 0) == 6)
        return 1;
    a = script(c, f, "GetAttitude", owner, self, 0);
    return a == 4 || script(c, f, "GetAttitude", owner, self, 0) == 6;
}

/* A replacement's return: EAX, then RET with `pop` bytes of arguments. */
static void g3f_return(X86 *c, uint32_t eax, uint32_t pop) {
    c->r[R_EAX] = eax;
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4u + pop;
    recomp_return(c);
}

/* 1: Entity::EndTransform. A hero who comes back from a transformation while the Magic Mist
   status is on and the mist entity is gone loses the status (else he stays unseen). */
void g3n_10f4b230(X86 *c) {
    fn_10f4b230(c);
    if (!g3f())
        return;
    g3f_first(1, "EndTransform");
    Frame f;
    frame_open(c, &f, 0x400);
    const uint32_t player = ent_player(c, &f);
    if (test1(c, NPC_HAS_STATUS, player + E_NPC, 0x40)) {
        const uint32_t scene = find_module(c, &f, "eCSceneAdmin");
        if (scene) {
            const uint32_t name = str_new(c, &f, "Smn_Mist");
            const uint32_t mist = thiscall1(c, SCENE_BY_NAME, scene, name);
            str_free(c, name);
            if (!mist) {
                call2(c, NPC_ENABLE_STATUS, player + E_NPC, 0x40, 0);
                fprintf(stderr, "g3mods: g3fixes: no Smn_Mist, Magic Mist status off\n");
            }
        }
    }
    ent_free(c, player);
    restore(c, &f.s);
}

/* 2: _AI_UseInventoryItem. When the hero uses an item (break block 7), each ';'-separated word of
   its UseFunc runs as a script (item, hero) before the original: the old ItemUseFuncEnabler. */
void g3n_10ffcdd0(X86 *c) {
    if (!g3f()) {
        fn_10ffcdd0(c);
        return;
    }
    g3f_first(2, "_AI_UseInventoryItem");
    const uint32_t sp = c->r[R_ESP], stack = rd32(sp + 4), spu = rd32(sp + 8);
    Frame f;
    frame_open(c, &f, 0x600);
    const uint32_t player = ent_player(c, &f);
    const uint32_t self = ent_of(c, &f, call0(c, SPU_SELF, spu));
    const uint32_t n = rd32(stack + 4);
    const uint32_t top = n ? rd32(stack) + n * 0x18u - 0x18u : 0;
    if (top && rd32(top) == 7 && ent_eq(c, self, player)) {
        const uint32_t args = rd32(top + 0xc);
        const uint32_t item = frame_alloc(&f, 0xa8);
        call2(c, INV_TEMPLATE_ITEM, args + 0x28, item, rd32(args + 0xac));
        const uint32_t use = frame_alloc(&f, 4);
        thiscall1(c, INTER_USE_FUNC, item + E_INTERACTION, use);
        if (!test0(c, STR_EMPTY, use)) {
            const uint32_t sep = str_new(c, &f, ";");
            const uint32_t word = frame_alloc(&f, 4);
            for (int32_t i = 0; i < (int32_t)thiscall1(c, STR_COUNT_WORDS, use, sep); ++i) {
                call0(c, STR_DEFAULT, word);
                const uint32_t a[5] = {(uint32_t)i, sep, word, 1, 1};
                thiscall(c, STR_GET_WORD, use, a, 5);
                if (!test0(c, STR_EMPTY, word))
                    script_named(c, &f, word, item, self, 0);
                str_free(c, word);
            }
            str_free(c, sep);
        }
        str_free(c, use);
        ent_free(c, item);
    }
    ent_free(c, self);
    ent_free(c, player);
    restore(c, &f.s);
    fn_10ffcdd0(c);
}

/* 3: IsDeadlyDamage, the mod's whole version (1 kills, 0 knocks out). */
void g3n_10fddb80(X86 *c) {
    if (!g3f()) {
        fn_10fddb80(c);
        return;
    }
    g3f_first(3, "IsDeadlyDamage");
    const uint32_t sp = c->r[R_ESP], spu = rd32(sp + 4);
    Frame f;
    frame_open(c, &f, 0x800);
    const uint32_t self = script_entity(c, &f, spu, rd32(sp + 8), SPU_SELF);
    const uint32_t damager = script_entity(c, &f, spu, rd32(sp + 0xc), SPU_OTHER);
    const uint32_t owner = damage_owner(c, &f, damager);
    uint32_t deadly;
    if (ent_eq(c, self, ENT_NONE) || ent_eq(c, owner, ENT_NONE) || !script(c, &f, "CanBeKilled", self, ENT_NONE, 0))
        deadly = 0;
    else if (script(c, &f, "IsBoss", self, ENT_NONE, 0))
        deadly = 1;
    else {
        const uint32_t name = frame_alloc(&f, 4);
        thiscall1(c, ENT_NAME, self, name);
        const int xardas = test1(c, STR_EQ_CHARS, name, frame_text(&f, "Xardas"));
        str_free(c, name);
        if (xardas || ent_pred(c, SG_IS_SPELL, damager))
            deadly = 1;
        else if (ent_pred(c, SG_IS_PROJECTILE, damager))
            deadly = call0(c, DAMAGE_TYPE, damager + E_DAMAGE) == 1 && script(c, &f, "IsHumanoid", self, ENT_NONE, 0)
                         ? (uint32_t)hostile_either(c, &f, self, owner)
                         : 1;
        else
            deadly = call0(c, ROUTINE_ACTION, owner + E_ROUTINE) != 15 && script(c, &f, "IsHumanoid", owner, ENT_NONE, 0) &&
                             script(c, &f, "IsHumanoid", self, ENT_NONE, 0)
                         ? (uint32_t)hostile_either(c, &f, self, owner)
                         : 1;
    }
    ent_free(c, owner);
    ent_free(c, damager);
    ent_free(c, self);
    restore(c, &f.s);
    g3f_return(c, deadly, 0x10);
}

/* 4 and 5: CanBurn and CanFreeze, the mod's whole version: a chance from G3Fixes.ini (melee, quick
   cast), 100 for spells without a projectile and for missiles, DamageHitMultiplier * 150 for
   arrows; the perk of resistance takes a third off; rolled 0..100. */
static uint32_t can_status(X86 *c, int burn) {
    const uint32_t sp = c->r[R_ESP], spu = rd32(sp + 4);
    const int32_t damage = (int32_t)rd32(sp + 0x10);
    Frame f;
    frame_open(c, &f, 0x800);
    const uint32_t self = script_entity(c, &f, spu, rd32(sp + 8), SPU_SELF);
    const uint32_t damager = script_entity(c, &f, spu, rd32(sp + 0xc), SPU_OTHER);
    const uint32_t player = ent_player(c, &f);
    uint32_t result = 0;
    const uint32_t species = call0(c, NPC_SPECIES, self + E_NPC);
    const int immune = species == 3 || species == 4 || species == 7 || species == 48 || species == 49 || species == 52;
    if (!immune && (int32_t)(script(c, &f, "GetHitPoints", self, ENT_NONE, 0) - (uint32_t)damage) > 0) {
        const uint32_t type = call0(c, DAMAGE_TYPE, damager + E_DAMAGE);
        const int by_item = test0(c, ENT_IS_ITEM, damager) &&
                            (call0(c, ITEM_QUALITY, damager + E_ITEM) & (burn ? 0x08u : 0x10u));
        if (by_item || type == (burn ? 4u : 5u)) {
            int32_t chance;
            if (ent_pred(c, SG_IS_SPELL, damager)) {
                if (!ent_pred(c, SG_IS_SPELL_PROJECTILE, damager))
                    chance = 100;
                else if (ent_pred(c, SG_IS_SPELL_PROJECTILE, damager) && call0(c, PROJ_PATH, damager + E_PROJECTILE) == 3)
                    chance = 100;
                else
                    chance = burn ? g3f_ini.quick_burn : g3f_ini.quick_freeze;
            } else if (ent_pred(c, SG_IS_PROJECTILE, damager)) {
                call0(c, DAMAGE_MULT, damager + E_DAMAGE);
                chance = (int32_t)(float)((double)st0_float(c) * 150.0); /* FMUL float, FSTP float, CVTTSS2SI */
            } else {
                const uint32_t owner = damage_owner(c, &f, damager);
                const int power = call0(c, ROUTINE_ACTION, owner + E_ROUTINE) == 2;
                chance = burn ? (power ? g3f_ini.melee_burn_power : g3f_ini.melee_burn)
                              : (power ? g3f_ini.melee_freeze_power : g3f_ini.melee_freeze);
                ent_free(c, owner);
            }
            if (ent_eq(c, self, player)) {
                const uint32_t perk = frame_alloc(&f, 0x14);
                thiscall1(c, TEMPLATE_CTOR, perk, frame_text(&f, burn ? "Perk_ResistHeat" : "Perk_ResistCold"));
                if (test1(c, INV_SKILL_ACTIVE, player + E_INVENTORY, perk))
                    chance = (int32_t)((double)chance * 0.66);
                call0(c, TEMPLATE_DTOR, perk);
            }
            result = g3f_rand101() < chance;
        }
    }
    ent_free(c, player);
    ent_free(c, damager);
    ent_free(c, self);
    restore(c, &f.s);
    return result;
}

void g3n_10fdd530(X86 *c) {
    if (!g3f()) {
        fn_10fdd530(c);
        return;
    }
    g3f_first(4, "CanBurn");
    g3f_return(c, can_status(c, 1), 0x10);
}

void g3n_10fdd7b0(X86 *c) {
    if (!g3f()) {
        fn_10fdd7b0(c);
        return;
    }
    g3f_first(5, "CanFreeze");
    g3f_return(c, can_status(c, 0), 0x10);
}

/* 6: Respawn. With BlockMonsterRespawn, the monster that just came back dies again. */
void g3n_1105c3c0(X86 *c) {
    const uint32_t sp = c->r[R_ESP], spu = rd32(sp + 4), self_arg = rd32(sp + 8);
    fn_1105c3c0(c);
    if (!g3f() || !g3f_ini.block_respawn || !c->r[R_EAX])
        return;
    g3f_first(6, "Respawn");
    Frame f;
    frame_open(c, &f, 0x200);
    const uint32_t self = script_entity(c, &f, spu, self_arg, SPU_SELF);
    call0(c, ENT_KILL, self);
    ent_free(c, self);
    restore(c, &f.s);
}

/* 7: ZS_ObserveSuspect. A NPC that starts watching the hero leaves the anvil, bed, chest ... it
   uses within 2000 and steps away from it. */
void g3n_10fce570(X86 *c) {
    const uint32_t spu = rd32(c->r[R_ESP] + 8);
    fn_10fce570(c);
    if (!g3f())
        return;
    g3f_first(7, "ZS_ObserveSuspect");
    Frame f;
    frame_open(c, &f, 0x800);
    const uint32_t self = ent_of(c, &f, call0(c, SPU_SELF, spu));
    const uint32_t last = frame_alloc(&f, 4);
    thiscall1(c, ROUTINE_LAST_TASK, self + E_ROUTINE, last);
    const uint32_t observe = str_new(c, &f, "ZS_ObserveSuspect");
    const int again = test1(c, STR_COMPARE_FAST, last, observe);
    str_free(c, observe);
    if (!again) {
        const uint32_t list = frame_alloc(&f, 12);
        thiscall1(c, ENT_BY_DISTANCE, self, list);
        const uint32_t items = rd32(list), count = rd32(list + 4);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t e = ent_copy(c, &f, items + i * 0xa8u);
            thiscall1(c, ENT_DISTANCE, self, e);
            const float d = st0_float(c);
            if ((int32_t)d <= 2000) {
                const uint32_t user = ent_from(c, &f, INTER_USER, e + E_INTERACTION);
                const int ours = !test1(c, ENT_NE, user, self);
                ent_free(c, user);
                const uint32_t use = ours ? call0(c, INTER_USE_TYPE, e + E_INTERACTION) : 0;
                if (use == 57 || use == 59 || use == 60 || use == 62 || use == 64 || use == 67 || use == 74 ||
                    use == 106 || use == 107) {
                    thiscall1(c, INTER_SET_USER, e + E_INTERACTION, ENT_NONE);
                    thiscall1(c, NAV_SET_DEST, self + E_NAVIGATION, ENT_NONE);
                    const uint32_t to = frame_alloc(&f, 16);
                    const uint32_t a[3] = {to, e, 2};
                    thiscall(c, ENT_EVADE, self, a, 3);
                    call2(c, ENT_GOTO, self, to, 1);
                    call0(c, MATRIX_DTOR, to);
                }
            }
            ent_free(c, e);
            f.at = e; /* each pass reuses the same scratch */
        }
        if (items) {
            for (uint32_t i = 0, n = rd32(list + 8); i < n; ++i)
                ent_free(c, items + i * 0xa8u);
            thiscall1(c, MEM_FREE, guest_call(c, MEM_ADMIN, 0, 0), items);
        }
    }
    str_free(c, last);
    ent_free(c, self);
    restore(c, &f.s);
}

/* Companions and guides in defence mode (the hotkey): the first NPC within 2000 of the hero, in
   combat, in sight of `self`, whose target is the hero, `self` or one of the hero's party, and who
   is not fighting in the arena or a duel, gets AssessTarget(self, it, PlayerCommand). */
static void g3f_defend_party(X86 *c, Frame *f, uint32_t self, uint32_t player) {
    const uint32_t list = frame_alloc(f, 12);
    guest_call(c, ENT_ENTITIES, &list, 1);
    const uint32_t sort[2] = {list, player};
    guest_call(c, ENT_SORT, sort, 2);
    const uint32_t mark = f->at;
    for (uint32_t i = 0; i < rd32(list + 4); ++i, f->at = mark) {
        const uint32_t e = ent_copy(c, f, rd32(list) + i * 0xa8u);
        int stop = 0;
        if (!ent_eq(c, e, player) && !ent_eq(c, e, self)) {
            const uint32_t leader = ent_from(c, f, PARTY_LEADER, e + E_PARTY);
            const int party = ent_eq(c, leader, player);
            ent_free(c, leader);
            if (!party) {
                thiscall1(c, ENT_DISTANCE, e, player);
                if ((int32_t)st0_float(c) > 2000)
                    stop = 1;
                else if (test0(c, ENT_IS_NPC, e) && call0(c, NPC_COMBAT_STATE, e + E_NPC) == 1 &&
                         (call2(c, ENT_FREE_LOS, self, e, 1) & 0xffu)) {
                    const uint32_t t = ent_from(c, f, NPC_TARGET, e + E_NPC);
                    int ours = ent_eq(c, t, player) || ent_eq(c, t, self);
                    if (!ours) {
                        const uint32_t tl = ent_from(c, f, PARTY_LEADER, t + E_PARTY);
                        ours = ent_eq(c, tl, player);
                        ent_free(c, tl);
                    }
                    ent_free(c, t);
                    const uint32_t reason = ours ? call0(c, NPC_ATTACK_REASON, e + E_NPC) : 0;
                    if (reason != 0 && reason != 17 && reason != 18) {
                        script(c, f, "AssessTarget", self, e, 16);
                        stop = 1;
                    }
                }
            }
        }
        ent_free(c, e);
        if (stop)
            break;
    }
    const uint32_t items = rd32(list);
    if (items) {
        for (uint32_t i = 0, n = rd32(list + 8); i < n; ++i)
            ent_free(c, items + i * 0xa8u);
        thiscall1(c, MEM_FREE, guest_call(c, MEM_ADMIN, 0, 0), items);
    }
}

/* 8: OnFollowPlayer. Companions sprint where they would walk or run, come back to the hero when
   they fall behind the ROI (TeleportCompanionTooFarAway), and defend in defence mode. */
void g3n_10fc2f50(X86 *c) {
    const uint32_t spu = rd32(c->r[R_ESP] + 4);
    fn_10fc2f50(c);
    if (!g3f() || !(c->r[R_EAX] & 0xffu))
        return;
    g3f_first(8, "OnFollowPlayer");
    Frame f;
    frame_open(c, &f, 0x1000);
    const uint32_t self = ent_of(c, &f, call0(c, SPU_SELF, spu));
    const uint32_t player = ent_player(c, &f);
    const uint32_t mode = call0(c, ENT_MOVE_MODE, self);
    if (mode == 3 || mode == 4)
        thiscall1(c, ENT_SET_MOVE_MODE, self, 5);
    if (g3f_ini.teleport_far) {
        const uint32_t scene = find_module(c, &f, "eCSceneAdmin");
        if (scene) {
            call0(c, SPHERE_RADIUS, call0(c, ENTITIES_ROI, call0(c, SCENE_ENTITIES, scene)));
            const float limit = (float)((double)st0_float(c) - 250.0); /* FSUB float, FSTP float */
            thiscall1(c, ENT_DISTANCE, self, player);
            const float d = st0_float(c);
            if (d >= limit) {
                const uint32_t pose = frame_alloc(&f, 0x40);
                call0(c, MATRIX_CTOR, pose);
                const uint32_t a[4] = {pose, player, 0, 1};
                if (thiscall(c, ENT_SPAWN_POSE, self, a, 4) & 0xffu)
                    thiscall1(c, ENT_MOVE_TO, self, pose);
                else
                    thiscall1(c, ENT_TELEPORT, self, player);
                call0(c, MATRIX_DTOR, pose);
            }
        }
    }
    if (g3f_defend)
        g3f_defend_party(c, &f, self, player);
    ent_free(c, player);
    ent_free(c, self);
    restore(c, &f.s);
}

/* 9: OnGuidePlayer. Guides keep the hero's pace (sprint and walk as he does, else run) and defend
   in defence mode. */
void g3n_10fc3d60(X86 *c) {
    const uint32_t spu = rd32(c->r[R_ESP] + 4);
    fn_10fc3d60(c);
    if (!g3f() || !(c->r[R_EAX] & 0xffu))
        return;
    g3f_first(9, "OnGuidePlayer");
    Frame f;
    frame_open(c, &f, 0x1000);
    const uint32_t self = ent_of(c, &f, call0(c, SPU_SELF, spu));
    const uint32_t player = ent_player(c, &f);
    const uint32_t mode = call0(c, ENT_MOVE_MODE, player);
    thiscall1(c, ENT_SET_MOVE_MODE, self, mode == 5 || mode == 3 ? mode : 4);
    if (g3f_defend)
        g3f_defend_party(c, &f, self, player);
    ent_free(c, player);
    ent_free(c, self);
    restore(c, &f.s);
}

/* 12 (native_sites, Script_Game 10fd9ab6): in the surroundings check FUN_10fd95f0, a creature that
   is not humanoid threatens the enemy only with a free line of sight; without one it goes on as
   after SetTask. ESP+0x24 is self, ESP+0x560 the copy of the enemy 110c9020. */
uint32_t native_site_10fd9ab6(X86 *c) {
    if (!g3f())
        return 0;
    g3f_first(12, "threaten line of sight");
    const uint32_t sp = c->r[R_ESP];
    Saved s;
    scratch(c, &s, 0x40);
    const int los = (call2(c, ENT_FREE_LOS, sp + 0x24, sp + 0x560, 0) & 0xffu) != 0;
    restore(c, &s);
    return los ? 0 : 0x10fd9aeau;
}

/* 13-15 (native_sites at CALL AssessTarget in FUN_10fd95f0, stdcall, 4 arguments): no call
   without a free line of sight, none against the hero's party from inside it, and against his
   controlled or summoned members only by an angry human or orc. A skipped call pops its
   arguments and goes on after the CALL. */
static uint32_t assess_site(X86 *c, uint32_t after) {
    if (!g3f())
        return 0;
    g3f_first(13, "AssessTarget in the surroundings check");
    const uint32_t sp = c->r[R_ESP];
    const uint32_t spu = rd32(sp), self = rd32(sp + 4), target = rd32(sp + 8), reason = rd32(sp + 12);
    int call = 0;
    if (spu && self && target) {
        Frame f;
        frame_open(c, &f, 0x400);
        call = (call2(c, ENT_FREE_LOS, self, target, 0) & 0xffu) != 0;
        if (call && reason == 0xc) {
            const uint32_t player = ent_player(c, &f);
            const uint32_t self_leader = ent_from(c, &f, PARTY_LEADER, self + E_PARTY);
            const uint32_t target_leader = ent_from(c, &f, PARTY_LEADER, target + E_PARTY);
            if (!ent_eq(c, target_leader, ENT_NONE) && !test1(c, ENT_NE, target_leader, player)) {
                if (ent_eq(c, self_leader, target_leader))
                    call = 0;
                else {
                    const uint32_t type = call0(c, PARTY_TYPE, target + E_PARTY);
                    if (type == 4 || type == 5) {
                        const uint32_t attitude = call0(c, NPC_ATTITUDE, self + E_NPC);
                        const uint32_t species = call0(c, NPC_SPECIES, self + E_NPC);
                        call = attitude == 3 && (species == 0 || species == 5);
                    }
                }
            }
            ent_free(c, target_leader);
            ent_free(c, self_leader);
            ent_free(c, player);
        }
        restore(c, &f.s);
    }
    if (call)
        return 0;
    c->r[R_ESP] += 16u;
    return after;
}

uint32_t native_site_10fd990d(X86 *c) {
    return assess_site(c, 0x10fd9912u);
}

uint32_t native_site_10fd99b8(X86 *c) {
    return assess_site(c, 0x10fd99bdu);
}

uint32_t native_site_10fd9a35(X86 *c) {
    return assess_site(c, 0x10fd9a3au);
}

#define ENT_IS_DEAD 0x10f49210u       /* Entity::IsDead(void) const */
#define ENT_IS_DOWN 0x10f49250u       /* Entity::IsDown(void) const */
#define ENT_NPCS 0x10f44610u          /* Entity::GetNPCs(void), stdcall, by value */
#define ROUTINE_TASK 0x10f42e60u      /* PSRoutine::GetCurrentTask(void) const, by value */
#define STR_COMPARE_NOCASE 0x10004250u /* bCString::CompareNoCaseFast(bCString const &) const */

/* 16, 17 (native_sites at CALL AssessTarget with reason 7 in ZS_Threaten_Loop): HerdUnity, off in
   the mod's ini. Before the original call, which runs either way, every NPC of the threatening
   one's species within 2500 of it that is up, not in the hero's party, not summoned, not fighting
   and not threatening already assesses the target too. */
static uint32_t herd_site(X86 *c) {
    if (!g3f() || !g3f_ini.herd_unity)
        return 0;
    const uint32_t sp = c->r[R_ESP];
    const uint32_t spu = rd32(sp), self = rd32(sp + 4), target = rd32(sp + 8), reason = rd32(sp + 12);
    if (!spu || !self || !target || reason != 7)
        return 0;
    g3f_first(16, "HerdUnity");
    Frame f;
    frame_open(c, &f, 0x800);
    const uint32_t player = ent_player(c, &f);
    const uint32_t list = frame_alloc(&f, 12);
    guest_call(c, ENT_NPCS, &list, 1);
    const uint32_t sort[2] = {list, self};
    guest_call(c, ENT_SORT, sort, 2);
    const uint32_t mark = f.at;
    for (uint32_t i = 0; i < rd32(list + 4); ++i, f.at = mark) {
        const uint32_t e = ent_copy(c, &f, rd32(list) + i * 0xa8u);
        if (ent_eq(c, e, self) || ent_eq(c, e, target)) {
            ent_free(c, e);
            continue;
        }
        thiscall1(c, ENT_DISTANCE, self, e);
        if (!(st0_float(c) < 2500.0f)) {
            ent_free(c, e);
            break;
        }
        int join = !test0(c, ENT_IS_DEAD, e) && !test0(c, ENT_IS_DOWN, e);
        if (join) {
            const uint32_t leader = ent_from(c, &f, PARTY_LEADER, e + E_PARTY);
            join = !ent_eq(c, leader, player) && call0(c, PARTY_TYPE, e + E_PARTY) != 5;
            ent_free(c, leader);
        }
        join = join && call0(c, NPC_SPECIES, e + E_NPC) == call0(c, NPC_SPECIES, self + E_NPC) &&
               call0(c, NPC_COMBAT_STATE, e + E_NPC) != 1;
        if (join) {
            const uint32_t task = frame_alloc(&f, 4);
            thiscall1(c, ROUTINE_TASK, e + E_ROUTINE, task);
            const uint32_t threaten = str_new(c, &f, "ZS_Threaten");
            join = !test1(c, STR_COMPARE_NOCASE, task, threaten);
            str_free(c, threaten);
            str_free(c, task);
        }
        if (join)
            script(c, &f, "AssessTarget", e, target, reason);
        ent_free(c, e);
    }
    const uint32_t items = rd32(list);
    if (items) {
        for (uint32_t i = 0, n = rd32(list + 8); i < n; ++i)
            ent_free(c, items + i * 0xa8u);
        thiscall1(c, MEM_FREE, guest_call(c, MEM_ADMIN, 0, 0), items);
    }
    ent_free(c, player);
    restore(c, &f.s);
    return 0;
}

uint32_t native_site_10fd64fa(X86 *c) {
    return herd_site(c);
}

uint32_t native_site_10fd6545(X86 *c) {
    return herd_site(c);
}

/* gCStat (a gCAttribute with BaseMaximum at +0x18) by the object's RTTI, as dynamic_cast finds it. */
static int is_stat(uint32_t attr) {
    const uint32_t col = rd32(rd32(attr) - 4u);
    const uint32_t hierarchy = rd32(col + 0x10);
    const uint32_t n = rd32(hierarchy + 8), bases = rd32(hierarchy + 0xc);
    for (uint32_t i = 0; i < n && i < 64; ++i) {
        const uint32_t type = rd32(rd32(bases + 4u * i));
        if (!memcmp(g_mem + type + 8, ".?AVgCStat@@", 13))
            return 1;
    }
    return 0;
}

/* The attribute value a requirement is checked against: GetMaximum() with equipment bonuses, else
   BaseMaximum for a stat and BaseValue for an attribute. */
static uint32_t requirement_value(X86 *c, uint32_t attr, int mode) {
    if (mode == 2)
        return call0(c, rd32(rd32(attr) + 0x64), attr);
    return rd32(attr + (is_stat(attr) ? 0x18u : 0x14u));
}

/* 19 (native_sites, Game 109c656f, MOV EBX,[EAX+0x14] in gCInventory_PS::CanLearnSkill): the
   attribute a skill needs, by AttributeRequirementFixModeSkills. */
uint32_t native_site_109c656f(X86 *c) {
    if (!g3f() || !g3f_ini.skills_mode)
        return 0;
    g3f_first(19, "CanLearnSkill");
    Saved s;
    scratch(c, &s, 0x40);
    const uint32_t value = requirement_value(c, s.x.r[R_EAX], g3f_ini.skills_mode);
    restore(c, &s);
    c->r[R_EBX] = value;
    return 0x109c6572u;
}

/* 20: gCPlayerMemory_PS::CalcMissingValue(name, required), by AttributeRequirementFixModeEquipment;
   an unknown attribute misses nothing. */
void g3n_10a5f180(X86 *c) {
    if (!g3f() || !g3f_ini.equipment_mode) {
        fn_10a5f180(c);
        return;
    }
    g3f_first(20, "CalcMissingValue");
    const uint32_t sp = c->r[R_ESP], self = c->r[R_ECX], name = rd32(sp + 4), required = rd32(sp + 8);
    Saved s;
    scratch(c, &s, 0x40);
    const uint32_t attr = thiscall1(c, PM_ATTRIBUTE, self, name);
    const uint32_t missing = attr ? required - requirement_value(c, attr, g3f_ini.equipment_mode) : 0;
    restore(c, &s);
    g3f_return(c, missing, 8);
}

/* 18 and mCG3Fixes::Process: after each eCApplication::Process, the defence mode hotkey, read only
   in a running, unpaused session with no menu, page, info or console open. */
static int g3f_hud_idle(X86 *c);

static void g3f_process(X86 *c) {
    Saved s;
    scratch(c, &s, 0x40);
    const int pressed = g3f_hud_idle(c)
                            ? test1(c, KEY_PRESSED, call0(c, APP_KEYBOARD, guest_call(c, APP_GET, 0, 0)), g3f_ini.hotkey)
                            : -1;
    restore(c, &s);
    if (pressed == 1 && !g3f_key_held) {
        g3f_defend = !g3f_defend;
        g3f_key_held = 1;
        fprintf(stderr, "g3mods: g3fixes: companions %s\n", g3f_defend ? "defend the party" : "passive");
    } else if (pressed == 0) {
        g3f_key_held = 0;
    }
}

uint64_t g3_phase_now(void);                    /* phases.c */
void g3_phase(const char *name, uint64_t t0);  /* phases.c */

void g3n_10276490(X86 *c) {
    const uint64_t t0 = g3_phase_now();
    fn_10276490(c);
    if (g3f())
        g3f_process(c);
    g3_phase("logic", t0);
}

#define STR_TEXT 0x10005680u          /* bCString::GetText(void) const */
#define STR_ASSIGN_CHARS 0x10005000u  /* bCString::operator=(char const *) */
#define ENT_NPCS 0x10f44610u          /* Entity::GetNPCs(void), stdcall, by value */
#define ENT_IS_DEAD 0x10f49210u       /* Entity::IsDead(void) const */
#define FOCUS_ENTITY 0x10f355f0u      /* PSFocus::GetFocusEntity(void) const, by value */
#define NAV_DEST 0x10f398c0u          /* PSNavigation::GetCurrentDestinationPoint(void) const, by value */
#define NPC_VALID 0x10f31330u         /* PSNpc::IsValid(void) const */
#define E_FOCUS 0x18u

static void guest_text(X86 *c, uint32_t str, char *out, size_t cap) {
    const uint32_t p = call0(c, STR_TEXT, str);
    size_t n = 0;
    while (p && n + 1 < cap && g_mem[p + n])
        out[n] = (char)g_mem[p + n], ++n;
    out[n] = 0;
}

/* A living, valid NPC (mod 0x10003160 and 0x10003030 start so). */
static int console_npc(X86 *c, uint32_t e) {
    return !ent_eq(c, e, ENT_NONE) && test0(c, ENT_IS_NPC, e) && test0(c, NPC_VALID, e + E_NPC);
}

static int console_act(X86 *c, Frame *f, uint32_t e, int revive) {
    if (!console_npc(c, e))
        return 0;
    if (!revive) {
        const uint32_t to = ent_from(c, f, NAV_DEST, e + E_NAVIGATION);
        thiscall1(c, ENT_TELEPORT, e, to);
        ent_free(c, to);
        return 1;
    }
    const uint32_t species = call0(c, NPC_SPECIES, e + E_NPC);
    if ((species != 0 && species != 5) || !test0(c, ENT_IS_DEAD, e))
        return 0;
    call0(c, ROUTINE_FULL_STOP, e + E_ROUTINE);
    call_str_arg(c, f, ROUTINE_SET_TASK, e + E_ROUTINE, "ZS_Unconscious");
    return 1;
}

/* 10: eCConsole::OnExecute(command, result). resetpos [name] puts a NPC (the hero's focus, or the
   first NPC so named) back on its destination point; revive [name] makes a dead human or orc
   unconscious; tprevive <name> teleports it to the hero first. Anything else is the original's. */
void g3n_102c6bd0(X86 *c) {
    if (!g3f()) {
        fn_102c6bd0(c);
        return;
    }
    const uint32_t sp = c->r[R_ESP], command = rd32(sp + 4), result = rd32(sp + 8);
    Frame f;
    frame_open(c, &f, 0x800);
    char line[512], *words[3] = {0, 0, 0};
    guest_text(c, command, line, sizeof line);
    int n = 0;
    for (char *p = strtok(line, " "); p && n < 3; p = strtok(0, " "))
        words[n++] = p;
    int kind = -1; /* 0 resetpos, 1 revive, 2 tprevive */
    if (n && !strcasecmp(words[0], "resetpos"))
        kind = 0;
    else if (n && !strcasecmp(words[0], "revive"))
        kind = 1;
    else if (n && !strcasecmp(words[0], "tprevive"))
        kind = 2;
    if (kind < 0) {
        restore(c, &f.s);
        fn_102c6bd0(c);
        return;
    }
    g3f_first(10, "console");
    static const char *const done[] = {"Reseted position for NPC: ", "Revived NPC: ", "Teleported and revived NPC: "};
    static const char *const failed[] = {"Could not find NPC to reset position", "Could not find NPC to revive",
                                         "Could not find NPC to teleport and revive"};
    const uint32_t player = ent_player(c, &f);
    char found[300] = "";
    int ok = 0;
    if (n > 1) {
        const uint32_t list = frame_alloc(&f, 12);
        guest_call(c, ENT_NPCS, &list, 1);
        const uint32_t mark = f.at;
        for (uint32_t i = 0; i < rd32(list + 4) && !ok; ++i, f.at = mark) {
            const uint32_t item = rd32(list) + i * 0xa8u;
            if (ent_eq(c, item, ENT_NONE))
                continue;
            const uint32_t e = ent_copy(c, &f, item);
            const uint32_t name = frame_alloc(&f, 4);
            thiscall1(c, ENT_NAME, e, name);
            guest_text(c, name, found, sizeof found);
            str_free(c, name);
            if (!strcmp(found, words[1])) {
                if (kind == 2)
                    thiscall1(c, ENT_TELEPORT, e, player);
                ok = console_act(c, &f, e, kind != 0);
            }
            ent_free(c, e);
        }
        const uint32_t items = rd32(list);
        if (items) {
            for (uint32_t i = 0, k = rd32(list + 8); i < k; ++i)
                ent_free(c, items + i * 0xa8u);
            thiscall1(c, MEM_FREE, guest_call(c, MEM_ADMIN, 0, 0), items);
        }
    } else if (kind != 2) {
        const uint32_t e = ent_from(c, &f, FOCUS_ENTITY, player + E_FOCUS);
        ok = console_act(c, &f, e, kind);
        if (ok) {
            const uint32_t name = frame_alloc(&f, 4);
            thiscall1(c, ENT_NAME, e, name);
            guest_text(c, name, found, sizeof found);
            str_free(c, name);
        }
        ent_free(c, e);
    }
    char message[400];
    if (ok)
        snprintf(message, sizeof message, "%s%s", done[kind], found);
    else
        snprintf(message, sizeof message, "%s", failed[kind]);
    thiscall1(c, STR_ASSIGN_CHARS, result, frame_text(&f, message));
    ent_free(c, player);
    restore(c, &f.s);
    g3f_return(c, (uint32_t)ok, 8);
}

/* GFC */
#define WND_DLG_ID 0x10dd3470u        /* CFFGFCWnd::GetDlgCtrlID(void) const */
#define WND_DESKTOP 0x10dd33d0u       /* CFFGFCWnd::GetDesktopWindow(void) const */
#define WND_DC 0x10dd3690u            /* CFFGFCWnd::GetDC(void) const */
#define WND_CLIENT_RECT 0x10dd3230u   /* CFFGFCWnd::GetClientRect(bCRect &) const */
#define BITMAP_CTOR 0x10dd1ff0u       /* CFFGFCBitmap::CFFGFCBitmap(void), 8 bytes */
#define BITMAP_CREATE 0x10dd20c0u     /* CFFGFCBitmap::Create(bCString const &) */
#define DC_DRAW_BITMAP 0x10dd2320u    /* CFFGFCDeviceContext::DrawBitmap(CFFGFCBitmap *, bCRect const *, unsigned long, float) */
#define DC_TEXT_RECT 0x10dd2240u      /* CFFGFCDeviceContext::CalcTextRect(unsigned long, wchar_t const *, int, bCRect &, unsigned long) */
#define DC_DRAW_TEXT 0x10dd22a0u      /* CFFGFCDeviceContext::DrawTextA(unsigned long, wchar_t const *, int, bCRect *, unsigned long, bCFloatAlphaColor, bCFloatAlphaColor *) */
/* SharedBase */
#define RECT_CTOR 0x1000ff70u         /* bCRect::bCRect(void) */
#define RECT_CTOR4 0x1001e2f0u        /* bCRect::bCRect(long, long, long, long) */
#define RECT_WIDTH 0x1001e6d0u        /* bCRect::GetWidth(void) const */
#define RECT_HEIGHT 0x1001e6e0u       /* bCRect::GetHeight(void) const */
#define RECT_DTOR 0x1001b3d0u         /* bCRect::~bCRect (the shared empty body) */
#define COLOR_CTOR 0x10065c90u        /* bCFloatAlphaColor::bCFloatAlphaColor(bCColorBase::bEColor), 0x14 bytes */
#define COLOR_DTOR 0x100647b0u        /* bCFloatAlphaColor::~bCFloatAlphaColor(void) */
#define USTR_CHARS 0x10008780u        /* bCUnicodeString::operator wchar_t *(void) */
#define HEAP_ALLOC 0x1001ff00u        /* MemHeap::Alloc(unsigned int), stdcall */
/* Script */
#define NPC_TRANSFORMED 0x10f3b920u   /* PSNpc::IsTransformed(void) const */
#define PM_TRANSFORM_LEFT 0x10f3fae0u /* PSPlayerMemory::operator SecondsTransformRemain, float in ST0 */
#define E_PLAYER_MEMORY 0x38u

/* A running, unpaused session with no menu, GUI page, info or console open: when the mod draws its
   icons and reads its hotkey. */
static int g3f_hud_idle(X86 *c) {
    const uint32_t session = guest_call(c, SESSION_GET, 0, 0);
    if (!(call0(c, rd32(rd32(session) + 4), session) & 0xffu) || test0(c, SESSION_PAUSED, session))
        return 0;
    const uint32_t gui = call0(c, SESSION_GUI, session);
    const uint32_t info = guest_call(c, INFO_MANAGER, 0, 0);
    if (!gui || test0(c, GUI_MENU_OPEN, gui) || test0(c, GUI_PAGE_OPEN, gui) || (info && g_mem[info + 0x14]))
        return 0;
    const uint32_t console = call0(c, APP_CONSOLE, guest_call(c, APP_GET, 0, 0));
    return !console || !test0(c, WND_ACTIVE, console);
}

static uint32_t g3f_bitmaps; /* two CFFGFCBitmap in guest memory, the mod's 0x1006eee0 and 0x1006eefc */

/* An icon `size` pixels square at percent (px, py) of the window `client`. */
static void g3f_icon(X86 *c, Frame *f, uint32_t dc, uint32_t client, uint32_t bitmap, const char *file, float px,
                     float py, int32_t size, int32_t *x, int32_t *y) {
    const uint32_t name = str_new(c, f, file);
    thiscall1(c, BITMAP_CREATE, bitmap, name);
    str_free(c, name);
    /* CVTDQ2PS, MULSS, DIVSS by 100, CVTTSS2SI */
    *x = (int32_t)((float)(int32_t)call0(c, RECT_WIDTH, client) * px / 100.0f);
    *y = (int32_t)((float)(int32_t)call0(c, RECT_HEIGHT, client) * py / 100.0f);
    const uint32_t rect = frame_alloc(f, 16);
    const uint32_t r[4] = {(uint32_t)*x, (uint32_t)*y, (uint32_t)(*x + size), (uint32_t)(*y + size)};
    thiscall(c, RECT_CTOR4, rect, r, 4);
    const uint32_t a[4] = {bitmap, rect, 0, 0};
    thiscall(c, DC_DRAW_BITMAP, dc, a, 4);
    call0(c, RECT_DTOR, rect);
}

/* The mod's 0x10005390 on the GUI desktop `wnd`: the companion mode icon, and while the hero is
   transformed the transformation icon (its _End picture under 16 s) with mm:ss beside it. */
static void g3f_draw_icons(X86 *c, uint32_t wnd) {
    Frame f;
    frame_open(c, &f, 0x600);
    const uint32_t dc = g3f_hud_idle(c) ? call0(c, WND_DC, wnd) : 0;
    if (dc) {
        if (!g3f_bitmaps) {
            g3f_bitmaps = guest_call(c, HEAP_ALLOC, &(uint32_t){16}, 1);
            call0(c, BITMAP_CTOR, g3f_bitmaps);
            call0(c, BITMAP_CTOR, g3f_bitmaps + 8);
        }
        const uint32_t client = frame_alloc(&f, 16);
        call0(c, RECT_CTOR, client);
        thiscall1(c, WND_CLIENT_RECT, wnd, client);
        int32_t x, y;
        g3f_icon(c, &f, dc, client, g3f_bitmaps,
                 g3f_defend ? "G3Fixes_Companion_Active.png" : "G3Fixes_Companion_Passive.png", g3f_ini.companion_x,
                 g3f_ini.companion_y, g3f_ini.companion_size, &x, &y);
        const uint32_t player = ent_player(c, &f);
        if (test0(c, NPC_TRANSFORMED, player + E_NPC)) {
            call0(c, PM_TRANSFORM_LEFT, player + E_PLAYER_MEMORY);
            const float left = st0_float(c);
            const int32_t secs = (int32_t)left;
            const int32_t size = g3f_ini.transform_size;
            g3f_icon(c, &f, dc, client, g3f_bitmaps + 8,
                     secs <= 15 ? "G3Fixes_Transformation_End.png" : "G3Fixes_Transformation.png", g3f_ini.transform_x,
                     g3f_ini.transform_y, size, &x, &y);
            char clock[32];
            snprintf(clock, sizeof clock, "%02d:%02d", secs / 60, secs - secs / 60 * 60);
            const uint32_t text = frame_alloc(&f, 4);
            thiscall1(c, USTR_CTOR, text, frame_text(&f, clock));
            const uint32_t measured = frame_alloc(&f, 16);
            call0(c, RECT_CTOR, measured);
            const uint32_t m[5] = {0, call0(c, USTR_CHARS, text), 0xffffffffu, measured, 0};
            thiscall(c, DC_TEXT_RECT, dc, m, 5);
            const int32_t h = (int32_t)call0(c, RECT_HEIGHT, measured), w = (int32_t)call0(c, RECT_WIDTH, measured);
            const int32_t tx = x + size, ty = size / 2 - h / 2 + y;
            const uint32_t at = frame_alloc(&f, 16);
            const uint32_t r[4] = {(uint32_t)tx, (uint32_t)ty, (uint32_t)(tx + w), (uint32_t)(ty + h)};
            thiscall(c, RECT_CTOR4, at, r, 4);
            const uint32_t shadow = frame_alloc(&f, 0x14);
            thiscall1(c, COLOR_CTOR, shadow, 0);
            /* the text colour (white, 1) by value in the callee's argument slots, which it destroys */
            const uint32_t chars = call0(c, USTR_CHARS, text);
            const uint32_t esp = c->r[R_ESP];
            c->r[R_ESP] = esp - 0x18u;
            const uint32_t slots = c->r[R_ESP];
            wr32(slots + 0x14, shadow);
            thiscall1(c, COLOR_CTOR, slots, 1);
            c->r[R_ESP] = slots - 20u;
            const uint32_t d[5] = {0, chars, 0xffffffffu, at, 0};
            for (int i = 0; i < 5; ++i)
                wr32(c->r[R_ESP] + 4u * i, d[i]);
            c->r[R_ECX] = dc;
            guest_call(c, DC_DRAW_TEXT, 0, 0);
            c->r[R_ESP] = esp;
            call0(c, COLOR_DTOR, shadow);
            call0(c, RECT_DTOR, at);
            call0(c, RECT_DTOR, measured);
            call0(c, USTR_DTOR, text);
        }
        ent_free(c, player);
        call0(c, RECT_DTOR, client);
    }
    restore(c, &f.s);
}

/* 11: the GFC thunk behind CFFGFCWnd::OnPaint and seven more handlers. For the GUI's top window
   (dialog id -1) the icons go on its desktop first, then the original runs. */
void g3n_10dd4680(X86 *c) {
    if (g3f()) {
        const uint32_t self = c->r[R_ECX];
        Saved s;
        scratch(c, &s, 0x40);
        const uint32_t desktop = call0(c, WND_DLG_ID, self) == 0xffffffffu ? call0(c, WND_DESKTOP, self) : 0;
        restore(c, &s);
        if (desktop) {
            g3f_first(11, "icons");
            g3f_draw_icons(c, desktop);
        }
    }
    fn_10dd4680(c);
}

/* ----------------------------------------------------------- Extended Frying -- */

/* Script_ExtendedFrying.dll (docs/mods/dll-extendedfrying.md). */
#define EF_FILTER_BASE 0x109b1570u     /* Game: the transform list's first check, thiscall (stack) */
#define EF_IAT_PROXY_ENTITY 0x10b5c048u /* Game's import of eCTemplateEntityProxy::GetEntity */
#define EF_IAT_ENTITY_NAME 0x10b5cd1cu /* Game's import of eCEntity::GetName, bCString const & */
#define STR_CONTAINS 0x10002ab0u       /* bCString::Contains(char const *, int) const */
#define STR_REPLACE 0x10003ae0u        /* bCString::Replace(char const *, char const *) */
#define TEMPLATE_VALID 0x10f4b9d0u     /* Template::IsValid(void) const */
#define INV_CREATE 0x10f36ce0u         /* PSInventory::CreateItems(Template const &, unsigned long, int) */
#define INV_DELETE 0x10f36b10u         /* PSInventory::DeleteItems(int, int) */

static int ef(void) {
    return (mods() & MOD_EXTENDED_FRYING) != 0;
}

static int ef_template_valid(X86 *c, Frame *f, const char *name) {
    const uint32_t t = frame_alloc(f, 0x14);
    thiscall1(c, TEMPLATE_CTOR, t, frame_text(f, name));
    const int ok = test0(c, TEMPLATE_VALID, t);
    call0(c, TEMPLATE_DTOR, t);
    return ok;
}

/* The mod's choice for the item named by the guest bCString `name` (0x10011ec0, and the filter's
   test 0x10011c80): the first valid template of <name with "_Raw" replaced by "_Fried"> (empty when
   nothing was replaced), <name>_Fried and <name>_Fried_Pan, written to out; 0 when none is. Contains
   and Replace are SharedBase's own, so their rules are the mod's. */
static int ef_fried(X86 *c, Frame *f, uint32_t name, char *out, size_t cap) {
    const uint32_t text = call0(c, STR_TEXT, name);
    char raw[256];
    snprintf(raw, sizeof raw, "%s", text ? (const char *)(g_mem + text) : "");
    const uint32_t copy = str_new(c, f, raw);
    char cand[3][300];
    if (call2(c, STR_REPLACE, copy, frame_text(f, "_Raw"), frame_text(f, "_Fried"))) {
        const uint32_t t = call0(c, STR_TEXT, copy);
        snprintf(cand[0], sizeof cand[0], "%s", t ? (const char *)(g_mem + t) : "");
    } else
        cand[0][0] = 0;
    str_free(c, copy);
    snprintf(cand[1], sizeof cand[1], "%s_Fried", raw);
    snprintf(cand[2], sizeof cand[2], "%s_Fried_Pan", raw);
    for (int i = 0; i < 3; ++i)
        if (ef_template_valid(c, f, cand[i])) {
            snprintf(out, cap, "%s", cand[i]);
            return 1;
        }
    return 0;
}

/* H1: gCHUDListCtrlInvTransforms slot 67, whether stack `stack` goes on the page's list. Mode 3 is
   the frying page: the original takes It_Meat_Raw and It_Meatbug_Raw; with the mod, an item
   without "_Fried" in its name that has a fried template, and It_Meatbug_Raw as before. */
void g3n_109b2280(X86 *c) {
    const uint32_t self = c->r[R_ECX], stack = rd32(c->r[R_ESP] + 4);
    if (!ef() || rd32(self + 0x34) != 3) {
        fn_109b2280(c);
        return;
    }
    uint32_t result = 0;
    Frame f;
    frame_open(c, &f, 0x400);
    if (test1(c, EF_FILTER_BASE, self, stack)) {
        const uint32_t list = rd32(self + 0x14) + 0x30;
        const uint32_t entry = stack < rd32(list + 4) ? rd32(rd32(list) + stack * 4u) : 0;
        const uint32_t ent = entry ? call0(c, rd32(EF_IAT_PROXY_ENTITY), entry + 0xc) : 0;
        if (ent) {
            const uint32_t name = call0(c, rd32(EF_IAT_ENTITY_NAME), ent);
            char fried[300];
            if (!(call2(c, STR_CONTAINS, name, frame_text(&f, "_Fried"), 0) & 0xffu) &&
                ef_fried(c, &f, name, fried, sizeof fried))
                result = 1;
            else
                result = test1(c, STR_EQ_CHARS, name, frame_text(&f, "It_Meatbug_Raw"));
        }
    }
    restore(c, &f.s);
    g3f_return(c, result, 4);
}

static int ef_logged;

/* H2, H3, P1: OnPlayerFryStack(spu, self, other, {raw item slot, amount}), stdcall, returns 1. The
   item in the hero's slot becomes `amount` of the mod's fried template and leaves the inventory.
   Where no template is valid the mod would create the previous call's (or none) and still delete the
   raw items; that case never passes the filter, and here it changes nothing. */
void g3n_1101a740(X86 *c) {
    if (!ef()) {
        fn_1101a740(c);
        return;
    }
    const uint32_t sp = c->r[R_ESP], spu = rd32(sp + 4), args = rd32(sp + 0x10);
    const uint32_t slot = rd32(args), amount = rd32(args + 4);
    Frame f;
    frame_open(c, &f, 0x800);
    const uint32_t self = script_entity(c, &f, spu, rd32(sp + 8), SPU_SELF);
    const uint32_t item = frame_alloc(&f, 0xa8);
    call2(c, INV_TEMPLATE_ITEM, self + E_INVENTORY, item, slot);
    const uint32_t name = frame_alloc(&f, 0x10);
    thiscall1(c, ENT_NAME, item, name);
    char fried[300];
    if (ef_fried(c, &f, name, fried, sizeof fried)) {
        const uint32_t t = frame_alloc(&f, 0x14);
        thiscall1(c, TEMPLATE_CTOR, t, frame_text(&f, fried));
        const uint32_t a[3] = {t, 0, amount};
        thiscall(c, INV_CREATE, self + E_INVENTORY, a, 3);
        call0(c, TEMPLATE_DTOR, t);
        call2(c, INV_DELETE, self + E_INVENTORY, slot, amount);
        if (ef_logged < 5) {
            ++ef_logged;
            fprintf(stderr, "g3mods: extended frying: %d x %s\n", (int)amount, fried);
        }
    } else
        fprintf(stderr, "g3mods: extended frying: no fried template for slot %u, nothing fried\n", slot);
    str_free(c, name);
    ent_free(c, item);
    ent_free(c, self);
    restore(c, &f.s);
    g3f_return(c, 1, 0x10);
}

/* ---------------------------------------------------------------- G3LOC lines -- */

static void loc_print(const char *id, size_t len) {
    X86 *c = guest_current_context();
    if (!c) {
        fprintf(stderr, "g3loc> no guest context: %.*s\n", (int)len, id);
        return;
    }
    Saved s;
    const uint32_t base = scratch(c, &s, 0x200);
    const uint32_t name = base, loc = base + 0x10, text = base + 0x20, chars = base + 0x40;
    if (len > 0x1b0)
        len = 0x1b0;
    memcpy(g_mem + chars, id, len);
    g_mem[chars + len] = 0;
    thiscall1(c, STR_CTOR, name, chars);
    thiscall1(c, LOC_CTOR, loc, name);
    thiscall1(c, LOC_TEXT, loc, text);
    char out[2048];
    size_t n = 0;
    const uint32_t p = rd32(text);
    for (uint32_t i = 0; p && n + 4 < sizeof out; ++i) {
        const uint32_t ch = g_mem[p + 2 * i] | (uint32_t)g_mem[p + 2 * i + 1] << 8;
        if (!ch)
            break;
        if (ch < 0x80)
            out[n++] = (char)ch;
        else if (ch < 0x800)
            out[n++] = (char)(0xc0 | ch >> 6), out[n++] = (char)(0x80 | (ch & 0x3f));
        else
            out[n++] = (char)(0xe0 | ch >> 12), out[n++] = (char)(0x80 | ((ch >> 6) & 0x3f)),
            out[n++] = (char)(0x80 | (ch & 0x3f));
    }
    out[n] = 0;
    thiscall(c, USTR_DTOR, text, 0, 0);
    thiscall(c, LOC_DTOR, loc, 0, 0);
    thiscall(c, STR_DTOR, name, 0, 0);
    restore(c, &s);
    fprintf(stderr, "g3loc> %.*s\ng3loc< %s\n", (int)len, id, out);
}

static char loc_prev[8192];

/* Every G3LOC= line of the control text that was not in the previous one (native/g3_opt.c). */
void g3_mods_control(const char *text) {
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' '))
            --n;
        if (n > 6 && !strncmp(p, "G3LOC=", 6)) {
            int seen = 0;
            for (const char *q = loc_prev; (q = strstr(q, "G3LOC=")) != 0 && !seen; q += 6)
                seen = (q == loc_prev || q[-1] == '\n') && !strncmp(q, p, n) &&
                       (q[n] == '\n' || q[n] == '\r' || q[n] == ' ' || q[n] == 0);
            if (!seen)
                loc_print(p + 6, n - 6);
        }
        p = eol ? eol + 1 : 0;
    }
    snprintf(loc_prev, sizeof loc_prev, "%s", text ? text : "");
}
