// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_ground_crate_tests.inc: the crates on the ground (items/ground_crates.h, session/player_ground_crate.cpp), on a connection
//
//    laying down      Drop Crate's notify takes the crate off the back (a Remove of the cargo slot) and lays it in front of the character: a world actor of the game's crate
//                     prop, opened on the client, whose interactable is ICD_CargoActor with its player interaction, allowed (CanInputActivate) and read back from the open
//    picking up       the client's ServerInteractWithInteractableComponent on its component, then the 1 s channel: the same crate (instance, quality) is on the back again,
//                     and the actor is gone from the world
//    refused          a use while a crate is carried, and a crate the switch keeps off the ground (it goes into the item bag, as before)
// ============================================================================

namespace ground_crate_test {

constexpr int64_t kCrateRecord = 9'100'000'001ll;   // a crate of this test's catalog: its record lists the cargo slot alone

/// The fixture's catalog with one crate more.
struct CrateCatalog : items::ItemCatalog {
    const items::ItemCatalog* base = nullptr;
    std::optional<items::ItemInfo> info(int64_t record) const override {
        if (record == kCrateRecord) {
            items::ItemInfo i;
            i.equip_slots = {items::equip::CargoHold};
            return i;
        }
        return base ? base->info(record) : std::nullopt;
    }
};

static svc::Character carrier() {
    std::vector<svc::InventoryItem> items = kit();
    svc::InventoryItem crate = make_item(Worn, items::equip::CargoHold, kCrateRecord, 700);
    crate.quality = 3;
    items.push_back(crate);
    return cyb_record(std::move(items));
}

struct CrateHarness : Harness {
    CrateCatalog                       catalog;
    std::optional<spawns::GenericNpcs> generic;
    const spawns::OpenTemplate*        crate_open = nullptr;
    explicit CrateHarness(bool crates_on = true, const std::string& file = {}) : Harness(carrier()) {
        catalog.base = fx.world->item_catalog();
        fx.world->set_item_catalog(&catalog);
        generic.emplace(*fx.captured, nullptr, spawns::GenericConfig{});
        content::NpcAssetRow asset;
        asset.archetype_path = items::kGroundCrateArchetype;
        crate_open = generic->prop_template(asset);
        fx.world->set_ground_crate_template(crate_open);
        fx.world->set_ground_crates_enabled(crates_on);
        fx.world->set_ground_crates_file(file);
        start();
    }
    const svc::InventoryItem* on_back() const { return s->inventory().at(Worn, items::equip::CargoHold); }
    /// The world's one crate actor (0: none).
    uint64_t crate_actor() const {
        uint64_t id = 0;
        fx.world->registry().for_each([&](const world::WorldActor& a) {
            if (fx.world->ground_crate(a.id)) id = a.id;
        });
        return id;
    }
};

static void test_ground_crate_flow() {
    std::printf("the crates on the ground: Drop Crate lays the crate down, and a use picks it up\n");
    CrateHarness h;
    CHECK(h.crate_open && h.crate_open->category == "prop" && h.crate_open->archetype_path == items::kGroundCrateArchetype,
          "the crate's actor is made from a recorded quest object with the game's crate prop as its Blueprint");
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.on_back() && h.on_back()->item_record_id == kCrateRecord, "the character carries the crate");
    h.s->drop_crate_for_test(h.conn, h.t);
    h.settle();
    const uint64_t actor = h.crate_actor();
    const world::WorldActor* a = actor ? h.fx.world->registry().find(actor) : nullptr;
    CHECK(!h.on_back() && h.fx.world->ground_crates() == 1 && h.s->crates_laid() == 1, "Drop Crate takes the crate off the back and lays it on the ground");
    CHECK(a && a->kind == world::ActorKind::Prop && a->npc.interact_component != 0 && a->prop.component_def == items::kIcdCargoActor && a->prop.interact_def == items::kPidCargoActor,
          "the crate is a world actor whose interactable is ICD_CargoActor with its player interaction");
    if (!a) return;
    CHECK(std::hypot(a->x - kCx, a->y - kCy) < 200.0, "it lies in front of the character");
    bool defined = false;
    for (const repl::Subobject& sub : a->actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UInteractableComponent") continue;
        const repl::Value* v = sub.obj.state.get(*sub.obj.schema, "ComponentDefId.Guid");
        defined = v && v->get_if<int64_t>() && *v->get_if<int64_t>() == items::kIcdCargoActor;
    }
    CHECK(defined, "its interactable component names ICD_CargoActor");
    h.run(milliseconds(1500));
    CHECK(h.s->actor_view()->state_of(actor) == world::ClientState::Awake, "the client has the crate open");
    const auto chains = chains_of(h, actor);
    const auto seen = chains.empty() ? std::nullopt : read_interact(*chains.front(), *a);
    CHECK(seen && !seen->can_input.empty() && seen->can_input[0], "the open reads back, and its first input is allowed (CanInputActivate)");

    // the pick-up: the use, then the channel
    const svc::InventoryItem laid = h.fx.world->ground_crate(actor)->item;
    h.client_request(interact(a->npc.interact_component), h.pawn_ch);
    CHECK(!h.on_back() && h.fx.world->ground_crates() == 1, "the crate is not on the back before the channel ends");
    h.run(milliseconds(1300));
    h.settle();
    CHECK(h.on_back() && h.on_back()->instance_guid == laid.instance_guid && h.on_back()->quality == 3, "after the channel the same crate is on the back again");
    CHECK(h.fx.world->ground_crates() == 0 && !h.fx.world->registry().find(actor) && h.s->crates_picked() == 1, "and it is gone from the ground and the world");
}

static void test_ground_crate_refusals() {
    std::printf("the crates on the ground: what is refused\n");
    {   // a crate is carried: another cannot be picked up
        CrateHarness h;
        h.play();
        h.run(milliseconds(1500));
        h.s->drop_crate_for_test(h.conn, h.t);
        h.settle();
        const uint64_t actor = h.crate_actor();
        CHECK(actor != 0, "a crate lies on the ground");
        // a second crate on the back (a game master's)
        svc::InventoryItem other = make_item(Worn, items::equip::CargoHold, kCrateRecord, 701);
        std::string why;
        const auto second = h.fx.world->lay_crate(h.fx.world->registry(), other, "someone", "someone", kCx + 50, kCy, bootstrap::kDefaultSpawn.z - 88, 0, 0, why);
        CHECK(second.has_value(), "another crate can lie beside it");
        h.run(milliseconds(1500));
        const world::WorldActor* a = h.fx.world->registry().find(actor);
        if (a) h.client_request(interact(a->npc.interact_component), h.pawn_ch);
        h.run(milliseconds(1300));
        h.settle();
        CHECK(h.on_back() && h.fx.world->ground_crates() == 1, "the first is picked up");
        const world::WorldActor* b = second ? h.fx.world->registry().find(*second) : nullptr;
        if (b) h.client_request(interact(b->npc.interact_component), h.pawn_ch);
        h.run(milliseconds(1300));
        h.settle();
        CHECK(h.on_back() && h.on_back()->instance_guid == guid_n(700) && h.fx.world->ground_crates() == 1, "the second is refused while a crate is carried: it stays on the ground");
    }
    {   // the switch off: the item bag, as before
        CrateHarness h(/*crates_on=*/false);
        h.play();
        h.run(milliseconds(1500));
        h.s->drop_crate_for_test(h.conn, h.t);
        h.settle();
        bool in_bag = false;
        for (const svc::InventoryItem& it : h.s->inventory().items()) in_bag = in_bag || (it.item_record_id == kCrateRecord && it.container == Bag);
        CHECK(!h.on_back() && in_bag && h.fx.world->ground_crates() == 0, "with the crates on the ground off, Drop Crate puts the crate in the item bag");
    }
}

static size_t crates_in_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const std::string s = text.str();
    size_t n = 0;
    for (size_t at = s.find("\"instance\""); at != std::string::npos; at = s.find("\"instance\"", at + 1)) ++n;
    return n;
}

static void test_ground_crate_kept() {
    std::printf("the crates on the ground outlive a restart (--ground-crates-file)\n");
    const std::string path = (std::filesystem::temp_directory_path() / "aoc_test_ground_crates.json").string();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::string instance;
    {
        CrateHarness h(true, path);
        h.fx.world->tick_world(10.0);   // (its first tick: no file yet, nothing to lay out)
        h.play();
        h.run(milliseconds(1500));
        h.s->drop_crate_for_test(h.conn, h.t);
        h.settle();
        const items::GroundCrate* laid = h.fx.world->ground_crate(h.crate_actor());
        CHECK(laid != nullptr, "(a crate lies on the ground)");
        if (laid) instance = laid->item.instance_guid;
        CHECK(std::filesystem::exists(path) && crates_in_file(path) == 1, "laying it down writes it to the file");
    }
    {
        CrateHarness h(true, path);   // the world after a restart
        CHECK(h.fx.world->ground_crates() == 0, "(a new world holds nothing until its first tick)");
        h.fx.world->tick_world(100.0);
        const uint64_t actor = h.crate_actor();
        const items::GroundCrate* back = actor ? h.fx.world->ground_crate(actor) : nullptr;
        CHECK(h.fx.world->ground_crates() == 1 && back && back->item.instance_guid == instance && back->item.quality == 3 && back->item.item_record_id == kCrateRecord,
              "after the restart the same crate lies on the ground again");
        CHECK(back && back->back_at > 100.0 + items::kGroundCrateBackSeconds - 120.0 && back->back_at <= 100.0 + items::kGroundCrateBackSeconds,
              "and keeps the time it had left before going back");
        const world::WorldActor* a = actor ? h.fx.world->registry().find(actor) : nullptr;
        CHECK(a && a->prop.component_def == items::kIcdCargoActor, "as a crate actor that can be picked up");
        CHECK(h.fx.world->take_crate(h.fx.world->registry(), actor).has_value() && crates_in_file(path) == 0, "picking it up takes it out of the file");
    }
    {
        std::ofstream bad(path, std::ios::binary | std::ios::trunc);
        bad << "{ not json";
    }
    {
        CrateHarness h(true, path);
        h.fx.world->tick_world(5.0);
        CHECK(h.fx.world->ground_crates() == 0 && std::filesystem::exists(path + ".unread"), "a file that does not read is copied aside, not thrown away");
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".unread", ec);
}

AOC_REGISTER_TEST(test_ground_crate_flow);
AOC_REGISTER_TEST(test_ground_crate_refusals);
AOC_REGISTER_TEST(test_ground_crate_kept);

}  // namespace ground_crate_test
