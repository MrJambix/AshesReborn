// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "protocol/repl/marketplace_rpc.h"

#include "protocol/repl/rpc_fields.h"

#include <functional>
#include <string_view>

namespace aoc { namespace protocol { namespace repl {

namespace {

bool is_rpc(const schema::RpcSchema& rpc, std::string_view name) { return rpc.name && name == rpc.name; }
bool decoded_as(const RpcCall& call, std::string_view name) { return call.rpc && call.decoded && is_rpc(*call.rpc, name); }

/// Reads an FGuid flattened as PREFIX.A .. PREFIX.D through `get` (a member left at its default reads as 0).
std::optional<GuidValue> read_guid(const std::function<const Value*(std::string_view)>& get, std::string_view prefix) {
    GuidValue g;
    uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
    static constexpr const char* kSuffix[4] = {".A", ".B", ".C", ".D"};
    for (int i = 0; i < 4; ++i) {
        const auto w = rpcf::int_or(get(std::string(prefix) + kSuffix[i]), 0);
        if (!w || *w < INT32_MIN || *w > INT32_MAX) return std::nullopt;
        *words[i] = static_cast<uint32_t>(static_cast<int32_t>(*w));
    }
    return g;
}

using Put = std::function<bool(std::string_view, Value)>;

bool put_guid(const Put& put, std::string_view prefix, const GuidValue& g) {
    const uint32_t words[4] = {g.a, g.b, g.c, g.d};
    static constexpr const char* kSuffix[4] = {".A", ".B", ".C", ".D"};
    for (int i = 0; i < 4; ++i) {
        if (!put(std::string(prefix) + kSuffix[i], Value::i64(static_cast<int32_t>(words[i])))) return false;
    }
    return true;
}

bool put_item_data(const Put& put, std::string_view prefix, const MarketplaceItemData& d) {
    const std::string p(prefix);
    return put(p + ".NodeGuid", Value::i64(d.node)) && put_guid(put, p + ".PlayerGuid", d.player) && put(p + ".PlayerName", Value::str(d.player_name)) &&
           put_guid(put, p + ".MarketplaceItemGuid", d.item_guid) && put(p + ".Item", Value(d.item)) && put(p + ".ItemName", Value::str(d.item_name)) &&
           put(p + ".PricePerUnit", Value(d.price_per_unit)) && put(p + ".IssueDate", Value::f64(d.issue_date)) &&
           put(p + ".ExpirationDate", Value::f64(d.expiration_date)) && put(p + ".Status", Value::u64(static_cast<uint8_t>(d.status)));
}

std::optional<MarketplaceItemData> read_item_data(const std::function<const Value*(std::string_view)>& get, std::string_view prefix) {
    const std::string p(prefix);
    MarketplaceItemData d;
    const auto node = rpcf::int_or(get(p + ".NodeGuid"), 0);
    const auto player = read_guid(get, p + ".PlayerGuid");
    const auto item_guid = read_guid(get, p + ".MarketplaceItemGuid");
    const auto status = rpcf::uint_or(get(p + ".Status"), 0);
    if (!node || !player || !item_guid || !status || *status > 5) return std::nullopt;
    d.node = *node;
    d.player = *player;
    d.item_guid = *item_guid;
    d.status = static_cast<MarketplaceItemStatus>(*status);
    if (const Value* v = get(p + ".PlayerName")) {
        const auto* s = v->get_if<std::string>();
        if (!s) return std::nullopt;
        d.player_name = *s;
    }
    if (const Value* v = get(p + ".ItemName")) {
        const auto* s = v->get_if<std::string>();
        if (!s) return std::nullopt;
        d.item_name = *s;
    }
    if (const Value* v = get(p + ".Item")) {
        const auto* i = v->get_if<ItemInstance>();
        if (!i) return std::nullopt;
        d.item = *i;
    }
    if (const Value* v = get(p + ".PricePerUnit")) {
        const auto* c = v->get_if<CurrencyContainer>();
        if (!c) return std::nullopt;
        d.price_per_unit = *c;
    }
    for (auto [path, out] : {std::pair<const char*, double*>{".IssueDate", &d.issue_date}, {".ExpirationDate", &d.expiration_date}}) {
        if (const Value* v = get(p + path)) {
            const auto* x = v->get_if<double>();
            if (!x) return std::nullopt;
            *out = *x;
        }
    }
    return d;
}

/// The getter and setter of a call's top-level parameters.
std::function<const Value*(std::string_view)> getter(const RpcCall& call) {
    return [&call](std::string_view path) { return rpcf::value_of(*call.rpc, call.params, path); };
}
Put setter(const schema::RpcSchema& rpc, RpcParams& params) {
    return [&rpc, &params](std::string_view path, Value v) { return set_rpc_param(rpc, params, path, std::move(v)); };
}

/// The getter and setter of an array element of command `array`, by the element commands' paths.
std::function<const Value*(std::string_view)> element_getter(std::span<const schema::RepCommand> elems, const ElementState& e) {
    return [elems, &e](std::string_view path) { return rpcf::element_value(e, rpcf::local(elems, path)); };
}
Put element_setter(std::span<const schema::RepCommand> elems, ElementState& e) {
    return [elems, &e](std::string_view path, Value v) {
        const uint16_t h = rpcf::local(elems, path);
        if (!h) return false;
        e.values[h] = std::move(v);
        return true;
    };
}

std::optional<std::string> string_of(const Value* v) {
    if (!v) return std::string();
    const auto* s = v->get_if<std::string>();
    if (!s) return std::nullopt;
    return *s;
}

}  // namespace

// ---- the client's requests --------------------------------------------------------------------------------------------------------------------------------

std::optional<MarketHeadersRequest> parse_market_headers_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestMarketplaceHeaders")) return std::nullopt;
    const auto get = getter(call);
    const auto market = read_guid(get, "MarketplaceActorGuid");
    const auto version = rpcf::uint_or(get("HeaderVersion"), 0);
    if (!market || !version) return std::nullopt;
    return MarketHeadersRequest{*market, *version};
}

std::optional<MarketDataRequest> parse_market_data_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestMarketplaceData")) return std::nullopt;
    const auto get = getter(call);
    const auto market = read_guid(get, "MarketplaceActorGuid");
    const auto player = read_guid(get, "PlayerGuid");
    if (!market || !player) return std::nullopt;
    return MarketDataRequest{*market, *player};
}

std::optional<MarketHeaderUpdateRequest> parse_market_header_update_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestMarketplaceHeaderUpdate")) return std::nullopt;
    const auto get = getter(call);
    const auto market = read_guid(get, "MarketplaceActorGuid");
    const auto text = string_of(get("HeaderDescription"));
    if (!market || !text) return std::nullopt;
    return MarketHeaderUpdateRequest{*market, *text};
}

std::optional<MarketPurchaseRequest> parse_market_purchase_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestMarketplaceItemPurchase")) return std::nullopt;
    const auto get = getter(call);
    const auto market = read_guid(get, "MarketplaceActorGuid");
    const auto item = read_guid(get, "MarketplaceItemGuid");
    const auto quantity = rpcf::int_or(get("Quantity"), 0);
    if (!market || !item || !quantity || *quantity < INT32_MIN || *quantity > INT32_MAX) return std::nullopt;
    return MarketPurchaseRequest{*market, *item, static_cast<int32_t>(*quantity)};
}

std::optional<MarketRegistrationRequest> parse_market_registration_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestMarketplaceItemRegistration")) return std::nullopt;
    const auto get = getter(call);
    const auto market = read_guid(get, "MarketplaceActorGuid");
    const auto quantity = rpcf::int_or(get("Quantity"), 0);
    if (!market || !quantity || *quantity < INT32_MIN || *quantity > INT32_MAX) return std::nullopt;
    MarketRegistrationRequest r;
    r.market = *market;
    r.quantity = static_cast<int32_t>(*quantity);
    if (const Value* v = get("Item")) {
        const auto* i = v->get_if<ItemInstance>();
        if (!i) return std::nullopt;
        r.item = *i;
    }
    if (const Value* v = get("PricePerUnit")) {
        const auto* c = v->get_if<CurrencyContainer>();
        if (!c) return std::nullopt;
        r.price_per_unit = *c;
    }
    return r;
}

std::optional<MarketRemovalRequest> parse_market_removal_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerRequestMarketplaceItemRemoval")) return std::nullopt;
    const auto get = getter(call);
    const auto market = read_guid(get, "MarketplaceActorGuid");
    const auto item = read_guid(get, "MarketplaceItemGuid");
    if (!market || !item) return std::nullopt;
    return MarketRemovalRequest{*market, *item};
}

std::optional<MarketSearchRequest> parse_market_search_request(const RpcCall& call) {
    if (!decoded_as(call, "ServerSearchMarketplaceHeaders")) return std::nullopt;
    const auto get = getter(call);
    const auto market = read_guid(get, "MarketplaceActorGuid");
    const auto text = string_of(get("SearchItemString"));
    const auto rarity = rpcf::uint_or(get("ItemRarity"), 0);
    if (!market || !text || !rarity || *rarity > 15) return std::nullopt;
    return MarketSearchRequest{*market, *text, static_cast<uint8_t>(*rarity)};
}

std::optional<RpcParams> market_headers_request_params(const schema::RpcSchema& rpc, const MarketHeadersRequest& r) {
    if (!is_rpc(rpc, "ServerRequestMarketplaceHeaders")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceActorGuid", r.market) || !put("HeaderVersion", Value::u64(r.version))) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_data_request_params(const schema::RpcSchema& rpc, const MarketDataRequest& r) {
    if (!is_rpc(rpc, "ServerRequestMarketplaceData")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceActorGuid", r.market) || !put_guid(put, "PlayerGuid", r.player)) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_header_update_request_params(const schema::RpcSchema& rpc, const MarketHeaderUpdateRequest& r) {
    if (!is_rpc(rpc, "ServerRequestMarketplaceHeaderUpdate")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceActorGuid", r.market) || !put("HeaderDescription", Value::str(r.description))) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_purchase_request_params(const schema::RpcSchema& rpc, const MarketPurchaseRequest& r) {
    if (!is_rpc(rpc, "ServerRequestMarketplaceItemPurchase")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceActorGuid", r.market) || !put_guid(put, "MarketplaceItemGuid", r.item_guid) || !put("Quantity", Value::i64(r.quantity))) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_registration_request_params(const schema::RpcSchema& rpc, const MarketRegistrationRequest& r) {
    if (!is_rpc(rpc, "ServerRequestMarketplaceItemRegistration")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceActorGuid", r.market) || !put("Item", Value(r.item)) || !put("Quantity", Value::i64(r.quantity)) ||
        !put("PricePerUnit", Value(r.price_per_unit))) {
        return std::nullopt;
    }
    return p;
}

std::optional<RpcParams> market_removal_request_params(const schema::RpcSchema& rpc, const MarketRemovalRequest& r) {
    if (!is_rpc(rpc, "ServerRequestMarketplaceItemRemoval")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceActorGuid", r.market) || !put_guid(put, "MarketplaceItemGuid", r.item_guid)) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_search_request_params(const schema::RpcSchema& rpc, const MarketSearchRequest& r) {
    if (!is_rpc(rpc, "ServerSearchMarketplaceHeaders")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceActorGuid", r.market) || !put("SearchItemString", Value::str(r.search)) || !put("ItemRarity", Value::u64(r.rarity))) return std::nullopt;
    return p;
}

// ---- the server's answers ---------------------------------------------------------------------------------------------------------------------------------

std::optional<RpcParams> open_marketplace_params(const schema::RpcSchema& rpc, const GuidValue& market, const NodeTaxDetails& taxes) {
    if (!is_rpc(rpc, "ClientOpenMarketplaceUI")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceGuid", market) || !put("NodeTaxDetails", Value(taxes))) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_headers_params(const schema::RpcSchema& rpc, const MarketHeaders& h) {
    if (!is_rpc(rpc, "ClientReceiveMarketplaceHeaders")) return std::nullopt;
    const schema::RepCommand* array = rpc_command(rpc, "Headers");
    if (!array || h.headers.size() > 0xFFFF) return std::nullopt;
    const auto elems = rpcf::elements_of(rpc, *array);
    ArrayValue a;
    for (const MarketplaceHeader& m : h.headers) {
        ElementState e;
        const Put put = element_setter(elems, e);
        if (!put("Headers.NodeGuid", Value::i64(m.node)) || !put_guid(put, "Headers.PlayerGuid", m.player) || !put("Headers.PlayerName", Value::str(m.player_name)) ||
            !put("Headers.Description", Value::str(m.description)) || !put("Headers.IssueDate", Value::f64(m.issue_date)) ||
            !put("Headers.bIsCityNodeHeader", Value::boolean(m.city_node_header))) {
            return std::nullopt;
        }
        a.elements.push_back(std::move(e));
    }
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put("NodeId", Value::i64(h.node)) || !put("Headers", Value(std::move(a))) || !put("HeaderVersion", Value::u64(h.version)) ||
        !put("SearchItemString", Value::str(h.search)) || !put("ItemRarity", Value::u64(h.rarity)) || !put("bComplete", Value::boolean(h.complete))) {
        return std::nullopt;
    }
    return p;
}

std::optional<MarketHeaders> parse_market_headers(const RpcCall& call) {
    if (!decoded_as(call, "ClientReceiveMarketplaceHeaders")) return std::nullopt;
    const auto get = getter(call);
    MarketHeaders h;
    const auto node = rpcf::int_or(get("NodeId"), 0);
    const auto version = rpcf::uint_or(get("HeaderVersion"), 0);
    const auto search = string_of(get("SearchItemString"));
    const auto rarity = rpcf::uint_or(get("ItemRarity"), 0);
    const auto complete = rpcf::bool_or(get("bComplete"), false);
    if (!node || !version || !search || !rarity || !complete) return std::nullopt;
    h.node = *node;
    h.version = *version;
    h.search = *search;
    h.rarity = static_cast<uint8_t>(*rarity);
    h.complete = *complete;
    if (const Value* v = get("Headers")) {
        const auto* a = v->get_if<ArrayValue>();
        const schema::RepCommand* array = rpc_command(*call.rpc, "Headers");
        if (!a || !array) return std::nullopt;
        const auto elems = rpcf::elements_of(*call.rpc, *array);
        for (const ElementState& e : a->elements) {
            const auto eg = element_getter(elems, e);
            MarketplaceHeader m;
            const auto n = rpcf::int_or(eg("Headers.NodeGuid"), 0);
            const auto player = read_guid(eg, "Headers.PlayerGuid");
            const auto name = string_of(eg("Headers.PlayerName"));
            const auto text = string_of(eg("Headers.Description"));
            const auto city = rpcf::bool_or(eg("Headers.bIsCityNodeHeader"), false);
            if (!n || !player || !name || !text || !city) return std::nullopt;
            m.node = *n;
            m.player = *player;
            m.player_name = *name;
            m.description = *text;
            m.city_node_header = *city;
            if (const Value* d = eg("Headers.IssueDate")) {
                const auto* x = d->get_if<double>();
                if (!x) return std::nullopt;
                m.issue_date = *x;
            }
            h.headers.push_back(std::move(m));
        }
    }
    return h;
}

std::optional<RpcParams> market_data_params(const schema::RpcSchema& rpc, const MarketData& d) {
    if (!is_rpc(rpc, "ClientReceiveMarketplaceData")) return std::nullopt;
    const schema::RepCommand* array = rpc_command(rpc, "MarketplaceItemData");
    if (!array || d.items.size() > 0xFFFF) return std::nullopt;
    const auto elems = rpcf::elements_of(rpc, *array);
    ArrayValue a;
    for (const MarketplaceItemData& item : d.items) {
        ElementState e;
        if (!put_item_data(element_setter(elems, e), "MarketplaceItemData", item)) return std::nullopt;
        a.elements.push_back(std::move(e));
    }
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "PlayerGuid", d.player) || !put("MarketplaceItemData", Value(std::move(a)))) return std::nullopt;
    return p;
}

std::optional<MarketData> parse_market_data(const RpcCall& call) {
    if (!decoded_as(call, "ClientReceiveMarketplaceData")) return std::nullopt;
    const auto get = getter(call);
    MarketData d;
    const auto player = read_guid(get, "PlayerGuid");
    if (!player) return std::nullopt;
    d.player = *player;
    if (const Value* v = get("MarketplaceItemData")) {
        const auto* a = v->get_if<ArrayValue>();
        const schema::RepCommand* array = rpc_command(*call.rpc, "MarketplaceItemData");
        if (!a || !array) return std::nullopt;
        const auto elems = rpcf::elements_of(*call.rpc, *array);
        for (const ElementState& e : a->elements) {
            auto item = read_item_data(element_getter(elems, e), "MarketplaceItemData");
            if (!item) return std::nullopt;
            d.items.push_back(std::move(*item));
        }
    }
    return d;
}

std::optional<RpcParams> market_purchase_response_params(const schema::RpcSchema& rpc, const GuidValue& item_guid, int32_t quantity, MarketplaceResponse response) {
    if (!is_rpc(rpc, "ClientMarketplaceItemPurchaseResponse")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceItemGuid", item_guid) || !put("Quantity", Value::i64(quantity)) || !put("Response", Value::u64(static_cast<uint8_t>(response)))) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_registration_response_params(const schema::RpcSchema& rpc, const MarketplaceItemData& item, MarketplaceResponse response) {
    if (!is_rpc(rpc, "ClientMarketplaceItemRegistrationResponse")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_item_data(put, "Item", item) || !put("Response", Value::u64(static_cast<uint8_t>(response)))) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_removal_response_params(const schema::RpcSchema& rpc, const GuidValue& item_guid, MarketplaceResponse response) {
    if (!is_rpc(rpc, "ClientMarketplaceItemRemovalResponse")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put_guid(put, "MarketplaceItemGuid", item_guid) || !put("Response", Value::u64(static_cast<uint8_t>(response)))) return std::nullopt;
    return p;
}

std::optional<RpcParams> market_header_update_response_params(const schema::RpcSchema& rpc, const std::string& description, MarketplaceResponse response) {
    if (!is_rpc(rpc, "ClientMarketplaceHeaderUpdateResponse")) return std::nullopt;
    RpcParams p;
    const Put put = setter(rpc, p);
    if (!put("HeaderDescription", Value::str(description)) || !put("Response", Value::u64(static_cast<uint8_t>(response)))) return std::nullopt;
    return p;
}

std::optional<MarketResponse> parse_market_response(const RpcCall& call) {
    if (!call.rpc || !call.decoded) return std::nullopt;
    const std::string_view name = call.rpc->name ? call.rpc->name : "";
    const auto get = getter(call);
    MarketResponse r;
    const auto response = rpcf::uint_or(get("Response"), 0);
    if (!response || *response > 19) return std::nullopt;
    r.response = static_cast<MarketplaceResponse>(*response);
    if (name == "ClientMarketplaceItemPurchaseResponse" || name == "ClientMarketplaceItemRemovalResponse") {
        const auto g = read_guid(get, "MarketplaceItemGuid");
        if (!g) return std::nullopt;
        r.item_guid = *g;
        if (name == "ClientMarketplaceItemPurchaseResponse") {
            const auto q = rpcf::int_or(get("Quantity"), 0);
            if (!q) return std::nullopt;
            r.quantity = static_cast<int32_t>(*q);
        }
        return r;
    }
    if (name == "ClientMarketplaceItemRegistrationResponse") {
        auto item = read_item_data(get, "Item");
        if (!item) return std::nullopt;
        r.item = std::move(*item);
        r.item_guid = r.item.item_guid;
        return r;
    }
    if (name == "ClientMarketplaceHeaderUpdateResponse") {
        const auto text = string_of(get("HeaderDescription"));
        if (!text) return std::nullopt;
        r.description = *text;
        return r;
    }
    return std::nullopt;
}

}}}  // namespace aoc::protocol::repl
