// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/spawn_filter_npc_tags_tests.inc  (contentc version 43: the creatures' tags for the Kill objectives by tag query)
//
//  A spawned creature carries the gameplay tags of its instance, set, asset and asset set (the spawn filter's RequiredTags read the same union), lower case with every parent,
//  interned so that every creature of the same tags shares one list; make_data_actor puts them into NpcInfo (a respawn copies the recipe's NpcInfo).  On the real data: the
//  Wolves of the Pop_Wolf asset set carry character.beast.wolf, which the asset itself does not.
// ============================================================================

void test_npc_tag_union(RealArea&) {
    std::printf("a creature's tags: the union of its instance's, set's, asset's and asset set's, with every parent, interned\n");
    content::PopulationData d;
    content::NpcSetRow set;
    set.guid = 1;
    set.tags = {"Quest.SA_0008.LowlandWolf"};
    content::NpcInstanceRow inst;
    inst.guid = 2;
    inst.tags = {"EVENT.RVR.Grembaby1"};
    content::NpcAssetRow asset;
    asset.guid = 3;
    asset.tags = {"npc.disposition.enemy"};
    d.asset_set_tags[4] = {"character.beast.wolf"};
    const auto tags = spawns::npc_tag_set(d, set, inst, 4, asset);
    const auto has = [&](const char* t) { return tags && std::binary_search(tags->begin(), tags->end(), std::string(t)); };
    CHECK(tags && has("character.beast.wolf") && has("character.beast") && has("character") && has("npc.disposition.enemy") && has("event.rvr.grembaby1") && has("event") &&
              has("quest.sa_0008.lowlandwolf"),
          "every source counts, in lower case, with its parents");
    CHECK(tags && std::is_sorted(tags->begin(), tags->end()) && std::adjacent_find(tags->begin(), tags->end()) == tags->end(), "the list is sorted, each tag once");
    const auto again = spawns::npc_tag_set(d, set, inst, 4, asset);
    CHECK(again && again.get() == tags.get(), "the same tags are the same list (one per kind of creature, not one per NPC)");
    content::NpcSetRow bare_set;
    content::NpcInstanceRow bare_inst;
    content::NpcAssetRow bare_asset;
    CHECK(spawns::npc_tag_set(d, bare_set, bare_inst, 0, bare_asset) == nullptr, "a creature with no tag has no list");
}
AOC_REGISTER_TEST(test_npc_tag_union);

void test_npc_tags_real(RealArea& fx) {
    std::printf("the real data: a creature of the Pop_Wolf asset set carries its tags into the world\n");
    if (!fx.db || !fx.captured || !fx.table) {
        std::printf("  [skip] the real-area tests need content.db, the captures and npc_motion.tsv\n");
        return;
    }
    std::string error;
    const auto data = fx.db->population_data(error);
    CHECK(data.has_value(), "the population data loads");
    if (!data) return;
    constexpr int64_t kPopWolf = 6064630270761783141ll;   // npc_asset_set Pop_Wolf: ["npc.disposition.enemy","character.beast.wolf"]
    const auto pw = data->asset_set_tags.find(kPopWolf);
    CHECK(pw != data->asset_set_tags.end() && std::find(pw->second.begin(), pw->second.end(), "character.beast.wolf") != pw->second.end(), "the Pop_Wolf asset set carries character.beast.wolf");
    spawns::AreaConfig area;
    spawns::GenericNpcs generic(*fx.captured, fx.table, spawns::GenericConfig{});
    const spawns::TemplateSource templates{fx.captured, &generic};
    SpawnContext ctx;
    ctx.hour = 12;
    SpawnFilter filter(ctx, fx.table);
    spawns::PopulationStats st;
    const auto spawned = spawns::select_data_spawns(*data, templates, area, st, &filter);
    size_t wolves = 0, tagged = 0, untagged = 0, with_tags = 0;
    const spawns::DataSpawn* first = nullptr;
    const content::TagQuery wolf_query = [] {
        content::TagQuery q;
        q.present = true;
        q.tok = {0, 1, 1, 1, 0};
        q.tags = {"character.beast.wolf"};
        return q;
    }();
    for (const spawns::DataSpawn& s : spawned) {
        with_tags += s.tags ? 1 : 0;
        if (s.asset_set != kPopWolf) continue;
        ++wolves;
        if (!first) first = &s;
        if (s.tags && content::tag_query_matches(wolf_query, *s.tags) && std::binary_search(s.tags->begin(), s.tags->end(), std::string("npc.disposition.enemy"))) ++tagged;
        else ++untagged;
    }
    std::printf("  [info] %zu NPCs spawned, %zu with tags; %zu of the Pop_Wolf asset set\n", spawned.size(), with_tags, wolves);
    CHECK(wolves > 0 && untagged == 0 && tagged == wolves, "every Wolf of the Pop_Wolf asset set carries character.beast.wolf (with its parents) and npc.disposition.enemy");
    CHECK(with_tags * 2 > spawned.size(), "most spawned creatures carry tags");
    if (!first) return;
    uint64_t next = 9000000;
    const spawns::Minter mint = [&] { return aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(next++); };
    auto actor = spawns::make_data_actor(*first, mint, 1, 0.0, nullptr, error);
    CHECK(actor && actor->npc.tags && actor->npc.tags.get() == first->tags.get(), "the actor made from the point holds the same list in its NpcInfo (what a kill hands the quests)");
}
AOC_REGISTER_TEST(test_npc_tags_real);
