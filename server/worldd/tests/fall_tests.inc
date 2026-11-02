// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/fall_tests.inc: the law of fall damage (session/fall_damage.h) and the routing of a quest item (items/inventory.h)
// ============================================================================
#include "session/fall_damage.h"

namespace {

/// A free fall from `metres` up, as the client's moves say it: every 33 ms, then the ground.
std::optional<fall::Landing> drop_from(fall::Track& t, double metres) {
    const double g = fall::kGravity * 100.0, top = metres * 100.0;
    double fallen = 0, speed = 0, at = 1.0;
    t.moved(repl::kMoveWalking, top, static_cast<float>(at));
    while (fallen < top) {
        t.moved(repl::kMoveFalling, top - fallen, static_cast<float>(at));
        at += 0.033;
        speed = std::min(speed + g * 0.033, fall::kTerminalCm);
        fallen += speed * 0.033;
    }
    return t.moved(repl::kMoveWalking, 0.0, static_cast<float>(at));
}

void test_fall_damage() {
    std::printf("=== fall damage: the game's equation\n");
    CHECK(fall::share_of(900, 4000, 0) == 0.0, "9 m hurts nobody");
    CHECK(std::abs(fall::share_of(2500, 4000, 0) - 0.5) < 1e-9, "25 m takes half: (25 - 10) / 30");
    CHECK(fall::share_of(9000, 4000, 2.0) == 1.0, "a long drop is lethal");
    CHECK(fall::share_of(9000, 800, 0) == 0.0, "a long slide at 8 m/s hurts nobody: the speed says it fell 1.3 m");
    fall::Track t;
    const auto small = drop_from(t, 8);
    CHECK(small && small->share == 0.0, "the moves of an 8 m drop deal nothing");
    const auto mid = drop_from(t, 25);
    CHECK(mid && std::abs(mid->fallen_cm - 2500) < 1 && mid->share > 0.35 && mid->share <= 0.5, "the moves of a 25 m drop deal up to half");
    const auto far = drop_from(t, 60);
    CHECK(far && far->share == 1.0, "the moves of a 60 m drop are lethal");
    t.moved(repl::kMoveFalling, 5000, 100.f);
    CHECK(!t.moved(repl::kMoveSwimming, 0, 101.f), "a fall into water ends with no landing");
    t.moved(repl::kMoveFalling, 5000, 110.f);
    t.forget();
    CHECK(!t.moved(repl::kMoveWalking, 0, 111.f), "a teleport forgets the fall");
}
AOC_REGISTER_TEST(test_fall_damage);

}  // namespace
