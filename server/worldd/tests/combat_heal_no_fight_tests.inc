// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_heal_no_fight_tests.inc
//
//  Registered tests of test_worldd_combat: a heal or a buff that lands on a creature does not start a fight, and nothing a player does to a town person (on the
//  players' side: NPC_Questgiver, Friendly) does; a hit that harms a creature does (reported live on 2 November: a Mend cast at the vendor in the target slot made the
//  town's people fight the Cleric).
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

void test_heal_starts_no_fight(Fixture& f) {
    std::printf("a heal on a creature, or any hit on a town person, starts no fight; a hit that harms a creature does\n");
    Rig rig(3, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    std::string vendor_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Vendors/General_Gear_Base_Vendor.Default__General_Gear_Base_Vendor_C") != std::string::npos) vendor_path = path;
    }
    const std::string wolf_path = wolf_archetype(f);
    CHECK(!vendor_path.empty() && !wolf_path.empty(), "(the gear vendor's and the Wolf's Blueprints)");
    if (vendor_path.empty() || wolf_path.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 900, 0, 10, 100);       // (a small aggro radius: it does not come for the player by itself)
    const uint64_t vendor = w.spawn(vendor_path, -900, 0, 25, 100);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    CHECK(wolf != 0 && vendor != 0, "(a Wolf and a town vendor near the player)");
    if (!wolf || !vendor) return;
    double now = 0;
    const auto run = [&](double seconds) {
        for (const double end = now + seconds; now < end;) {
            now += 1.0 / 15.0;
            w.npcs->note_player(77, 0, 0, now);
            w.npcs->tick(now);
            world.tick(now);
        }
    };
    run(0.5);
    const auto fighting = [&](uint64_t id) {
        const auto s = world.ai_state(id);
        return s && (*s == npc::ai::State::Chase || *s == npc::ai::State::Attack);
    };
    combat::Resolved heal;
    heal.mods.push_back({1, content::kGuidHealth, 120.0});
    world.apply_hit_to_npc(wolf, 77, heal, now);
    world.apply_hit_to_npc(vendor, 77, heal, now);
    run(2.0);
    CHECK(!fighting(wolf), "a heal that lands on a Wolf starts no fight");
    CHECK(!fighting(vendor), "nor on the town vendor (the live report: a Mend at the vendor in the target slot)");
    CHECK(player.hits.empty(), "and nothing hits the player");
    combat::Resolved blow;
    blow.mods.push_back({1, content::kGuidHealth, -50.0});
    world.apply_hit_to_npc(vendor, 77, blow, now);
    run(2.0);
    CHECK(!fighting(vendor), "a town person, on the players' side, does not fight back even a blow");
    world.apply_hit_to_npc(wolf, 77, blow, now);
    run(1.0);
    CHECK(fighting(wolf), "a blow on the Wolf starts its fight, as before");
}
AOC_REGISTER_TEST(test_heal_starts_no_fight);
