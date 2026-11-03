// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/tests/test_worldd_market.cpp
//
//  The player marketplace with no network and no database: market/market_book.h (listing, the storefronts, buying, the proceeds and collecting them,
//  taking a listing back, expiry, the limits, the rows and their revisions, a load of saved rows) and the rows' trip through the character saver
//  (session/position_saver.h: merged by id with the higher revision kept, through a newer save and through a retry).
// ============================================================================
#include "market/market_book.h"
#include "session/position_saver.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace aoc::worldd;
using market::Book;
using market::Listing;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", std::string(msg).c_str()); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", std::string(msg).c_str()); g_fail++; } \
} while (0)

namespace {

constexpr int64_t kNode = 6064633652044300331ll, kOtherNode = 6064633652044300332ll;
const std::string kSeller = "0123456789abcdef0123456789abcdef", kBuyer = "fedcba9876543210fedcba9876543210";
constexpr double kNow = 1793577600.0;

Listing listing(uint32_t n, const std::string& seller, int32_t quantity, int64_t price, int64_t node = kNode, double issued = kNow) {
    Listing l;
    l.guid = market::GuidValue{n, 0x80000000u, 2, 3};
    l.node = node;
    l.seller_id = seller;
    l.seller = market::GuidValue{seller == kSeller ? 11u : 22u, 0, 0, 1};
    l.seller_name = seller == kSeller ? "Seller" : "Buyer";
    l.item.item_record_id = 6064629132706054144ll;
    l.item.instance_guid = market::guid_hex(market::GuidValue{n, 9, 9, 9});
    l.item.quantity = quantity;
    l.item.quality = 3;
    l.item.created_at_ms = 1793577600000;
    l.item_name = n % 2 ? "Copper Ore" : "Ash Wood";
    l.price = price;
    l.issued = issued;
    l.expires = issued + market::kListingSeconds;
    return l;
}

void test_listing_and_storefronts() {
    std::printf("listing, and the storefronts it makes\n");
    Book b;
    std::string why;
    const uint64_t v0 = b.version(kNode);
    const auto rows = b.list(listing(1, kSeller, 10, 25), why);
    CHECK(rows && rows->size() == 1 && rows->front().id == market::listing_id(listing(1, kSeller, 10, 25).guid) && !rows->front().gone, "a listing is one row, under its id");
    CHECK(b.version(kNode) > v0 && v0 >= 1, "the node's version goes up (and is never 0)");
    CHECK(b.headers(kNode, kNow).size() == 1 && b.headers(kNode, kNow)[0].seller_name == "Seller", "the seller's storefront shows at the node");
    CHECK(b.headers(kOtherNode, kNow).empty(), "and at no other node");
    CHECK(!b.list(listing(1, kSeller, 1, 1), why), "the same guid twice is refused");
    b.list(listing(3, kSeller, 1, 5), why);
    CHECK(b.headers(kNode, kNow).size() == 1, "two listings of one seller are one storefront");
    b.list(listing(6, kBuyer, 2, 7), why);
    CHECK(b.headers(kNode, kNow).size() == 2 && b.headers(kNode, kNow)[1].seller_name == "Buyer", "another seller is a second storefront, after the first");
    CHECK(b.for_sale(kNode, kSeller, kNow).size() == 2 && b.own(kNode, kSeller).size() == 2, "the seller's two listings are for sale and in their Sell tab");
    CHECK(b.seller_of(kNode, market::GuidValue{11, 0, 0, 1}) == kSeller && b.seller_of(kNode, market::GuidValue{99, 0, 0, 0}).empty(), "a player guid finds its seller");
    const auto d = b.describe(kNode, kSeller, market::GuidValue{11, 0, 0, 1}, "Seller", "Fine ores", kNow);
    CHECK(d.size() == 1 && d[0].id == market::storefront_id(kNode, kSeller), "a description is the storefront's row");
    CHECK(b.headers(kNode, kNow)[0].description == "Fine ores", "and shows in the storefront");
    const auto search = b.search(kNode, "ORE", std::nullopt, nullptr, kNow);
    CHECK(search.size() == 1 && search[0].seller_name == "Seller", "a search by a part of the item's name (any case) finds the seller's storefront only");
    const auto by_rarity = b.search(kNode, "", 4, [](int64_t, int32_t q) { return q == 3 ? 1 : 0; }, kNow);
    CHECK(by_rarity.empty(), "a rarity no listing has finds nothing");
    CHECK(b.search(kNode, "", 1, [](int64_t, int32_t q) { return q == 3 ? 1 : 0; }, kNow).size() == 2, "the rarity every listing has finds both");
}

void test_buying_and_collecting() {
    std::printf("buying, the proceeds and collecting them\n");
    Book b;
    std::string why;
    const Listing l = listing(1, kSeller, 10, 25);
    b.list(l, why);
    CHECK(!b.quote(l.guid, 1, kSeller, kNow, why), "a seller does not buy their own listing");
    CHECK(!b.quote(l.guid, 0, kBuyer, kNow, why) && !b.quote(l.guid, 11, kBuyer, kNow, why), "0 units and more than listed are refused");
    CHECK(b.quote(l.guid, 4, kBuyer, kNow, why) == 100, "4 units at 25 cost 100");
    const auto bought = b.buy(l.guid, 4, kBuyer, kNow, why);
    CHECK(bought && bought->item.quantity == 4 && bought->cost == 100 && bought->item.item_record_id == l.item.item_record_id, "the buyer gets 4 of the item for 100");
    CHECK(bought && bought->rows.size() == 1 && bought->rows[0].revision > 0, "the listing's row is saved with the buyer");
    CHECK(b.find(l.guid)->item.quantity == 6 && b.find(l.guid)->proceeds == 100, "6 are left and 100 is owed to the seller");
    b.buy(l.guid, 6, kBuyer, kNow, why);
    CHECK(b.find(l.guid) && b.find(l.guid)->item.quantity == 0 && b.find(l.guid)->proceeds == 250, "sold out, the listing stays while 250 is owed");
    CHECK(b.headers(kNode, kNow).empty() && b.for_sale(kNode, kSeller, kNow).empty(), "a sold out listing is no storefront and not for sale");
    CHECK(b.own(kNode, kSeller).size() == 1, "but stays in the seller's Sell tab");
    CHECK(!b.buy(l.guid, 1, kBuyer, kNow, why), "and nothing more is sold");
    const auto none = b.collect(kBuyer, 1000000);
    CHECK(none.copper == 0 && none.rows.empty(), "another character collects nothing");
    const auto part = b.collect(kSeller, 200);
    CHECK(part.copper == 200 && part.rows.size() == 1 && !part.rows[0].gone && b.find(l.guid)->proceeds == 50, "a purse with room for 200 takes 200; 50 stays owed");
    const auto rest = b.collect(kSeller, 1000);
    CHECK(rest.copper == 50 && rest.rows.size() == 1 && rest.rows[0].gone && !b.find(l.guid), "the last 50: the listing, sold out and paid, goes (a tombstone)");
    CHECK(rest.rows[0].revision > part.rows[0].revision, "every change has a higher revision");
}

void test_removal_and_expiry() {
    std::printf("taking a listing back, and expiry\n");
    Book b;
    std::string why;
    const Listing l = listing(1, kSeller, 10, 25);
    b.list(l, why);
    CHECK(!b.remove(l.guid, kBuyer, why), "another character cannot take it back");
    b.buy(l.guid, 3, kBuyer, kNow, why);
    const auto r = b.remove(l.guid, kSeller, why);
    CHECK(r && r->item.quantity == 7 && r->rows.size() == 1 && !r->rows[0].gone, "the seller takes back the 7 left; the listing stays while 75 is owed");
    CHECK(b.find(l.guid)->item.quantity == 0 && b.find(l.guid)->proceeds == 75, "nothing left for sale, the proceeds kept");
    const Listing m = listing(3, kSeller, 2, 9);
    b.list(m, why);
    const auto gone = b.remove(m.guid, kSeller, why);
    CHECK(gone && gone->item.quantity == 2 && gone->rows.size() == 1 && gone->rows[0].gone && !b.find(m.guid), "a listing with nothing owed goes whole (a tombstone)");
    const Listing old = listing(5, kSeller, 1, 9, kNode, kNow - market::kListingSeconds - 1);
    b.list(old, why);
    CHECK(!b.quote(old.guid, 1, kBuyer, kNow, why) && b.for_sale(kNode, kSeller, kNow).empty(), "an expired listing is not sold");
    CHECK(b.own(kNode, kSeller).size() == 2, "it stays in the Sell tab");
    const auto back = b.remove(old.guid, kSeller, why);
    CHECK(back && back->item.quantity == 1, "and goes back to the seller when removed");
}

void test_limits() {
    std::printf("the limits\n");
    Book b;
    std::string why;
    for (uint32_t i = 0; i < market::kMaxListingsPerSeller; ++i) b.list(listing(100 + i, kSeller, 1, 1), why);
    CHECK(!b.list(listing(999, kSeller, 1, 1), why), "one listing past " + std::to_string(market::kMaxListingsPerSeller) + " at one node is refused");
    CHECK(b.list(listing(998, kSeller, 1, 1, kOtherNode), why).has_value(), "another node has its own count");
    CHECK(b.list(listing(997, kBuyer, 1, 1), why).has_value(), "another seller has their own count");
}

void test_rows_and_load() {
    std::printf("the rows, read back\n");
    Book b;
    std::string why;
    Listing l = listing(1, kSeller, 10, 25);
    l.item.persisted_health = 50.5f;
    l.item.lock_expiration = 1234;
    std::vector<svc::MarketRow> rows = *b.list(l, why);
    const auto bought = b.buy(l.guid, 4, kBuyer, kNow, why);
    rows = bought->rows;   // the newest row of the listing
    const auto d = b.describe(kNode, kSeller, l.seller, "Seller", "Fine ores", kNow);
    rows.insert(rows.end(), d.begin(), d.end());
    b.list(listing(3, kSeller, 1, 1), why);
    const auto gone = b.remove(listing(3, kSeller, 1, 1).guid, kSeller, why);
    rows.push_back(gone->rows[0]);
    rows.push_back(svc::MarketRow{"lnot-a-listing", 1, false, "{}"});
    rows.push_back(svc::MarketRow{market::listing_id(market::GuidValue{7, 7, 7, 7}), 2, false, "not json"});
    Book again;
    const size_t skipped = again.load(rows);
    CHECK(skipped == 2, "the two rows that do not read are skipped (" + std::to_string(skipped) + ")");
    const Listing* back = again.find(l.guid);
    CHECK(back && back->item.quantity == 6 && back->proceeds == 100 && back->price == 25 && back->seller_name == "Seller" && back->item_name == l.item_name,
          "the listing reads back as it was: 6 left, 100 owed, its price and names");
    CHECK(back && back->item.persisted_health && *back->item.persisted_health == 50.5f && back->item.lock_expiration == 1234 && back->item.instance_guid == l.item.instance_guid,
          "with its item's persisted state");
    CHECK(!again.find(listing(3, kSeller, 1, 1).guid), "a tombstone loads as nothing");
    CHECK(again.headers(kNode, kNow).size() == 1 && again.headers(kNode, kNow)[0].description == "Fine ores", "the storefront and its description are back");
    const auto next = again.list(listing(9, kSeller, 1, 1), why);
    CHECK(next && next->front().revision > gone->rows[0].revision, "the revisions go on above every loaded row");
}

/// A character service that keeps what it was asked to save.
class FakeApi : public svc::CharacterApi {
public:
    svc::CharacterResult get_character(const std::string&) override { return {}; }
    svc::CallStatus save_position(const std::string&, const svc::Position&) override { return svc::CallStatus::Ok; }
    svc::CallStatus save_inventory(const std::string& id, const std::vector<svc::InventoryItem>& items, const std::vector<svc::WalletEntry>&, const std::vector<svc::ProcessingJob>&,
                                   const std::vector<int64_t>&, const std::vector<svc::WarehouseItem>&, const std::vector<svc::MarketRow>& market) override {
        std::lock_guard<std::mutex> l(mu);
        if (fail_next > 0) {
            --fail_next;
            return svc::CallStatus::Unavailable;
        }
        calls.push_back({id, items.size(), market});
        return svc::CallStatus::Ok;
    }
    svc::CallStatus save_inventory(const std::string& id, const std::vector<svc::InventoryItem>& items, const std::vector<svc::WalletEntry>&, const std::vector<svc::ProcessingJob>&,
                                   const std::vector<int64_t>&, const std::vector<svc::WarehouseItem>&) override {
        std::lock_guard<std::mutex> l(mu);
        calls.push_back({id, items.size(), {}});
        return svc::CallStatus::Ok;
    }
    struct Call {
        std::string id;
        size_t items = 0;
        std::vector<svc::MarketRow> market;
    };
    std::mutex mu;
    std::vector<Call> calls;
    int fail_next = 0;
};

std::vector<svc::InventoryItem> one_item(int32_t quantity) {
    svc::InventoryItem it;
    it.instance_guid = kSeller;
    it.item_record_id = 1;
    it.quantity = quantity;
    return {it};
}

void test_saver_carries_the_rows() {
    std::printf("the rows through the character saver\n");
    {
        FakeApi api;
        CharacterSaver::Config cfg;
        cfg.retry_delay = std::chrono::milliseconds(1);
        CharacterSaver saver(api, cfg);
        // three saves while the first waits: the rows of all three are merged, the newest revision of each id kept, with the newest inventory
        {
            std::lock_guard<std::mutex> l(api.mu);   // the worker cannot finish a call while the test holds the lock: the saves pile up behind it
            saver.save_inventory(kSeller, one_item(5), {}, {}, {}, {}, {svc::MarketRow{"la", 3, false, "{\"v\":3}"}});
            saver.save_inventory(kSeller, one_item(4), {}, {}, {}, {}, {svc::MarketRow{"la", 2, false, "{\"v\":2}"}, svc::MarketRow{"lb", 1, false, "{}"}});
            saver.save_inventory(kSeller, one_item(3), {}, {}, {}, {});
        }
        CHECK(saver.flush(std::chrono::milliseconds(2000)), "the saves are done");
        std::lock_guard<std::mutex> l(api.mu);
        // what the store keeps: per id the highest revision any call carried (the worker may have taken the first save before the others came)
        std::map<std::string, int64_t> kept;
        for (const FakeApi::Call& c : api.calls) {
            for (const svc::MarketRow& r : c.market) kept[r.id] = std::max(kept[r.id], r.revision);
        }
        CHECK(kept.size() == 2 && kept["la"] == 3 && kept["lb"] == 1, "every row reaches the store, the newest revision of each kept");
        CHECK(!api.calls.empty() && api.calls.back().items == 1, "with the newest inventory");
    }
    {
        FakeApi api;
        api.fail_next = 1;
        CharacterSaver::Config cfg;
        cfg.retry_delay = std::chrono::milliseconds(1);
        CharacterSaver saver(api, cfg);
        saver.save_inventory(kSeller, one_item(2), {}, {}, {}, {}, {svc::MarketRow{"lc", 5, false, "{}"}});
        CHECK(saver.flush(std::chrono::milliseconds(2000)), "a save the service could not take is tried again");
        std::lock_guard<std::mutex> l(api.mu);
        CHECK(api.calls.size() == 1 && api.calls[0].market.size() == 1 && api.calls[0].market[0].id == "lc", "and its rows go with the retry");
    }
}

}  // namespace

int main() {
    test_listing_and_storefronts();
    test_buying_and_collecting();
    test_removal_and_expiry();
    test_limits();
    test_rows_and_load();
    test_saver_carries_the_rows();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
