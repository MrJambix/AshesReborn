// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  warehouse/warehouse.h  (storage clerks, step 0, "put it in, take it out"; header only)
//
//  A character's warehouse as the storage clerk's window shows it.  Pure logic over the design data and the character's items: no network, no clock, no database.
//
//  The containers.  A warehouse has three kinds, each a container the window lists:
//    * the item chunks (area Items): the same in every town, so a character's chunk is kept by its record and no town.  The free base chunk ("Default") holds 40 slots, a plain
//      list: an item takes one slot, its slot is its index.  It holds items (gear, tools, usables ...), never a material.
//    * the material tabs (area Materials): one set for each town.  The free base tab ("BaseStorage") is a grid of 8 x 16 cells, laid out like a material bag: an item takes inv_w x inv_h
//      cells and its slot is the row-major index of its top-left cell.  It holds materials (and Glint, which is one), never anything else.
//    * the cargo tab (area Cargo): one for each town of the Village level and up, a grid of 6 x 8.  It holds crates and nothing else.
//  What a character owns is the free ones and what it bought (Storage::bought), each bought once: an item chunk for good in every town, a tab in the town it was bought in.  The rules of a
//  purchase are the client's own copy of the retail code [exe: APlayerCharacter::PurchaseWarehouseStorageItemChunk 0x146c360d0, ...MaterialTab 0x146c39f80]: the town's def lists the container,
//  it is not owned yet, its availability expression holds, and every cost is paid.
//
//  A move.  The window asks the server to move a stack between a bag (the item bag, a material bag, or the cargo slot for a crate: ours) and a warehouse container, or inside one container.  The server moves the whole
//  stack to an empty place or splits part of it there, joins a stack onto a stack it passes the joining rule with (the target keeps its guid and quality), or swaps two whole stacks.  What is
//  refused changes nothing and tells the client nothing but the storages as they are.  The answer is a Remove from the place the stack left and an Add where it arrived; the bag's side
//  carries the item toast, the warehouse's does not.
//
//  The rules of what may go where are the client's own (a material only in a material container, anything else only in an item container) and the owner's of the bags (the item bag
//  holds no materials).  A container the window does not have open, another town's tab and a place outside the container are refused.
//
//  The shared warehouse (migration 0026).  Every character of an account uses one warehouse, a Bank: the item chunks in every town, a town's tabs in that town (a tab stays its town's).
//  worldd keeps one Bank per account (Banks) from the first join of any of its characters to its end, and every session of the account holds the same one, so two characters online at
//  once see the same items and a move of one is the other's at once.  Each change counts the revision; a save carries it, and the character service writes the warehouse only when it is
//  above the stored one (a character's older save that arrives later leaves it alone).  The items the characters kept in their own warehouses before are its legacy items: merge_legacy
//  puts each where it fits (its own place first, then the first free place of a container of its kind the account owns), and the ones put in are named in every save, which takes them
//  out of the old table.  One that does not fit waits, and is tried again when a window opens.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "dialogue/expression.h"
#include "items/inventory.h"
#include "items/resource_grid.h"
#include "protocol/repl/warehouse_rpc.h"
#include "svc/services.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc::worldd::warehouse {

namespace repl = ::aoc::protocol::repl;

/// The kinds of container (the values of the character service's area column, by name).
enum class Area : uint8_t { Items, Materials, Cargo };

inline const char* area_name(Area a) { return a == Area::Items ? "Items" : a == Area::Materials ? "Materials" : "Cargo"; }
inline std::optional<Area> area_of(const std::string& name) {
    if (name == "Items") return Area::Items;
    if (name == "Materials") return Area::Materials;
    if (name == "Cargo") return Area::Cargo;
    return std::nullopt;
}

/// A container of a character's warehouse: its kind, the town (0 for an item chunk) and the record of the chunk or tab.
struct Key {
    Area    area = Area::Items;
    int64_t node = 0;
    int64_t record = 0;
    auto operator<=>(const Key&) const = default;
};

/// A container as the window shows it: its place in the data and its shape.  `columns` x `rows` is the grid of a tab; a chunk has none (columns 0).
struct Spec {
    Key  key;
    int  slots = 0;
    int  columns = 0, rows = 0;
    int  index = 0;           // a tab's ResourceStorageComponentIndex
    bool member = false;      // a tab only a citizen may have
    bool grid() const { return columns > 0; }
    bool operator==(const Spec&) const = default;
};

// ---- the facts the data's expressions ask about ---------------------------------------------------------------------------------------------------------------------

/// What an availability or cost expression is asked: the town the window is for and its level (the ENodeLevel ordinal), and whether the character is a citizen of it.  The functions are
/// the ones the data's warehouse expressions use; any other ends the evaluation with no value, and the expression does not hold.
class Facts final : public dialogue::ExprHost {
public:
    int64_t node = 0;           // the town of the warehouse (where the player stands)
    int     level = 0;
    int64_t citizen_node = 0;   // the node the player is a citizen of (0: none)

    std::optional<dialogue::ExprValue> call(const std::string& name, const std::vector<dialogue::ExprValue>& args) const override {
        using dialogue::ExprValue;
        constexpr int64_t kPlayer = 1;
        if (name == "GetPlayer") return ExprValue::of(kPlayer);
        if (name == "GetCurrentNode") return args.size() == 1 ? std::optional<ExprValue>(ExprValue::of(node)) : std::nullopt;
        // a player's city node is the node the player is a citizen of (vendor/town_shops.h has the evidence): the citizen tabs ask that the player stand in it and be its citizen
        if (name == "Node_GetActorCityNode") return args.size() == 1 ? std::optional<ExprValue>(ExprValue::of(citizen_node)) : std::nullopt;
        if (name == "Node_GetLevel") return args.size() == 1 ? std::optional<ExprValue>(ExprValue::of(level)) : std::nullopt;
        if (name == "Node_IsActorInNodeName") return args.size() == 2 ? std::optional<ExprValue>(ExprValue::of(args[1].i == node ? 1 : 0)) : std::nullopt;   // the player stands in `node`
        if (name == "Node_IsPlayerCitizenOfNode") return args.size() == 2 ? std::optional<ExprValue>(ExprValue::of(citizen_node != 0 && citizen_node == args[1].i ? 1 : 0)) : std::nullopt;
        if (name == "GetNodeToPlayerReputationRank") {
            ExprValue v;
            v.text = "ENodeReputationRank::None";   // no reputation tier exists yet: the character is nobody's noble or liege
            return v;
        }
        if (name == "EnumInt") {
            if (args.size() != 1) return std::nullopt;
            if (args[0].text.empty()) return ExprValue::of(args[0].i);
            const int level_of = level_named(args[0].text);
            if (level_of < 0) return std::nullopt;
            return ExprValue::of(level_of);
        }
        return std::nullopt;
    }
    int64_t random_range(int64_t lo, int64_t) const override { return lo; }

    /// The ENodeLevel ordinal of "ENodeLevel::Village" (or "Village"); -1 for a name that is none.
    static int level_named(const std::string& text) {
        static constexpr const char* kLevels[] = {"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis"};
        const size_t colons = text.rfind("::");
        const std::string name = colons == std::string::npos ? text : text.substr(colons + 2);
        for (int i = 0; i < 7; ++i) {
            if (name == kLevels[i]) return i;
        }
        return -1;
    }
};

/// Whether an availability expression holds (the empty text always does; one that does not read or calls an unknown function does not).
inline bool holds(const std::string& expression, const Facts& facts) {
    if (expression.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    const auto v = dialogue::evaluate_expression(expression, facts);
    return v && v->i != 0;
}

/// The amount of a cost: the value expression times the multiplier (an empty multiplier is 1), in the currency's own units.  nullopt when an expression does not read.
inline std::optional<double> cost_amount(const content::WarehouseCostRow& cost, const Facts& facts) {
    const auto value = dialogue::evaluate_expression(cost.value, facts);
    if (!value) return std::nullopt;
    double mult = 1.0;
    if (cost.multiplier.find_first_not_of(" \t\r\n") != std::string::npos) {
        const auto m = dialogue::evaluate_expression(cost.multiplier, facts);
        if (!m) return std::nullopt;
        mult = static_cast<double>(m->i);
    }
    return static_cast<double>(value->i) * mult;
}

// ---- the book: the data -----------------------------------------------------------------------------------------------------------------------------------------

class Book {
public:
    Book() = default;
    explicit Book(content::WarehouseData data) : d_(std::move(data)) {}

    size_t nodes() const { return d_.nodes.size(); }
    size_t chunks() const { return d_.chunks.size(); }
    size_t tabs() const { return d_.tabs.size(); }
    const content::WarehouseData::Node* node(int64_t guid) const {
        const auto it = d_.nodes.find(guid);
        return it == d_.nodes.end() ? nullptr : &it->second;
    }

    /// The summary the window asks for: the two defs the town names and, in the defs' order, every item chunk, material tab and cargo tab with whether the character may buy it and what it
    /// costs (the free ones list no cost).  nullopt for a town that has no warehouse.
    std::optional<repl::WarehouseSummary> summary(int64_t node_guid, const Facts& facts) const {
        const content::WarehouseData::Node* n = node(node_guid);
        if (!n) return std::nullopt;
        repl::WarehouseSummary s;
        s.id = repl::WarehouseId{repl::kWarehouseAreaNode, node_guid};
        s.resource_def = n->resource_def;
        s.crate_def = n->crate_def;
        if (const auto it = d_.chunk_defs.find(n->chunks_def); it != d_.chunk_defs.end()) {
            for (const int64_t guid : it->second) {
                const auto c = d_.chunks.find(guid);
                if (c == d_.chunks.end()) continue;
                s.chunks.push_back(entry(guid, c->second.requirement, c->second.costs, facts));
            }
        }
        const auto tabs_of = [&](int64_t def, std::vector<repl::WarehouseSummaryEntry>& out) {
            const auto d = d_.defs.find(def);
            if (d == d_.defs.end()) return;
            for (const int64_t guid : d->second.tabs) {
                const auto t = d_.tabs.find(guid);
                if (t == d_.tabs.end()) continue;
                out.push_back(entry(guid, t->second.requirement, t->second.costs, facts));
            }
        };
        tabs_of(n->resource_def, s.tabs);
        tabs_of(n->crate_def, s.crates);
        return s;
    }

    /// The containers a character has in a town's warehouse: the free base chunk, the free base tab and, where its requirement holds, the free cargo tab, then what it bought, in the defs' order
    /// (a tab's index is its place among the tabs of its kind).  Empty for a town that has no warehouse.  What was bought stays the character's whatever its requirement says now.
    std::vector<Spec> owned(int64_t node_guid, const Facts& facts, const std::set<Key>& bought = {}) const {
        std::vector<Spec> out;
        const content::WarehouseData::Node* n = node(node_guid);
        if (!n) return out;
        if (const auto it = d_.chunk_defs.find(n->chunks_def); it != d_.chunk_defs.end()) {
            for (const int64_t guid : it->second) {
                const auto c = d_.chunks.find(guid);
                if (c == d_.chunks.end() || !c->second.costs.empty() || !holds(c->second.requirement, facts)) continue;
                Spec s;
                s.key = Key{Area::Items, 0, guid};
                s.slots = c->second.size;
                out.push_back(s);
                break;   // the first free one: the character's base chunk
            }
            for (const int64_t guid : it->second) {
                if (!bought.count(Key{Area::Items, 0, guid})) continue;
                if (const auto s = chunk_spec(guid)) out.push_back(*s);
            }
        }
        const auto first_free_tab = [&](int64_t def, Area area) {
            const auto d = d_.defs.find(def);
            if (d == d_.defs.end()) return;
            for (const int64_t guid : d->second.tabs) {
                const auto t = d_.tabs.find(guid);
                if (t == d_.tabs.end() || !t->second.costs.empty() || t->second.member || !holds(t->second.requirement, facts)) continue;
                if ((area == Area::Cargo) != (t->second.storage_type == 1)) continue;
                Spec s;
                s.key = Key{area, node_guid, guid};
                s.columns = d->second.columns;
                s.rows = d->second.rows;
                s.slots = d->second.columns * d->second.rows;
                s.index = 0;
                s.member = t->second.member;
                out.push_back(s);
                break;
            }
            int index = 0;
            for (const int64_t guid : d->second.tabs) {
                const auto t = d_.tabs.find(guid);
                if (t == d_.tabs.end() || !bought.count(Key{area, node_guid, guid})) continue;
                Spec s;
                s.key = Key{area, node_guid, guid};
                s.columns = d->second.columns;
                s.rows = d->second.rows;
                s.slots = d->second.columns * d->second.rows;
                s.index = ++index;
                s.member = t->second.member;
                out.push_back(s);
            }
        };
        first_free_tab(n->resource_def, Area::Materials);
        first_free_tab(n->crate_def, Area::Cargo);
        return out;
    }

    /// An item chunk as a container; nullopt for a record that is none.
    std::optional<Spec> chunk_spec(int64_t guid) const {
        const auto c = d_.chunks.find(guid);
        if (c == d_.chunks.end()) return std::nullopt;
        Spec s;
        s.key = Key{Area::Items, 0, guid};
        s.slots = c->second.size;
        return s;
    }

    /// What a purchase would be: the container's place and its costs, or why not.
    struct Purchase {
        std::string                                  refusal;   // empty when it may be bought
        Key                                          key;
        std::vector<std::pair<int64_t, double>>      costs;     // currency def, amount in that def's units
    };

    /// The rules of a purchase in the town's warehouse [exe: 0x146c360d0, 0x146c39f80].  `tab` says which RPC asked: the material tab one also buys cargo tabs (the client's "purchase cargo slot").
    Purchase purchase(int64_t node_guid, int64_t record, bool tab, const Facts& facts, const std::set<Key>& bought) const {
        Purchase p;
        const content::WarehouseData::Node* n = node(node_guid);
        if (!n) {
            p.refusal = "the town has no warehouse";
            return p;
        }
        const std::string* requirement = nullptr;
        const std::vector<content::WarehouseCostRow>* costs = nullptr;
        if (tab) {
            const auto listed_in = [&](int64_t def) {
                const auto d = d_.defs.find(def);
                return d != d_.defs.end() && std::find(d->second.tabs.begin(), d->second.tabs.end(), record) != d->second.tabs.end();
            };
            const auto t = d_.tabs.find(record);
            const bool material = listed_in(n->resource_def), cargo = listed_in(n->crate_def);
            if (t == d_.tabs.end() || (!material && !cargo)) {
                p.refusal = "the town's warehouse does not list that tab";
                return p;
            }
            p.key = Key{cargo && !material ? Area::Cargo : Area::Materials, node_guid, record};
            requirement = &t->second.requirement;
            costs = &t->second.costs;
        } else {
            const auto def = d_.chunk_defs.find(n->chunks_def);
            const auto c = d_.chunks.find(record);
            if (c == d_.chunks.end() || def == d_.chunk_defs.end() || std::find(def->second.begin(), def->second.end(), record) == def->second.end()) {
                p.refusal = "the town's warehouse does not list that item chunk";
                return p;
            }
            p.key = Key{Area::Items, 0, record};
            requirement = &c->second.requirement;
            costs = &c->second.costs;
        }
        if (bought.count(p.key)) {
            p.refusal = "the character owns it already";
            return p;
        }
        if (costs->empty()) {
            p.refusal = "it costs nothing: it is not bought";
            return p;
        }
        if (!holds(*requirement, facts)) {
            p.refusal = "it is not available in this town to this character";
            return p;
        }
        for (const content::WarehouseCostRow& c : *costs) {
            const auto amount = cost_amount(c, facts);
            if (!amount || *amount < 0) {
                p.refusal = "a cost of its record does not read";
                return p;
            }
            p.costs.emplace_back(c.currency, *amount);
        }
        return p;
    }

    /// The character's base item chunk, the same in every town: the first free chunk of a town's chunk def (the item chunks do not depend on the town, so any town gives it).  It is made
    /// and listed when the character joins, as retail's login does, not when a window opens.  nullopt when the data holds none.
    std::optional<Spec> base_chunk() const {
        const Facts facts;   // (a base chunk asks nothing of the town)
        for (const auto& [guid, n] : d_.nodes) {
            for (const Spec& s : owned(guid, facts)) {
                if (s.key.area == Area::Items) return s;
            }
        }
        return std::nullopt;
    }

private:
    repl::WarehouseSummaryEntry entry(int64_t guid, const std::string& requirement, const std::vector<content::WarehouseCostRow>& costs, const Facts& facts) const {
        repl::WarehouseSummaryEntry e;
        e.record = guid;
        e.available = holds(requirement, facts);
        for (const content::WarehouseCostRow& c : costs) {
            const auto amount = cost_amount(c, facts);
            e.costs.push_back({c.currency, c.type_id, amount ? *amount : 0.0});
        }
        return e;
    }

    content::WarehouseData d_;
};

// ---- the character's stored items -------------------------------------------------------------------------------------------------------------------------------

/// What a character keeps in its warehouse: the items of every container by place.  A place holds one stack; an instance is one item (the character service checks that it is not also in a
/// bag).
class Storage {
public:
    /// Loads the rows the character service sent (a row of an area or town this build does not know is kept: it is saved back as it was).
    void load(const std::vector<svc::WarehouseItem>& rows) {
        items_.clear();
        bought_.clear();
        for (const svc::WarehouseItem& w : rows) {
            const auto area = area_of(w.area);
            if (!area) continue;
            if (w.container_only) {
                bought_.insert(Key{*area, w.node_guid, w.container_guid});
                continue;
            }
            items_[{Key{*area, w.node_guid, w.container_guid}, w.item.slot}] = w.item;
        }
    }

    std::vector<svc::WarehouseItem> rows() const {
        std::vector<svc::WarehouseItem> out;
        out.reserve(items_.size());
        for (const auto& [place, item] : items_) {
            svc::WarehouseItem w;
            w.area = area_name(place.first.area);
            w.node_guid = place.first.node;
            w.container_guid = place.first.record;
            w.item = item;
            w.item.container = svc::ItemContainer::Backpack;
            w.item.slot = place.second;
            out.push_back(std::move(w));
        }
        for (const Key& k : bought_) {
            svc::WarehouseItem w;
            w.area = area_name(k.area);
            w.node_guid = k.node;
            w.container_guid = k.record;
            w.container_only = true;
            out.push_back(std::move(w));
        }
        return out;
    }

    /// The containers the character bought.
    const std::set<Key>& bought() const { return bought_; }
    void add_bought(const Key& key) { bought_.insert(key); }

    size_t size() const { return items_.size(); }
    const svc::InventoryItem* at(const Key& key, int slot) const {
        const auto it = items_.find({key, slot});
        return it == items_.end() ? nullptr : &it->second;
    }
    /// The items of one container, by slot.
    std::vector<svc::InventoryItem> items_of(const Key& key) const {
        std::vector<svc::InventoryItem> out;
        for (auto it = items_.lower_bound({key, 0}); it != items_.end() && it->first.first == key; ++it) out.push_back(it->second);
        return out;
    }
    bool holds_guid(const std::string& guid) const {
        for (const auto& [place, item] : items_) {
            if (item.instance_guid == guid) return true;
        }
        return false;
    }
    void put(const Key& key, int slot, svc::InventoryItem item) {
        item.container = svc::ItemContainer::Backpack;
        item.slot = slot;
        items_[{key, slot}] = std::move(item);
    }
    void erase(const Key& key, int slot) { items_.erase({key, slot}); }

private:
    std::map<std::pair<Key, int>, svc::InventoryItem> items_;
    std::set<Key>                                     bought_;
};

// ---- a move ------------------------------------------------------------------------------------------------------------------------------------------------------

/// One end of a request: a bag (the item bag or a material bag) or a warehouse container, and the slot.
struct End {
    bool               in_warehouse = false;
    svc::ItemContainer bag = svc::ItemContainer::Backpack;
    Key                key;
    int                slot = 0;
};

/// One target slot of a Fill that names more than one: where the part goes, how many, and the stack it joins (empty for a free slot).
struct Target {
    End         to;
    int32_t     count = 0;
    std::string to_guid;      // the stack in the slot (empty: a free slot)
    int32_t     to_count = 0;
};

struct Request {
    repl::StorageEventType type = repl::StorageEventType::Move;   // Move, Fill or Swap
    End                    from, to;
    int32_t                count = 0;      // what the client says the source slot holds
    std::string            guid;           // the instance in the source slot
    std::string            to_guid;        // a Swap: the instance in the target slot
    int32_t                to_count = 0;
    std::string            new_guid;       // the instance a part of a stack becomes, should the move make one
    std::vector<Target>    targets;        // a Fill that names more than one target slot: the slots and what goes in each (empty: the single `to`)
};

/// One event the client is told, in order: an Add or a Remove at a bag or a warehouse place.
struct Step {
    repl::StorageEventType type = repl::StorageEventType::Add;
    bool                   in_warehouse = false;
    svc::ItemContainer     bag = svc::ItemContainer::Backpack;
    Key                    key;
    int                    slot = 0;
    svc::InventoryItem     item;
    int32_t                count = 0;   // a Remove of part of a stack: how many leave, `item` being the stack as it was (0: the whole of it)
};

struct Result {
    bool              ok = false;
    std::string       refusal;       // why not (for the log); empty when ok
    std::vector<Step> steps;         // empty when refused
    bool              bags_changed = false;       // a bag's items changed (the inventory is to be saved)
    bool              cargo_changed = false;      // the crate on the back came or went
};

/// What the window has open: the containers of the town it was opened for.
struct OpenWindow {
    int64_t           node = 0;
    std::vector<Spec> specs;
    const Spec* find(const Key& key) const {
        for (const Spec& s : specs) {
            if (s.key == key) return &s;
        }
        return nullptr;
    }
};

namespace detail {

inline bool is_bag(svc::ItemContainer c) { return c == svc::ItemContainer::Backpack || items::is_resource(c); }
inline bool is_cargo_slot(const End& e) { return !e.in_warehouse && e.bag == svc::ItemContainer::Equipment && e.slot == items::equip::CargoHold; }

/// The cells a grid container's items cover, `ignore` and `ignore2` aside.
inline std::vector<bool> covered(const Spec& spec, const Storage& wh, const items::ResourceGrid& grid, const items::ItemCatalog& catalog, const std::string* ignore = nullptr,
                                 const std::string* ignore2 = nullptr) {
    std::vector<bool> cells(static_cast<size_t>(grid.cells()), false);
    for (const svc::InventoryItem& it : wh.items_of(spec.key)) {
        if (ignore && it.instance_guid == *ignore) continue;
        if (ignore2 && it.instance_guid == *ignore2) continue;
        const auto info = catalog.info(it.item_record_id);
        grid.cover(cells, it.slot, info ? info->inv_w : 1, info ? info->inv_h : 1);
    }
    return cells;
}

/// Whether `item` may be put in warehouse place (`spec`, `slot`) when the stacks `ignore` and `ignore2` (the ones that are moving) do not count.  Empty when it may, else why not.
inline std::string why_not_here(const Spec& spec, int slot, const svc::InventoryItem& item, const Storage& wh, const items::ItemCatalog& catalog, const std::string* ignore,
                                const std::string* ignore2 = nullptr) {
    const auto info = catalog.info(item.item_record_id);
    if (!info) return "the item's record is not one this server knows";
    switch (spec.key.area) {
        case Area::Items:
            if (info->resource) return "a material is not put in an item chunk (materials go to a material tab)";
            break;
        case Area::Materials:
            if (!info->resource) return "only a material is put in a material tab";
            break;
        case Area::Cargo:
            if (!info->cargo_crate()) return "only a crate is put in a cargo tab";
            break;
    }
    if (slot < 0 || slot >= spec.slots) return "slot " + std::to_string(slot) + " is not a slot of the container";
    if (spec.grid()) {
        const items::ResourceGrid grid(spec.columns, spec.rows, false);
        if (!grid.fits(slot, info->inv_w, info->inv_h, covered(spec, wh, grid, catalog, ignore, ignore2))) {
            return "a " + std::to_string(info->inv_w) + " x " + std::to_string(info->inv_h) + " item does not fit slot " + std::to_string(slot) + " (outside the tab or on another item)";
        }
    } else {
        const svc::InventoryItem* there = wh.at(spec.key, slot);
        if (there && !(ignore && there->instance_guid == *ignore) && !(ignore2 && there->instance_guid == *ignore2)) {
            return "slot " + std::to_string(slot) + " is not empty (a move onto an item is a swap)";
        }
    }
    return {};
}

}  // namespace detail

/// Applies a request to the bags and the warehouse, or refuses it and changes nothing.  `open` is what the window has open.
inline Result transfer(items::Inventory& bags, Storage& wh, const items::ItemCatalog& catalog, const OpenWindow& open, const Request& r) {
    Result out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.steps.clear();
        return out;
    };
    if (r.type != repl::StorageEventType::Move && r.type != repl::StorageEventType::Fill && r.type != repl::StorageEventType::Swap) {
        return refuse("an event of type " + std::to_string(static_cast<int>(r.type)) + " is not made in the warehouse");
    }
    // the ends: a bag is the item bag or a material bag, a warehouse container is one the window has open
    const auto check_end = [&](const End& e) -> std::string {
        if (e.in_warehouse) {
            if (!open.find(e.key)) return "that container is not open in the warehouse window";
        } else if (!detail::is_bag(e.bag) && !detail::is_cargo_slot(e)) {
            return "only the item bag, the material bags and the cargo slot exchange items with the warehouse";
        }
        return {};
    };
    if (std::string why = check_end(r.from); !why.empty()) return refuse("the source: " + why);
    if (std::string why = check_end(r.to); !why.empty()) return refuse("the target: " + why);
    if (!r.from.in_warehouse && !r.to.in_warehouse) return refuse("a move between two bags is the bags' own");
    if (r.from.in_warehouse == r.to.in_warehouse && r.from.in_warehouse && r.from.key == r.to.key && r.from.slot == r.to.slot) return refuse("the item is moved onto its own slot");
    if (r.from.in_warehouse && r.to.in_warehouse && r.from.key.area != r.to.key.area) return refuse("an item does not move between warehouse containers of different kinds");

    // the stack that moves
    const svc::InventoryItem* source = r.from.in_warehouse ? wh.at(r.from.key, r.from.slot) : bags.at(r.from.bag, r.from.slot);
    if (!source) return refuse("no item in the source slot");
    if (items::guid_of(r.guid) == std::nullopt || source->instance_guid != r.guid) return refuse("the item in the source slot is not the instance the client named");
    if (r.count < 1 || r.count > source->quantity) return refuse("the move names " + std::to_string(r.count) + " of a stack of " + std::to_string(source->quantity));
    const svc::InventoryItem moving = *source;
    const svc::InventoryItem* target = r.to.in_warehouse ? wh.at(r.to.key, r.to.slot) : bags.at(r.to.bag, r.to.slot);
    const bool target_named = items::guid_of(r.to_guid).has_value();
    const Spec* from_spec = r.from.in_warehouse ? open.find(r.from.key) : nullptr;
    const Spec* to_spec = r.to.in_warehouse ? open.find(r.to.key) : nullptr;
    const auto is_list = [&](const End& e) { return e.in_warehouse ? !open.find(e.key)->grid() : e.bag == svc::ItemContainer::Backpack; };
    const auto info = catalog.info(moving.item_record_id);
    Storage trial_wh = wh;
    items::Inventory trial_bags = bags;
    // takes `count` off a stack on the copies: a part of it, or the whole stack
    const auto take_from = [&](const End& e, const svc::InventoryItem& item, int32_t count) -> std::string {
        if (e.in_warehouse) {
            if (count >= item.quantity) trial_wh.erase(e.key, e.slot);
            else {
                svc::InventoryItem left = item;
                left.quantity -= count;
                trial_wh.put(e.key, e.slot, left);
            }
            return {};
        }
        if (detail::is_cargo_slot(e)) {   // ours: a crate leaves the back whole
            if (count != item.quantity) return std::string("a crate moves whole");
            const items::Outcome taken = trial_bags.remove_cargo(item.instance_guid);
            return taken.ok ? std::string() : taken.refusal;
        }
        const items::Outcome removed = trial_bags.consume(e.bag, e.slot, item.instance_guid, count);
        return removed.ok ? std::string() : removed.refusal;
    };
    // puts a stack at an end on the copies
    const auto put_at = [&](const End& e, svc::InventoryItem item) -> std::string {
        if (e.in_warehouse) {
            trial_wh.put(e.key, e.slot, item);
            return {};
        }
        item.container = e.bag;
        const items::Outcome added = detail::is_cargo_slot(e) ? trial_bags.grant_cargo(item, catalog) : trial_bags.add_at(e.bag, e.slot, item, catalog);   // (ours: a crate goes on the back)
        return added.ok ? std::string() : added.refusal;
    };
    const auto fill = [&](Step& s, repl::StorageEventType type, const End& e, const svc::InventoryItem& item, int32_t count) {
        s.type = type;
        s.in_warehouse = e.in_warehouse;
        s.bag = e.bag;
        s.key = e.key;
        s.slot = e.slot;
        s.item = item;
        s.count = count;
    };

    if (r.type == repl::StorageEventType::Swap) {
        // two whole stacks change places: both list places (the item bag and an item chunk) or both grids (a material bag and a material tab)
        if (!target) return refuse("no item in the target slot to swap with");
        if (!target_named || target->instance_guid != r.to_guid) return refuse("the item in the target slot is not the instance the client named");
        if (r.count != moving.quantity || r.to_count != target->quantity) return refuse("a swap names whole stacks only");
        if (is_list(r.from) != is_list(r.to)) return refuse("only stacks of the item bag and the item chunks, or of two grids, are swapped");
        if (r.from.in_warehouse && r.to.in_warehouse && r.from.key == r.to.key && to_spec->grid()) {
            // the final places are judged together: the moving stack's new place, then the target's against it and every other item (each checked on its own
            // with both ignored would let two rectangles of one grid land on one cell)
            const std::string why = detail::why_not_here(*to_spec, r.to.slot, moving, trial_wh, catalog, &target->instance_guid, &moving.instance_guid);
            if (!why.empty()) return refuse("the source: " + why);
            const items::ResourceGrid grid(to_spec->columns, to_spec->rows, false);
            std::vector<bool> cells = detail::covered(*to_spec, trial_wh, grid, catalog, &moving.instance_guid, &target->instance_guid);
            grid.cover(cells, r.to.slot, info->inv_w, info->inv_h);
            const auto target_info = catalog.info(target->item_record_id);
            if (!target_info) return refuse("the target: the item's record is not one this server knows");
            if (!grid.fits(r.from.slot, target_info->inv_w, target_info->inv_h, cells)) {
                return refuse("the target: a " + std::to_string(target_info->inv_w) + " x " + std::to_string(target_info->inv_h) + " item does not fit slot " +
                              std::to_string(r.from.slot) + " (outside the tab or on another item)");
            }
        } else {
            if (r.to.in_warehouse) {
                const std::string why = detail::why_not_here(*to_spec, r.to.slot, moving, trial_wh, catalog, &target->instance_guid, &moving.instance_guid);
                if (!why.empty()) return refuse("the source: " + why);
            }
            if (r.from.in_warehouse) {
                const std::string why = detail::why_not_here(*from_spec, r.from.slot, *target, trial_wh, catalog, &moving.instance_guid, &target->instance_guid);
                if (!why.empty()) return refuse("the target: " + why);
            }
        }
        if (std::string why = take_from(r.from, moving, moving.quantity); !why.empty()) return refuse(why);
        if (std::string why = take_from(r.to, *target, target->quantity); !why.empty()) return refuse(why);
        svc::InventoryItem b_at_from = *target;
        b_at_from.slot = r.from.slot;
        if (std::string why = put_at(r.from, b_at_from); !why.empty()) return refuse(why);
        svc::InventoryItem a_at_to = moving;
        a_at_to.slot = r.to.slot;
        if (std::string why = put_at(r.to, a_at_to); !why.empty()) return refuse(why);
        Step ra, rb, ab, aa;
        fill(ra, repl::StorageEventType::Remove, r.from, moving, 0);
        fill(rb, repl::StorageEventType::Remove, r.to, *target, 0);
        fill(ab, repl::StorageEventType::Add, r.from, b_at_from, 0);
        fill(aa, repl::StorageEventType::Add, r.to, a_at_to, 0);
        bags = std::move(trial_bags);
        wh = std::move(trial_wh);
        out.steps = {ra, rb, ab, aa};
        out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
        out.cargo_changed = detail::is_cargo_slot(r.from) || detail::is_cargo_slot(r.to);
        out.ok = true;
        return out;
    }

    if (!r.targets.empty()) {
        // a Fill that names more than one target slot: the existing stacks to top up, then the free slot for the rest (the client's quick-send)
        if (r.type != repl::StorageEventType::Fill) return refuse("a move that names more than one target slot is a Fill");
        if (!info) return refuse("the item's record is not one this server knows");
        const int32_t max_stack = std::max(1, info->max_stack);
        int64_t total = 0;
        for (const Target& t : r.targets) {
            if (t.count < 1) return refuse("a target slot names no amount");
            total += t.count;
        }
        if (total != r.count) return refuse("the move names " + std::to_string(r.count) + " but its target slots name " + std::to_string(total));
        for (size_t i = 0; i < r.targets.size(); ++i) {
            const Target& t = r.targets[i];
            for (size_t j = 0; j < i; ++j) {
                const End& earlier = r.targets[j].to;
                if (earlier.in_warehouse == t.to.in_warehouse && earlier.bag == t.to.bag && earlier.key == t.to.key && earlier.slot == t.to.slot) {
                    return refuse("the request names the same target slot twice");
                }
            }
            if (t.to.in_warehouse != r.to.in_warehouse || t.to.bag != r.to.bag || t.to.key != r.to.key) {
                return refuse("the target slots are not all in the target container");
            }
            if (t.to.in_warehouse == r.from.in_warehouse && t.to.in_warehouse && t.to.key == r.from.key && t.to.slot == r.from.slot) {
                return refuse("the item is moved onto its own slot");
            }
            if (!t.to.in_warehouse && !r.from.in_warehouse && t.to.bag == r.from.bag && t.to.slot == r.from.slot) {
                return refuse("the item is moved onto its own slot");
            }
        }
        // the whole request is judged on the copies: every target slot must take its part before anything is taken from the source, and each accepted target is applied
        // to the copies in turn, so a later target sees the raised stack or the cells a new stack took
        std::vector<Step> adds;
        adds.reserve(r.targets.size());
        for (const Target& t : r.targets) {
            const svc::InventoryItem* there = t.to.in_warehouse ? trial_wh.at(t.to.key, t.to.slot) : trial_bags.at(t.to.bag, t.to.slot);
            if (there) {
                if (!items::guid_of(t.to_guid) || there->instance_guid != t.to_guid) return refuse("the target slot holds an item, and the client did not name it");
                if (!items::stacks_with(moving, *there, info->rarity)) return refuse("the target holds another stack (a move onto an item is a swap)");
                const int32_t room = max_stack > there->quantity ? max_stack - there->quantity : 0;
                if (t.count > room) {
                    return refuse("the move names " + std::to_string(t.count) + " of a stack of " + std::to_string(moving.quantity) + "; only " + std::to_string(room) +
                                  " fit onto the target");
                }
                Step add;
                fill(add, repl::StorageEventType::Add, t.to, *there, 0);
                add.item.quantity = t.count;
                adds.push_back(std::move(add));
                // the top-up raises the target stack on the copies, so a later target sees it
                svc::InventoryItem merged = *there;
                merged.quantity = there->quantity + t.count;
                if (!t.to.in_warehouse) {
                    if (std::string why = take_from(t.to, *there, there->quantity); !why.empty()) return refuse(why);
                }
                if (std::string why = put_at(t.to, merged); !why.empty()) return refuse(why);
            } else {
                if (items::guid_of(t.to_guid).has_value()) return refuse("the target slot is empty, but the client named an item in it");
                if (t.to.in_warehouse) {
                    const std::string* ignore = r.from.in_warehouse && total >= moving.quantity ? &moving.instance_guid : nullptr;
                    const std::string why = detail::why_not_here(*to_spec, t.to.slot, moving, trial_wh, catalog, ignore);
                    if (!why.empty()) return refuse(why);
                }
                svc::InventoryItem arriving = moving;
                arriving.quantity = t.count;
                arriving.slot = t.to.slot;
                if (!t.to.in_warehouse) arriving.container = t.to.bag;
                if (total < moving.quantity || r.targets.size() > 1) {
                    do {
                        arriving.instance_guid = bags.fresh_guid();
                    } while (!items::guid_of(arriving.instance_guid) || bags.find_guid(arriving.instance_guid) || wh.holds_guid(arriving.instance_guid));
                }
                Step add;
                fill(add, repl::StorageEventType::Add, t.to, arriving, 0);
                adds.push_back(std::move(add));
                // the new stack takes its cells on the copies, so a later target sees them
                if (std::string why = put_at(t.to, arriving); !why.empty()) return refuse(why);
            }
        }
        if (std::string why = take_from(r.from, moving, static_cast<int32_t>(total)); !why.empty()) return refuse(why);
        Step remove;
        fill(remove, repl::StorageEventType::Remove, r.from, moving, total < moving.quantity ? static_cast<int32_t>(total) : 0);
        bags = std::move(trial_bags);
        wh = std::move(trial_wh);
        out.steps.push_back(std::move(remove));
        for (Step& s : adds) out.steps.push_back(std::move(s));
        out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
        out.ok = true;
        return out;
    }

    if (!target) {
        // an empty place: the whole stack moves, or a part of it becomes a new stack there (the client names no item in the place)
        if (target_named) return refuse("the target slot is empty, but the client named an item in it");
        const bool split = r.count < moving.quantity;
        // a whole stack leaves its cells, so a same-container move may slide over itself; a split leaves the source in place
        if (r.to.in_warehouse) {
            const std::string* ignore = r.from.in_warehouse && !split ? &moving.instance_guid : nullptr;
            const std::string why = detail::why_not_here(*to_spec, r.to.slot, moving, trial_wh, catalog, ignore);
            if (!why.empty()) return refuse(why);
        }
        svc::InventoryItem arriving = moving;
        if (split) {
            if (!info) return refuse("the item's record is not one this server knows");
            arriving.quantity = r.count;
            if (!r.new_guid.empty()) {   // ours: the session mints the new stack's guid for the request; one that is taken is refused
                arriving.instance_guid = r.new_guid;
                if (!items::guid_of(arriving.instance_guid) || bags.find_guid(arriving.instance_guid) || wh.holds_guid(arriving.instance_guid)) return refuse("the new stack's guid is taken");
            } else {
                do {
                    arriving.instance_guid = bags.fresh_guid();
                } while (!items::guid_of(arriving.instance_guid) || bags.find_guid(arriving.instance_guid) || wh.holds_guid(arriving.instance_guid));
            }
        }
        arriving.slot = r.to.slot;
        if (!r.to.in_warehouse) arriving.container = r.to.bag;
        if (std::string why = take_from(r.from, moving, r.count); !why.empty()) return refuse(why);
        if (std::string why = put_at(r.to, arriving); !why.empty()) return refuse(why);
        Step remove, add;
        fill(remove, repl::StorageEventType::Remove, r.from, moving, split ? r.count : 0);
        fill(add, repl::StorageEventType::Add, r.to, arriving, 0);
        bags = std::move(trial_bags);
        wh = std::move(trial_wh);
        out.steps = {remove, add};
        out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
        out.cargo_changed = detail::is_cargo_slot(r.from) || detail::is_cargo_slot(r.to);
        out.ok = true;
        return out;
    }

    // onto a stack: it joins when the two stacks pass the rule, and the target keeps its own guid and quality
    if (!target_named || target->instance_guid != r.to_guid) return refuse("the target slot holds an item, and the client did not name it (a Move or a Fill to a free slot names no item)");
    if (!info) return refuse("the item's record is not one this server knows");
    if (!items::stacks_with(moving, *target, info->rarity)) return refuse("the target holds another stack (a move onto an item is a swap)");
    const int32_t max_stack = std::max(1, info->max_stack);
    const int32_t room = max_stack > target->quantity ? max_stack - target->quantity : 0;
    if (r.count > room) return refuse("the move names " + std::to_string(r.count) + " of a stack of " + std::to_string(moving.quantity) + "; only " + std::to_string(room) + " fit onto the target");
    if (std::string why = take_from(r.from, moving, r.count); !why.empty()) return refuse(why);
    svc::InventoryItem merged = *target;
    merged.quantity += r.count;
    if (!r.to.in_warehouse) {
        if (std::string why = take_from(r.to, *target, target->quantity); !why.empty()) return refuse(why);
    }
    if (std::string why = put_at(r.to, merged); !why.empty()) return refuse(why);
    Step remove, add;
    fill(remove, repl::StorageEventType::Remove, r.from, moving, r.count < moving.quantity ? r.count : 0);
    svc::InventoryItem added = *target;
    added.quantity = r.count;
    fill(add, repl::StorageEventType::Add, r.to, added, 0);
    bags = std::move(trial_bags);
    wh = std::move(trial_wh);
    out.steps = {remove, add};
    out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
    out.cargo_changed = detail::is_cargo_slot(r.from) || detail::is_cargo_slot(r.to);
    out.ok = true;
    return out;
}

// ---- the shared warehouse ---------------------------------------------------------------------------------------------------------------------------------------------

/// An account's warehouse, shared by its characters' sessions.  A Bank that is not shared is one character's alone (a character service that keeps no shared warehouse, a test): its
/// rows are saved as the character's, as before.
struct Bank {
    Storage                         storage;
    bool                            shared = false;
    int64_t                         revision = 0;   // the stored one at the load, counted up by each change
    std::vector<svc::WarehouseItem> legacy;         // legacy items that did not fit yet
    std::vector<std::string>        merged;         // legacy instances put in since the load (every save names them all)

    void changed() { ++revision; }

    /// What a save carries: the storage's rows and, for a shared one, its revision and the merged instances.
    std::vector<svc::WarehouseItem> rows() const {
        std::vector<svc::WarehouseItem> out = storage.rows();
        if (!shared) return out;
        svc::WarehouseItem rev;
        rev.meta = svc::WarehouseItem::Meta::Revision;
        rev.revision = revision;
        out.push_back(std::move(rev));
        for (const std::string& guid : merged) {
            svc::WarehouseItem m;
            m.meta = svc::WarehouseItem::Meta::Merged;
            m.item.instance_guid = guid;
            out.push_back(std::move(m));
        }
        return out;
    }

    /// The Bank of what the character service sent for a character: the account's shared warehouse when it sent one, else the character's own.
    static Bank of(const svc::Character& c) {
        Bank b;
        if (!c.shared_warehouse) {
            b.storage.load(c.warehouse_items);
            return b;
        }
        b.shared = true;
        b.revision = c.shared_warehouse->revision;
        b.storage.load(c.shared_warehouse->items);
        b.legacy = c.shared_warehouse->legacy;
        return b;
    }
};

/// The Banks of the accounts whose characters played since worldd started, by account.  A Bank stays when its characters log out: it is what the account's warehouse is (its last save
/// may still be waiting in the saver), so a character that joins later takes it rather than what its login read.
class Banks {
public:
    std::shared_ptr<Bank> find(int64_t account) const {
        const auto it = by_account_.find(account);
        return it == by_account_.end() ? nullptr : it->second;
    }
    std::shared_ptr<Bank> add(int64_t account, Bank bank) {
        auto made = std::make_shared<Bank>(std::move(bank));
        by_account_[account] = made;
        return made;
    }
    size_t size() const { return by_account_.size(); }

private:
    std::map<int64_t, std::shared_ptr<Bank>> by_account_;
};

/// What merge_legacy did.
struct MergeResult {
    size_t put = 0;       // legacy items now in the storage
    size_t waiting = 0;   // legacy items that still do not fit
};

/// Puts the bank's legacy items where they fit: an item's own place when it is free and the account owns the container, else the first free place (in the containers' order, then the
/// slots') of a container of its kind and town the account owns.  level_of gives a town's ENodeLevel ordinal (a tab's availability may ask it).  An item whose instance the storage
/// holds already is not put again, only named as merged.  Counts a change when anything moved.
inline MergeResult merge_legacy(Bank& bank, const Book& book, const items::ItemCatalog& catalog, const std::function<int(int64_t)>& level_of, int64_t citizen_node = 0) {
    MergeResult out;
    if (bank.legacy.empty()) return out;
    std::map<int64_t, std::vector<Spec>> by_node;   // the tabs the account owns, by town
    const auto specs_of = [&](Area area, int64_t node) {
        std::vector<Spec> specs;
        if (area == Area::Items) {
            if (const auto base = book.base_chunk()) specs.push_back(*base);
            for (const Key& k : bank.storage.bought()) {
                if (k.area != Area::Items) continue;
                if (const auto c = book.chunk_spec(k.record)) specs.push_back(*c);
            }
            return specs;
        }
        auto it = by_node.find(node);
        if (it == by_node.end()) {
            Facts facts;
            facts.node = node;
            facts.level = level_of ? level_of(node) : 0;
            facts.citizen_node = citizen_node;
            it = by_node.emplace(node, book.owned(node, facts, bank.storage.bought())).first;
        }
        for (const Spec& s : it->second) {
            if (s.key.area == area) specs.push_back(s);
        }
        return specs;
    };
    const size_t merged_before = bank.merged.size();
    std::vector<svc::WarehouseItem> waiting;
    for (svc::WarehouseItem& w : bank.legacy) {
        const auto area = area_of(w.area);
        if (!area || w.container_only) {
            waiting.push_back(std::move(w));
            continue;
        }
        if (bank.storage.holds_guid(w.item.instance_guid)) {   // (in already: its legacy row only goes)
            bank.merged.push_back(w.item.instance_guid);
            continue;
        }
        const Key own{*area, w.node_guid, w.container_guid};
        const std::vector<Spec> specs = specs_of(*area, *area == Area::Items ? 0 : w.node_guid);
        std::optional<std::pair<Key, int>> place;
        for (const Spec& s : specs) {
            if (s.key == own && detail::why_not_here(s, w.item.slot, w.item, bank.storage, catalog, nullptr).empty()) place = std::make_pair(s.key, w.item.slot);
        }
        for (size_t i = 0; !place && i < specs.size(); ++i) {
            for (int slot = 0; slot < specs[i].slots; ++slot) {
                if (detail::why_not_here(specs[i], slot, w.item, bank.storage, catalog, nullptr).empty()) {
                    place = std::make_pair(specs[i].key, slot);
                    break;
                }
            }
        }
        if (!place) {
            waiting.push_back(std::move(w));
            continue;
        }
        bank.storage.put(place->first, place->second, w.item);
        bank.merged.push_back(w.item.instance_guid);
        ++out.put;
    }
    bank.legacy = std::move(waiting);
    out.waiting = bank.legacy.size();
    if (bank.merged.size() != merged_before) bank.changed();
    return out;
}

/// The payload a container sends the client: its slots (one element for each, an empty one where nothing lies) and its shape.
inline repl::StoragePayload payload_of(const Spec& spec, const Storage& wh) {
    repl::StoragePayload p;
    p.maximum_items = spec.slots;
    p.dimension = repl::Vec2d{static_cast<double>(spec.columns), static_cast<double>(spec.rows)};
    p.storage.resize(static_cast<size_t>(spec.slots));
    for (const svc::InventoryItem& it : wh.items_of(spec.key)) {
        if (it.slot < 0 || it.slot >= spec.slots) continue;
        repl::StorageSlot& slot = p.storage[static_cast<size_t>(it.slot)];
        slot.item = items::to_instance(it);
        slot.slot = it.slot;
    }
    return p;
}

}  // namespace aoc::worldd::warehouse
