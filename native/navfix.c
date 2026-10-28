/* native/navfix.c - gCNavigation::FindWay with a path record whose entity has no property set (28.10, night).
 *
 * work/soak-probe.sh with the player's mods (Alternative World among them) fell over after 10-16 minutes in FindWay
 * (Game 10a3efc0), the build from before the night (344967a) as well: building the way, each record (0x34 bytes)
 * adds its position, then the ID of its entity: eCPropertySetProxy::GetPropertySet(record + 0xc) (the import in
 * EDI), the set's vtable +0x68 (its node), eCNode::GetID, FUN_10a18cf0. A record whose proxy is empty (its words
 * 106d64a8 0 0 0, logged by this file's first version) gives a NULL set; the game reads its vtable from address 0
 * and calls what it finds there. On Windows that read faults and the game shows its crash report (Dialogs.dll);
 * here the call to 0 does the same.
 *
 * native_site_10a40550 (after the call, the record's +0x30 byte 0) and native_site_10a4051b (the same, byte 1): a
 * NULL set gives the way an empty ID (zeroes: no entity) and goes on at the PUSH of the ID; what follows adds the
 * set itself, NULL, as that branch does anyway. G3_NAVFIX=0 leaves the game's own code. */
#include "chunk.h"

#include <stdio.h>
#include <stdlib.h>

/* 64 zero bytes in Gothic3Final.exe's zero-filled .data tail (0x00427660-0x00427fff), which the game never uses
 * (game.toml's sentinels sit at 0x00427f00 and above): an empty bCPropertyID to copy from. */
#define NF_EMPTY_ID 0x00427700u

static int nf_on = -1;
static unsigned nf_count;

static uint32_t nf_site(X86 *c, uint32_t resume) {
    if (nf_on < 0) {
        const char *e = getenv("G3_NAVFIX");
        nf_on = !(e && *e == '0');
    }
    if (!nf_on || c->r[R_EAX] != 0u)
        return 0;
    for (uint32_t i = 0; i < 64u; i += 4u)
        wr32(NF_EMPTY_ID + i, 0u);
    if (nf_count++ < 5u)
        fprintf(stderr, "[g3] navfix: FindWay's record has no property set (record %08x): an empty ID instead\n",
                rd32(rd32(c->r[R_ESP] + 0xcu)) + c->r[R_ESI]);
    c->r[R_EAX] = NF_EMPTY_ID;
    return resume;
}

uint32_t native_site_10a40550(X86 *c) {
    return nf_site(c, 0x10a40561u);
}

uint32_t native_site_10a4051b(X86 *c) {
    return nf_site(c, 0x10a4052cu);
}
