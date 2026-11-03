// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_marketplace.cpp
//
//  The player marketplace's calls (protocol/repl/marketplace_rpc.h) and FCurrencyContainer.  No recording of any marketplace call exists, so these
//  are round trips over the schema, not retail bits:
//
//    * FCurrencyContainer: the exact bits of a price (an int32 count, then per pair 64 + 64 + 64 bits), read back; a count above the limit and a
//      cut stream are refused.
//    * Every client request and every server answer: built from its typed form, written, read with the schema entry of its RPC and parsed back
//      equal, with an item, a price, strings and the guids' sign bits set.
//    * Hostile input: every truncation of every written call reads or is refused without crashing, and what reads parses back to a call that
//      writes again.
// ============================================================================
#include "test_support.h"

#include "protocol/repl/codecs.h"
#include "protocol/repl/marketplace_rpc.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/wire/packet_reader.h"

#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using namespace tsupport;
using emit::BunchWriter;
using wire::PacketReader;

static const schema::RpcSchema& rpc_of(const char* name) {
    const schema::RpcSchema* r = schema_named("AoCPlayerControllerBP_C").rpc(name);
    if (!r) { std::printf("  [FAIL] no RPC %s\n", name); std::exit(2); }
    return *r;
}

static std::optional<RpcCall> read_call(const schema::RpcSchema& rpc, const Bits& bits) {
    return with_exact(bits, [&](const uint8_t* d, size_t, size_t n) -> std::optional<RpcCall> {
        size_t pos = 0;
        auto p = read_rpc_params(rpc, d, n, pos, {});
        if (!p || pos != n) return std::nullopt;
        RpcCall c;
        c.rpc = &rpc;
        c.params = std::move(*p);
        c.decoded = true;
        c.param_bits = n;
        return c;
    });
}

static std::optional<Bits> write_call(const schema::RpcSchema& rpc, const RpcParams& params) {
    BunchWriter w;
    bool failed = false;
    write_rpc_params(rpc, params, w, {}, &failed);
    if (failed) return std::nullopt;
    return Bits(w);
}

static CurrencyContainer copper(uint64_t amount) {
    CurrencyContainer c;
    c.values.push_back({6064629127873626112ll, 456318420933392641ll, amount});
    return c;
}

static ItemInstance an_item() {
    ItemInstance i;
    i.has_item = true;
    i.record_id = 6064629132706054144ull;
    i.quantity = 7;
    i.quality = 3;
    i.durability = 100.0f;
    i.guid = GuidValue{0x80000001u, 2u, 0xFFFFFFFFu, 4u};
    i.creation_time = 638000000000000000ll;
    return i;
}

static MarketplaceItemData a_listing() {
    MarketplaceItemData d;
    d.node = 6064633652044300331ll;
    d.player = GuidValue{0xDEADBEEFu, 1u, 2u, 0x7FFFFFFFu};
    d.player_name = "Seller";
    d.item_guid = GuidValue{0x90000000u, 9u, 8u, 7u};
    d.item = an_item();
    d.item_name = "Copper Ore";
    d.price_per_unit = copper(1234);
    d.issue_date = 1793577600.0;
    d.expiration_date = 1794182400.0;
    d.status = MarketplaceItemStatus::Sold;
    return d;
}

static void test_currency_container() {
    std::printf("FCurrencyContainer:\n");
    const CurrencyContainer price = copper(250);
    BunchWriter w;
    CHECK(write_struct("FCurrencyContainer", Value(price), w), "a price of one tier writes");
    const Bits bits(w);
    CHECK(bits.size() == 32 + 3 * 64, "it is an int32 count and three 64-bit words: " + num(bits.size()) + " bits");
    const auto back = with_exact(bits, [](const uint8_t* d, size_t bytes, size_t n) -> std::optional<Value> {
        PacketReader r(d, bytes, n);
        return read_struct("FCurrencyContainer", r);
    });
    const CurrencyContainer* c = back ? back->get_if<CurrencyContainer>() : nullptr;
    CHECK(c && *c == price, "it reads back to the same tier, type and amount");

    CurrencyContainer big;
    big.values.resize(65);
    BunchWriter w2;
    CHECK(!write_struct("FCurrencyContainer", Value(big), w2), "65 pairs are refused by the writer (the limit is 64)");
    BunchWriter w3;
    w3.write_uint32(1000);
    const Bits huge(w3);
    const auto refused = with_exact(huge, [](const uint8_t* d, size_t bytes, size_t n) { PacketReader r(d, bytes, n); return read_struct("FCurrencyContainer", r); });
    CHECK(!refused, "a count of 1,000 is refused by the reader");
    const Bits cut = bits.slice(0, bits.size() - 1);
    const auto short_read = with_exact(cut, [](const uint8_t* d, size_t bytes, size_t n) { PacketReader r(d, bytes, n); return read_struct("FCurrencyContainer", r); });
    CHECK(!short_read, "a container cut by one bit is refused");
}

struct Case {
    const char* rpc;
    std::function<std::optional<RpcParams>(const schema::RpcSchema&)> build;
    std::function<bool(const RpcCall&)> same;   // the call parses back to what was built
};

static std::vector<Case> cases() {
    const GuidValue market{0x81234567u, 0x11111111u, 0xFFFFFFFEu, 0x22222222u};
    const MarketHeadersRequest headers_req{market, 42};
    const MarketDataRequest data_req{market, GuidValue{1, 2, 3, 0x80000000u}};
    const MarketHeaderUpdateRequest update_req{market, "Fine ores, fair prices"};
    const MarketPurchaseRequest buy_req{market, GuidValue{5, 6, 7, 8}, 3};
    MarketRegistrationRequest list_req;
    list_req.market = market;
    list_req.item = an_item();
    list_req.quantity = 5;
    list_req.price_per_unit = copper(99);
    const MarketRemovalRequest remove_req{market, GuidValue{9, 10, 11, 12}};
    const MarketSearchRequest search_req{market, "ore", kMarketplaceAnyRarity};

    NodeTaxDetails taxes;
    taxes.node = 6064633652044300331ll;
    taxes.entries.push_back({"NodeTax", 6064632157081894912ll, 0, 0.5f, 1.0f, true, true});

    MarketHeaders headers;
    headers.node = 6064633652044300331ll;
    headers.version = 7;
    headers.headers.push_back({headers.node, GuidValue{1, 2, 3, 4}, "Seller", "Fine ores", 1793577600.0, false});
    headers.headers.push_back({headers.node, GuidValue{0x80000000u, 0, 0, 1}, "Other", "", 1793577601.5, true});
    MarketData data;
    data.player = a_listing().player;
    data.items.push_back(a_listing());
    MarketplaceItemData second = a_listing();
    second.status = MarketplaceItemStatus::Listed;
    second.item.quantity = 1;
    data.items.push_back(second);

    std::vector<Case> v;
    v.push_back({"ServerRequestMarketplaceHeaders", [=](auto& r) { return market_headers_request_params(r, headers_req); },
                 [=](const RpcCall& c) { return parse_market_headers_request(c) == headers_req; }});
    v.push_back({"ServerRequestMarketplaceData", [=](auto& r) { return market_data_request_params(r, data_req); },
                 [=](const RpcCall& c) { return parse_market_data_request(c) == data_req; }});
    v.push_back({"ServerRequestMarketplaceHeaderUpdate", [=](auto& r) { return market_header_update_request_params(r, update_req); },
                 [=](const RpcCall& c) { return parse_market_header_update_request(c) == update_req; }});
    v.push_back({"ServerRequestMarketplaceItemPurchase", [=](auto& r) { return market_purchase_request_params(r, buy_req); },
                 [=](const RpcCall& c) { return parse_market_purchase_request(c) == buy_req; }});
    v.push_back({"ServerRequestMarketplaceItemRegistration", [=](auto& r) { return market_registration_request_params(r, list_req); },
                 [=](const RpcCall& c) { return parse_market_registration_request(c) == list_req; }});
    v.push_back({"ServerRequestMarketplaceItemRemoval", [=](auto& r) { return market_removal_request_params(r, remove_req); },
                 [=](const RpcCall& c) { return parse_market_removal_request(c) == remove_req; }});
    v.push_back({"ServerSearchMarketplaceHeaders", [=](auto& r) { return market_search_request_params(r, search_req); },
                 [=](const RpcCall& c) { return parse_market_search_request(c) == search_req; }});
    v.push_back({"ClientOpenMarketplaceUI", [=](auto& r) { return open_marketplace_params(r, market, taxes); }, [=](const RpcCall& c) {
                     const Value* t = rpcf::value_of(*c.rpc, c.params, "NodeTaxDetails");
                     const auto* d = t ? t->get_if<NodeTaxDetails>() : nullptr;
                     return d && *d == taxes;
                 }});
    v.push_back({"ClientReceiveMarketplaceHeaders", [=](auto& r) { return market_headers_params(r, headers); },
                 [=](const RpcCall& c) { return parse_market_headers(c) == headers; }});
    v.push_back({"ClientReceiveMarketplaceData", [=](auto& r) { return market_data_params(r, data); }, [=](const RpcCall& c) { return parse_market_data(c) == data; }});
    v.push_back({"ClientMarketplaceItemPurchaseResponse", [=](auto& r) { return market_purchase_response_params(r, buy_req.item_guid, 3, MarketplaceResponse::InventoryFull); },
                 [=](const RpcCall& c) {
                     const auto p = parse_market_response(c);
                     return p && p->item_guid == buy_req.item_guid && p->quantity == 3 && p->response == MarketplaceResponse::InventoryFull;
                 }});
    v.push_back({"ClientMarketplaceItemRegistrationResponse", [=](auto& r) { return market_registration_response_params(r, a_listing(), MarketplaceResponse::Success); },
                 [=](const RpcCall& c) {
                     const auto p = parse_market_response(c);
                     return p && p->item == a_listing() && p->response == MarketplaceResponse::Success;
                 }});
    v.push_back({"ClientMarketplaceItemRemovalResponse", [=](auto& r) { return market_removal_response_params(r, remove_req.item_guid, MarketplaceResponse::ItemNotFound); },
                 [=](const RpcCall& c) {
                     const auto p = parse_market_response(c);
                     return p && p->item_guid == remove_req.item_guid && p->response == MarketplaceResponse::ItemNotFound;
                 }});
    v.push_back({"ClientMarketplaceHeaderUpdateResponse", [=](auto& r) { return market_header_update_response_params(r, "Fine ores", MarketplaceResponse::InvalidDescriptionLen); },
                 [=](const RpcCall& c) {
                     const auto p = parse_market_response(c);
                     return p && p->description == "Fine ores" && p->response == MarketplaceResponse::InvalidDescriptionLen;
                 }});
    return v;
}

static void test_round_trips() {
    std::printf("every call, built, written, read and parsed back:\n");
    for (const Case& k : cases()) {
        const schema::RpcSchema& rpc = rpc_of(k.rpc);
        const auto params = k.build(rpc);
        CHECK(params.has_value(), std::string(k.rpc) + ": builds");
        if (!params) continue;
        const auto bits = write_call(rpc, *params);
        CHECK(bits.has_value(), std::string(k.rpc) + ": writes");
        if (!bits) continue;
        const auto call = read_call(rpc, *bits);
        CHECK(call.has_value(), std::string(k.rpc) + ": reads back to its last bit (" + num(bits->size()) + " bits)");
        if (!call) continue;
        CHECK(k.same(*call), std::string(k.rpc) + ": parses back to what was built");
        // another RPC's call is not taken for this one
        const schema::RpcSchema& other = rpc_of(std::string(k.rpc) == "ServerRequestMarketplaceHeaders" ? "ServerRequestMarketplaceData" : "ServerRequestMarketplaceHeaders");
        RpcCall wrong = *call;
        wrong.rpc = &other;
        CHECK(!k.same(wrong), std::string(k.rpc) + ": a call of another RPC does not parse as this one");
    }
}

static void test_hostile() {
    std::printf("hostile input:\n");
    size_t cuts = 0, read = 0, bad = 0;
    for (const Case& k : cases()) {
        const schema::RpcSchema& rpc = rpc_of(k.rpc);
        const auto params = k.build(rpc);
        const auto bits = params ? write_call(rpc, *params) : std::nullopt;
        if (!bits) continue;
        for (size_t n = 0; n < bits->size(); ++n) {
            ++cuts;
            const auto call = read_call(rpc, bits->slice(0, n));
            if (!call) continue;
            ++read;
            if (!write_call(rpc, call->params)) ++bad;
        }
    }
    CHECK(cuts > 1000 && bad == 0, "every truncation of every call (" + num(cuts) + ", " + num(read) + " of which read) is refused or writes again");
}

int main() {
    test_currency_container();
    test_round_trips();
    test_hostile();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
