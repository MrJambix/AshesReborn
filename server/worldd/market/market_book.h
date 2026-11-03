// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/market/market_book.h
//
//  The player marketplace's book: the listings of every node's market and the storefronts' descriptions.  Pure logic: no sockets, no session.
//
//  The market holds what is listed.  A listed item leaves the seller's bags into the book; a purchase takes units out of the listing into the buyer's
//  bags and leaves their price in the listing as proceeds; the seller collects the proceeds the next time they open any market.  So every operation
//  touches the book and ONE character, the one who acts: what it changes in the book (whole rows) is saved in the same call, and so in the same
//  database transaction, as that character's inventory and purse (svc::MarketRow, CharacterSaver).  An offline seller is never written.
//
//  Rows carry a revision, a counter of the book that only grows: the store keeps the higher one, so two characters' saves that reach it out of order
//  cannot put back an older row.  A listing that is gone is saved as a tombstone (`gone`) for the same reason; the store drops the tombstones when
//  worldd starts and loads the book.
//
//  Modelled (no recording of the marketplace exists, and its rules are the retail server's, which the client does not hold):
//    * a listing lasts kListingSeconds (seven days); an expired one is no longer sold and goes back to the seller when they remove it;
//    * no listing fee and no tax: the buyer pays the seller's price, the seller gets all of it;
//    * at most kMaxListingsPerSeller listings of one seller at one node;
//    * the price is money (copper) only, at most items::kMoneyCap a unit;
//    * a storefront's description is at most 50 characters (the client cuts it there too).
// ============================================================================
#pragma once

#include "protocol/repl/value.h"
#include "svc/services.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::market {

using GuidValue = ::aoc::protocol::repl::GuidValue;

inline constexpr double  kListingSeconds = 7.0 * 86400.0;
inline constexpr size_t  kMaxListingsPerSeller = 30;
inline constexpr size_t  kMaxDescriptionChars = 50;
inline constexpr size_t  kMaxSearchChars = 50;
inline constexpr int64_t kMaxPricePerUnit = 10'000'000;   // items::kMoneyCap

/// One listing.  `item` is what the book holds: its quantity is the units still for sale (its container and slot mean nothing here).
struct Listing {
    GuidValue         guid;            // the MarketplaceItemGuid the client names
    int64_t           node = 0;
    std::string       seller_id;       // the seller's character id (32 hex digits)
    GuidValue         seller;          // the seller's player guid as the client knows it (the storefront's PlayerGuid)
    std::string       seller_name;
    svc::InventoryItem item;
    std::string       item_name;
    int64_t           price = 0;       // copper per unit
    double            issued = 0;      // Unix seconds
    double            expires = 0;
    int64_t           proceeds = 0;    // copper owed to the seller for units sold and not yet collected
    int64_t           revision = 0;
    bool operator==(const Listing&) const = default;

    bool expired(double now) const { return now >= expires; }
    /// Still sold to buyers.
    bool active(double now) const { return item.quantity > 0 && !expired(now); }
};

/// A storefront's description: one per seller per node.
struct Storefront {
    int64_t     node = 0;
    std::string seller_id;
    GuidValue   seller;
    std::string seller_name;
    std::string description;
    double      issued = 0;
    int64_t     revision = 0;
    bool operator==(const Storefront&) const = default;
};

/// What a storefront list shows of one seller.
struct Header {
    GuidValue   seller;
    std::string seller_name;
    std::string description;
    double      issued = 0;
    bool operator==(const Header&) const = default;
};

std::string guid_hex(const GuidValue& g);
std::optional<GuidValue> guid_from_hex(const std::string& hex);

/// The rows of the store (svc::MarketRow): a listing's id is "l" and its guid, a storefront's "s", its node and its seller.
svc::MarketRow row_of(const Listing& l);
svc::MarketRow row_of(const Storefront& s);
svc::MarketRow tombstone(const std::string& id, int64_t revision);
std::string listing_id(const GuidValue& g);
std::string storefront_id(int64_t node, const std::string& seller_id);
/// A stored row back; nullopt for a tombstone or a row that does not read.
std::optional<Listing>    listing_of(const svc::MarketRow& row);
std::optional<Storefront> storefront_of(const svc::MarketRow& row);

class Book {
public:
    /// Fills an empty book from the store's rows (tombstones and rows that do not read are skipped and counted).  The revision counter starts above every row's.
    size_t load(const std::vector<svc::MarketRow>& rows);

    // ---- what the windows show
    const Listing* find(const GuidValue& g) const;
    /// The storefronts of the node: every seller with at least one listing still sold, in the order they first listed.
    std::vector<Header> headers(int64_t node, double now) const;
    /// The storefronts whose listings still sold have an item whose name holds `text` (any case) and, unless `rarity` is nullopt, of that rarity
    /// (rarity_of(item record, quality)).
    std::vector<Header> search(int64_t node, const std::string& text, std::optional<int> rarity, const std::function<int(int64_t, int32_t)>& rarity_of, double now) const;
    /// A storefront's listings as a buyer sees them: those still sold.
    std::vector<const Listing*> for_sale(int64_t node, const std::string& seller_id, double now) const;
    /// The seller's own listings at the node, sold out ones with proceeds and expired ones included: the Sell tab.
    std::vector<const Listing*> own(int64_t node, const std::string& seller_id) const;
    /// Bumped by every change of what the node's market shows; never 0.
    uint64_t version(int64_t node) const;
    /// The seller id of a player guid at a node, as the storefronts know it (empty when the guid sells nothing there).
    std::string seller_of(int64_t node, const GuidValue& player) const;
    const Storefront* storefront(int64_t node, const std::string& seller_id) const;
    size_t listings() const { return listings_.size(); }

    // ---- what changes it: each returns the rows to save with the acting character's inventory (empty when nothing changed)
    /// A new listing (the caller took the item out of the seller's bags).  Refused (nullopt, with `why`) past kMaxListingsPerSeller.
    std::optional<std::vector<svc::MarketRow>> list(Listing l, std::string& why);
    /// `quantity` units of a listing bought: the listing's item with that quantity (the caller gives it a new instance guid), the cost and the rows.
    struct Bought {
        svc::InventoryItem          item;
        int64_t                     cost = 0;
        std::vector<svc::MarketRow> rows;
    };
    std::optional<Bought> buy(const GuidValue& g, int32_t quantity, const std::string& buyer_id, double now, std::string& why);
    /// The cost of `quantity` units, refused as buy() would be.
    std::optional<int64_t> quote(const GuidValue& g, int32_t quantity, const std::string& buyer_id, double now, std::string& why) const;
    /// The seller takes a listing back: the units left (quantity 0 when it sold out) and the rows.  Proceeds still owed stay in the book for collect().
    struct Removed {
        svc::InventoryItem          item;
        std::vector<svc::MarketRow> rows;
    };
    std::optional<Removed> remove(const GuidValue& g, const std::string& seller_id, std::string& why);
    /// The seller's proceeds at every node, at most `room` copper of them: what is paid and the rows.  A listing sold out and paid in full goes.
    struct Collected {
        int64_t                     copper = 0;
        size_t                      listings = 0;
        std::vector<svc::MarketRow> rows;
    };
    Collected collect(const std::string& seller_id, int64_t room);
    /// A storefront's description (the caller checked its length).
    std::vector<svc::MarketRow> describe(int64_t node, const std::string& seller_id, const GuidValue& seller, const std::string& seller_name, const std::string& text, double now);

private:
    int64_t next_revision() { return ++revision_; }
    void touch(int64_t node) { ++versions_[node]; }

    std::map<std::string, Listing>    listings_;      // by listing_id
    std::vector<std::string>          order_;         // listing ids in the order they were listed
    std::map<std::string, Storefront> storefronts_;   // by storefront_id
    std::map<int64_t, uint64_t>       versions_;
    int64_t                           revision_ = 0;
};

}  // namespace aoc::worldd::market
