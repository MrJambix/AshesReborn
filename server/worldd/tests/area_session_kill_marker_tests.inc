// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_kill_marker_tests.inc  (quests, "map markers": a Kill objective named by tags only)
//
//  The map marks a Kill objective whose creatures the data names by a tag query (contentc version 43) at the camps of the world's creatures that carry the tags: their spawn
//  points, clustered as the objective's settings say, as an objective that names its sets is.
// ============================================================================

void test_kill_markers_by_tags() {
    std::printf("a Kill objective named by tags is marked where the creatures that carry them spawned\n");
    world::ActorRegistry reg;
    const auto goblin = std::make_shared<const content::TagSet>(content::TagSet{"character", "character.humanoid", "character.humanoid.goblin"});
    const auto wolf = std::make_shared<const content::TagSet>(content::TagSet{"character", "character.beast", "character.beast.wolf"});
    uint64_t next = 9800000;
    const auto spawn = [&](double x, double y, const std::shared_ptr<const content::TagSet>& tags) {
        world::WorldActor a;
        next += 2;
        a.id = next;
        a.actor.guid.ObjectId = next;
        a.kind = world::ActorKind::Npc;
        a.x = x;
        a.y = y;
        a.npc.home_x = x;
        a.npc.home_y = y;
        a.npc.tags = tags;
        return reg.add(std::move(a));
    };
    bool added = true;
    for (int i = 0; i < 6; ++i) added = spawn(100000 + 300.0 * i, 200000, goblin) && added;   // a goblin camp
    for (int i = 0; i < 6; ++i) added = spawn(-400000 + 300.0 * i, -100000, wolf) && added;   // wolves elsewhere
    CHECK(added, "(six goblins in a camp and six wolves far from it)");
    content::QuestObjectiveRow o = kill_tags::kill_objective(9800001, 4);
    o.tag_query = true;
    o.kill_tags = kill_tags::make_query(1, {"character.humanoid.goblin"});
    o.cluster.epsilon = 2000;
    o.cluster.min_points = 2;
    quest::RegistryPoints points(reg, nullptr);
    const std::vector<quest::MarkerLocation> marks = quest::compute_locations(o, points);
    CHECK(!marks.empty(), "the objective named by tags only is marked");
    bool at_camp = !marks.empty();
    for (const quest::MarkerLocation& m : marks) at_camp = at_camp && std::hypot(m.x - 100750, m.y - 200000) < 5000;
    CHECK(at_camp, "at the goblins' camp, not where the wolves are");
    content::QuestObjectiveRow none = o;
    none.kill_tags = kill_tags::make_query(1, {"character.undead"});
    CHECK(quest::compute_locations(none, points).empty(), "a query no creature of the world matches gives no marker");
    content::QuestObjectiveRow old_file = o;
    old_file.kill_tags = content::TagQuery{};   // (an older content.db: the flag only)
    CHECK(quest::compute_locations(old_file, points).empty(), "a query the file does not hold gives none");
}
AOC_REGISTER_TEST(test_kill_markers_by_tags);
