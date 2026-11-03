// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_market.cpp
//
//  The player marketplace (market/market_book.h holds the listings, protocol/repl/marketplace_rpc.h the calls).  A use of a marketplace NPC (Node_Vendor_Gear_marketplace,
//  whose ID_MarketplaceVendor completes with AR_MarketplaceVendor: EActionResponse::OpenMarketplaceWindow) pays the player's proceeds and opens the window for the node the NPC
//  stands in; every request then names the window's guid, which must be the one it was opened with, with the NPC still within reach (the exe's own check, 0x145f008f0, is the
//  same: the guid the controller keeps, then the distance).
//
//  Every act saves at once: the book's changed rows go with the character's inventory and purse in one SaveInventory (session/position_saver.h).
//  Modelled (no recording): the guid of the window is the NPC's network GUID's four words; the replies' order; the own listings sent again after a listing or a removal;
//  the rules market_book.h lists (no fee, no tax, seven days, 30 listings).
// ============================================================================
#include "session/player_session.h"

#include "items/acquired.h"
#include "session/player_session_util.h"
#include "vendor/node_tax.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <string_view>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// ID_MarketplaceVendor: the only PlayerInteractionDef of the data whose completion opens the marketplace window (AR_MarketplaceVendor).
constexpr int64_t kMarketplacePid = 6064632772967071745ll;
/// Node_Vendor_Gear_marketplace: the only population instance that names it (in ServiceBuilding_L3/L4_Marketplace_Operational_Vendors).
constexpr int64_t kMarketplaceInstance = 6064632772964253696ll;
/// The money tier's FDesignDataObjectId TypeId (CurrencyTierDefRecordBase), the key of a price in FCurrencyContainer.
constexpr int64_t kCurrencyTierTypeId = 456318420933392641ll;

repl::GuidValue guid_of(const protocol::emit::FIntrepidNetworkGUID& g) {
    return repl::GuidValue{static_cast<uint32_t>(g.ObjectId), static_cast<uint32_t>(g.ObjectId >> 32), g.ServerId, g.Randomizer};
}

/// The node an NPC belongs to: its UAoCNodePlotComponent's NodeRecordGuid (a town's shopkeepers and plot NPCs carry it), 0 when it has none.
int64_t plot_node_of(const world::WorldActor& a) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (!s.obj.schema || std::string_view(s.obj.schema->name) != "UAoCNodePlotComponent") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "NodeRecordGuid");
        const auto* g = v ? v->get_if<int64_t>() : nullptr;
        return g ? *g : 0;
    }
    return 0;
}

size_t utf8_chars(const std::string& s) {
    size_t n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

}  // namespace

bool PlayerSession::is_marketplace(const world::WorldActor& a) const {
    if (a.kind != world::ActorKind::Npc) return false;
    if (a.npc.interact_def == kMarketplacePid || a.npc.instance_guid == kMarketplaceInstance) return true;
    if (const spawns::AreaReport* r = world_ ? world_->area_report() : nullptr) {
        const auto it = r->instances.find(a.npc.instance_guid);
        return it != r->instances.end() && it->second.interact_response == kMarketplacePid;
    }
    return false;
}

double PlayerSession::market_clock() const {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

repl::GuidValue PlayerSession::market_player() const {
    const auto& g = pb_.setup().character_guid;
    return repl::GuidValue{g[0], g[1], g[2], g[3]};
}

repl::MarketplaceItemData PlayerSession::market_item(const market::Listing& l) const {
    repl::MarketplaceItemData d;
    d.node = l.node;
    d.player = l.seller;
    d.player_name = l.seller_name;
    d.item_guid = l.guid;
    d.item = items::to_instance(l.item);
    d.item_name = l.item_name;
    d.price_per_unit.values.push_back({items::kMoneyTier, kCurrencyTierTypeId, static_cast<uint64_t>(l.price)});
    d.issue_date = l.issued;
    d.expiration_date = l.expires;
    const double now = market_clock();
    d.status = l.item.quantity <= 0 ? repl::MarketplaceItemStatus::Sold : l.expired(now) ? repl::MarketplaceItemStatus::Expired : repl::MarketplaceItemStatus::Listed;
    return d;
}

void PlayerSession::save_market(std::vector<svc::MarketRow> rows, const char* why) {
    if (rows.empty()) return;
    market_rows_.insert(market_rows_.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    inventory_changed_ = true;
    hand_over_inventory(why, /*periodic=*/false);
}

void PlayerSession::open_marketplace(Connection& conn, const world::WorldActor& npc, Clock::time_point now) {
    (void)now;
    if (!room_for(3)) return;
    if (!market_book_ && world_ && world_->market_enabled()) market_book_ = world_->market_book();
    // the market is the node's: the NPC's own plot component names it, else the zone of influence it stands in
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    int64_t node_guid = plot_node_of(npc);
    std::string node_name = node_guid != 0 ? std::to_string(node_guid) : std::string();
    if (node_guid == 0 && nodes) {
        if (const world::CityNodeInfo* node = nodes->by_rank(nodes->node_at(npc.x, npc.y).node_rank)) {
            node_guid = node->guid;
            node_name = node->display_name;
        }
    } else if (nodes) {
        if (const world::CityNodeInfo* node = nodes->by_guid(node_guid)) node_name = node->display_name;
    }
    if (!market_book_ || node_guid == 0) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: the marketplace {} does not open: {}", name(), npc.npc.name, !market_book_ ? "the marketplace is off (the character service did not list it at the start)" : "it stands in no node");
        return;
    }
    market_actor_ = npc.id;
    market_guid_ = guid_of(npc.actor.guid);
    market_node_ = node_guid;
    ++market_opens_;
    collect_market_proceeds(conn);
    const repl::GuidValue market = market_guid_;
    spdlog::log(level_for(market_logged_), "[session] {}: opens the marketplace of {} ({} listing(s) in the book)", name(), node_name, market_book_->listings());
    queue("ClientOpenMarketplaceUI",
          [market, node_guid](bootstrap::PlayerBootstrap& pb) {
              return pb.market_call("ClientOpenMarketplaceUI", [&](const protocol::schema::RpcSchema& rpc) { return repl::open_marketplace_params(rpc, market, vendor::default_table(node_guid)); });
          },
          conn, /*must_build=*/true);
}

const world::WorldActor* PlayerSession::open_market(const repl::GuidValue& market, std::string& why) {
    if (market_actor_ == 0 || !market_book_) {
        why = "no marketplace window is open";
        return nullptr;
    }
    if (!(market == market_guid_)) {
        why = "the request names another marketplace";
        return nullptr;
    }
    const world::WorldActor* a = registry_ ? registry_->find(market_actor_) : nullptr;
    if (!a) {
        why = "the marketplace is gone";
        market_actor_ = 0;
        return nullptr;
    }
    const GoodPosition at = checker_.good();
    const double dx = a->x - at.x, dy = a->y - at.y, dz = a->z - at.z;
    if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
        why = "the marketplace is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away";
        market_actor_ = 0;
        return nullptr;
    }
    return a;
}

void PlayerSession::collect_market_proceeds(Connection& conn) {
    if (!market_book_) return;
    market::Book::Collected c = market_book_->collect(market_seller_id(), wallet_.room());
    if (c.copper <= 0 && c.rows.empty()) return;
    const int64_t added = wallet_.give(c.copper);
    spdlog::info("[session] {}: collects {} copper of proceeds from {} listing(s)", name(), added, c.listings);
    save_market(std::move(c.rows), "marketplace proceeds");
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("proceeds", [added](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(items::kMoneyTier, added); }, conn, /*must_build=*/true);
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
}

void PlayerSession::send_market_data(Connection& conn, const std::string& seller_id, const repl::GuidValue& seller, bool own) {
    repl::MarketData data;
    data.player = seller;
    if (market_book_ && !seller_id.empty()) {
        const std::vector<const market::Listing*> listed = own ? market_book_->own(market_node_, seller_id) : market_book_->for_sale(market_node_, seller_id, market_clock());
        for (const market::Listing* l : listed) data.items.push_back(market_item(*l));
    }
    queue("ClientReceiveMarketplaceData",
          [data](bootstrap::PlayerBootstrap& pb) {
              return pb.market_call("ClientReceiveMarketplaceData", [&](const protocol::schema::RpcSchema& rpc) { return repl::market_data_params(rpc, data); });
          },
          conn, /*must_build=*/true);
}

void PlayerSession::on_market_headers(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto req = repl::parse_market_headers_request(rpc.call);
    if (!req || !within_budget(market_budget_, "ServerRequestMarketplaceHeaders", now) || !room_for(1)) return;
    std::string why;
    if (!open_market(req->market, why)) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: the storefronts are not sent: {}", name(), why);
        return;
    }
    repl::MarketHeaders h;
    h.node = market_node_;
    h.version = market_book_->version(market_node_);
    for (const market::Header& m : market_book_->headers(market_node_, market_clock())) h.headers.push_back({market_node_, m.seller, m.seller_name, m.description, m.issued, false});
    spdlog::log(level_for(market_logged_), "[session] {}: {} storefront(s), version {}", name(), h.headers.size(), h.version);
    queue("ClientReceiveMarketplaceHeaders",
          [h](bootstrap::PlayerBootstrap& pb) {
              return pb.market_call("ClientReceiveMarketplaceHeaders", [&](const protocol::schema::RpcSchema& r) { return repl::market_headers_params(r, h); });
          },
          conn, /*must_build=*/true);
}

void PlayerSession::on_market_search(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto req = repl::parse_market_search_request(rpc.call);
    if (!req || !within_budget(market_budget_, "ServerSearchMarketplaceHeaders", now) || !room_for(1)) return;
    std::string why;
    if (!open_market(req->market, why)) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: a search is not answered: {}", name(), why);
        return;
    }
    // the exe's rule: 1 to 50 characters, else INVALID_SEARCH_LEN in the header update's answer (0x145efc3c0)
    const size_t chars = utf8_chars(req->search);
    if (chars < 1 || chars > market::kMaxSearchChars) {
        const std::string text = req->search;
        queue("ClientMarketplaceHeaderUpdateResponse",
              [text](bootstrap::PlayerBootstrap& pb) {
                  return pb.market_call("ClientMarketplaceHeaderUpdateResponse",
                                        [&](const protocol::schema::RpcSchema& r) { return repl::market_header_update_response_params(r, text, repl::MarketplaceResponse::InvalidSearchLen); });
              },
              conn, /*must_build=*/true);
        return;
    }
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const auto rarity_of = [db](int64_t record, int32_t quality) {
        std::string error;
        const auto rec = db ? db->item(record, error) : std::nullopt;
        return rec ? rec->rarity_of(quality) : items::default_rarity_facts().rarity_of(quality);
    };
    const std::optional<int> rarity = req->rarity < repl::kMarketplaceAnyRarity ? std::optional<int>(req->rarity) : std::nullopt;
    repl::MarketHeaders h;
    h.node = market_node_;
    h.version = market_book_->version(market_node_);
    h.search = req->search;
    h.rarity = req->rarity;
    for (const market::Header& m : market_book_->search(market_node_, req->search, rarity, rarity_of, market_clock())) {
        h.headers.push_back({market_node_, m.seller, m.seller_name, m.description, m.issued, false});
    }
    spdlog::log(level_for(market_logged_), "[session] {}: a search for \"{}\" finds {} storefront(s)", name(), req->search, h.headers.size());
    queue("ClientReceiveMarketplaceHeaders",
          [h](bootstrap::PlayerBootstrap& pb) {
              return pb.market_call("ClientReceiveMarketplaceHeaders", [&](const protocol::schema::RpcSchema& r) { return repl::market_headers_params(r, h); });
          },
          conn, /*must_build=*/true);
}

void PlayerSession::on_market_data(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto req = repl::parse_market_data_request(rpc.call);
    if (!req || !within_budget(market_budget_, "ServerRequestMarketplaceData", now) || !room_for(1)) return;
    std::string why;
    if (!open_market(req->market, why)) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: a storefront is not sent: {}", name(), why);
        return;
    }
    const bool own = req->player == market_player();
    send_market_data(conn, own ? market_seller_id() : market_book_->seller_of(market_node_, req->player), req->player, own);
}

void PlayerSession::on_market_header_update(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto req = repl::parse_market_header_update_request(rpc.call);
    if (!req || !within_budget(market_budget_, "ServerRequestMarketplaceHeaderUpdate", now) || !room_for(1)) return;
    std::string why;
    repl::MarketplaceResponse response = repl::MarketplaceResponse::Success;
    if (!open_market(req->market, why)) {
        response = repl::MarketplaceResponse::NodeNotFound;
    } else if (utf8_chars(req->description) > market::kMaxDescriptionChars) {
        response = repl::MarketplaceResponse::InvalidDescriptionLen;
        why = "a description of " + std::to_string(utf8_chars(req->description)) + " characters";
    } else {
        save_market(market_book_->describe(market_node_, market_seller_id(), market_player(), name(), req->description, market_clock()), "marketplace description");
    }
    if (response != repl::MarketplaceResponse::Success) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: the storefront's description is not changed: {}", name(), why);
    }
    const std::string text = req->description;
    queue("ClientMarketplaceHeaderUpdateResponse",
          [text, response](bootstrap::PlayerBootstrap& pb) {
              return pb.market_call("ClientMarketplaceHeaderUpdateResponse", [&](const protocol::schema::RpcSchema& r) { return repl::market_header_update_response_params(r, text, response); });
          },
          conn, /*must_build=*/true);
}

void PlayerSession::on_market_registration(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto req = repl::parse_market_registration_request(rpc.call);
    if (!req || !within_budget(market_budget_, "ServerRequestMarketplaceItemRegistration", now) || !room_for(4)) return;
    repl::MarketplaceItemData answer;
    answer.item = req->item;
    const auto refuse = [&](repl::MarketplaceResponse response, const std::string& why) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: a listing is refused: {}", name(), why);
        queue("ClientMarketplaceItemRegistrationResponse",
              [answer, response](bootstrap::PlayerBootstrap& pb) {
                  return pb.market_call("ClientMarketplaceItemRegistrationResponse", [&](const protocol::schema::RpcSchema& r) { return repl::market_registration_response_params(r, answer, response); });
              },
              conn, /*must_build=*/true);
    };
    std::string why;
    if (dead_) return refuse(repl::MarketplaceResponse::InvalidPlayer, "the character is dead");
    if (!open_market(req->market, why)) return refuse(repl::MarketplaceResponse::NodeNotFound, why);
    // the item is the character's own, in a bag, by its instance guid; the client's copy of it decides nothing else
    const std::string guid = items::guid_hex(req->item.guid);
    const svc::InventoryItem* held = inventory_.find_guid(guid);
    if (!held) return refuse(repl::MarketplaceResponse::ItemNotFound, "an item that is in none of this character's bags");
    if (!is_bag(held->container)) return refuse(repl::MarketplaceResponse::ItemInUse, "an item that is worn or in the quest storage");
    const auto info = item_catalog().info(held->item_record_id);
    if (!info || info->quest || info->cargo_crate()) return refuse(repl::MarketplaceResponse::ItemUntradable, "item " + std::to_string(held->item_record_id) + " is not traded");
    if (held->lock_expiration > static_cast<int64_t>(market_clock())) return refuse(repl::MarketplaceResponse::ItemIsLocked, "the item is locked");
    if (req->quantity < 1 || req->quantity > held->quantity) {
        return refuse(repl::MarketplaceResponse::InvalidQuantity, std::to_string(req->quantity) + " of a stack of " + std::to_string(held->quantity));
    }
    // the price: copper only, one tier
    if (req->price_per_unit.values.size() != 1 || req->price_per_unit.values[0].tier != items::kMoneyTier || req->price_per_unit.values[0].amount < 1 ||
        req->price_per_unit.values[0].amount > static_cast<uint64_t>(market::kMaxPricePerUnit)) {
        return refuse(repl::MarketplaceResponse::CurrencyNotFound, "a price that is no amount of money from 1 to " + std::to_string(market::kMaxPricePerUnit) + " copper");
    }
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    std::string error;
    const auto record = db ? db->item(held->item_record_id, error) : std::nullopt;
    const double issued = market_clock();
    market::Listing l;
    l.guid = *market::guid_from_hex(fresh_guid());
    l.node = market_node_;
    l.seller_id = market_seller_id();
    l.seller = market_player();
    l.seller_name = name();
    l.item = *held;
    l.item.quantity = req->quantity;
    if (req->quantity < held->quantity) l.item.instance_guid = fresh_guid();   // the part listed is its own instance; the rest keeps the stack's
    l.item_name = record ? (!record->display_name.empty() ? record->display_name : record->name) : std::to_string(held->item_record_id);
    l.price = static_cast<int64_t>(req->price_per_unit.values[0].amount);
    l.issued = issued;
    l.expires = issued + market::kListingSeconds;
    const svc::InventoryItem taken = *held;
    const items::Inventory kept = inventory_;
    items::Outcome outcome = inventory_.consume(taken.container, taken.slot, taken.instance_guid, req->quantity);
    if (!outcome.ok) {
        inventory_ = kept;
        return refuse(repl::MarketplaceResponse::ErrorGeneric, outcome.refusal);
    }
    auto rows = market_book_->list(l, why);
    if (!rows) {
        inventory_ = kept;
        return refuse(repl::MarketplaceResponse::ErrorGeneric, why);
    }
    ++market_listed_;
    inventory_dirty_ = true;
    ++items_applied_;
    save_market(std::move(*rows), "marketplace listing");
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    add_item_toasts(events, /*removals=*/true);
    queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    answer = market_item(*market_book_->find(l.guid));
    queue("ClientMarketplaceItemRegistrationResponse",
          [answer](bootstrap::PlayerBootstrap& pb) {
              return pb.market_call("ClientMarketplaceItemRegistrationResponse",
                                    [&](const protocol::schema::RpcSchema& r) { return repl::market_registration_response_params(r, answer, repl::MarketplaceResponse::Success); });
          },
          conn, /*must_build=*/true);
    send_market_data(conn, market_seller_id(), market_player(), /*own=*/true);
    spdlog::info("[session] {}: lists {} x {} at {} copper each (listing {})", name(), l.item.quantity, l.item_name, l.price, market::guid_hex(l.guid));
}

void PlayerSession::on_market_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto req = repl::parse_market_purchase_request(rpc.call);
    if (!req || !within_budget(market_budget_, "ServerRequestMarketplaceItemPurchase", now) || !room_for(4)) return;
    const auto answer = [&](repl::MarketplaceResponse response) {
        const repl::GuidValue item = req->item_guid;
        const int32_t quantity = req->quantity;
        queue("ClientMarketplaceItemPurchaseResponse",
              [item, quantity, response](bootstrap::PlayerBootstrap& pb) {
                  return pb.market_call("ClientMarketplaceItemPurchaseResponse", [&](const protocol::schema::RpcSchema& r) { return repl::market_purchase_response_params(r, item, quantity, response); });
              },
              conn, /*must_build=*/true);
    };
    const auto refuse = [&](repl::MarketplaceResponse response, const std::string& why) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: a purchase of {} is refused: {}", name(), market::guid_hex(req->item_guid), why);
        answer(response);
    };
    std::string why;
    if (dead_) return refuse(repl::MarketplaceResponse::InvalidPlayer, "the character is dead");
    if (!open_market(req->market, why)) return refuse(repl::MarketplaceResponse::NodeNotFound, why);
    const market::Listing* listing = market_book_->find(req->item_guid);
    if (!listing || listing->node != market_node_) return refuse(repl::MarketplaceResponse::ItemNotFound, "no such listing at this market");
    const double clock = market_clock();
    const auto cost = market_book_->quote(req->item_guid, req->quantity, market_seller_id(), clock, why);
    if (!cost) return refuse(req->quantity <= 0 ? repl::MarketplaceResponse::InvalidQuantity : repl::MarketplaceResponse::ItemNotFound, why);
    if (!wallet_.can_pay(*cost)) return refuse(repl::MarketplaceResponse::CurrencyNotFound, "the purse holds " + std::to_string(wallet_.copper()) + " copper of " + std::to_string(*cost));
    svc::InventoryItem goods = listing->item;
    goods.quantity = req->quantity;
    goods.instance_guid = fresh_guid();
    goods.container = svc::ItemContainer::Backpack;
    goods.slot = 0;
    if (!inventory_.can_grant({goods}, item_catalog())) return refuse(repl::MarketplaceResponse::InventoryFull, "the bags have no room");
    auto bought = market_book_->buy(req->item_guid, req->quantity, market_seller_id(), clock, why);
    if (!bought) return refuse(repl::MarketplaceResponse::ErrorGeneric, why);
    const items::Outcome outcome = inventory_.grant({goods}, item_catalog());
    if (!outcome.ok) {   // can_grant said yes: this is a bug, and the units go back to the listing on the next start (the rows are not saved)
        spdlog::error("[session] {}: a marketplace purchase does not fit after all ({}): the book holds the change until worldd restarts", name(), outcome.refusal);
        return refuse(repl::MarketplaceResponse::InventoryFull, outcome.refusal);
    }
    wallet_.pay(bought->cost);
    ++market_bought_;
    inventory_dirty_ = true;
    ++items_applied_;
    note_quest_event([record = goods.item_record_id, quantity = goods.quantity](quest::Engine& eng) { eng.collected("Purchase", record, quantity); });
    save_market(std::move(bought->rows), "marketplace purchase");
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    add_item_toasts(events, /*removals=*/false);
    const int64_t delta = -bought->cost;
    queue("purchase answer", [events, delta](bootstrap::PlayerBootstrap& pb) { return pb.purchase_answer(items::kMoneyTier, delta, events); }, conn, /*must_build=*/true);
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    answer(repl::MarketplaceResponse::Success);
    spdlog::info("[session] {}: buys {} x {} from {} for {} copper", name(), req->quantity, listing->item_name, listing->seller_name, bought->cost);
}

void PlayerSession::on_market_removal(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto req = repl::parse_market_removal_request(rpc.call);
    if (!req || !within_budget(market_budget_, "ServerRequestMarketplaceItemRemoval", now) || !room_for(4)) return;
    const auto answer = [&](repl::MarketplaceResponse response) {
        const repl::GuidValue item = req->item_guid;
        queue("ClientMarketplaceItemRemovalResponse",
              [item, response](bootstrap::PlayerBootstrap& pb) {
                  return pb.market_call("ClientMarketplaceItemRemovalResponse", [&](const protocol::schema::RpcSchema& r) { return repl::market_removal_response_params(r, item, response); });
              },
              conn, /*must_build=*/true);
    };
    const auto refuse = [&](repl::MarketplaceResponse response, const std::string& why) {
        ++market_refused_;
        spdlog::log(level_for(market_logged_), "[session] {}: a removal of {} is refused: {}", name(), market::guid_hex(req->item_guid), why);
        answer(response);
    };
    std::string why;
    if (dead_) return refuse(repl::MarketplaceResponse::InvalidPlayer, "the character is dead");
    if (!open_market(req->market, why)) return refuse(repl::MarketplaceResponse::NodeNotFound, why);
    const market::Listing* listing = market_book_->find(req->item_guid);
    if (!listing || listing->seller_id != market_seller_id()) return refuse(repl::MarketplaceResponse::ItemNotFound, "no listing of this character's by that guid");
    svc::InventoryItem back = listing->item;
    back.container = svc::ItemContainer::Backpack;
    back.slot = 0;
    if (inventory_.find_guid(back.instance_guid)) back.instance_guid = fresh_guid();
    if (back.quantity > 0 && !inventory_.can_grant({back}, item_catalog())) return refuse(repl::MarketplaceResponse::InventoryFull, "the bags have no room for the item");
    auto removed = market_book_->remove(req->item_guid, market_seller_id(), why);
    if (!removed) return refuse(repl::MarketplaceResponse::ItemNotFound, why);
    std::vector<bootstrap::ItemEvent> events;
    if (back.quantity > 0) {
        const items::Outcome outcome = inventory_.grant({back}, item_catalog());
        if (!outcome.ok) {
            spdlog::error("[session] {}: a listing taken back does not fit after all ({}): the book holds the change until worldd restarts", name(), outcome.refusal);
            return refuse(repl::MarketplaceResponse::InventoryFull, outcome.refusal);
        }
        events = item_events_for(outcome);
        add_item_toasts(events, /*removals=*/false);
        inventory_dirty_ = true;
        ++items_applied_;
    }
    ++market_removed_;
    save_market(std::move(removed->rows), "marketplace removal");
    if (!events.empty()) queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    answer(repl::MarketplaceResponse::Success);
    send_market_data(conn, market_seller_id(), market_player(), /*own=*/true);
    spdlog::info("[session] {}: takes back {} x {} (listing {})", name(), back.quantity, removed->item.item_record_id, market::guid_hex(req->item_guid));
}

/// The client calls of the player marketplace (session/rpc_table.h).
void PlayerSession::register_market_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceHeaders", [](PlayerSession& s, const RpcContext& c) { s.on_market_headers(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceData", [](PlayerSession& s, const RpcContext& c) { s.on_market_data(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceHeaderUpdate", [](PlayerSession& s, const RpcContext& c) { s.on_market_header_update(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceItemPurchase", [](PlayerSession& s, const RpcContext& c) { s.on_market_purchase(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceItemRegistration", [](PlayerSession& s, const RpcContext& c) { s.on_market_registration(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceItemRemoval", [](PlayerSession& s, const RpcContext& c) { s.on_market_removal(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSearchMarketplaceHeaders", [](PlayerSession& s, const RpcContext& c) { s.on_market_search(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
