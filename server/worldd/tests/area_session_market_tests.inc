// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/tests/area_session_market_tests.inc  (listed in tests/area_session_registered.inc)
//
//  The player marketplace through a session (session/player_market.cpp): the marketplace NPC's use opens the window for its node with the NPC's guid; a
//  listing takes the stack out of the bag and saves the book's row with the inventory; the storefronts and the Sell tab show it; the seller cannot buy their
//  own; another player's purchase leaves proceeds the seller collects at the next opening; the listing taken back returns to the bag; a request with another
//  guid, or with the marketplace off, does nothing.
// ============================================================================
#include "protocol/repl/marketplace_rpc.h"

namespace {

constexpr int64_t kTestMarketplaceInstance = 6064632772964253696ll;   // Node_Vendor_Gear_marketplace

struct MarketHarness : Harness {
    uint64_t market = 0;
    explicit MarketHarness(bool enabled = true) : Harness(cyb_record()) {
        fx.world->set_market_enabled(enabled);
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) {
            std::printf("  [FAIL] no vendor template\n");
            std::exit(2);
        }
        spawns::InstanceParams p;
        p.x = kCx + 500;
        p.y = kCy;
        p.z = bootstrap::kDefaultSpawn.z;
        p.server_id = kServerId;
        p.name = "Marketplace";
        p.seed = fx.guids.next().ObjectId;
        std::string err;
        auto w = spawns::instantiate(*vendor_t, fx.minter(), p, err);
        if (!w) {
            std::printf("  [FAIL] instantiate: %s\n", err.c_str());
            std::exit(2);
        }
        w->npc.instance_guid = kTestMarketplaceInstance;
        w->npc.name = "Marketplace";
        world::allow_inputs(w->actor);
        market = w->id;
        if (!fx.world->registry().add(std::move(*w))) std::exit(2);
        start();
    }
    repl::GuidValue market_guid() const {
        const world::WorldActor* a = fx.world->registry().find(market);
        const auto& g = a->actor.guid;
        return repl::GuidValue{static_cast<uint32_t>(g.ObjectId), static_cast<uint32_t>(g.ObjectId >> 32), g.ServerId, g.Randomizer};
    }
    void use_market() {
        const world::WorldActor* a = fx.world->registry().find(market);
        client_request(interact(a->npc.interact_component), pawn_ch);
    }
    template <class F>
    void pc_call(const char* rpc, F build) {
        const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
        const auto params = build(*pc.rpc(rpc));
        if (!params) {
            std::printf("  [FAIL] %s does not build\n", rpc);
            return;
        }
        client_request(actor_rpc(pc, rpc, *params), pc_ch);
    }
};

const repl::RpcCall* last_call(const std::vector<repl::RpcCall>& calls, const char* name) {
    const repl::RpcCall* found = nullptr;
    for (const repl::RpcCall& c : calls) {
        if (std::string(c.rpc->name) == name) found = &c;
    }
    return found;
}

void test_marketplace_flow() {
    std::printf("the player marketplace through a session\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_MARKET_LOG") ? spdlog::level::info : spdlog::level::off);
    const svc::InventoryItem* potions = h.s->inventory().at(Bag, 1);
    CHECK(potions && potions->quantity == 5, "the character has 5 potions in bag slot 1");
    if (!potions) return;
    const std::string stack = potions->instance_guid;

    // the use: the window, with the NPC's guid and the node's taxes
    size_t from = h.got.size();
    h.use_market();
    auto calls = pc_calls_since(h, from);
    const repl::RpcCall* open = last_call(calls, "ClientOpenMarketplaceUI");
    CHECK(open && h.s->market_opens() == 1 && count_calls(calls, "ClientOpenVendor") == 0, "the marketplace's use opens the marketplace window, not a shop");
    bool right_guid = false, has_taxes = false;
    if (open) {
        const auto word = [&](const char* path) { return repl::rpcf::int_or(repl::rpcf::value_of(*open->rpc, open->params, path), 0).value_or(-1); };
        const repl::GuidValue g = h.market_guid();
        right_guid = static_cast<uint32_t>(word("MarketplaceGuid.A")) == g.a && static_cast<uint32_t>(word("MarketplaceGuid.B")) == g.b &&
                     static_cast<uint32_t>(word("MarketplaceGuid.C")) == g.c && static_cast<uint32_t>(word("MarketplaceGuid.D")) == g.d;
        const repl::Value* t = repl::rpcf::value_of(*open->rpc, open->params, "NodeTaxDetails");
        has_taxes = t && t->get_if<repl::NodeTaxDetails>() && t->get_if<repl::NodeTaxDetails>()->node != 0 && !t->get_if<repl::NodeTaxDetails>()->entries.empty();
    }
    CHECK(right_guid, "MarketplaceGuid is the NPC's guid");
    CHECK(has_taxes, "with the tax table of the node it stands in");

    // a listing: 2 of the 5 potions at 30 copper
    const repl::GuidValue market = h.market_guid();
    repl::MarketRegistrationRequest list;
    list.market = market;
    list.item = items::to_instance(*potions);
    list.quantity = 2;
    list.price_per_unit.values.push_back({items::kMoneyTier, 456318420933392641ll, 30});
    const size_t saves_before = h.sink.market.size();
    from = h.got.size();
    h.pc_call("ServerRequestMarketplaceItemRegistration", [&](const schema::RpcSchema& r) { return repl::market_registration_request_params(r, list); });
    calls = pc_calls_since(h, from);
    const repl::RpcCall* reg = last_call(calls, "ClientMarketplaceItemRegistrationResponse");
    const auto reg_answer = reg ? repl::parse_market_response(*reg) : std::nullopt;
    CHECK(reg_answer && reg_answer->response == repl::MarketplaceResponse::Success && h.s->market_listed() == 1, "the listing is answered SUCCESS");
    CHECK(reg_answer && reg_answer->item.price_per_unit.values.size() == 1 && reg_answer->item.price_per_unit.values[0].amount == 30 && reg_answer->item.item.quantity == 2 &&
              reg_answer->item.status == repl::MarketplaceItemStatus::Listed,
          "the answer names the listing: 2 at 30 copper, listed");
    const svc::InventoryItem* left = h.s->inventory().at(Bag, 1);
    CHECK(left && left->quantity == 3 && left->instance_guid == stack, "3 potions stay in the bag, in their stack");
    CHECK(count_calls(calls, "ClientReceiveStorageEvent") >= 1, "the bag's change is sent");
    CHECK(h.sink.market.size() > saves_before && !h.sink.market.back().empty(), "the book's row is saved in the inventory's call");
    const repl::GuidValue listing = reg_answer ? reg_answer->item.item_guid : repl::GuidValue{};
    const repl::RpcCall* own = last_call(calls, "ClientReceiveMarketplaceData");
    const auto own_data = own ? repl::parse_market_data(*own) : std::nullopt;
    CHECK(own_data && own_data->items.size() == 1 && own_data->items[0].item_guid == listing, "the Sell tab gets the seller's listings again");

    // the storefronts
    from = h.got.size();
    h.pc_call("ServerRequestMarketplaceHeaders", [&](const schema::RpcSchema& r) { return repl::market_headers_request_params(r, {market, 0}); });
    calls = pc_calls_since(h, from);
    const repl::RpcCall* hc = last_call(calls, "ClientReceiveMarketplaceHeaders");
    const auto headers = hc ? repl::parse_market_headers(*hc) : std::nullopt;
    CHECK(headers && headers->headers.size() == 1 && headers->complete && headers->rarity == repl::kMarketplaceNoSearchRarity && headers->version >= 1,
          "one storefront, complete, as no search");
    // a search: by name and with no rarity
    from = h.got.size();
    h.pc_call("ServerSearchMarketplaceHeaders", [&](const schema::RpcSchema& r) { return repl::market_search_request_params(r, {market, "zzz", repl::kMarketplaceAnyRarity}); });
    calls = pc_calls_since(h, from);
    hc = last_call(calls, "ClientReceiveMarketplaceHeaders");
    const auto none = hc ? repl::parse_market_headers(*hc) : std::nullopt;
    CHECK(none && none->headers.empty() && none->search == "zzz", "a search no item matches finds no storefront, and carries its words back");

    // the seller cannot buy their own
    from = h.got.size();
    h.pc_call("ServerRequestMarketplaceItemPurchase", [&](const schema::RpcSchema& r) { return repl::market_purchase_request_params(r, {market, listing, 1}); });
    calls = pc_calls_since(h, from);
    const repl::RpcCall* pc = last_call(calls, "ClientMarketplaceItemPurchaseResponse");
    const auto refused = pc ? repl::parse_market_response(*pc) : std::nullopt;
    CHECK(refused && refused->response != repl::MarketplaceResponse::Success && h.s->market_bought() == 0, "the seller's purchase of their own listing is refused");

    // another player buys one; the proceeds wait for the seller's next opening
    std::string why;
    auto other = h.fx.world->market_book()->buy(listing, 1, "fedcba9876543210fedcba9876543210", 1.0e9, why);
    CHECK(other && other->cost == 30, "another player buys one potion for 30");
    const int64_t copper_before = h.s->wallet().copper();
    h.use_market();
    CHECK(h.s->wallet().copper() == copper_before + 30, "the seller collects the 30 copper at the next opening");

    // a request naming another marketplace does nothing
    repl::GuidValue wrong = market;
    wrong.d ^= 1;
    from = h.got.size();
    h.pc_call("ServerRequestMarketplaceItemRemoval", [&](const schema::RpcSchema& r) { return repl::market_removal_request_params(r, {wrong, listing}); });
    calls = pc_calls_since(h, from);
    const repl::RpcCall* rc = last_call(calls, "ClientMarketplaceItemRemovalResponse");
    const auto wrong_answer = rc ? repl::parse_market_response(*rc) : std::nullopt;
    CHECK(wrong_answer && wrong_answer->response == repl::MarketplaceResponse::NodeNotFound && h.s->market_removed() == 0, "a removal naming another marketplace is refused");

    // the seller takes the last one back
    const size_t bag_before = bag_items(h);
    from = h.got.size();
    h.pc_call("ServerRequestMarketplaceItemRemoval", [&](const schema::RpcSchema& r) { return repl::market_removal_request_params(r, {market, listing}); });
    calls = pc_calls_since(h, from);
    rc = last_call(calls, "ClientMarketplaceItemRemovalResponse");
    const auto removed = rc ? repl::parse_market_response(*rc) : std::nullopt;
    CHECK(removed && removed->response == repl::MarketplaceResponse::Success && h.s->market_removed() == 1, "the removal is answered SUCCESS");
    CHECK(bag_items(h) >= bag_before && h.s->inventory().count_of(fi::kPotion) == 4, "the potion left comes back: 4 in the bags");
    CHECK(!h.fx.world->market_book()->find(listing), "the listing, sold and paid and taken back, is gone from the book");
}

void test_marketplace_off() {
    std::printf("the player marketplace when the character service did not list it\n");
    MarketHarness h(/*enabled=*/false);
    h.play();
    h.run(milliseconds(1500));
    const size_t from = h.got.size();
    h.use_market();
    const auto calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientOpenMarketplaceUI") == 0 && h.s->market_opens() == 0 && h.s->market_refused() == 1, "the window does not open");
}

}  // namespace

AOC_REGISTER_TEST(test_marketplace_flow);
AOC_REGISTER_TEST(test_marketplace_off);
