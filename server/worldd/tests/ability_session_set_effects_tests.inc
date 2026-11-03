// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_set_effects_tests.inc: the effects of the worn sets (contentc version 43, set_effect_bonus)
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
//
//  The ArmorType sets give nothing but an effect (their SetStatBonuses are empty): five pieces of heavy armour of Common rarity put ArmorType_Heavy_1 on the character
//  (PhysicalMitigation +0.1), four do not.  The effect is the ability runtime's: its stat mod changes the stats, not only the icon.
// ============================================================================

static constexpr int64_t kArmorTypeHeavy1 = 6064633946081984518ll;    // ArmorType_Heavy_1: PhysicalMitigation +0.1 for five Common pieces of the ArmorType_Heavy set
static constexpr int64_t kArmorTypeHeavySet = 6064632299839554424ll;  // the set (SetBonus ArmorType_Heavy)
static constexpr int64_t kPhysicalMitigation = 109343399171798ll;

/// A level-25 Bard wearing the first `pieces` of the Implacable heavy armour (level 1, quality 1: Common), each in its own slot.
static svc::Character heavy_bard(int pieces) {
    static constexpr struct {
        int64_t item;
        int32_t slot;
    } kPieces[] = {
        {6064632072381464604ll, 2},   // Gear_Armor_Heavy_Implacable_Chest
        {6064632072411283493ll, 6},   // ... Feet
        {6064632072414822438ll, 3},   // ... Forearms (Wrists)
        {6064632072818065415ll, 4},   // ... Hands
        {6064632072818327560ll, 1},   // ... Head (Helmet)
    };
    svc::Character c = bard25();
    for (int i = 0; i < pieces && i < 5; ++i) {
        svc::InventoryItem it = stack_of(kPieces[i].item, kPieces[i].slot, 1, 92000u + static_cast<uint32_t>(i));
        it.container = svc::ItemContainer::Equipment;
        c.items.push_back(it);
    }
    return c;
}

static void test_set_effects() {
    std::printf("the worn sets' effects (contentc version 43): five pieces of heavy armour put ArmorType_Heavy_1 on the character and its PhysicalMitigation; four do not\n");
    const svc::Character four = heavy_bard(4), five = heavy_bard(5);
    Run a(0, true, &four);
    phase_in(a);   // (the effects go on once the client has phased in, as the crate's do)
    a.play(1.0, false);
    Run b(0, true, &five);
    phase_in(b);
    b.play(1.0, false);
    const AbilityPipeline* pa = a.h.s->pipeline();
    const AbilityPipeline* pb = b.h.s->pipeline();
    const content::ContentDb* db = b.h.fx.world->content();
    CHECK(pa && pb && db, "both sessions run the generic ability runtime on the real content.db");
    if (!pa || !pb || !db) return;
    std::string error;
    const auto set = db->set_bonus(kArmorTypeHeavySet, error);
    if (!set || set->effects.empty()) {
        std::printf("  [skip] the content.db (contentc version %s) holds no effect of the ArmorType_Heavy set: it predates version 43\n", db->meta().contentc_version.c_str());
        return;
    }
    CHECK(set->tiers.empty() && set->effects.size() == 6 && set->effects[0].pieces == 5 && set->effects[0].effect == kArmorTypeHeavy1 && set->effects[0].min_rarity == 0 &&
              set->effects[0].max_rarity == 0 && set->effects[5].min_rarity == 5 && set->effects[5].max_rarity == 6,
          "the set has no stat bonus and six effects of five pieces, one for each rarity (Legendary and Artifact share the last)");
    CHECK(a.h.s->character_stats() && a.h.s->character_stats()->unknown_sets() == 0, "the set is no longer unknown to the stats");
    CHECK(pa->stacks_of(kArmorTypeHeavy1) == 0, "four pieces: no effect");
    CHECK(pb->stacks_of(kArmorTypeHeavy1) == 1, "five pieces: ArmorType_Heavy_1 is on the character, one stack");
    const stats::CharacterStats* cs = b.h.s->character_stats();
    if (!cs) return;
    bool mod = false;
    for (const stats::ActiveEffect& e : cs->effects()) {
        if (e.effect.guid != kArmorTypeHeavy1) continue;
        for (const stats::EffectMod& m : e.mods) mod = mod || (m.stat == kPhysicalMitigation && std::fabs(m.value - 0.1) < 1e-6);
    }
    CHECK(mod, "the stats run with its mod: PhysicalMitigation 0.1");
    // the same character's stats without the effect: PhysicalMitigation differs by what the effect adds
    stats::CharacterStats without = *cs;
    std::vector<stats::ActiveEffect> others;
    for (const stats::ActiveEffect& e : cs->effects()) {
        if (e.effect.guid != kArmorTypeHeavy1) others.push_back(e);
    }
    without.set_effects(others);
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    const bool ran = without.prepare(bootstrap::worn_items_of(b.h.s->inventory()), changed, next, error);
    const stats::StatValue* with_effect = cs->find(kPhysicalMitigation);
    float bare = -1;
    for (const stats::StatValue& v : next.values) {
        if (v.guid == kPhysicalMitigation) bare = v.value;
    }
    std::printf("  [info] PhysicalMitigation %.4f with the set's effect, %.4f without\n", with_effect ? with_effect->value : -1.0f, bare);
    CHECK(ran && with_effect && bare >= 0 && std::fabs(with_effect->value - bare) > 1e-4f, "the effect changes the character's PhysicalMitigation (it is not only an icon)");
    const stats::StatValue* four_value = a.h.s->character_stats() ? a.h.s->character_stats()->find(kPhysicalMitigation) : nullptr;
    CHECK(four_value && with_effect && std::fabs(four_value->value - with_effect->value) > 1e-4f, "... and the character with four pieces has another PhysicalMitigation");
}
AOC_REGISTER_TEST(test_set_effects);

/// The client takes a worn item off (ServerItemUsed on the Equipment storage, 4): it goes to the bag.
static void take_off(Run& r, int32_t slot, uint32_t serial) {
    const schema::RpcSchema* rpc = class_named("PlayerPawn_C").rpc("ServerItemUsed");
    if (!rpc) {
        std::printf("  [FAIL] no RPC ServerItemUsed\n");
        return;
    }
    const auto g = items::guid_of(guid_hex_n(serial));
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    repl::RpcParams p;
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(4));
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "ServerItemUsed", p));
}

static void test_set_effects_follow_the_gear() {
    std::printf("the worn sets' effects follow the gear: a piece taken off takes the effect off, its stats with it\n");
    const svc::Character five = heavy_bard(5);
    Run r(0, true, &five);
    phase_in(r);
    r.play(1.0, false);
    const AbilityPipeline* p = r.h.s->pipeline();
    const content::ContentDb* db = r.h.fx.world->content();
    std::string error;
    const auto set = db ? db->set_bonus(kArmorTypeHeavySet, error) : std::nullopt;
    if (!p || !set || set->effects.empty()) {
        std::printf("  [skip] no ability runtime, or a content.db older than contentc version 43\n");
        return;
    }
    CHECK(p->stacks_of(kArmorTypeHeavy1) == 1, "five pieces worn: the effect is on");
    const stats::StatValue* on = r.h.s->character_stats() ? r.h.s->character_stats()->find(kPhysicalMitigation) : nullptr;
    const float with_effect = on ? on->value : -1.0f;
    take_off(r, 1, 92004);   // the Implacable Head (Helmet, slot 1)
    r.play(0.5, false);
    CHECK(r.h.s->inventory().at(svc::ItemContainer::Equipment, 1) == nullptr, "(the helmet is off)");
    CHECK(p->stacks_of(kArmorTypeHeavy1) == 0, "four pieces left: the effect is taken off");
    const stats::StatValue* off = r.h.s->character_stats() ? r.h.s->character_stats()->find(kPhysicalMitigation) : nullptr;
    std::printf("  [info] PhysicalMitigation %.4f with five pieces, %.4f with four\n", with_effect, off ? off->value : -1.0f);
    CHECK(off && std::fabs(off->value - with_effect) > 1e-4f, "... and the stats follow");
    CHECK(!r.h.s->abilities_failed(), "(nothing failed)");
}
AOC_REGISTER_TEST(test_set_effects_follow_the_gear);
