// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  vendor/player_currency.cpp  (the economy: a reward's currency lines)
//
//  It is part of session/player_session.cpp (included at its end, inside the namespace).
//
//  A crate buyer.  Using a shopkeeper whose shop buys crates opens the crate window; its button sells the crate on the back, as an ordinary sale of that instance [exe: 0x146794d90].  The crate's
//  record says what it is worth and in which currency: the data prices Market and Citizen crates in money, Node crates in the node currency, Guild crates in Seals of Valor and Harbor and Lawless
//  crates in Verdant Flux.  The window's per-commodity counts and per-node costs are sent empty, and the price does not change with the town or with how many were sold: nothing keeps them yet.
//  No retail capture shows any of this; the client's code gives the calls, the data the prices.
//
//  A reward table's Currency lines name a kind of a tier.  Money comes to copper, the node currency and any other tier that is no item (reputation) go to the purse under the kind's own Guid: the
//  purse saves them all and the client's list shows the first two.  A tier made of items is not in any table of the data today and is logged.  What a kind's tier allows past its largest quantity
//  is not added.  The callout and the new balance are the shape a GM's grant sends.  No retail capture shows a quest paying money or the node currency, so the order of the frames is modelled.
// ============================================================================
#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

void PlayerSession::pay_rolled_currency(Connection& conn, const std::vector<combat::RolledCurrency>& paid, const char* why) {
    if (paid.empty()) return;
    const items::CurrencyTable* currencies = world_ ? world_->currencies() : nullptr;
    if (!currencies) {
        spdlog::warn("[session] {}: {} currency line(s) of {} are not paid: this world has no currency tiers", name(), paid.size(), why);
        return;
    }
    for (const combat::RolledCurrency& line : paid) {
        const items::CurrencyTier* in_tier = nullptr;
        const items::CurrencyKind* kind = nullptr;
        for (const items::CurrencyTier& tier : currencies->tiers()) {
            for (const items::CurrencyKind& k : tier.kinds) {
                if (k.def == line.currency) {
                    in_tier = &tier;
                    kind = &k;
                }
            }
        }
        if (!in_tier || !kind || in_tier->carried) {
            spdlog::warn("[session] {}: {} of currency {} from {} is not paid: {}", name(), line.amount, line.currency, why, in_tier ? "its tier is made of items" : "no tier holds it");
            continue;
        }
        const bool money = in_tier->guid == items::kMoneyTier;
        const int64_t base = line.amount * kind->value;
        const int64_t added = money ? wallet_.give(base)
                                    : wallet_.give_currency(in_tier->kinds.front().def, base, in_tier->max_base_quantity > 0 ? in_tier->max_base_quantity : items::kGlintCap);
        if (added < 1) {
            spdlog::info("[session] {}: {} of currency {} from {} is not added: the purse is full", name(), line.amount, line.currency, why);
            continue;
        }
        inventory_dirty_ = true;
        inventory_changed_ = true;
        if (money) pb_.set_currency(items::kCopperCurrency, wallet_.copper());
        else if (in_tier->guid == items::kNodeCurrencyTier) pb_.set_currency(items::kNodeCurrency, wallet_.balance(items::kNodeCurrency));
        const int64_t tier_guid = in_tier->guid;
        queue("ClientShowCurrencyCallout", [tier_guid, added](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(tier_guid, added); }, conn, /*must_build=*/true);
        spdlog::info("[session] {}: {} from {} in {}", name(), added, why, in_tier->name);
    }
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
}

bool PlayerSession::open_crate_sell(Connection& conn, const world::WorldActor& npc) {
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::VendorData::Def* def = shops && npc.npc.vendor_guid != 0 ? shops->def(npc.npc.vendor_guid) : nullptr;
    if (!def || !vendor::buys_crates(def->name) || !shops->is_open(npc.npc.vendor_guid)) return false;
    const GoodPosition at = checker_.good();
    const double dx = npc.x - at.x, dy = npc.y - at.y, dz = npc.z - at.z;
    if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm || !room_for(1)) return true;   // (it is a crate buyer: nothing else answers this use)
    vendor_actor_ = npc.id;
    vendor_def_ = npc.npc.vendor_guid;
    vendor_by_talk_ = true;   // (the sale's check: no use link names this shop)
    ++vendor_opens_;
    const int64_t shop = vendor_def_;
    const FIntrepidNetworkGUID actor = npc.actor.guid;
    queue("ClientOpenCrateSellWindow", [shop, actor](bootstrap::PlayerBootstrap& pb) { return pb.open_crate_sell(shop, actor); }, conn, /*must_build=*/true);
    spdlog::log(level_for(vendor_logged_), "[session] {}: opens the crate window of {} ({})", name(), npc.npc.name, def->name);
    return true;
}

void PlayerSession::sell_crate(Connection& conn, const svc::InventoryItem& held, Clock::time_point now) {
    const char* what = "ServerHandleVendorTransaction (a crate)";
    const auto refuse = [&](const std::string& why, const char* to_player = nullptr) {
        ++vendor_refused_;
        if (to_player) say(conn, to_player, now);
        refuse_item_request(conn, what, why, now);
    };
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const items::CurrencyTable* currencies = world_ ? world_->currencies() : nullptr;
    if (!shops || !db || !currencies) return refuse("worldd has no town shops");
    const svc::InventoryItem crate = held;
    std::string error;
    const auto facts = db->item_sale(crate.item_record_id, error);
    if (!facts) return refuse(error);
    // the currency the record is worth something in, money first
    const int rarity = facts->rarity_of(crate.quality);
    int64_t tier_guid = 0;
    if (facts->values.count({rarity, items::kMoneyTier})) tier_guid = items::kMoneyTier;
    else {
        for (const auto& [key, value] : facts->values) {
            if (key.first == rarity) {
                tier_guid = key.second;
                break;
            }
        }
    }
    const items::CurrencyTier* tier = currencies->tier(tier_guid);
    if (!tier) return refuse("the crate is worth nothing in a currency this server knows", "This crate cannot be sold here.");
    const vendor::SaleQuote quote = vendor::quote_sale(shops->data(), vendor_def_, crate.item_record_id, 1, crate.quality, *facts, tier_guid, progress_.level);
    if (!quote.ok) return refuse("the crate is not sold: " + quote.refusal, "This merchant does not buy that crate.");

    items::Inventory kept = inventory_;
    items::Outcome gone = inventory_.remove_cargo(crate.instance_guid);
    if (!gone.ok) return refuse(gone.refusal);
    if (tier_guid == items::kMoneyTier) {
        if (quote.total > wallet_.room()) {
            inventory_ = std::move(kept);
            return refuse("the purse cannot hold the price", "Your purse is too full to sell that crate.");
        }
        wallet_.give(quote.total);
        pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    } else if (!tier->carried) {
        wallet_.give_currency(tier->kinds.front().def, quote.total, tier->max_base_quantity > 0 ? tier->max_base_quantity : items::kGlintCap);
        if (tier_guid == items::kNodeCurrencyTier) pb_.set_currency(items::kNodeCurrency, wallet_.balance(items::kNodeCurrency));
    } else {
        // a price in a tier of items comes as its smallest kind, into the bags
        const auto rec = db->item(tier->kinds.front().item, error);
        if (!rec) {
            inventory_ = std::move(kept);
            return refuse("content.db has no item for the crate's price");
        }
        std::vector<svc::InventoryItem> pay;
        stacks_of(tier->kinds.front().item, static_cast<int32_t>(quote.total), *rec, pay, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000);
        for (svc::InventoryItem& it : pay) it.instance_guid = fresh_guid();
        const items::Outcome paid = inventory_.grant(std::move(pay), item_catalog());
        if (!paid.ok) {
            inventory_ = std::move(kept);
            return refuse(paid.refusal, "You have no room for the price of that crate.");
        }
        gone.events.insert(gone.events.end(), paid.events.begin(), paid.events.end());
    }
    apply_outcome(conn, gone, "a crate was sold");
    note_quest_event([record = crate.item_record_id](quest::Engine& eng) { eng.collected("Sell", record, 1); });
    const int64_t shown = quote.total;
    queue("ClientShowCurrencyCallout", [tier_guid, shown](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(tier_guid, shown); }, conn, /*must_build=*/true);
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::info("[session] {}: sold the crate {} for {} of {}", name(), crate.item_record_id, quote.total, tier->name);
}

}}  // namespace aoc::worldd
