// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/session/guild_saver.h
//
//  Guilds and the character service: a guild as the JSON the guilds table keeps, and one worker
//  thread that takes the saves off the endpoint thread.  Latest wins: the queue holds at most one
//  entry per guild.  A save the service could not take for now is tried again; anything else is
//  logged and dropped, since the Book still holds the guild and the next change saves it whole.
// ============================================================================
#pragma once

#include "session/guild.h"
#include "svc/services.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace aoc::worldd::guild {

inline std::string guid_hex(const Guid& g) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const uint32_t word : g) {
        for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(word >> shift) & 15];
    }
    return out;
}

inline std::optional<Guid> guid_from_hex(const std::string& s) {
    if (s.size() != 32) return std::nullopt;
    Guid g{};
    for (size_t i = 0; i < 32; ++i) {
        const char c = s[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return std::nullopt;
        g[i / 8] = (g[i / 8] << 4) | static_cast<uint32_t>(v);
    }
    return g;
}

/// What the table's `data` column holds: everything of a guild but its guid and name, which have columns of their own.
inline std::string to_json(const Guild& g) {
    nlohmann::json j;
    j["level"] = g.level;
    j["experience"] = g.experience;
    j["ranks"] = nlohmann::json::array();
    for (const Rank& r : g.ranks) {
        std::string bits;
        for (const bool p : r.permissions) bits += p ? '1' : '0';
        j["ranks"].push_back({{"name", r.name}, {"permissions", bits}});
    }
    j["members"] = nlohmann::json::array();
    for (const GuildMember& m : g.members) {
        j["members"].push_back({{"name", m.name}, {"guid", guid_hex(m.guid)}, {"rank", m.rank}, {"level", m.level}, {"class", m.character_class},
                                {"joined", m.joined_unix}, {"last_online", m.last_online_unix}});
    }
    return j.dump();
}

/// A row back into a guild.  Nothing when it does not read or could not be played: no ranks, or a member with a bad guid.
inline std::optional<Guild> from_json(const std::string& guid, const std::string& name, const std::string& data) {
    const auto parsed = guid_from_hex(guid);
    if (!parsed) return std::nullopt;
    const nlohmann::json j = nlohmann::json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    Guild g;
    g.guid = *parsed;
    g.name = name;
    try {
        g.level = j.at("level").get<uint8_t>();
        g.experience = j.at("experience").get<uint32_t>();
        for (const auto& r : j.at("ranks")) {
            Rank rank;
            rank.name = r.at("name").get<std::string>();
            const std::string bits = r.at("permissions").get<std::string>();
            for (size_t i = 0; i < kPermissions && i < bits.size(); ++i) rank.permissions[i] = bits[i] == '1';
            g.ranks.push_back(std::move(rank));
        }
        for (const auto& m : j.at("members")) {
            GuildMember row;
            row.name = m.at("name").get<std::string>();
            const auto member_guid = guid_from_hex(m.at("guid").get<std::string>());
            if (!member_guid) return std::nullopt;
            row.guid = *member_guid;
            row.rank = m.at("rank").get<uint8_t>();
            row.level = m.at("level").get<uint8_t>();
            row.character_class = m.at("class").get<uint8_t>();
            row.joined_unix = m.at("joined").get<uint64_t>();
            row.last_online_unix = m.at("last_online").get<uint64_t>();
            g.members.push_back(std::move(row));
        }
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (g.ranks.empty()) return std::nullopt;
    return g;
}

/// The rejoin cooldowns have a row of their own in the guilds table, under a guid no guild has: a save of them is one more guild-sized write.
inline const std::string kCooldownRow(32, '0');

inline std::string cooldowns_to_json(const std::map<Guid, double>& cooldowns) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [guid, until] : cooldowns) j[guid_hex(guid)] = static_cast<int64_t>(until);
    return nlohmann::json{{"cooldowns", j}}.dump();
}

inline std::map<Guid, double> cooldowns_from_json(const std::string& data) {
    std::map<Guid, double> out;
    const nlohmann::json j = nlohmann::json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("cooldowns") || !j["cooldowns"].is_object()) return out;
    for (const auto& [hex, until] : j["cooldowns"].items()) {
        const auto guid = guid_from_hex(hex);
        if (guid && until.is_number()) out[*guid] = until.get<double>();
    }
    return out;
}

class GuildSaver {
public:
    explicit GuildSaver(svc::CharacterApi& api, std::chrono::milliseconds retry = std::chrono::seconds(5)) : api_(api), retry_(retry), worker_([this] { run(); }) {}
    ~GuildSaver() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    GuildSaver(const GuildSaver&) = delete;
    GuildSaver& operator=(const GuildSaver&) = delete;

    void save(const Guild& g) {
        {
            std::lock_guard<std::mutex> lock(m_);
            pending_[guid_hex(g.guid)] = Entry{g.name, to_json(g)};
        }
        cv_.notify_one();
    }
    void save_cooldowns(const std::map<Guid, double>& cooldowns) {
        {
            std::lock_guard<std::mutex> lock(m_);
            pending_[kCooldownRow] = Entry{"(rejoin cooldowns)", cooldowns_to_json(cooldowns)};
        }
        cv_.notify_one();
    }
    void remove(const Guid& guid) {
        {
            std::lock_guard<std::mutex> lock(m_);
            pending_[guid_hex(guid)] = std::nullopt;
        }
        cv_.notify_one();
    }

private:
    struct Entry {
        std::string name, data;
    };

    void run() {
        std::unique_lock<std::mutex> lock(m_);
        for (;;) {
            cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) return;   // stopping, with everything written
            const auto first = pending_.begin();
            const std::string guid = first->first;
            const std::optional<Entry> entry = std::move(first->second);
            pending_.erase(first);
            lock.unlock();
            const svc::CallStatus status = entry ? api_.save_guild(guid, entry->name, entry->data) : api_.delete_guild(guid);
            lock.lock();
            if (status == svc::CallStatus::Ok) continue;
            if (status == svc::CallStatus::Unavailable && !stopping_) {
                spdlog::warn("[guild] the character service is unavailable: guild {} will be tried again", guid);
                if (!pending_.count(guid)) pending_[guid] = entry;   // (a newer save wins)
                cv_.wait_for(lock, retry_, [&] { return stopping_; });
                continue;
            }
            spdlog::error("[guild] guild {} could not be {}, status {}", guid, entry ? "saved" : "deleted", static_cast<int>(status));
        }
    }

    svc::CharacterApi& api_;
    std::chrono::milliseconds retry_;
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::optional<Entry>> pending_;
    bool stopping_ = false;
    std::thread worker_;   // last: it starts with everything above ready
};

}  // namespace aoc::worldd::guild
