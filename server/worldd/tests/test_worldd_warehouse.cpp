// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/test_worldd_warehouse.cpp
//
//  warehouse/warehouse.h with no network and no database: stacks that split and join across the window, moves between containers of a kind, crates in the cargo tab, and the rules of buying room.
//  The purchase's rules are the client's copy of the retail code; the split and the join in the warehouse are modelled on the bags' (no retail capture shows them).
// ============================================================================
#include "warehouse/warehouse.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace aoc::worldd;
using svc::ItemContainer;
namespace repl = ::aoc::protocol::repl;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

constexpr int64_t kPotion = 100, kSword = 101, kOre = 102, kCrate = 103;
constexpr int64_t kNode = 7000, kBaseChunk = 500, kVillageChunk = 501, kUnlisted = 502;
constexpr int64_t kBaseTab = 600, kVillageTab = 601, kCitizenTab = 602, kCargoTab = 603;
constexpr int64_t kGold = 9000;
const std::string kVillageRule = "EnumInt(Node_GetLevel(GetCurrentNode(GetPlayer()))) >= EnumInt(ENodeLevel::Village)";

class Catalog : public items::ItemCatalog {
public:
    std::optional<items::ItemInfo> info(int64_t record) const override {
        items::ItemInfo i;
        switch (record) {
            case kPotion: i.max_stack = 10; return i;
            case kSword: return i;
            case kOre: i.resource = true; i.max_stack = 20; return i;
            case kCrate: i.equip_slots = {items::equip::CargoHold}; return i;
            default: return std::nullopt;
        }
    }
};

std::string guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
    return buf;
}

svc::InventoryItem item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    return it;
}

items::Inventory bags_of(const std::vector<svc::InventoryItem>& held, const Catalog& catalog) {
    std::string error;
    auto inv = items::Inventory::from_items(held, error, &catalog);
    if (!inv) {
        std::printf("  [FAIL] the bags do not load: %s\n", error.c_str());
        g_fail++;
        return items::Inventory{};
    }
    return *inv;
}

const warehouse::Key kChunk{warehouse::Area::Items, 0, kBaseChunk}, kChunk2{warehouse::Area::Items, 0, kVillageChunk};
const warehouse::Key kTab{warehouse::Area::Materials, kNode, kBaseTab}, kTab2{warehouse::Area::Materials, kNode, kVillageTab};
const warehouse::Key kCargo{warehouse::Area::Cargo, kNode, kCargoTab};

warehouse::OpenWindow window() {
    warehouse::OpenWindow w;
    w.node = kNode;
    warehouse::Spec chunk;
    chunk.key = kChunk;
    chunk.slots = 40;
    warehouse::Spec chunk2 = chunk;
    chunk2.key = kChunk2;
    chunk2.slots = 8;
    warehouse::Spec tab;
    tab.key = kTab;
    tab.columns = 8;
    tab.rows = 16;
    tab.slots = 128;
    warehouse::Spec tab2 = tab;
    tab2.key = kTab2;
    tab2.index = 1;
    warehouse::Spec cargo;
    cargo.key = kCargo;
    cargo.columns = 6;
    cargo.rows = 8;
    cargo.slots = 48;
    w.specs = {chunk, chunk2, tab, tab2, cargo};
    return w;
}

warehouse::End in_wh(const warehouse::Key& key, int slot) {
    warehouse::End e;
    e.in_warehouse = true;
    e.key = key;
    e.slot = slot;
    return e;
}

warehouse::End in_bag(ItemContainer c, int slot) {
    warehouse::End e;
    e.bag = c;
    e.slot = slot;
    return e;
}

warehouse::Request move(const warehouse::End& from, const warehouse::End& to, int32_t count, const std::string& guid) {
    warehouse::Request r;
    r.type = repl::StorageEventType::Move;
    r.from = from;
    r.to = to;
    r.count = count;
    r.guid = guid;
    r.new_guid = guid_n(999);
    return r;
}

void test_split() {
    std::printf("a part of a stack makes a stack of its own, both ways\n");
    Catalog catalog;
    items::Inventory bags = bags_of({item(ItemContainer::Backpack, 0, kPotion, 1, 8)}, catalog);
    warehouse::Storage wh;
    const auto open = window();
    auto res = warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Backpack, 0), in_wh(kChunk, 3), 3, guid_n(1)));
    CHECK(res.ok && res.bags_changed, "3 of 8 go into the chunk");
    CHECK(bags.at(ItemContainer::Backpack, 0) && bags.at(ItemContainer::Backpack, 0)->quantity == 5, "the bag keeps 5");
    CHECK(wh.at(kChunk, 3) && wh.at(kChunk, 3)->quantity == 3 && wh.at(kChunk, 3)->instance_guid == guid_n(999), "the chunk holds 3 of a new instance");
    CHECK(res.steps.size() == 2 && res.steps[0].count == 3 && res.steps[1].type == repl::StorageEventType::Add, "the Remove names how many leave, then the Add");

    res = warehouse::transfer(bags, wh, catalog, open, move(in_wh(kChunk, 3), in_bag(ItemContainer::Backpack, 4), 1, guid_n(999)));
    CHECK(!res.ok, "the guid of the new stack is refused when it is the source's own");   // (the session mints a fresh one for each request)
    auto r = move(in_wh(kChunk, 3), in_bag(ItemContainer::Backpack, 4), 1, guid_n(999));
    r.new_guid = guid_n(998);
    res = warehouse::transfer(bags, wh, catalog, open, r);
    CHECK(res.ok && wh.at(kChunk, 3)->quantity == 2 && bags.at(ItemContainer::Backpack, 4)->quantity == 1, "1 of 3 comes back out");

    r = move(in_wh(kChunk, 3), in_wh(kChunk, 4), 5, guid_n(999));
    CHECK(!warehouse::transfer(bags, wh, catalog, open, r).ok, "a count above the stack is refused");
    r = move(in_wh(kChunk, 3), in_wh(kChunk, 4), 0, guid_n(999));
    CHECK(!warehouse::transfer(bags, wh, catalog, open, r).ok, "a count of 0 is refused");
    CHECK(wh.at(kChunk, 3)->quantity == 2, "a refusal changes nothing");
}

void test_join() {
    std::printf("a move onto a stack of the same item joins it, as much as fits\n");
    Catalog catalog;
    items::Inventory bags = bags_of({item(ItemContainer::Backpack, 0, kPotion, 1, 5), item(ItemContainer::Backpack, 1, kPotion, 2, 4), item(ItemContainer::Backpack, 2, kSword, 3)}, catalog);
    warehouse::Storage wh;
    wh.put(kChunk, 0, item(ItemContainer::Backpack, 0, kPotion, 4, 8));
    wh.put(kChunk, 1, item(ItemContainer::Backpack, 1, kPotion, 5, 10));
    const auto open = window();
    auto res = warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Backpack, 0), in_wh(kChunk, 0), 5, guid_n(1)));
    CHECK(res.ok && wh.at(kChunk, 0)->quantity == 10 && bags.at(ItemContainer::Backpack, 0)->quantity == 3, "5 onto 8: 2 fit, 3 stay");
    CHECK(res.steps.size() == 3 && res.steps[0].count == 2 && res.steps[2].item.quantity == 10, "the source's Remove of 2, the target's Remove and Add");
    CHECK(!warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Backpack, 1), in_wh(kChunk, 1), 4, guid_n(2))).ok, "a full stack takes nothing");
    CHECK(!warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Backpack, 2), in_wh(kChunk, 1), 1, guid_n(3))).ok, "another item is no join");

    wh.erase(kChunk, 1);
    wh.put(kChunk, 1, item(ItemContainer::Backpack, 1, kPotion, 5, 2));
    res = warehouse::transfer(bags, wh, catalog, open, move(in_wh(kChunk, 1), in_bag(ItemContainer::Backpack, 1), 2, guid_n(5)));
    CHECK(res.ok && !wh.at(kChunk, 1) && bags.at(ItemContainer::Backpack, 1)->quantity == 6 && bags.at(ItemContainer::Backpack, 1)->instance_guid == guid_n(2), "a chunk's whole stack joins the bag's, which keeps its instance");
    auto fill = move(in_wh(kChunk, 0), in_wh(kChunk, 9), 1, guid_n(4));
    fill.type = repl::StorageEventType::Fill;
    CHECK(!warehouse::transfer(bags, wh, catalog, open, fill).ok, "a Fill needs a stack to join");
}

void test_containers() {
    std::printf("between containers of a kind\n");
    Catalog catalog;
    items::Inventory bags = bags_of({item(ItemContainer::Backpack, 0, kSword, 1)}, catalog);
    warehouse::Storage wh;
    wh.put(kChunk, 0, item(ItemContainer::Backpack, 0, kPotion, 2, 4));
    wh.put(kTab, 0, item(ItemContainer::Backpack, 0, kOre, 3, 9));
    const auto open = window();
    CHECK(warehouse::transfer(bags, wh, catalog, open, move(in_wh(kChunk, 0), in_wh(kChunk2, 2), 4, guid_n(2))).ok && wh.at(kChunk2, 2) && !wh.at(kChunk, 0), "from one item chunk to another");
    CHECK(warehouse::transfer(bags, wh, catalog, open, move(in_wh(kTab, 0), in_wh(kTab2, 5), 9, guid_n(3))).ok && wh.at(kTab2, 5) && !wh.at(kTab, 0), "from one material tab to another");
    CHECK(!warehouse::transfer(bags, wh, catalog, open, move(in_wh(kTab2, 5), in_wh(kChunk, 1), 9, guid_n(3))).ok, "not from a tab to a chunk");
    CHECK(!warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Backpack, 0), in_wh(kTab, 0), 1, guid_n(1))).ok, "not gear into a material tab");
    warehouse::OpenWindow chunk_only;
    chunk_only.node = kNode;
    chunk_only.specs = {open.specs[0]};
    CHECK(!warehouse::transfer(bags, wh, catalog, chunk_only, move(in_bag(ItemContainer::Backpack, 0), in_wh(kChunk2, 0), 1, guid_n(1))).ok, "not into a container the window has closed");
}

void test_cargo() {
    std::printf("crates go into the cargo tab and back onto the back\n");
    Catalog catalog;
    items::Inventory bags = bags_of({item(ItemContainer::Backpack, 0, kSword, 1), item(ItemContainer::Equipment, items::equip::CargoHold, kCrate, 2)}, catalog);
    warehouse::Storage wh;
    const auto open = window();
    CHECK(!warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Backpack, 0), in_wh(kCargo, 0), 1, guid_n(1))).ok, "a sword is no crate");
    auto res = warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Equipment, items::equip::CargoHold), in_wh(kCargo, 7), 1, guid_n(2)));
    CHECK(res.ok && res.cargo_changed && !bags.at(ItemContainer::Equipment, items::equip::CargoHold) && wh.at(kCargo, 7), "the crate on the back is stored");
    res = warehouse::transfer(bags, wh, catalog, open, move(in_wh(kCargo, 7), in_bag(ItemContainer::Equipment, items::equip::CargoHold), 1, guid_n(2)));
    CHECK(res.ok && res.cargo_changed && !wh.at(kCargo, 7) && bags.at(ItemContainer::Equipment, items::equip::CargoHold), "and comes back onto the back");
    wh.put(kCargo, 1, item(ItemContainer::Backpack, 1, kCrate, 3));
    CHECK(!warehouse::transfer(bags, wh, catalog, open, move(in_wh(kCargo, 1), in_bag(ItemContainer::Equipment, items::equip::CargoHold), 1, guid_n(3))).ok && wh.at(kCargo, 1), "not onto a back that holds a crate");
    CHECK(!warehouse::transfer(bags, wh, catalog, open, move(in_bag(ItemContainer::Equipment, 1), in_wh(kCargo, 2), 1, guid_n(1))).ok, "no other worn slot exchanges items with the warehouse");
}

warehouse::Book book() {
    content::WarehouseData d;
    d.nodes[kNode] = {/*resource_def=*/1, /*crate_def=*/2, /*chunks_def=*/3};
    const auto gold = [](const char* v) { content::WarehouseCostRow c; c.currency = kGold; c.value = v; c.multiplier = "1"; return std::vector<content::WarehouseCostRow>{c}; };
    d.chunk_defs[3] = {kBaseChunk, kVillageChunk};
    d.chunks[kBaseChunk] = {kBaseChunk, "Default", 40, 0, "", {}};
    d.chunks[kVillageChunk] = {kVillageChunk, "Level3_General", 8, 3, kVillageRule, gold("5")};
    d.chunks[kUnlisted] = {kUnlisted, "Small_1", 8, 0, "", gold("1")};
    d.defs[1] = {1, "Default", 8, 16, {kBaseTab, kVillageTab, kCitizenTab}};
    d.defs[2] = {2, "DefaultCrate", 6, 8, {kCargoTab}};
    d.tabs[kBaseTab] = {kBaseTab, "BaseStorage", 0, false, false, "true", {}};
    d.tabs[kVillageTab] = {kVillageTab, "Level3_General", 0, false, true, kVillageRule, gold("5")};
    d.tabs[kCitizenTab] = {kCitizenTab, "Level3_Citizen", 0, true, true, "Node_IsPlayerCitizenOfNode(GetPlayer())", gold("1")};
    d.tabs[kCargoTab] = {kCargoTab, "DefaultCargoTab", 1, false, false, kVillageRule, {}};
    return warehouse::Book(std::move(d));
}

void test_buying() {
    std::printf("buying room\n");
    const warehouse::Book b = book();
    warehouse::Facts low, village;
    low.node = village.node = kNode;
    low.level = 1;
    village.level = 3;
    std::set<warehouse::Key> bought;

    auto p = b.purchase(kNode, kVillageChunk, false, low, bought);
    CHECK(!p.refusal.empty(), "a Village chunk is not for sale below Village");
    p = b.purchase(kNode, kVillageChunk, false, village, bought);
    CHECK(p.refusal.empty() && p.costs.size() == 1 && p.costs[0].first == kGold && p.costs[0].second == 5.0 && p.key == kChunk2, "at Village it costs 5 of its currency");
    CHECK(!b.purchase(kNode, kUnlisted, false, village, bought).refusal.empty(), "a chunk the town's def does not list");
    CHECK(!b.purchase(kNode, kBaseChunk, false, village, bought).refusal.empty(), "the free base chunk is not bought");
    CHECK(!b.purchase(kNode + 1, kVillageChunk, false, village, bought).refusal.empty(), "a town with no warehouse");
    CHECK(!b.purchase(kNode, kCitizenTab, true, village, bought).refusal.empty(), "a citizen's tab, and nobody is a citizen");
    p = b.purchase(kNode, kVillageTab, true, village, bought);
    CHECK(p.refusal.empty() && p.key == kTab2, "a material tab, kept per town");

    const size_t before = b.owned(kNode, village).size();
    bought.insert(kChunk2);
    bought.insert(kTab2);
    CHECK(!b.purchase(kNode, kVillageChunk, false, village, bought).refusal.empty(), "what is owned is not bought twice");
    const auto owned = b.owned(kNode, village, bought);
    CHECK(owned.size() == before + 2, "what was bought joins what the character has");
    bool chunk_seen = false, tab_seen = false;
    for (const warehouse::Spec& s : owned) {
        chunk_seen = chunk_seen || (s.key == kChunk2 && s.slots == 8);
        tab_seen = tab_seen || (s.key == kTab2 && s.index == 1 && s.slots == 128);
    }
    CHECK(chunk_seen && tab_seen, "the chunk has its size; the tab its grid and index");
    bool stays = false;
    for (const warehouse::Spec& s : b.owned(kNode, low, bought)) stays = stays || s.key == kTab2;
    CHECK(stays, "what was bought stays when the town falls below its level");

    warehouse::Storage wh;
    svc::WarehouseItem row;
    row.area = "Items";
    row.container_guid = kVillageChunk;
    row.container_only = true;
    svc::WarehouseItem stored;
    stored.area = "Items";
    stored.container_guid = kVillageChunk;
    stored.item = item(ItemContainer::Backpack, 2, kPotion, 7, 3);
    wh.load({row, stored});
    CHECK(wh.size() == 1 && wh.bought().count(kChunk2) == 1, "a loaded container row is no item");
    wh.add_bought(kTab2);
    size_t containers = 0;
    for (const svc::WarehouseItem& w : wh.rows()) containers += w.container_only ? 1 : 0;
    CHECK(containers == 2 && wh.rows().size() == 3, "a save carries what was bought with the items");
}

}  // namespace

int main() {
    test_split();
    test_join();
    test_containers();
    test_cargo();
    test_buying();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
