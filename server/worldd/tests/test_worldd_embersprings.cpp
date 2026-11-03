// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/test_worldd_embersprings.cpp: the death window's choices (respawn/embersprings.h)
//
//  The client's death window (URespawnAlert::Init 0x146666b70) builds its buttons only when RespawnChoices.HomeRespawn is set: with no home it shows a line of text and no
//  choice.  A character that discovered places and has no home takes the first it discovered (Attunements::settle_home), so a new character's death window offers the Aelan
//  Divine Gate.
// ============================================================================
#include "respawn/embersprings.h"

#include <cstdio>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

using namespace aoc::worldd;

int main() {
    std::printf("the first place discovered is the home, so the death window has a choice\n");
    constexpr int64_t kGateRespawn = 6064632337032413186ll;   // RVR_Divine_Gate_Auto_Attune_Respawn (place number 1)
    respawn::ShrineBook book;
    respawn::Shrine gate;
    gate.actor = 10;
    gate.place = 6064631703410180096ll;
    gate.respawn = kGateRespawn;
    gate.number = 1;
    gate.x = 0;
    gate.y = 0;
    gate.name = "Aelan Divine Gate";
    book.add(gate);
    respawn::Shrine far = gate;
    far.actor = 11;
    far.place = 2;
    far.respawn = 2222;
    far.number = 7;
    far.x = 500000;
    far.name = "a far Emberspring";
    book.add(far);

    respawn::Attunements a;
    CHECK(!a.settle_home() && a.home == 0, "a character with no place has no home to take");
    CHECK(a.discover(1), "(the Divine Gate is discovered)");
    respawn::Choices before = respawn::choices_for(book, a, 100, 100);
    CHECK(before.closest.size() == 1 && before.home == 0, "with no home the choices name no home: the client would show no button");
    CHECK(a.settle_home() && a.home == 1, "the first place discovered becomes the home");
    const respawn::Choices after = respawn::choices_for(book, a, 100, 100);
    CHECK(after.home == kGateRespawn && after.closest.size() == 1, "the death window now has its home (the Gate) and the nearest place");
    CHECK(a.discover(7) && !a.settle_home() && a.home == 1, "a later discovery leaves the home alone");
    respawn::Attunements b;
    b.discover(7);
    b.discover(1);
    b.home = 0;
    CHECK(b.settle_home() && b.home == 7, "the home is the first place discovered, whichever it is");
    respawn::Attunements c;
    c.discover(1);
    c.home = 7;
    CHECK(!c.settle_home() && c.home == 7, "a home the player chose stays");
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
