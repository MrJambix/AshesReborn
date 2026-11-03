// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_pipeline_vengeance_tests.inc: the Tank's Vengeance comes back sooner for each hit that hurts the Tank (a hit taken, Hit/OwnerIsHit)
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_pipeline.cpp runs it through aoc_tests::run; it is included from ability_pipeline_registered.inc)
// ============================================================================

void test_tank_vengeance_hit_taken() {
    std::printf("Vengeance: a hit that hurts the Tank takes 0.5 s off its cooldown, then its lockout holds\n");
    // the carrier is the talent Tank_Vengeance_DamageTrigger's passive; the reaction's trigger also asks for Vengeance itself bought (GetSkillPurchased)
    Rig r(7, 25, {"Tank_Vengeance", "Tank_Vengeance_DamageTrigger"});
    CHECKF(r.ok(), "the Tank rig: %s", r.problem.c_str());
    if (!r.ok()) return;
    const int64_t vengeance = r.ability("Tank_Vengeance"), lockout = r.effect("Tank_Vengeance_DamageTrigger_ICD");
    CHECK(vengeance != 0 && lockout != 0, "(the data has Vengeance and its trigger's lockout)");
    r.know({"Tank_Vengeance"});
    CHECK(r.trigger("Tank_Vengeance", 2.0, true) == Refusal::None, "Vengeance is accepted");
    r.run_to(3.0);
    const bootstrap::CooldownItem* cd = r.pawn.cooldown(vengeance);
    CHECK(cd != nullptr, "its cooldown runs");
    if (!cd) return;
    const double end_before = cd->end_time;
    combat::IncomingHit blow;
    blow.instigator = 0;
    blow.hit_guid = 1;
    blow.mods.push_back({1, content::kGuidHealth, -50.0});
    r.pipe->receive_hit(blow, 4.0);
    r.take();
    r.run_to(4.2);
    cd = r.pawn.cooldown(vengeance);
    CHECKF(cd && near(end_before - cd->end_time, 0.5, 0.05), "a hit that hurts takes 0.5 s off Vengeance's cooldown (%.3f s)", cd ? end_before - cd->end_time : -1.0);
    CHECK(r.pipe->stacks_of(lockout) == 1, "and puts its lockout on");
    const double end_after = cd ? cd->end_time : 0;
    r.pipe->receive_hit(blow, 4.3);
    r.take();
    r.run_to(4.5);
    cd = r.pawn.cooldown(vengeance);
    CHECK(cd && near(cd->end_time, end_after, 0.01), "a second hit during the lockout takes nothing off");
    CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_tank_vengeance_hit_taken);
