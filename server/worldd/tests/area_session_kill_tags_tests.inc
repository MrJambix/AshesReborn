// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_kill_tags_tests.inc  (contentc version 43: Kill objectives by tag query)
//
//  The quest engine counts a kill for an objective whose TagQuery matches the creature's tags (the union of its asset's, instance's, asset set's and set's, lower case with every
//  parent), and gives a quest item whose NPCTagQuery matches; a query that is not in the file (an older content.db) names no creature and is told once, a Destiny "any creature"
//  objective is not told.  On the real content.db: the starting areas' goblin objectives and the Harbinger lobbies' named kills hold their queries.
// ============================================================================

namespace kill_tags {

constexpr int64_t kQuest = 9400001, kGoblins = 9400011, kLobby = 9400012, kOldFile = 9400013, kRabbidTable = 9400099;

/// A query of the data's shape: version 0, not empty, one expression of `kind` (1 any, 2 all) over the tags, in order.
content::TagQuery make_query(int64_t kind, std::vector<std::string> tags) {
    content::TagQuery q;
    q.tok = {0, 1, kind, static_cast<int64_t>(tags.size())};
    for (size_t i = 0; i < tags.size(); ++i) q.tok.push_back(static_cast<int64_t>(i));
    for (std::string& t : tags) {
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    q.tags = std::move(tags);
    q.present = true;
    return q;
}

content::QuestObjectiveRow kill_objective(int64_t guid, int64_t count) {
    content::QuestObjectiveRow o;
    o.guid = guid;
    o.name = "objective " + std::to_string(guid);
    o.type = "Kill";
    o.count = count;
    o.awake = true;
    return o;
}

content::QuestData quest_data() {
    content::QuestData d;
    content::QuestRow q;
    q.guid = kQuest;
    q.name = "KillTags_TEST";
    content::QuestObjectiveRow goblins = kill_objective(kGoblins, 2);
    goblins.tag_query = true;
    goblins.kill_tags = make_query(1, {"character.humanoid.goblin"});
    content::QuestObjectiveRow::ItemReward ears;
    ears.table = kRabbidTable;
    ears.tag_query = true;
    ears.tags = make_query(1, {"character.beast.rabbid"});
    goblins.item_rewards.push_back(ears);
    content::QuestObjectiveRow lobby = kill_objective(kLobby, 1);
    lobby.tag_query = true;
    lobby.kill_tags = make_query(2, {"npc.stature.rareSpawn", "HarbingerEvent.LobbyA"});
    content::QuestObjectiveRow old_file = kill_objective(kOldFile, 1);
    old_file.tag_query = true;   // the flag of a file older than version 43: no query to read
    q.objectives = {goblins, lobby, old_file};
    d.by_name[q.name] = q.guid;
    d.quests[q.guid] = q;
    return d;
}

}  // namespace kill_tags

void test_kill_by_tag_query() {
    std::printf("quests (contentc version 43): a Kill objective counts the creatures its tag query names, by their tags\n");
    using namespace kill_tags;
    const qs::QuestBook qb{quest_data()};
    const dl::Book book{content::DialogueData{}};
    qs::QuestLog log(12);
    LogFacts facts(log);
    dl::Evaluator ev(book, facts);
    std::vector<qs::Effect> effects;
    {
        qs::Engine eng(log, qb, ev, facts, effects, 1.0);
        CHECK(eng.start(*qb.quest(kQuest), 0) != nullptr, "the quest starts");
    }
    const auto count = [&](int64_t objective) {
        const qs::QuestInstance* q = log.find(kQuest);
        if (!q) return -1;
        for (const qs::ObjectiveState& o : q->objectives) {
            if (o.record == objective) return static_cast<int>(o.count);
        }
        return -1;
    };
    const auto completed = [&](int64_t objective) {
        const qs::QuestInstance* q = log.find(kQuest);
        if (!q) return false;
        for (const qs::ObjectiveState& o : q->objectives) {
            if (o.record == objective) return o.completed;
        }
        return false;
    };
    const auto told = [&] {
        size_t n = 0;
        for (const qs::Effect& e : effects) n += e.kind == qs::Effect::Kind::Told && e.text.find("tag query") != std::string::npos ? 1 : 0;
        return n;
    };
    CHECK(told() == 1, "the objective whose query the file does not hold is told once (the two whose queries read are not)");
    const content::TagSet goblin = content::tag_set_with_parents({"npc.disposition.enemy", "character.humanoid.goblin"});
    const content::TagSet wolf = content::tag_set_with_parents({"npc.disposition.enemy", "character.beast.wolf"});
    const content::TagSet rabbid = content::tag_set_with_parents({"character.beast.rabbid"});
    const content::TagSet named_a = content::tag_set_with_parents({"npc.stature.rareSpawn", "HarbingerEvent.LobbyA", "character.monster"});
    const content::TagSet named_b = content::tag_set_with_parents({"npc.stature.rareSpawn", "HarbingerEvent.LobbyB"});
    CHECK(std::binary_search(goblin.begin(), goblin.end(), std::string("character.humanoid")) && std::binary_search(goblin.begin(), goblin.end(), std::string("character")),
          "a creature's tag set holds every parent, in lower case");
    effects.clear();
    {
        qs::Engine eng(log, qb, ev, facts, effects, 2.0);
        eng.kill(777001, 0, &wolf);
    }
    CHECK(count(kGoblins) == 0 && count(kLobby) == 0, "a wolf counts for none of them");
    const auto grants = [&] {
        size_t n = 0;
        for (const qs::Effect& e : effects) n += e.kind == qs::Effect::Kind::GrantTable && !e.tables.empty() && e.tables[0] == kRabbidTable ? 1 : 0;
        return n;
    };
    CHECK(grants() == 0, "... and drops no quest item");
    {
        qs::Engine eng(log, qb, ev, facts, effects, 2.5);
        eng.kill(777008, 0, &rabbid);
    }
    CHECK(grants() == 1 && count(kGoblins) == 0, "a rabbid drops the quest item the goblins' objective's NPCTagQuery names while it is active (and is no goblin)");
    {
        qs::Engine eng(log, qb, ev, facts, effects, 3.0);
        eng.kill(777002, 0, &goblin);
        eng.kill(777003, 0, nullptr);
    }
    CHECK(count(kGoblins) == 1, "a goblin counts for the goblins' objective (AnyTagsMatch of character.humanoid.goblin); a creature whose tags are not known does not");
    {
        qs::Engine eng(log, qb, ev, facts, effects, 4.0);
        eng.kill(0, 0, &goblin);   // a creature of no population instance: its tags still name it
    }
    CHECK(count(kGoblins) == 2 && completed(kGoblins), "a second goblin, of no population instance, completes it");
    effects.clear();
    {
        qs::Engine eng(log, qb, ev, facts, effects, 5.0);
        eng.kill(777004, 0, &named_b);
    }
    CHECK(count(kLobby) == 0, "the named creature of another lobby does not count (AllTagsMatch of npc.stature.rareSpawn and HarbingerEvent.LobbyA)");
    {
        qs::Engine eng(log, qb, ev, facts, effects, 6.0);
        eng.kill(777005, 0, &named_a);
    }
    CHECK(completed(kLobby), "lobby A's named creature does (the query's tags are matched in any case)");
    CHECK(count(kOldFile) == 0 && log.find(kQuest) && log.find(kQuest)->status == qs::Status::Active, "the objective with no query to read counted nothing: the quest goes on");
}
AOC_REGISTER_TEST(test_kill_by_tag_query);

void test_kill_tag_queries_in_data() {
    std::printf("quests (contentc version 43): the real data's Kill objectives by tag query\n");
    std::string why;
    const auto d = quest_test_data(why);
    if (!d) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const qs::QuestBook qb(d->quests);
    const content::QuestRow* bucktown = qb.by_name("BucktownStartingAreaQuest");
    const content::QuestRow* lobby = qb.by_name("GBL_EVT_HAR_MSTR_CORE_P3");
    CHECK(bucktown && lobby, "(the starting area's quest and the Harbinger's third phase are in the data)");
    if (!bucktown || !lobby) return;
    size_t with_flag = 0, with_query = 0;
    for (const auto& [guid, q] : d->quests.quests) {
        for (const content::QuestObjectiveRow& o : q.objectives) {
            with_flag += o.tag_query ? 1 : 0;
            with_query += o.kill_tags.present ? 1 : 0;
        }
    }
    std::printf("  [info] %zu objectives name creatures by a tag query, %zu of them with the query in the file\n", with_flag, with_query);
    const content::QuestObjectiveRow* goblins = nullptr;
    for (const content::QuestObjectiveRow& o : bucktown->objectives) {
        if (o.type == "Kill" && o.tag_query) goblins = &o;
    }
    if (with_query == 0) {
        std::printf("  [skip] the content.db predates contentc version 43: its objectives hold the flag only\n");
        CHECK(goblins && !goblins->kill_tags.present, "an older file: the flag reads, the query is not there (the objective names no creature)");
        return;
    }
    CHECK(with_flag >= 900 && with_query == with_flag, "every Kill objective of a tag query holds its query (over 900 of them)");
    const content::TagSet goblin = content::tag_set_with_parents({"npc.disposition.enemy", "character.humanoid.goblin"});
    const content::TagSet wolf = content::tag_set_with_parents({"npc.disposition.enemy", "character.beast.wolf"});
    CHECK(goblins && content::tag_query_matches(goblins->kill_tags, goblin) && !content::tag_query_matches(goblins->kill_tags, wolf),
          "Bucktown's starting objective names the goblins (character.humanoid.goblin), not the wolves");
    size_t lobbies = 0;
    const content::TagSet named_a = content::tag_set_with_parents({"npc.stature.rareSpawn", "HarbingerEvent.LobbyA"});
    const content::TagSet rare_only = content::tag_set_with_parents({"npc.stature.rareSpawn"});
    for (const content::QuestObjectiveRow& o : lobby->objectives) {
        if (o.name != "GBL_EVT_HAR_MSTR_CORE_P3_LobbyA_NamedKill") continue;
        ++lobbies;
        CHECK(content::tag_query_matches(o.kill_tags, named_a) && !content::tag_query_matches(o.kill_tags, rare_only), "lobby A's named kill wants both its tags (a rare spawn of lobby A)");
    }
    CHECK(lobbies == 1, "(the lobby A objective is there)");
}
AOC_REGISTER_TEST(test_kill_tag_queries_in_data);
