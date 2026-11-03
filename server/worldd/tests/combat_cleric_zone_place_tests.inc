// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_cleric_zone_place_tests.inc
//
//  Registered tests of test_worldd_combat: a Cleric's placed zone (Chains of Restraint) lands where the cast's TargetTransforms put it, not at the caster's feet.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

void test_cleric_zone_at_the_clicked_place(Fixture& f) {
    std::printf("a Cleric's placed zone lands at the place the cast names (TargetTransforms), 12 m from the caster\n");
    const abilities::AbilityDef* chains = f.catalog->ability_named("Cleric_ChainsOfRestraint");
    CHECK(chains != nullptr, "(Chains of Restraint is in the tables)");
    if (!chains) return;
    int64_t spawn_hit = 0, targeting = content::kTargetingDefaultSelf;
    for (const auto& h : chains->hits) {
        const abilities::HitDef* hd = f.catalog->hit(h.hit_guid);
        if (!hd || hd->row.name != "Cleric_ChainsOfRestraint_Spawn") continue;
        spawn_hit = h.hit_guid;
        for (const content::TargetingRow& t : chains->targetings) {
            if (t.key == h.key) targeting = t.guid;
        }
    }
    CHECK(spawn_hit != 0, "(its spawn hit)");
    if (!spawn_hit) return;
    const std::string wolf_path = wolf_archetype(f);
    // place: the Wolf's spot; no place: the caster's feet, 12 m away from it
    for (const bool placed : {true, false}) {
        Rig rig(3, 25, {});
        CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
        if (!rig.ok()) return;
        combat_rig::World w;
        if (!w.problem.empty()) return;
        combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
        combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
        const uint64_t wolf = w.spawn(wolf_path, 1200, 0, 8, 100);
        combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
        world.add_player(&player);
        combat::WorldHitPort port(world, w.registry, player.fighter());
        rig.pipe->set_combat(&port);
        rig.know({"Cleric_ChainsOfRestraint"});
        const world::WorldActor* wa = w.registry.find(wolf);
        CHECK(wa != nullptr, "(a Wolf 12 m away)");
        if (!wa) return;
        const double before = world.health_of(wolf);
        TriggerInfo info;
        if (placed) {
            info.has_place = true;
            info.place_x = wa->x;
            info.place_y = wa->y;
            info.place_z = wa->z;
        }
        double t = 1.0;
        CHECK(rig.pipe->trigger(chains->row.guid, 10, t, info) == Refusal::None, "Chains of Restraint is accepted");
        rig.run_to(t + 2.2);
        t += 2.2;
        rig.pipe->hit_request(chains->row.guid, targeting, spawn_hit, 10, 0, t, {});
        rig.run_to(t + 6.0);
        (void)before;   // (the world knows the Wolf's Health once it ticks: health_of is -1 before)
        const double hp = world.health_of(wolf), max_hp = world.max_health_of(wolf);
        if (placed) {
            CHECKF(hp >= 0 && hp < max_hp, "with a place: the zone at the Wolf hurts it (%.1f of %.1f)", hp, max_hp);
        } else {
            CHECKF(hp < 0 || hp >= max_hp, "with no place: the zone is at the caster's feet, 12 m from the Wolf, which it does not reach (%.1f of %.1f)", hp, max_hp);
        }
        CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
    }
}
AOC_REGISTER_TEST(test_cleric_zone_at_the_clicked_place);
