// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/elections.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <set>

namespace aoc::worldd::world {

namespace {

constexpr int64_t kDayMs = 86400000;
constexpr size_t kMaxMessage = 512;
constexpr size_t kMaxRanked = 32;

nlohmann::json person_json(const ElectionPerson& p) { return {{"account", p.account}, {"character", p.character_id}, {"name", p.name}, {"at", p.at_ms}}; }

ElectionPerson person_of(const nlohmann::json& j) {
    ElectionPerson p;
    p.account = j.value("account", int64_t{0});
    p.character_id = j.value("character", std::string());
    p.name = j.value("name", std::string());
    p.at_ms = j.value("at", int64_t{0});
    return p;
}

}  // namespace

const char* election_state_name(ElectionState s) {
    switch (s) {
        case ElectionState::AwaitingFirst: return "AwaitingFirstElection";
        case ElectionState::Registration: return "RegistrationPeriod";
        case ElectionState::Active: return "ActiveElection";
        case ElectionState::AwaitingResults: return "AwaitingElectionResults";
        case ElectionState::Post: return "PostElection";
    }
    return "?";
}

std::optional<ElectionRules> ElectionRules::load(const std::string& path, std::string& error) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "cannot open " + path;
        if (db) sqlite3_close(db);
        return std::nullopt;
    }
    sqlite3_stmt* st = nullptr;
    std::optional<ElectionRules> out;
    if (sqlite3_prepare_v2(db,
                           "SELECT election_type, registration_days, election_days, first_registration_secs, first_wait_for_results_secs FROM election_def "
                           "WHERE name NOT LIKE 'TEST%' ORDER BY guid LIMIT 1",
                           -1, &st, nullptr) != SQLITE_OK) {
        error = path + " has no election_def table";
    } else if (sqlite3_step(st) == SQLITE_ROW) {
        ElectionRules r;
        const unsigned char* type = sqlite3_column_text(st, 0);
        const std::string t = type ? reinterpret_cast<const char*>(type) : "";
        r.election_type = t == "Divine" ? 1 : t == "Military" ? 2 : t == "Economic" ? 3 : 0;
        if (const int d = sqlite3_column_int(st, 1); d > 0 && d <= 60) r.registration_days = d;
        if (const int d = sqlite3_column_int(st, 2); d > 0 && d <= 60) r.election_days = d;
        if (const int64_t s = sqlite3_column_int64(st, 3); s > 0 && s <= 60LL * 86400) r.first_registration_secs = s;
        if (const int64_t s = sqlite3_column_int64(st, 4); s > 0 && s <= 86400) r.results_secs = s;
        out = r;
    } else {
        error = path + "'s election_def has no definition";
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

void Elections::enter(int64_t node, NodeElection& e, ElectionState s, int64_t start_ms, std::vector<Change>& out) {
    Change c;
    c.node = node;
    c.state = s;
    e.state = s;
    e.phase_started_ms = start_ms;
    switch (s) {
        case ElectionState::AwaitingFirst: e.phase_ends_ms = start_ms + rules_.first_registration_secs * 1000; break;
        case ElectionState::Registration:
            e.phase_ends_ms = start_ms + static_cast<int64_t>(rules_.registration_days) * kDayMs;
            e.candidates.clear();
            e.votes.clear();
            e.voters.clear();
            break;
        case ElectionState::Active: e.phase_ends_ms = start_ms + static_cast<int64_t>(rules_.election_days) * kDayMs; break;
        case ElectionState::AwaitingResults: e.phase_ends_ms = start_ms + rules_.results_secs * 1000; break;
        case ElectionState::Post: {
            e.phase_ends_ms = start_ms + static_cast<int64_t>(rules_.term_days) * kDayMs;
            c.results = true;
            if (const auto w = tally(e)) {
                const int64_t before = e.mayor.account;
                e.mayor = e.candidates[*w];
                e.mayor.at_ms = start_ms;
                if (e.mayor.account != before) e.mayor_message.clear();   // a new mayor starts with no message
                c.winner = e.mayor.name;
            } else {
                e.mayor = ElectionPerson{};
                e.mayor_message.clear();
            }
            break;
        }
    }
    ++version_;
    out.push_back(std::move(c));
}

std::vector<Elections::Change> Elections::advance(int64_t now_ms, const std::function<bool(int64_t)>& takes_citizens, const std::vector<int64_t>& nodes) {
    std::vector<Change> out;
    for (const int64_t node : nodes) {
        // a node whose cycle has not started (none yet, or only its citizens kept): it starts when the node takes citizens
        const auto it = nodes_.find(node);
        if ((it == nodes_.end() || it->second.phase_ends_ms == 0) && takes_citizens && takes_citizens(node)) enter(node, nodes_[node], ElectionState::AwaitingFirst, now_ms, out);
    }
    for (auto& [node, e] : nodes_) {
        for (int guard = 0; guard < 64 && e.phase_ends_ms != 0 && now_ms >= e.phase_ends_ms; ++guard) {
            const int64_t at = e.phase_ends_ms;
            switch (e.state) {
                case ElectionState::AwaitingFirst: enter(node, e, ElectionState::Registration, at, out); break;
                case ElectionState::Registration:
                    // no candidate: the registration runs again
                    enter(node, e, e.candidates.empty() ? ElectionState::Registration : ElectionState::Active, at, out);
                    break;
                case ElectionState::Active: enter(node, e, ElectionState::AwaitingResults, at, out); break;
                case ElectionState::AwaitingResults: enter(node, e, ElectionState::Post, at, out); break;
                case ElectionState::Post: enter(node, e, ElectionState::Registration, at, out); break;
            }
        }
    }
    return out;
}

std::optional<Elections::Change> Elections::skip(int64_t node, int64_t now_ms) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) return std::nullopt;
    it->second.phase_ends_ms = now_ms;
    std::vector<Change> out = advance(now_ms, nullptr, {});
    if (out.empty()) return std::nullopt;
    return out.back();
}

const NodeElection* Elections::node(int64_t guid) const {
    const auto it = nodes_.find(guid);
    return it == nodes_.end() ? nullptr : &it->second;
}

void Elections::remove_citizen(int64_t account) {
    if (account == 0) return;
    for (auto& [node, e] : nodes_) {
        bool changed = e.citizens.erase(account) > 0;
        const auto before = e.candidates.size();
        e.candidates.erase(std::remove_if(e.candidates.begin(), e.candidates.end(), [&](const ElectionPerson& c) { return c.account == account; }), e.candidates.end());
        changed = changed || e.candidates.size() != before;
        changed = e.votes.erase(account) > 0 || changed;
        changed = e.voters.erase(account) > 0 || changed;
        if (e.mayor.account == account) {
            e.mayor = ElectionPerson{};
            e.mayor_message.clear();
            changed = true;
        }
        if (changed) ++version_;
    }
}

void Elections::set_citizen(int64_t node, const ElectionPerson& who) {
    if (who.account == 0 || node == 0) return;
    for (auto& [other, e] : nodes_) {
        if (other != node && e.citizens.count(who.account)) remove_citizen(who.account);
    }
    NodeElection& e = nodes_[node];   // (a node with no election yet keeps its citizens until its first one starts)
    if (e.phase_ends_ms == 0 && e.citizens.empty() && e.candidates.empty()) e.state = ElectionState::AwaitingFirst;
    const auto it = e.citizens.find(who.account);
    if (it != e.citizens.end() && it->second.character_id == who.character_id && it->second.name == who.name) return;
    e.citizens[who.account] = who;
    ++version_;
}

bool Elections::register_candidate(int64_t node, const ElectionPerson& who, int64_t now_ms, std::string& why) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) { why = "the node holds no election"; return false; }
    NodeElection& e = it->second;
    if (e.state != ElectionState::Registration) { why = std::string("the node's election is in ") + election_state_name(e.state) + ", not its registration period"; return false; }
    if (e.citizens.count(who.account) == 0) { why = "the account is not a citizen of the node"; return false; }
    if (is_candidate(node, who.account)) { why = "the account stands already"; return false; }
    ElectionPerson c = who;
    c.at_ms = now_ms;
    e.candidates.push_back(std::move(c));
    ++version_;
    return true;
}

bool Elections::vote(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) { why = "the node holds no election"; return false; }
    NodeElection& e = it->second;
    if (e.state != ElectionState::Active) { why = std::string("the node's election is in ") + election_state_name(e.state) + ", not voting"; return false; }
    if (e.citizens.count(who.account) == 0) { why = "the account is not a citizen of the node"; return false; }
    std::vector<std::string> kept;
    std::set<std::string> seen;
    for (const std::string& id : ranked) {
        if (kept.size() >= kMaxRanked || !seen.insert(id).second) continue;
        const bool known = std::any_of(e.candidates.begin(), e.candidates.end(), [&](const ElectionPerson& c) { return c.character_id == id; });
        if (known) kept.push_back(id);
    }
    if (kept.empty()) { why = "it names no candidate of the node"; return false; }
    e.votes[who.account] = std::move(kept);
    ElectionPerson v = who;
    v.at_ms = now_ms;
    e.voters[who.account] = std::move(v);
    ++version_;
    return true;
}

bool Elections::set_message(int64_t node, int64_t account, const std::string& message, std::string& why) {
    auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.mayor.account == 0 || it->second.mayor.account != account) { why = "the account is not the node's mayor"; return false; }
    if (message.size() > kMaxMessage) { why = "the message is longer than 512 bytes"; return false; }
    it->second.mayor_message = message;
    ++version_;
    return true;
}

void Elections::set_mayor(int64_t node, const ElectionPerson& who, int64_t now_ms) {
    NodeElection& e = nodes_[node];
    e.mayor = who;
    e.mayor.at_ms = now_ms;
    if (who.account == 0) e.mayor_message.clear();
    ++version_;
}

bool Elections::is_candidate(int64_t node, int64_t account) const {
    const NodeElection* e = this->node(node);
    return e && std::any_of(e->candidates.begin(), e->candidates.end(), [&](const ElectionPerson& c) { return c.account == account; });
}

int64_t Elections::mayor_node(int64_t account) const {
    if (account == 0) return 0;
    for (const auto& [node, e] : nodes_) {
        if (e.mayor.account == account) return node;
    }
    return 0;
}

std::optional<size_t> Elections::tally(const NodeElection& e) {
    if (e.candidates.empty()) return std::nullopt;
    std::set<std::string> out;   // candidates out of the count
    for (size_t round = 0; round < e.candidates.size(); ++round) {
        std::map<std::string, size_t> firsts;
        size_t total = 0;
        for (const auto& [account, ranked] : e.votes) {
            for (const std::string& id : ranked) {
                if (out.count(id)) continue;
                ++firsts[id];
                ++total;
                break;
            }
        }
        // a majority of the ballots still counting wins; else the weakest goes (ties: the later registered goes first)
        std::optional<size_t> best, weakest;
        size_t best_n = 0, weakest_n = SIZE_MAX, standing = 0;
        for (size_t i = 0; i < e.candidates.size(); ++i) {
            const std::string& id = e.candidates[i].character_id;
            if (out.count(id)) continue;
            ++standing;
            const size_t n = firsts[id];
            if (!best || n > best_n) { best = i; best_n = n; }
            if (!weakest || n <= weakest_n) { weakest = i; weakest_n = n; }
        }
        if (!best) return std::nullopt;
        if (standing == 1 || total == 0 || best_n * 2 > total) return best;
        out.insert(e.candidates[*weakest].character_id);
    }
    return std::nullopt;
}

std::string Elections::to_json() const {
    nlohmann::json j;
    j["version"] = 1;
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& [node, e] : nodes_) {
        nlohmann::json n;
        n["node"] = node;
        n["state"] = static_cast<int>(e.state);
        n["started"] = e.phase_started_ms;
        n["ends"] = e.phase_ends_ms;
        n["mayor"] = person_json(e.mayor);
        n["message"] = e.mayor_message;
        for (const ElectionPerson& c : e.candidates) n["candidates"].push_back(person_json(c));
        for (const auto& [a, p] : e.citizens) n["citizens"].push_back(person_json(p));
        for (const auto& [a, ranked] : e.votes) {
            nlohmann::json v = person_json(e.voters.count(a) ? e.voters.at(a) : ElectionPerson{a, "", "", 0});
            v["ranked"] = ranked;
            n["votes"].push_back(std::move(v));
        }
        nodes.push_back(std::move(n));
    }
    j["nodes"] = std::move(nodes);
    return j.dump(1);
}

bool Elections::from_json(const std::string& text, std::string& error) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("nodes") || !j["nodes"].is_array()) {
        error = "not an elections file";
        return false;
    }
    std::map<int64_t, NodeElection> nodes;
    try {
        for (const nlohmann::json& n : j["nodes"]) {
            NodeElection e;
            const int s = n.value("state", 0);
            if (s < 0 || s > 4) throw std::runtime_error("a state out of range");
            e.state = static_cast<ElectionState>(s);
            e.phase_started_ms = n.value("started", int64_t{0});
            e.phase_ends_ms = n.value("ends", int64_t{0});
            if (n.contains("mayor")) e.mayor = person_of(n["mayor"]);
            e.mayor_message = n.value("message", std::string());
            if (n.contains("candidates")) for (const auto& c : n["candidates"]) e.candidates.push_back(person_of(c));
            if (n.contains("citizens")) for (const auto& c : n["citizens"]) { const ElectionPerson p = person_of(c); if (p.account != 0) e.citizens[p.account] = p; }
            if (n.contains("votes")) {
                for (const auto& v : n["votes"]) {
                    const ElectionPerson p = person_of(v);
                    if (p.account == 0) continue;
                    e.votes[p.account] = v.value("ranked", std::vector<std::string>{});
                    e.voters[p.account] = p;
                }
            }
            nodes[n.value("node", int64_t{0})] = std::move(e);
        }
    } catch (const std::exception& ex) {
        error = std::string("the elections file does not read: ") + ex.what();
        return false;
    }
    nodes_ = std::move(nodes);
    ++version_;
    return true;
}

}  // namespace aoc::worldd::world
