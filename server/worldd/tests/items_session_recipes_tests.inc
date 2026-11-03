// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_recipes_tests.inc: a recipe item used from the bag teaches its recipe (activation LearnRecipes, contentc version 44's item_recipe)
//  (registered with AOC_REGISTER_TEST: test_worldd_items_session.cpp runs it through aoc_tests::run; it is included from items_session_registered.inc)
// ============================================================================

namespace recipes_fx {
inline constexpr int64_t kThistlemawItem = 6064633921833206357ll;     // Consumable_Recipe_Tailoring_T1_Armor_Thistlemaw_Waist
inline constexpr int64_t kThistlemawRecipe = 6064634092895471350ll;   // Tailoring_T1_Armor_Thistlemaw_Waist (learnable)
inline constexpr int64_t kObsoleteItem = 6064630446994030854ll;       // OBSOLETE_Consumable_Recipe_Tailoring_T2_Armor_ArcaneAegis_Chest: its recipe is not learnable
inline constexpr int64_t kEmptyItem = 6064634597980045319ll;          // Consumable_Recipe_Weaponsmithing_T3_Weapon_Sword_2H_Xiryth: names no recipe
}  // namespace recipes_fx

void test_recipe_item_teaches() {
    using namespace recipes_fx;
    const char* path = professions_db_path();
    if (!path) {
        std::printf("a recipe item teaches its recipe: skipped (no real content.db)\n");
        return;
    }
    svc::Character rec = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto& it : rec.items)
        if (it.container == Worn) items.push_back(it);
    items.push_back(make_item(Bag, 4, kThistlemawItem, 320, 2));
    items.push_back(make_item(Bag, 5, kObsoleteItem, 321, 1));
    items.push_back(make_item(Bag, 6, kEmptyItem, 322, 1));
    rec.items = items;
    Harness h(rec, true, true, path);
    h.fx.world->load_crafting_data();   // (load_area reads the recipes; the harness loads no area)
    std::string err;
    const auto use = h.fx.content->item_use(kThistlemawItem, err);
    if (!use || !use->recipes_listed) {
        std::printf("a recipe item teaches its recipe: skipped (the content.db is older than contentc 44)\n");
        return;
    }
    std::printf("a recipe item teaches its recipe\n");
    CHECK(use->activation == "LearnRecipes" && use->recipes == std::vector<int64_t>{kThistlemawRecipe}, "(content.db: the Thistlemaw Waist recipe item teaches its one recipe)");
    CHECK(h.s->taught_recipes().empty(), "(the character knows no taught recipe)");

    const size_t saves = h.sink.inventories.size();
    h.request_both(item_used(3, 4, guid_n(320)), item_activate(3, 4, guid_n(320)));
    const svc::InventoryItem* left = h.s->inventory().at(Bag, 4);
    CHECK(h.s->taught_recipes() == std::vector<int64_t>{kThistlemawRecipe}, "using the item teaches its recipe");
    CHECK(left && left->quantity == 1, "one of the stack of 2 is used up");
    CHECK(h.sink.inventories.size() == saves + 1, "the inventory, which carries the recipe list, is saved at once");
    CHECK(h.s->uses_applied() == 1 && h.s->uses_refused() == 0, "the use counts as applied");

    h.request_both(item_used(3, 4, guid_n(320)), item_activate(3, 4, guid_n(320)));
    left = h.s->inventory().at(Bag, 4);
    CHECK(h.s->taught_recipes().size() == 1 && left && left->quantity == 1 && h.s->uses_refused() == 1,
          "the same recipe again is refused and the item stays: nothing is learned twice");

    h.request_both(item_used(3, 5, guid_n(321)), item_activate(3, 5, guid_n(321)));
    CHECK(h.s->inventory().at(Bag, 5) && h.s->taught_recipes().size() == 1 && h.s->uses_refused() == 2,
          "an item whose recipe every character knows (not learnable) teaches nothing and stays");

    h.request_both(item_used(3, 6, guid_n(322)), item_activate(3, 6, guid_n(322)));
    CHECK(h.s->inventory().at(Bag, 6) && h.s->taught_recipes().size() == 1 && h.s->uses_refused() == 3,
          "an item that names no recipe teaches nothing and stays");
    CHECK(!h.s->failed(), "the session goes on");
}
AOC_REGISTER_TEST(test_recipe_item_teaches);
