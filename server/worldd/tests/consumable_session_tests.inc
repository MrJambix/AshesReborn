// SPDX-License-Identifier: AGPL-3.0-only
// Real content and the shipping client's item-bearing RPC, exercised through PlayerSession.

static void consumable_run(Harness& h, int milliseconds_to_run = 2200) {
    for (int elapsed = 0; elapsed < milliseconds_to_run; elapsed += 20) h.step(milliseconds(20));
}

static svc::Character consumable_character(std::vector<svc::InventoryItem> bag) {
    svc::Character c = cyb_record();
    c.items.erase(std::remove_if(c.items.begin(), c.items.end(), [](const auto& item) { return item.container != Worn; }), c.items.end());
    c.items.insert(c.items.end(), bag.begin(), bag.end());
    return c;
}

static void consumable_trigger(Harness& h, int slot, int32_t id, int64_t override_ability = 0,
                               std::optional<repl::ItemInstance> override_item = std::nullopt) {
    const svc::InventoryItem* held = h.s->inventory().at(Bag, slot);
    if (!held) return;
    std::string error;
    const auto cast = h.fx.content->item_cast(held->item_record_id, error);
    CHECK(cast && error.empty(), "the owned consumable has a configured ability");
    if (!cast) return;
    const auto item = override_item ? *override_item : items::to_instance(*held);
    h.request(trigger_with_item(h.s->bootstrap().setup().guids.ability, override_ability ? override_ability : *cast, id, slot, item, 3), h.pawn_ch);
}

/// The abilities the client's AbilityComponent has loaded, rebuilt from what the server sent on the pawn's channel: the client shows "Ability not loaded" and sends no
/// trigger for an item whose ability is not among them, so a use that works in the tests above can still do nothing in the game.
static std::vector<int64_t> client_loaded_records(const Harness& h) {
    std::vector<int64_t> out;
    const repl::Subobject* ab = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.ability);
    if (!ab) return out;
    const schema::ClassSchema& cls = *ab->obj.schema;
    for (const fake::Delivered& d : h.got) {
        if (d.channel != h.pawn_ch || d.open || d.close || d.content.bit_pos() <= 8) continue;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) continue;
        for (const repl::BlockView& v : *views) {
            if (!(v.header.object == h.s->bootstrap().setup().guids.ability) || !v.header.has_rep_layout) continue;
            size_t p = v.payload_pos;
            const auto st = repl::read_rep_block(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
            if (!st) continue;
            const repl::Value* val = st->get(cls, "ReplicatedRecordsToLoad");
            const repl::ArrayValue* a = val ? val->get_if<repl::ArrayValue>() : nullptr;
            if (!a) continue;
            out.resize(a->elements.size());
            for (size_t i = 0; i < a->elements.size(); ++i) {
                const auto it = a->elements[i].values.find(1);
                if (it != a->elements[i].values.end() && it->second.get_if<int64_t>()) out[i] = *it->second.get_if<int64_t>();
            }
        }
    }
    return out;
}

static void test_real_consumables() {
    std::printf("consumables: real potion/elixir item casts, ownership, potency and cooldowns\n");
    const char* configured = std::getenv("AOC_CONTENT_DB");
    const std::string path = configured && *configured ? configured : "dist/content/content.db";
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        std::printf("  [skip] no real content.db for consumables\n");
        return;
    }
    sqlite3_stmt* query = nullptr;
    if (sqlite3_prepare_v2(raw, "SELECT i.guid, i.name, c.ability_guid FROM item i LEFT JOIN item_cast c ON c.item_guid=i.guid "
                              "WHERE i.name LIKE 'Consumable_Potion_%' OR i.name LIKE 'Consumable_Elixir_%' ORDER BY i.guid", -1, &query, nullptr) != SQLITE_OK) {
        sqlite3_close(raw);
        std::printf("  [skip] consumables need contentc 38's item_cast table\n");
        return;
    }
    struct Row { int64_t item, ability; std::string name; };
    std::vector<Row> rows;
    while (sqlite3_step(query) == SQLITE_ROW)
        rows.push_back({sqlite3_column_int64(query, 0), sqlite3_column_int64(query, 2), reinterpret_cast<const char*>(sqlite3_column_text(query, 1))});
    sqlite3_finalize(query);
    sqlite3_close(raw);
    CHECK(rows.size() >= 61, "the complete set of 61 potion/elixir item records is checked");
    for (const Row& row : rows) {
        Harness h(consumable_character({make_item(Bag, 0, row.item, 900, 2)}), true, true, path.c_str());
        CHECK(row.ability != 0, (row.name + ": content authorizes its ability").c_str());
        const std::vector<int64_t> loaded = client_loaded_records(h);
        CHECK(std::find(loaded.begin(), loaded.end(), row.ability) != loaded.end(), (row.name + ": the client has its ability loaded from the join, or it would send no trigger").c_str());
        consumable_trigger(h, 0, 700);
        consumable_run(h);
        const auto* left = h.s->inventory().at(Bag, 0);
        CHECK(h.s->uses_applied() == 1 && left && left->quantity == 1 && !h.s->failed()
              && h.mirror.unreadable == 0 && mirror_matches(h),
              (row.name + ": casts and consumes exactly one, with a readable matching client inventory").c_str());
    }
    // the other kinds that run through the same path: one of each
    for (const char* kind : {"Consumable_Food_%", "Consumable_Drink_%", "Consumable_Scroll_%", "Consumable_Salve_%"}) {
        sqlite3* sample_db = nullptr;
        sqlite3_stmt* sample = nullptr;
        int64_t item = 0;
        std::string sample_name;
        if (sqlite3_open_v2(path.c_str(), &sample_db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
            sqlite3_prepare_v2(sample_db, "SELECT i.guid, i.name FROM item i JOIN item_cast c ON c.item_guid=i.guid JOIN item_use u ON u.item_guid=i.guid WHERE u.activation='CastAbility' AND i.name LIKE ?1 ORDER BY i.guid LIMIT 1", -1, &sample, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(sample, 1, kind, -1, SQLITE_STATIC);
            if (sqlite3_step(sample) == SQLITE_ROW) {
                item = sqlite3_column_int64(sample, 0);
                sample_name = reinterpret_cast<const char*>(sqlite3_column_text(sample, 1));
            }
        }
        if (sample) sqlite3_finalize(sample);
        if (sample_db) sqlite3_close(sample_db);
        CHECK(item != 0, (std::string("the data has a ") + kind + " to try").c_str());
        if (item == 0) continue;
        Harness h(consumable_character({make_item(Bag, 0, item, 905, 2)}), true, true, path.c_str());
        std::string error;
        const auto cast = h.fx.content->item_cast(item, error);
        const std::vector<int64_t> loaded = client_loaded_records(h);
        CHECK(cast && std::find(loaded.begin(), loaded.end(), *cast) != loaded.end(), (sample_name + ": the client has its ability loaded").c_str());
        consumable_trigger(h, 0, 695);
        consumable_run(h);
        const auto* left = h.s->inventory().at(Bag, 0);
        CHECK(h.s->uses_applied() == 1 && left && left->quantity == 1 && !h.s->failed() && mirror_matches(h), (sample_name + ": casts and consumes exactly one").c_str());
    }
    constexpr int64_t health = 6064629336879989989ll, mana = 6064629489381870366ll;
    {   // bought or given during the session: the record reaches the client before it can be used
        Harness h(consumable_character({}), true, true, path.c_str());
        h.request(actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), h.pawn_ch);   // (the sync waits for the phase-in)
        std::string error;
        const auto cast = h.fx.content->item_cast(health, error);
        const auto before = client_loaded_records(h);
        CHECK(cast && std::find(before.begin(), before.end(), *cast) == before.end(), "with no potion carried its ability is not loaded");
        h.s->gm_give_item(h.conn, health, 1, Clock::now(), "the test");
        consumable_run(h);
        const auto after = client_loaded_records(h);
        CHECK(cast && std::find(after.begin(), after.end(), *cast) != after.end(), "a potion that arrives during the session gets its ability loaded on the client");
        CHECK(h.s->inventory().at(Bag, 0) != nullptr, "(the potion is in the bag)");
        consumable_trigger(h, 0, 690);
        consumable_run(h);
        CHECK(h.s->uses_applied() == 1 && !h.s->failed(), "and it can be used");
    }
    for (const auto [record, stat] : {std::pair{health, content::kGuidHealth}, std::pair{mana, content::kGuidMana}}) {
        const auto character = consumable_character({make_item(Bag, 0, record, 901, 2)});
        Harness drink(character, true, true, path.c_str()), control(character, true, true, path.c_str());
        auto* ds = const_cast<stats::CharacterStats*>(drink.s->character_stats());
        auto* cs = const_cast<stats::CharacterStats*>(control.s->character_stats());
        CHECK(ds && cs, "the potion test has real character stats");
        if (!ds || !cs) continue;
        ds->set_current(stat, 1.0f);
        cs->set_current(stat, 1.0f);
        consumable_trigger(drink, 0, 701);
        consumable_run(drink);
        consumable_run(control);
        CHECK(ds->find(stat)->value > cs->find(stat)->value + 10.0f,
              stat == content::kGuidHealth ? "a health potion heals beyond natural regeneration using its item potency"
                                          : "a mana potion restores mana beyond natural regeneration using its item potency");
        const auto* catalog = drink.fx.world->ability_catalog();
        std::string error;
        const auto cast = drink.fx.content->item_cast(record, error);
        const auto* definition = catalog->ability(*cast);
        if (!definition->hits.empty()) {
            auto* pipeline = const_cast<AbilityPipeline*>(drink.s->pipeline());
            const double now = std::chrono::duration<double>(drink.t - t0).count();
            const auto verdict = pipeline->hit_request(*cast, 0, definition->hits.front().hit_guid, 1, 1, now);
            CHECK(verdict == AbilityPipeline::HitVerdict::Quiet,
                  "a late client hit request cannot apply the consumable twice");
        }
        consumable_trigger(drink, 0, 702);
        consumable_run(drink);
        CHECK(drink.s->uses_applied() == 1 && drink.s->inventory().at(Bag, 0)->quantity == 1,
              "a potion on cooldown does not consume another item");
        drink.s->finish("consumable persistence test");
        CHECK(!drink.sink.inventories.empty() && drink.sink.inventories.back().items == drink.s->inventory().items(),
              "the spent consumable is saved when the session finishes");
    }
    {
        Harness h(consumable_character({make_item(Bag, 0, health, 902, 2)}), true, true, path.c_str());
        auto forged = items::to_instance(*h.s->inventory().at(Bag, 0));
        ++forged.guid.a;
        consumable_trigger(h, 0, 710, 0, forged);
        consumable_run(h);
        CHECK(h.s->uses_applied() == 0 && h.s->inventory().at(Bag, 0)->quantity == 2, "a forged item instance cannot grant a potion effect or consume an item");
        std::string error;
        const auto mana_cast = h.fx.content->item_cast(mana, error);
        consumable_trigger(h, 0, 711, *mana_cast);
        consumable_run(h);
        CHECK(h.s->uses_applied() == 0 && h.s->inventory().at(Bag, 0)->quantity == 2, "an owned health potion cannot authorize a different ability");
        h.request(item_used(3, 0, guid_n(902)), h.pawn_ch);
        CHECK(h.s->uses_applied() == 0 && h.s->inventory().at(Bag, 0)->quantity == 2, "the item-use preamble does not consume before its ability trigger");
        consumable_trigger(h, 0, 712);
        consumable_run(h);
        consumable_trigger(h, 0, 712);
        consumable_run(h);
        CHECK(h.s->uses_applied() == 1 && h.s->inventory().at(Bag, 0)->quantity == 1, "replaying the same client instance cannot consume twice");
    }
    {
        const auto character = consumable_character({make_item(Bag, 0, health, 905, 2)});
        Harness h(character, true, true, path.c_str()), control(character, true, true, path.c_str());
        auto* current = const_cast<stats::CharacterStats*>(h.s->character_stats());
        auto* baseline = const_cast<stats::CharacterStats*>(control.s->character_stats());
        current->set_current(content::kGuidHealth, 1.0f);
        baseline->set_current(content::kGuidHealth, 1.0f);
        const auto held = *h.s->inventory().at(Bag, 0);
        std::string error;
        const auto cast = h.fx.content->item_cast(health, error);
        const auto& guids = h.s->bootstrap().setup().guids;
        repl::ItemStorageEvent move;
        move.type = static_cast<uint8_t>(repl::StorageEventType::Move);
        move.item = items::to_instance(held);
        move.move_source.object = guids.backpack;
        move.move_source.slots.push_back({0, 2, move.item.guid});
        repl::StorageEventContainer destination;
        destination.object = guids.backpack;
        destination.slots.push_back({1, 1, {}});
        move.targets.push_back(destination);
        const auto& cg = h.s->bootstrap().setup().character_guid;
        move.instigator = repl::GuidValue{cg[0], cg[1], cg[2], cg[3]};
        h.client_sends({h.client.actor(h.pawn_ch, trigger_with_item(guids.ability, *cast, 730, 0, move.item, 3), true),
                        h.client.actor(h.pc_ch, storage_request(move), true)});
        consumable_run(h);
        consumable_run(control);
        CHECK(h.s->uses_applied() == 0 && !h.s->inventory().at(Bag, 0) && h.s->inventory().at(Bag, 1)
              && h.s->inventory().at(Bag, 1)->quantity == 2 && mirror_matches(h),
              "moving the stack before the cost cancels its cast without consuming or duplicating an item");
        CHECK(current->find(content::kGuidHealth)->value <= baseline->find(content::kGuidHealth)->value + 0.1f,
              "an unpaid consumable cast cannot grant free healing");
    }
    {
        constexpr int64_t gather = 6064632105722970126ll, process = 6064632105742434319ll;
        Harness h(consumable_character({make_item(Bag, 0, gather, 903, 2), make_item(Bag, 1, process, 904, 2)}), true, true, path.c_str());
        consumable_trigger(h, 0, 720);
        consumable_run(h);
        const auto before = h.s->character_stats()->effects();
        CHECK(!before.empty(), "an experience elixir applies a real active stat effect");
        consumable_trigger(h, 1, 721);
        consumable_run(h);
        const auto& after = h.s->character_stats()->effects();
        CHECK(h.s->uses_applied() == 2 && before.size() == 1 && after.size() == 1 && before.front().effect.guid != after.front().effect.guid,
              "a second elixir replaces the first, respecting the one-elixir effect group");
        const auto* effect = after.empty() ? nullptr : h.fx.world->ability_catalog()->effect(after.front().effect.guid);
        CHECK(effect && effect->row.group_limit == 1 && !after.front().mods.empty()
              && std::any_of(after.front().mods.begin(), after.front().mods.end(), [](const auto& mod) { return mod.value != 0.0; }),
              "elixir modifiers have nonzero potency from the consumed item");
    }
}

