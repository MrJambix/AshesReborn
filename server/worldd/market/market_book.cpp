// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "market/market_book.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace aoc::worldd::market {

std::string guid_hex(const GuidValue& g) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const uint32_t word : {g.a, g.b, g.c, g.d}) {
        for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(word >> shift) & 15];
    }
    return out;
}

std::optional<GuidValue> guid_from_hex(const std::string& s) {
    if (s.size() != 32) return std::nullopt;
    uint32_t w[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 32; ++i) {
        const char c = s[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return std::nullopt;
        w[i / 8] = (w[i / 8] << 4) | static_cast<uint32_t>(v);
    }
    return GuidValue{w[0], w[1], w[2], w[3]};
}

std::string listing_id(const GuidValue& g) { return "l" + guid_hex(g); }
std::string storefront_id(int64_t node, const std::string& seller_id) { return "s" + std::to_string(node) + ":" + seller_id; }

namespace {

nlohmann::json item_json(const svc::InventoryItem& it) {
    nlohmann::json j{{"record", it.item_record_id}, {"guid", it.instance_guid}, {"quantity", it.quantity}, {"quality", it.quality},
                     {"durability", it.durability}, {"created_ms", it.created_at_ms}, {"lock", it.lock_expiration}, {"container", static_cast<int>(it.container)}};
    if (it.persisted_health) j["health"] = *it.persisted_health;
    if (it.persisted_mana) j["mana"] = *it.persisted_mana;
    return j;
}

svc::InventoryItem item_from(const nlohmann::json& j) {
    svc::InventoryItem it;
    it.item_record_id = j.at("record").get<int64_t>();
    it.instance_guid = j.at("guid").get<std::string>();
    it.quantity = j.at("quantity").get<int32_t>();
    it.quality = j.at("quality").get<int32_t>();
    it.durability = j.at("durability").get<float>();
    it.created_at_ms = j.at("created_ms").get<int64_t>();
    it.lock_expiration = j.value("lock", int64_t{0});
    it.container = static_cast<svc::ItemContainer>(j.value("container", 0));
    if (j.contains("health")) it.persisted_health = j.at("health").get<float>();
    if (j.contains("mana")) it.persisted_mana = j.at("mana").get<float>();
    return it;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

svc::MarketRow row_of(const Listing& l) {
    nlohmann::json j{{"node", l.node},           {"seller_id", l.seller_id}, {"seller", guid_hex(l.seller)}, {"seller_name", l.seller_name}, {"item", item_json(l.item)},
                     {"item_name", l.item_name}, {"price", l.price},         {"issued", l.issued},           {"expires", l.expires},         {"proceeds", l.proceeds}};
    return svc::MarketRow{listing_id(l.guid), l.revision, false, j.dump()};
}

svc::MarketRow row_of(const Storefront& s) {
    nlohmann::json j{{"node", s.node}, {"seller_id", s.seller_id}, {"seller", guid_hex(s.seller)}, {"seller_name", s.seller_name}, {"description", s.description}, {"issued", s.issued}};
    return svc::MarketRow{storefront_id(s.node, s.seller_id), s.revision, false, j.dump()};
}

svc::MarketRow tombstone(const std::string& id, int64_t revision) { return svc::MarketRow{id, revision, true, "{}"}; }

std::optional<Listing> listing_of(const svc::MarketRow& row) {
    if (row.gone || row.id.size() != 33 || row.id[0] != 'l') return std::nullopt;
    const auto guid = guid_from_hex(row.id.substr(1));
    const nlohmann::json j = nlohmann::json::parse(row.data, nullptr, false);
    if (!guid || j.is_discarded() || !j.is_object()) return std::nullopt;
    try {
        Listing l;
        l.guid = *guid;
        l.node = j.at("node").get<int64_t>();
        l.seller_id = j.at("seller_id").get<std::string>();
        const auto seller = guid_from_hex(j.at("seller").get<std::string>());
        if (!seller) return std::nullopt;
        l.seller = *seller;
        l.seller_name = j.at("seller_name").get<std::string>();
        l.item = item_from(j.at("item"));
        l.item_name = j.value("item_name", std::string());
        l.price = j.at("price").get<int64_t>();
        l.issued = j.at("issued").get<double>();
        l.expires = j.at("expires").get<double>();
        l.proceeds = j.value("proceeds", int64_t{0});
        l.revision = row.revision;
        if (l.price <= 0 || l.item.quantity < 0 || l.proceeds < 0) return std::nullopt;
        return l;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::optional<Storefront> storefront_of(const svc::MarketRow& row) {
    if (row.gone || row.id.empty() || row.id[0] != 's') return std::nullopt;
    const nlohmann::json j = nlohmann::json::parse(row.data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    try {
        Storefront s;
        s.node = j.at("node").get<int64_t>();
        s.seller_id = j.at("seller_id").get<std::string>();
        const auto seller = guid_from_hex(j.at("seller").get<std::string>());
        if (!seller) return std::nullopt;
        s.seller = *seller;
        s.seller_name = j.at("seller_name").get<std::string>();
        s.description = j.value("description", std::string());
        s.issued = j.value("issued", 0.0);
        s.revision = row.revision;
        if (row.id != storefront_id(s.node, s.seller_id)) return std::nullopt;
        return s;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

size_t Book::load(const std::vector<svc::MarketRow>& rows) {
    size_t skipped = 0;
    std::vector<Listing> loaded;
    for (const svc::MarketRow& row : rows) {
        revision_ = std::max(revision_, row.revision);
        if (row.gone) continue;
        if (auto l = listing_of(row)) {
            loaded.push_back(std::move(*l));
        } else if (auto s = storefront_of(row)) {
            storefronts_[row.id] = std::move(*s);
        } else {
            ++skipped;
        }
    }
    // the storefronts keep the order their sellers first listed in
    std::stable_sort(loaded.begin(), loaded.end(), [](const Listing& a, const Listing& b) { return a.issued < b.issued; });
    for (Listing& l : loaded) {
        const std::string id = listing_id(l.guid);
        touch(l.node);
        order_.push_back(id);
        listings_[id] = std::move(l);
    }
    return skipped;
}

const Listing* Book::find(const GuidValue& g) const {
    const auto it = listings_.find(listing_id(g));
    return it == listings_.end() ? nullptr : &it->second;
}

std::vector<Header> Book::headers(int64_t node, double now) const {
    std::vector<Header> out;
    std::set<std::string> seen;
    for (const std::string& id : order_) {
        const auto it = listings_.find(id);
        if (it == listings_.end()) continue;
        const Listing& l = it->second;
        if (l.node != node || !l.active(now) || !seen.insert(l.seller_id).second) continue;
        Header h{l.seller, l.seller_name, std::string(), l.issued};
        if (const Storefront* s = storefront(node, l.seller_id)) h.description = s->description;
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Header> Book::search(int64_t node, const std::string& text, std::optional<int> rarity, const std::function<int(int64_t, int32_t)>& rarity_of, double now) const {
    const std::string needle = lower(text);
    std::set<std::string> matching;
    for (const auto& [id, l] : listings_) {
        if (l.node != node || !l.active(now)) continue;
        if (!needle.empty() && lower(l.item_name).find(needle) == std::string::npos) continue;
        if (rarity && rarity_of && rarity_of(l.item.item_record_id, l.item.quality) != *rarity) continue;
        matching.insert(l.seller_id);
    }
    std::vector<Header> out;
    for (Header& h : headers(node, now)) {
        const std::string seller = seller_of(node, h.seller);
        if (matching.count(seller)) out.push_back(std::move(h));
    }
    return out;
}

std::vector<const Listing*> Book::for_sale(int64_t node, const std::string& seller_id, double now) const {
    std::vector<const Listing*> out;
    for (const std::string& id : order_) {
        const auto it = listings_.find(id);
        if (it != listings_.end() && it->second.node == node && it->second.seller_id == seller_id && it->second.active(now)) out.push_back(&it->second);
    }
    return out;
}

std::vector<const Listing*> Book::own(int64_t node, const std::string& seller_id) const {
    std::vector<const Listing*> out;
    for (const std::string& id : order_) {
        const auto it = listings_.find(id);
        if (it != listings_.end() && it->second.node == node && it->second.seller_id == seller_id) out.push_back(&it->second);
    }
    return out;
}

uint64_t Book::version(int64_t node) const {
    const auto it = versions_.find(node);
    return 1 + (it == versions_.end() ? 0 : it->second);
}

std::string Book::seller_of(int64_t node, const GuidValue& player) const {
    for (const auto& [id, l] : listings_) {
        if (l.node == node && l.seller == player) return l.seller_id;
    }
    for (const auto& [id, s] : storefronts_) {
        if (s.node == node && s.seller == player) return s.seller_id;
    }
    return std::string();
}

const Storefront* Book::storefront(int64_t node, const std::string& seller_id) const {
    const auto it = storefronts_.find(storefront_id(node, seller_id));
    return it == storefronts_.end() ? nullptr : &it->second;
}

std::optional<std::vector<svc::MarketRow>> Book::list(Listing l, std::string& why) {
    size_t held = 0;
    for (const auto& [id, other] : listings_) held += other.node == l.node && other.seller_id == l.seller_id && other.item.quantity > 0 ? 1 : 0;
    if (held >= kMaxListingsPerSeller) {
        why = "the seller already has " + std::to_string(held) + " listings at this market (at most " + std::to_string(kMaxListingsPerSeller) + ")";
        return std::nullopt;
    }
    const std::string id = listing_id(l.guid);
    if (listings_.count(id)) {
        why = "a listing with that guid exists";
        return std::nullopt;
    }
    l.revision = next_revision();
    touch(l.node);
    std::vector<svc::MarketRow> rows{row_of(l)};
    order_.push_back(id);
    listings_[id] = std::move(l);
    return rows;
}

std::optional<int64_t> Book::quote(const GuidValue& g, int32_t quantity, const std::string& buyer_id, double now, std::string& why) const {
    const Listing* l = find(g);
    if (!l) {
        why = "no such listing";
        return std::nullopt;
    }
    if (!l->active(now)) {
        why = l->item.quantity <= 0 ? "the listing is sold out" : "the listing has expired";
        return std::nullopt;
    }
    if (l->seller_id == buyer_id) {
        why = "a seller does not buy from their own storefront";
        return std::nullopt;
    }
    if (quantity <= 0 || quantity > l->item.quantity) {
        why = "a purchase of " + std::to_string(quantity) + " of the " + std::to_string(l->item.quantity) + " listed";
        return std::nullopt;
    }
    return l->price * static_cast<int64_t>(quantity);
}

std::optional<Book::Bought> Book::buy(const GuidValue& g, int32_t quantity, const std::string& buyer_id, double now, std::string& why) {
    const auto cost = quote(g, quantity, buyer_id, now, why);
    if (!cost) return std::nullopt;
    Listing& l = listings_.at(listing_id(g));
    Bought b;
    b.item = l.item;
    b.item.quantity = quantity;
    b.cost = *cost;
    l.item.quantity -= quantity;
    l.proceeds += *cost;
    l.revision = next_revision();
    touch(l.node);
    b.rows.push_back(row_of(l));
    return b;
}

std::optional<Book::Removed> Book::remove(const GuidValue& g, const std::string& seller_id, std::string& why) {
    const std::string id = listing_id(g);
    const auto it = listings_.find(id);
    if (it == listings_.end()) {
        why = "no such listing";
        return std::nullopt;
    }
    Listing& l = it->second;
    if (l.seller_id != seller_id) {
        why = "the listing is another seller's";
        return std::nullopt;
    }
    Removed r;
    r.item = l.item;
    l.item.quantity = 0;
    l.revision = next_revision();
    touch(l.node);
    if (l.proceeds > 0) {
        r.rows.push_back(row_of(l));   // kept until its proceeds are collected
    } else {
        r.rows.push_back(tombstone(id, l.revision));
        listings_.erase(it);
        order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    }
    return r;
}

Book::Collected Book::collect(const std::string& seller_id, int64_t room) {
    Collected c;
    for (auto it = listings_.begin(); it != listings_.end();) {
        Listing& l = it->second;
        if (l.seller_id != seller_id || l.proceeds <= 0 || room <= 0) {
            ++it;
            continue;
        }
        const int64_t paid = std::min(l.proceeds, room);
        room -= paid;
        c.copper += paid;
        ++c.listings;
        l.proceeds -= paid;
        l.revision = next_revision();
        touch(l.node);
        if (l.proceeds == 0 && l.item.quantity == 0) {
            c.rows.push_back(tombstone(it->first, l.revision));
            order_.erase(std::remove(order_.begin(), order_.end(), it->first), order_.end());
            it = listings_.erase(it);
        } else {
            c.rows.push_back(row_of(l));
            ++it;
        }
    }
    return c;
}

std::vector<svc::MarketRow> Book::describe(int64_t node, const std::string& seller_id, const GuidValue& seller, const std::string& seller_name, const std::string& text,
                                           double now) {
    const std::string id = storefront_id(node, seller_id);
    Storefront& s = storefronts_[id];
    if (s.seller_id.empty()) {
        s.node = node;
        s.seller_id = seller_id;
        s.issued = now;
    }
    s.seller = seller;
    s.seller_name = seller_name;
    s.description = text;
    s.revision = next_revision();
    touch(node);
    return {row_of(s)};
}

}  // namespace aoc::worldd::market
