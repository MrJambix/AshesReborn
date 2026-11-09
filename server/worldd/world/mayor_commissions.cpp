// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/mayor_commissions.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace aoc::worldd::world::mayor {

namespace qc = quest::commissions;

namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// The region whose commissions name `key` ("" for a node no commission names).
const char* region_of(const std::string& key) {
    for (const qc::MayoralRegionNode& r : qc::kMayoralRegionNodes) {
        if (key == r.node) return r.region;
    }
    return "";
}

bool in(const std::vector<int64_t>& v, int64_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

bool running(const MayorCommissions& s, int64_t record) {
    return std::any_of(s.active.begin(), s.active.end(), [&](const MayorActiveCommission& a) { return a.record == record; });
}

bool cooling(const MayorCommissions& s, int64_t record) {
    return std::any_of(s.cooling.begin(), s.cooling.end(), [&](const MayorCoolingCommission& c) { return c.record == record; });
}

void finish(MayorCommissions& s, size_t i, int64_t now_ms) {
    const MayorActiveCommission a = s.active[i];
    s.active.erase(s.active.begin() + static_cast<std::ptrdiff_t>(i));
    const MayoralCommission* c = commission(a.record);
    s.cooling.push_back(MayorCoolingCommission{a.record, now_ms + (c ? c->cooldown_ms : 0), true, false});
    if (c && c->node_mod != 0) s.rewards.push_back(NodeRewardMod{c->node_mod, a.record, now_ms + c->node_mod_ms});
}

}  // namespace

const MayoralCommission* commission(int64_t record) {
    for (const MayoralCommission& c : qc::kMayoralCommissions) {
        if (c.quest == record) return &c;
    }
    return nullptr;
}

std::string node_key(const std::string& display_name) {
    std::string out;
    for (const char ch : display_name) {
        if (std::isalnum(static_cast<unsigned char>(ch))) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::vector<const MayoralCommission*> pool(const std::string& key) {
    std::vector<const MayoralCommission*> out;
    const char* region = region_of(key);
    if (key.empty() || !*region) return out;
    for (const MayoralCommission& c : qc::kMayoralCommissions) {
        if (key == c.node || (!*c.node && std::strcmp(c.region, region) == 0)) out.push_back(&c);
    }
    return out;
}

bool refill_hand(MayorCommissions& s, const std::string& key, int hand_slots, int64_t node) {
    const std::vector<const MayoralCommission*> mine = pool(key);
    bool changed = false;
    const size_t before = s.hand.size();
    // (a card the pool does not hold stays: a game master put it there)
    s.hand.erase(std::remove_if(s.hand.begin(), s.hand.end(), [&](int64_t r) { return !commission(r) || running(s, r) || cooling(s, r); }), s.hand.end());
    changed = s.hand.size() != before;
    const size_t want = static_cast<size_t>(std::max(0, hand_slots));
    if (s.hand.size() > want) {
        s.hand.resize(want);
        changed = true;
    }
    std::vector<int64_t> free;
    for (const MayoralCommission* c : mine) {
        if (!in(s.hand, c->quest) && !running(s, c->quest) && !cooling(s, c->quest)) free.push_back(c->quest);
    }
    while (s.hand.size() < want && !free.empty()) {
        uint64_t seed = static_cast<uint64_t>(node) ^ (s.serial++ * 0xD1B54A32D192ED03ull);
        const size_t k = static_cast<size_t>(splitmix(seed) % free.size());
        s.hand.push_back(free[k]);
        free.erase(free.begin() + static_cast<std::ptrdiff_t>(k));
        changed = true;
    }
    return changed;
}

const char* buy_name(Buy b) {
    switch (b) {
        case Buy::Ok: return "bought";
        case Buy::Unknown: return "it is no mayor's commission";
        case Buy::NotInHand: return "it is not in the mayor's hand";
        case Buy::NoSlot: return "every commission slot of the node is running";
        case Buy::Cooling: return "it cools down";
        case Buy::ResourceCost: return "it costs a resource of the node's storage, which is not built";
        case Buy::Treasury: return "the node's treasury holds too little";
    }
    return "?";
}

MayorActiveCommission& start(MayorCommissions& s, int64_t node, const MayoralCommission& c, int64_t now_ms) {
    uint64_t seed = static_cast<uint64_t>(node) ^ static_cast<uint64_t>(c.quest) ^ (++s.serial * 0x9E3779B97F4A7C15ull);
    MayorActiveCommission a;
    a.guid = MayorGuid{static_cast<uint32_t>(splitmix(seed)), static_cast<uint32_t>(splitmix(seed)), static_cast<uint32_t>(splitmix(seed)),
                       static_cast<uint32_t>(splitmix(seed)) | 1u};
    a.record = c.quest;
    a.started_ms = now_ms;
    a.expires_ms = now_ms + std::max<int64_t>(60000, c.expiration_ms);
    s.hand.erase(std::remove(s.hand.begin(), s.hand.end(), c.quest), s.hand.end());
    s.active.push_back(std::move(a));
    return s.active.back();
}

Buy purchase(MayorCommissions& s, int64_t node, int64_t record, int slots, int64_t treasury, int64_t now_ms, MayorActiveCommission* started,
             const std::map<int64_t, int32_t>* stored) {
    const MayoralCommission* c = commission(record);
    if (!c) return Buy::Unknown;
    if (!in(s.hand, record)) return Buy::NotInHand;
    if (static_cast<int>(s.active.size()) >= slots) return Buy::NoSlot;
    if (cooling(s, record) || running(s, record)) return Buy::Cooling;
    if (c->resource != 0 && c->resource_qty > 0) {   // the node's stores must hold it
        const auto held = stored ? stored->find(c->resource) : std::map<int64_t, int32_t>::const_iterator{};
        if (!stored || held == stored->end() || held->second < c->resource_qty) return Buy::ResourceCost;
    }
    if (treasury < c->money_cost) return Buy::Treasury;
    const MayorActiveCommission& a = start(s, node, *c, now_ms);
    if (started) *started = a;
    return Buy::Ok;
}

const MayorActiveCommission* find(const MayorCommissions& s, const MayorGuid& g) {
    for (const MayorActiveCommission& a : s.active) {
        if (a.guid == g) return &a;
    }
    return nullptr;
}

bool completed_by(const MayorActiveCommission& a, const std::string& character) {
    return !character.empty() && std::find(a.completed_by.begin(), a.completed_by.end(), character) != a.completed_by.end();
}

const char* done_name(Done d) {
    switch (d) {
        case Done::Counted: return "counted";
        case Done::Finished: return "done";
        case Done::Already: return "the character completed it already";
        case Done::NotActive: return "it does not run";
    }
    return "?";
}

Done complete(MayorCommissions& s, const MayorGuid& g, const std::string& character, int64_t now_ms, int32_t count) {
    for (size_t i = 0; i < s.active.size(); ++i) {
        MayorActiveCommission& a = s.active[i];
        if (!(a.guid == g)) continue;
        if (now_ms >= a.expires_ms) return Done::NotActive;
        const MayoralCommission* c = commission(a.record);
        if (completed_by(a, character) && !(c && c->multiple)) return Done::Already;
        a.completions += std::max(1, count);
        if (!character.empty() && !completed_by(a, character)) a.completed_by.push_back(character);
        if (c && a.completions >= c->completions) {
            finish(s, i, now_ms);
            return Done::Finished;
        }
        return Done::Counted;
    }
    return Done::NotActive;
}

std::vector<Ended> advance(MayorCommissions& s, int64_t now_ms) {
    std::vector<Ended> out;
    for (size_t i = 0; i < s.active.size();) {
        if (now_ms < s.active[i].expires_ms) {
            ++i;
            continue;
        }
        const MayorActiveCommission a = s.active[i];
        s.active.erase(s.active.begin() + static_cast<std::ptrdiff_t>(i));
        const MayoralCommission* c = commission(a.record);
        s.cooling.push_back(MayorCoolingCommission{a.record, a.expires_ms + (c ? c->cooldown_ms : 0), false, false});
        out.push_back(Ended{Ended::Expired, a.record});
    }
    for (size_t i = 0; i < s.cooling.size();) {
        if (now_ms < s.cooling[i].until_ms) {
            ++i;
            continue;
        }
        out.push_back(Ended{Ended::Cooled, s.cooling[i].record});
        s.cooling.erase(s.cooling.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (size_t i = 0; i < s.rewards.size();) {
        if (now_ms < s.rewards[i].until_ms) {
            ++i;
            continue;
        }
        out.push_back(Ended{Ended::RewardEnded, s.rewards[i].commission});
        s.rewards.erase(s.rewards.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return out;
}

}  // namespace aoc::worldd::world::mayor
