// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_reflect_tests.inc
//
//  The Tank's Reflect (Tank_Reflect_Trigger) reacts to a hit that hurts the Tank by applying its reflect hit to the hit's source:
//  the creature that made the hit takes damage back.  This test gives the Tank the Reflect brace, puts a Wolf in front of him,
//  delivers the Wolf's bite and checks the Wolf lost Health.
// ============================================================================

void test_reflect_reflects_damage(Fixture& f) {
    std::printf("Reflect: a hit that hurts the Tank reflects damage back to the attacker\n");
    Rig rig(7, 25, {"Tank_Reflect"});
    if (!rig.ok()) return;
    rig.know({"Tank_Reflect"});
    CHECK(rig.trigger("Tank_Reflect", 1.0) == Refusal::None, "Reflect is accepted");
    rig.run_to(2.5);   // the brace is up (its block mitigation lasts the recorded 2.9 s)
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 300, 0, 10);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    combat::Fighter* wf = world.fighter_of(wolf);
    CHECK(wf != nullptr, "the Wolf becomes a fighter");
    if (!wf) return;
    const int64_t brace = rig.effect("Tank_Reflect");
    // the rig's client requests every hit of the cast at once, and Tank_Deflect_RemoveBuff takes the hidden brace off at the start;
    // a real client leaves it until the first reflected attack, so put it back for this test
    if (rig.pipe->stacks_of(brace) != 1) rig.pipe->grant_effect(brace, 2.5);
    CHECK(rig.pipe->stacks_of(brace) == 1, "the Reflect brace is up before the hit");
    const double before = wf->stats().find(content::kGuidHealth)->value;
    combat::IncomingHit in;
    in.instigator = wolf;
    in.hit_guid = 109234434627193;      // Wolf_Bite_Hit
    in.source_type = content::kSourceTypeAbility;
    in.source_guid = 109184795825810;   // Wolf_Bite
    in.mods.push_back({109234434627194, content::kGuidHealth, -50.0});
    rig.pipe->receive_hit(in, 2.5);
    rig.take();
    const double after = wf->stats().find(content::kGuidHealth)->value;
    CHECKF(after < before, "the Wolf took reflected damage (%.1f -> %.1f)", before, after);
    rig.pipe->set_combat(nullptr);
}

AOC_REGISTER_TEST(test_reflect_reflects_damage);
