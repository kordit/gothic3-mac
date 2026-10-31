/* native/strshare.c - a bCString's text is not shared past 0xC000 copies (31.10, the Kap Dun arena).
 *
 * SharedBase's bCString is one pointer to text that every copy shares, with a header before it: the length at
 * text - 8 and the count of copies, a 16-bit word, at text - 4. A copy adds 1 to the word with no check, a release
 * subtracts 1 and frees the text when the word comes to 0 (~bCString 10001a00). So more than 65535 copies at once
 * wrap the word, and the text is freed under the copies still holding it.
 *
 * Gothic 3 gets there with the player's mods (Main's NavigationMap.xnav, 2453 nav zones): every eCPropertySetProxy to
 * a zone holds a copy of one text, "gCNavZone_PS", the class name the archive read handed out.
 * gCNavigationMap::ReadLists (Game 10a36810) builds its temporary lists and then the map's own, and in between the
 * word went from 55.5 thousand to 65775 copies (239 over), wrapped, and came down through 0 as the temporaries went
 * (native/arenatrace.c, G3_ARENA_TRACE_SAMPLE, the player's saves 0030 and 0045 on 31.10). After that every zone of
 * the map named a freed text; once MemHeap handed the block out again (save 0045: its first word became 1, leaving
 * "\x01\0\0\0vZone_PS") no zone gave its property set: GetZone found nothing, gCNavigation::FindWay stopped at status
 * 8, and the arena fighters stood where they were. Whether the block is reused depends on the heap, which is why
 * session 19 saw the same bytes and could not make them come back.
 *
 * The copies a proxy makes all go through bCString::SetText(bCString const &) (10002e20, operator= 10005040 calls
 * it): eCPropertySetProxy::SetPropertySet from Read, CopyFrom and SetPropertySetComfort. The exported copy
 * constructor (100019e0) is the other way to share. Both, when the source's word is 0xC000 or more, give the target
 * a text of its own (bCString::Realloc 10003d90 to the source's length, then its bytes, NULs included) instead of
 * one more share; under that they run the original. The value of every string stays what it was, only which copies
 * share a text changes. 0xC000 leaves 16383 copies of room for the copies other modules make inline (Game, Engine
 * and Script_Game add 1 to the word themselves in about 480 places, each a short-lived local); the release side
 * is all in SharedBase and stays the original.
 *
 * SetText(bCString const &), thiscall: ECX the target, [ESP+4] the source, RET 4; the callers ignore EAX. The copy
 * constructor: the same, returns the target in EAX. The new text's other registers are the call's scratch. */
#include "chunk.h"

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");

void fn_10002e20(X86 *c); /* SharedBase bCString::SetText(bCString const &) */
void fn_100019e0(X86 *c); /* SharedBase bCString::bCString(bCString const &) */

#define SS_REALLOC 0x10003d90u /* SharedBase bCString::Realloc(int), thiscall, RET 4 */
#define SS_LIMIT 0xc000u

unsigned g3n_strshare_copies; /* texts given their own copy, for a test or a debugger */

/* The source's text when it should not be shared any more, else 0. */
static uint32_t ss_unshare(uint32_t target, uint32_t source) {
    const uint32_t text = rd32(source);
    if (!text || rd32(text - 8u) == 0u || rd16(text - 4u) < SS_LIMIT || rd32(target) == text)
        return 0u;
    return text;
}

static void ss_copy(X86 *c, uint32_t target, uint32_t text) {
    const uint32_t len = rd32(text - 8u);
    c->r[R_ECX] = target;
    guest_call(c, SS_REALLOC, &len, 1);
    const uint32_t mine = rd32(target);
    for (uint32_t i = 0; i < len; ++i)
        wr8(mine + i, rd8(text + i));
    ++g3n_strshare_copies;
}

void g3n_10002e20(X86 *c) {
    const uint32_t sp = c->r[R_ESP], target = c->r[R_ECX], text = ss_unshare(target, rd32(sp + 4u));
    if (!text) {
        fn_10002e20(c);
        return;
    }
    ss_copy(c, target, text);
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 8u;
    recomp_return(c);
}

void g3n_100019e0(X86 *c) {
    const uint32_t sp = c->r[R_ESP], target = c->r[R_ECX], source = rd32(sp + 4u);
    const uint32_t text = rd32(source);
    if (!text || rd32(text - 8u) == 0u || rd16(text - 4u) < SS_LIMIT) {
        fn_100019e0(c);
        return;
    }
    wr32(target, 0u); /* the target is new: Realloc takes it as empty */
    ss_copy(c, target, text);
    c->r[R_EAX] = target;
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 8u;
    recomp_return(c);
}
