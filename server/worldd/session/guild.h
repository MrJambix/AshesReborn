// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/session/guild.h
//
//  Guilds, with no network and no database: who belongs to what, with what rank,
//  and who has to hear about a change.  Accessed on worldd's endpoint thread;
//  each session holds a Member and drains it in its tick (session/player_guild.cpp).
//
//  From the data (GuildGenericRecordBase, Guild_Information_Base): the three
//  default ranks and their permissions, a capacity of 50 and level cap of 10,
//  invitations of 60 s, a 5 day cooldown after leaving, the next highest ranking
//  player taking over from a removed leader, members of a deleted rank going to the
//  lowest rank.  From the client (0x1462d6c20): the rules of a name.  Modelled, the
//  retail server's rules being stripped from the client: who may do what beyond the
//  permission names, the length of a name (the client asks two game constants not
//  read here), 10 ranks at most, and the cooldown not surviving a restart.
// ============================================================================
#pragma once

#include "session/chat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::guild {

using Guid = std::array<uint32_t, 4>;
inline constexpr Guid kNoGuid{};

// EGuildRequestType (layouts/aoc.usmap)
inline constexpr uint8_t kReqCreate = 1, kReqInvite = 2, kReqKick = 3, kReqAccept = 4, kReqLeave = 5, kReqDisband = 7, kReqLeader = 8, kReqDecline = 9;
inline constexpr uint8_t kReqRanks = 14, kReqMemberRank = 15, kReqNotifyLevelUp = 16;
// EChatCategory
inline constexpr uint8_t kGuildSay = 31, kGuildShout = 32, kGuildOfficerSay = 33;

inline constexpr size_t kPermissions = 35;          // FGuildPermissionsSettings
inline constexpr size_t kMaxRanks = 10;
inline constexpr size_t kStartingCapacity = 50;
inline constexpr uint8_t kLevelCap = 10;
inline constexpr double kInviteSeconds = 60;
inline constexpr double kCooldownSeconds = 432000;
inline constexpr double kExperienceShare = 0.1;     // of what a member earns: modelled
inline constexpr size_t kMinName = 3, kMaxName = 24;

/// FGuildPermissionsSettings, in its order; the wire's handles are these + 2 in an element of Ranks.
enum Permission : size_t {
    kInvitePlayer, kKickPlayer, kReadMemberDescription, kEditOwnDescription, kReadOfficerDescription, kEditOtherDescription, kEditOtherOfficerDescription,
    kDisbandGuild, kRenameGroups, kReassignGroup, kManageRanks, kManageRankPermissions, kAssignMemberRanks, kViewBank, kDepositBank, kWithdrawBank,
    kViewBankLedger, kManageBankAccounts, kAllianceBank, kGuildHall, kGuildCastle, kGuildStore, kViewCalendar, kPostCalendar, kViewAlerts, kPostAlerts,
    kViewBoard, kPostBoard, kViewMemberLocation, kReadChat, kWriteChat, kAllocateSkillPoints, kDeclareWar, kRewardPoints, kAdjustPoints,
};

using Permissions = std::array<bool, kPermissions>;

struct Rank {
    std::string name;
    Permissions permissions{};
    bool operator==(const Rank&) const = default;
};

struct GuildMember {
    std::string name;
    Guid guid = kNoGuid;
    uint8_t rank = 0;
    uint8_t level = 1;
    uint8_t character_class = 0;
    uint64_t joined_unix = 0, last_online_unix = 0;
    bool operator==(const GuildMember&) const = default;
};

struct Guild {
    Guid guid = kNoGuid;
    std::string name;
    uint8_t level = 1;
    uint32_t experience = 0;
    std::vector<Rank> ranks;
    std::vector<GuildMember> members;
    bool operator==(const Guild&) const = default;
};

/// Guild_Information_Base's DefaultRanks.
inline std::vector<Rank> default_ranks() {
    Permissions all;
    all.fill(true);
    Permissions officer = all;
    for (const Permission p : {kAdjustPoints, kAllocateSkillPoints, kDeclareWar, kDisbandGuild, kManageBankAccounts, kReassignGroup, kRenameGroups, kRewardPoints, kViewBankLedger}) officer[p] = false;
    Permissions member{};
    for (const Permission p : {kReadMemberDescription, kReadOfficerDescription, kDepositBank, kViewAlerts, kViewBank, kViewBoard, kReadChat, kWriteChat}) member[p] = true;
    return {{"Guild Leader", all}, {"Officer", officer}, {"Member", member}};
}

/// What the client checks before it asks (0x1462d6c20), which a hand-made client need not have done.  Empty: good.
inline std::string name_problem(const std::string& name) {
    if (name.empty()) return "A guild needs a name.";
    if (name.size() < kMinName || name.size() > kMaxName) return "A guild's name must have 3 to 24 characters.";
    if (name.front() == ' ' || name.back() == ' ') return "A guild's name cannot start or end with a space.";
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = name[i];
        if (c > 127 || (!std::isalpha(c) && c != ' ')) return "A guild's name can only hold letters and spaces.";
        if (c == ' ' && name[i + 1] == ' ') return "A guild's name cannot hold two spaces in a row.";
    }
    return {};
}

/// GuildLevelingDefRecordBase: the experience a guild needs for each level, from level 2.  A guild stops at kLevelCap (CurrentLevelCap), whatever the table goes on to.
inline constexpr uint32_t kLevelExperience[] = {290000, 460000, 750000, 1210000, 1960000, 3170000, 5130000, 8300000, 14300000};

/// The level a guild's experience is worth.
inline uint8_t level_for_experience(uint32_t experience) {
    uint8_t level = 1;
    for (const uint32_t needed : kLevelExperience) {
        if (experience < needed || level >= kLevelCap) break;
        ++level;
    }
    return level;
}

/// A reply the client owes its player: ClientGuildRequestSucceeded or ClientGuildRequestFailed.
struct Reply {
    uint8_t request = 0;
    std::string message;
    bool success = true;
    bool operator==(const Reply&) const = default;
};

struct Invitation {
    std::string uuid, inviter, guild_name;
};

/// One character's end of the Book; the session holds it for as long as it is in the world.
struct Member {
    std::string name;
    Guid guid = kNoGuid;
    uint8_t level = 1, character_class = 0;
    bool view_dirty = false;               // the guild's properties have to be sent
    std::optional<Invitation> invitation;  // ClientInvitePlayerToGuild is owed
    std::vector<Reply> replies;
    std::vector<chat::Message> chat;
};

/// What the guild component's properties say to a player.
struct View {
    Guid guid = kNoGuid;
    std::string name;
    uint8_t level = 1;
    uint32_t experience = 0;
    uint16_t capacity = kStartingCapacity;
    uint8_t level_cap = kLevelCap;
    std::vector<Rank> ranks;
    struct Row { GuildMember member; bool online = false; };
    std::vector<Row> members;
    std::optional<Row> self;
    double cooldown_until_unix = 0;
};

/// A guild's guid: random, never zero.
inline Guid mint_guid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (;;) {
        const uint64_t a = rng(), b = rng();
        const Guid g{static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a), static_cast<uint32_t>(b >> 32), static_cast<uint32_t>(b)};
        if (g != kNoGuid) return g;
    }
}

/// How the Book's changes reach the database: called on the endpoint thread, so must not block.
struct Store {
    std::function<void(const Guild&)> save;
    std::function<void(const Guid&)> remove;
    std::function<void(const std::map<Guid, double>&)> save_cooldowns;
};

class Book {
public:
    explicit Book(std::function<Guid()> mint = mint_guid) : mint_(std::move(mint)) {}

    void set_store(Store store) { store_ = std::move(store); }

    /// The guilds the database holds, at start-up.
    void load(std::vector<Guild> guilds, std::map<Guid, double> cooldowns = {}) {
        guilds_ = std::move(guilds);
        cooldowns_ = std::move(cooldowns);
    }
    size_t guilds() const { return guilds_.size(); }

    std::shared_ptr<Member> connect(const std::string& name, const Guid& guid, uint8_t level, uint8_t character_class, double now) {
        auto m = std::make_shared<Member>();
        m->name = name;
        m->guid = guid;
        m->level = level;
        m->character_class = character_class;
        live_.erase(std::remove_if(live_.begin(), live_.end(), [&](const std::weak_ptr<Member>& w) {
            auto p = w.lock();
            return !p || p->guid == guid;
        }), live_.end());
        live_.push_back(m);
        if (Guild* g = guild_of(guid)) {
            GuildMember* row = row_of(*g, guid);
            const bool changed = row->name != name || row->level != level || row->character_class != character_class;
            row->name = name;
            row->level = level;
            row->character_class = character_class;
            row->last_online_unix = static_cast<uint64_t>(now);
            if (changed) save(*g);
            touch(*g);
        }
        if (guild_of(guid) || cooldown_until(guid, now) > 0) m->view_dirty = true;
        return m;
    }

    void disconnect(const Member& who, double now) {
        invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.invitee == who.guid; }), invites_.end());
        if (member(who.guid).get() != &who) return;   // a newer login replaced this session
        live_.erase(std::remove_if(live_.begin(), live_.end(), [&](const std::weak_ptr<Member>& w) { auto p = w.lock(); return !p || p.get() == &who; }), live_.end());
        if (Guild* g = guild_of(who.guid)) {
            row_of(*g, who.guid)->last_online_unix = static_cast<uint64_t>(now);
            save(*g);
            touch(*g);
        }
    }

    void expire(double now) {
        invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Pending& p) {
            if (now - p.at < kInviteSeconds) return false;
            if (auto from = member(p.inviter)) from->replies.push_back({kReqInvite, p.invitee_name + " did not answer your invitation.", false});
            return true;
        }), invites_.end());
    }

    // ---- what a client asks: the reply goes to the Member -------------------------------------------------------------------------------

    void create(Member& who, const std::string& name, double now) {
        const auto refuse = [&](std::string why) { who.replies.push_back({kReqCreate, std::move(why), false}); };
        if (guild_of(who.guid)) return refuse("You are already in a guild.");
        if (cooldown_until(who.guid, now) > 0) return refuse("You cannot join a guild until your cooldown is finished.");
        if (const std::string why = name_problem(name); !why.empty()) return refuse(why);
        for (const Guild& g : guilds_) {
            if (chat::same_name(g.name, name)) return refuse("That guild name is taken.");
        }
        Guild g;
        g.guid = mint_();
        g.name = name;
        g.ranks = default_ranks();
        g.members.push_back(row_for(who, 0, now));
        guilds_.push_back(std::move(g));
        save(guilds_.back());
        touch(guilds_.back());
        who.replies.push_back({kReqCreate, "You founded the guild " + name + ".", true});
    }

    void invite(Member& from, const std::string& target_name, double now) {
        const auto refuse = [&](std::string why) { from.replies.push_back({kReqInvite, std::move(why), false}); };
        Guild* g = guild_of(from.guid);
        if (!g) return refuse("You are not in a guild.");
        if (!may(*g, from.guid, kInvitePlayer)) return refuse("Your rank cannot invite players.");
        auto target = member_named(target_name);
        if (!target) return refuse("That player is not online.");
        if (guild_of(target->guid)) return refuse(target->name + " is already in a guild.");
        if (cooldown_until(target->guid, now) > 0) return refuse(target->name + " cannot join a guild until a cooldown is finished.");
        if (g->members.size() >= kStartingCapacity) return refuse("Your guild is full.");
        for (const Pending& p : invites_) {
            if (p.invitee == target->guid) return refuse(target->name + " is considering another invitation.");
        }
        Pending p;
        p.guild = g->guid;
        p.inviter = from.guid;
        p.invitee = target->guid;
        p.invitee_name = target->name;
        p.at = now;
        invites_.push_back(p);
        target->invitation = Invitation{uuid_of(mint_()), from.name, g->name};
        from.replies.push_back({kReqInvite, "You invited " + target->name + " to the guild.", true});
    }

    void accept(Member& who, double now) {
        const auto refuse = [&](std::string why) { who.replies.push_back({kReqAccept, std::move(why), false}); };
        const auto it = std::find_if(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.invitee == who.guid; });
        if (it == invites_.end()) return refuse("That invitation is no longer open.");
        const Guid guild_guid = it->guild;
        invites_.erase(it);
        if (guild_of(who.guid)) return refuse("You are already in a guild.");
        Guild* g = find(guild_guid);
        if (!g) return refuse("That guild no longer exists.");
        if (g->members.size() >= kStartingCapacity) return refuse("That guild is full.");
        g->members.push_back(row_for(who, static_cast<uint8_t>(g->ranks.size() - 1), now));
        save(*g);
        touch(*g);
        tell_all(*g, kReqAccept, who.name + " joined the guild.");
    }

    void decline(Member& who) {
        const auto it = std::find_if(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.invitee == who.guid; });
        if (it == invites_.end()) return;
        if (auto from = member(it->inviter)) from->replies.push_back({kReqDecline, who.name + " declined your invitation.", true});
        invites_.erase(it);
    }

    void leave(Member& who, double now) {
        Guild* g = guild_of(who.guid);
        if (!g) { who.replies.push_back({kReqLeave, "You are not in a guild.", false}); return; }
        if (g->members.size() == 1) {   // the last one out takes the guild with him
            finish_disband(g->guid, now, "You left the guild, and it was disbanded.");
            return;
        }
        remove_member(*g, who.guid, now, who.name + " left the guild.", kReqLeave);
    }

    void kick(Member& from, const Guid& target, double now) {
        const auto refuse = [&](std::string why) { from.replies.push_back({kReqKick, std::move(why), false}); };
        Guild* g = guild_of(from.guid);
        if (!g) return refuse("You are not in a guild.");
        if (!may(*g, from.guid, kKickPlayer)) return refuse("Your rank cannot remove members.");
        const GuildMember* row = row_of(*g, target);
        if (!row) return refuse("That player is not in your guild.");
        if (target == from.guid) return refuse("Leave the guild instead.");
        if (row->rank <= row_of(*g, from.guid)->rank) return refuse("You can only remove members of a lower rank.");
        const std::string line = row->name + " was removed from the guild.";
        remove_member(*g, target, now, line, kReqKick);
    }

    void disband(Member& from, double now) {
        Guild* g = guild_of(from.guid);
        if (!g) { from.replies.push_back({kReqDisband, "You are not in a guild.", false}); return; }
        if (!may(*g, from.guid, kDisbandGuild)) { from.replies.push_back({kReqDisband, "Your rank cannot disband the guild.", false}); return; }
        finish_disband(g->guid, now, "The guild was disbanded.");
    }

    void assign_leader(Member& from, const Guid& target) {
        const auto refuse = [&](std::string why) { from.replies.push_back({kReqLeader, std::move(why), false}); };
        Guild* g = guild_of(from.guid);
        if (!g) return refuse("You are not in a guild.");
        GuildMember* me = row_of(*g, from.guid);
        if (me->rank != 0) return refuse("Only the guild's leader can do that.");
        GuildMember* next = row_of(*g, target);
        if (!next) return refuse("That player is not in your guild.");
        if (next == me) return;
        next->rank = 0;
        me->rank = 1;
        save(*g);
        touch(*g);
        tell_all(*g, kReqLeader, next->name + " now leads the guild.");
    }

    /// ServerReassignMemberRanks.
    void set_ranks(Member& from, const std::vector<std::pair<Guid, uint8_t>>& changes) {
        const auto refuse = [&](std::string why) { from.replies.push_back({kReqMemberRank, std::move(why), false}); };
        Guild* g = guild_of(from.guid);
        if (!g) return refuse("You are not in a guild.");
        if (!may(*g, from.guid, kAssignMemberRanks)) return refuse("Your rank cannot change ranks.");
        const uint8_t mine = row_of(*g, from.guid)->rank;
        for (const auto& [guid, rank] : changes) {
            const GuildMember* row = row_of(*g, guid);
            if (!row) return refuse("That player is not in your guild.");
            if (rank >= g->ranks.size()) return refuse("That rank does not exist.");
            if (row->rank <= mine || rank <= mine) return refuse("You can only change ranks below your own.");
        }
        for (const auto& [guid, rank] : changes) row_of(*g, guid)->rank = rank;
        save(*g);
        touch(*g);
        from.replies.push_back({kReqMemberRank, "", true});
    }

    /// ServerUpdateRankHierarchy: the guild's ranks as they are to be, the leader's first.
    void set_hierarchy(Member& from, std::vector<Rank> ranks) {
        const auto refuse = [&](std::string why) { from.replies.push_back({kReqRanks, std::move(why), false}); };
        Guild* g = guild_of(from.guid);
        if (!g) return refuse("You are not in a guild.");
        if (!may(*g, from.guid, kManageRanks)) return refuse("Your rank cannot manage ranks.");
        if (ranks.size() < 2) return refuse("A guild needs at least two ranks.");
        if (ranks.size() > kMaxRanks) return refuse("A guild can have at most 10 ranks.");
        for (const Rank& r : ranks) {
            if (!chat::valid_text(r.name)) return refuse("A rank needs a name.");
        }
        Permissions all;
        all.fill(true);
        ranks[0].permissions = all;   // nobody can lock the leader out of his own guild
        // A rank's permissions are the only thing an officer without the second permission must leave as they are.
        if (!may(*g, from.guid, kManageRankPermissions)) {
            for (size_t i = 1; i < ranks.size(); ++i) {
                if (i >= g->ranks.size() || ranks[i].permissions != g->ranks[i].permissions) return refuse("Your rank cannot change permissions.");
            }
        }
        const uint8_t lowest = static_cast<uint8_t>(ranks.size() - 1);
        for (GuildMember& m : g->members) {
            if (m.rank > lowest) m.rank = lowest;   // EInvalidGuildRankAction::SetToLowestRank
        }
        g->ranks = std::move(ranks);
        save(*g);
        touch(*g);
        from.replies.push_back({kReqRanks, "", true});
    }

    /// A member's level or class changed.
    void publish(Member& who, uint8_t level, uint8_t character_class) {
        if (who.level == level && who.character_class == character_class) return;
        who.level = level;
        who.character_class = character_class;
        Guild* g = guild_of(who.guid);
        if (!g) return;
        GuildMember* row = row_of(*g, who.guid);
        row->level = level;
        row->character_class = character_class;
        save(*g);
        touch(*g);
    }

    /// A member earned experience: a share goes to his guild (modelled: EGameConstant's ASHES_MOB_KILL_GUILD_EXPERIENCE_MULTIPLIER is not read).  It is written and shown by flush_experience, or at once when the
    /// guild rises a level: a kill must not write a guild and send it to every member.
    void add_experience(const Member& who, int64_t earned) {
        Guild* g = guild_of(who.guid);
        if (!g || earned <= 0) return;
        const uint64_t added = static_cast<uint64_t>(static_cast<double>(earned) * kExperienceShare);
        const uint32_t cap = kLevelExperience[kLevelCap - 2];
        g->experience = static_cast<uint32_t>(std::min<uint64_t>(cap, g->experience + added));
        const uint8_t level = level_for_experience(g->experience);
        if (level != g->level) {
            g->level = level;
            experience_dirty_.erase(g->guid);
            save(*g);
            touch(*g);
            tell_all(*g, kReqNotifyLevelUp, "The guild reached level " + std::to_string(level) + ".");
            return;
        }
        experience_dirty_.insert(g->guid);
    }

    /// Writes and shows the experience gathered since the last time.  The caller calls it now and then.
    void flush_experience() {
        for (const Guid& guid : experience_dirty_) {
            if (Guild* g = find(guid)) {
                save(*g);
                touch(*g);
            }
        }
        experience_dirty_.clear();
    }

    /// Chat in a guild channel.  True when `category` is one, so the caller stops routing it; `refusal` holds what to tell the sender.
    bool say(Member& from, const std::string& text, uint8_t category, std::string& refusal) {
        if (category != kGuildSay && category != kGuildShout && category != kGuildOfficerSay) return false;
        if (!chat::valid_text(text)) { refusal = "Chat messages must contain 1 to 256 characters on one line."; return true; }
        Guild* g = guild_of(from.guid);
        if (!g) { refusal = "You are not in a guild."; return true; }
        if (!may(*g, from.guid, kWriteChat)) { refusal = "Your rank cannot speak in guild chat."; return true; }
        const bool officers = category == kGuildOfficerSay;
        if (officers && !may(*g, from.guid, kManageRanks)) { refusal = "Only officers can use that channel."; return true; }
        for (const GuildMember& m : g->members) {
            const Permissions& held = g->ranks[std::min<size_t>(m.rank, g->ranks.size() - 1)].permissions;
            if (!held[kReadChat] || (officers && !held[kManageRanks])) continue;
            if (auto to = member(m.guid); to && to->chat.size() < chat::kMaxInbox) to->chat.push_back({text, from.name, category});
        }
        return true;
    }

    View view(const Member& who, double now) const {
        View v;
        v.cooldown_until_unix = cooldown_until(who.guid, now);
        const Guild* g = guild_of(who.guid);
        if (!g) return v;
        v.guid = g->guid;
        v.name = g->name;
        v.level = g->level;
        v.experience = g->experience;
        v.ranks = g->ranks;
        for (const GuildMember& m : g->members) {
            View::Row row{m, member(m.guid) != nullptr};
            if (m.guid == who.guid) v.self = row;
            v.members.push_back(std::move(row));
        }
        return v;
    }

    /// When a character may join a guild again, 0 when he may now.
    double cooldown_until(const Guid& guid, double now) const {
        const auto it = cooldowns_.find(guid);
        return it != cooldowns_.end() && it->second > now ? it->second : 0;
    }
    bool in_guild(const Guid& guid) const { return guild_of(guid) != nullptr; }
    /// ClientGetUpdatedGuildsList.
    std::vector<std::pair<std::string, Guid>> names() const {
        std::vector<std::pair<std::string, Guid>> out;
        for (const Guild& g : guilds_) out.emplace_back(g.name, g.guid);
        return out;
    }
    bool same_guild(const Guid& a, const Guid& b) const {
        const Guild* g = guild_of(a);
        return g && row_of(*g, b);
    }

private:
    struct Pending {
        Guid guild = kNoGuid, inviter = kNoGuid, invitee = kNoGuid;
        std::string invitee_name;
        double at = 0;
    };

    static std::string uuid_of(const Guid& g) {
        std::string out;
        static constexpr char kHex[] = "0123456789abcdef";
        for (const uint32_t word : g) {
            for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(word >> shift) & 15];
        }
        return out;
    }
    static GuildMember row_for(const Member& who, uint8_t rank, double now) {
        GuildMember row;
        row.name = who.name;
        row.guid = who.guid;
        row.rank = rank;
        row.level = who.level;
        row.character_class = who.character_class;
        row.joined_unix = row.last_online_unix = static_cast<uint64_t>(now);
        return row;
    }
    Guild* find(const Guid& guid) {
        const auto it = std::find_if(guilds_.begin(), guilds_.end(), [&](const Guild& g) { return g.guid == guid; });
        return it == guilds_.end() ? nullptr : &*it;
    }
    const Guild* guild_of(const Guid& guid) const {
        for (const Guild& g : guilds_) {
            if (row_of(g, guid)) return &g;
        }
        return nullptr;
    }
    Guild* guild_of(const Guid& guid) { return const_cast<Guild*>(static_cast<const Book*>(this)->guild_of(guid)); }
    static const GuildMember* row_of(const Guild& g, const Guid& guid) {
        for (const GuildMember& m : g.members) {
            if (m.guid == guid) return &m;
        }
        return nullptr;
    }
    static GuildMember* row_of(Guild& g, const Guid& guid) { return const_cast<GuildMember*>(row_of(static_cast<const Guild&>(g), guid)); }
    static bool may(const Guild& g, const Guid& who, Permission p) {
        const GuildMember* row = row_of(g, who);
        return row && g.ranks[std::min<size_t>(row->rank, g.ranks.size() - 1)].permissions[p];
    }
    std::shared_ptr<Member> member(const Guid& guid) const {
        for (const auto& w : live_) {
            auto p = w.lock();
            if (p && p->guid == guid) return p;
        }
        return nullptr;
    }
    std::shared_ptr<Member> member_named(const std::string& name) const {
        for (const auto& w : live_) {
            auto p = w.lock();
            if (p && chat::same_name(p->name, name)) return p;
        }
        return nullptr;
    }
    void touch(const Guild& g) {
        for (const GuildMember& m : g.members) {
            if (auto p = member(m.guid)) p->view_dirty = true;
        }
    }
    void tell_all(const Guild& g, uint8_t request, const std::string& line) {
        for (const GuildMember& m : g.members) {
            if (auto p = member(m.guid)) p->replies.push_back({request, line, true});
        }
    }
    void save(const Guild& g) { if (store_.save) store_.save(g); }

    /// ERemovedGuildLeaderAction::PromoteNextHighestRankingPlayer: lowest rank index, then who joined first.
    static void promote_after_leader(Guild& g) {
        const auto next = std::min_element(g.members.begin(), g.members.end(), [](const GuildMember& a, const GuildMember& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.joined_unix < b.joined_unix;
        });
        if (next != g.members.end()) next->rank = 0;
    }

    void remove_member(Guild& g, const Guid& guid, double now, const std::string& line, uint8_t request) {
        const auto it = std::find_if(g.members.begin(), g.members.end(), [&](const GuildMember& m) { return m.guid == guid; });
        if (it == g.members.end()) return;
        const bool was_leader = it->rank == 0;
        g.members.erase(it);
        start_cooldowns({guid}, now);
        if (was_leader) promote_after_leader(g);
        save(g);
        touch(g);
        tell_all(g, request, line);
        if (auto gone = member(guid)) {
            gone->view_dirty = true;
            gone->replies.push_back({request, line, true});
        }
    }

    void start_cooldowns(const std::vector<Guid>& who, double now) {
        for (auto it = cooldowns_.begin(); it != cooldowns_.end();) it = it->second <= now ? cooldowns_.erase(it) : std::next(it);
        for (const Guid& g : who) cooldowns_[g] = now + kCooldownSeconds;
        if (store_.save_cooldowns) store_.save_cooldowns(cooldowns_);
    }

    void finish_disband(const Guid& guid, double now, const std::string& line) {
        const auto it = std::find_if(guilds_.begin(), guilds_.end(), [&](const Guild& g) { return g.guid == guid; });
        if (it == guilds_.end()) return;
        const std::vector<GuildMember> members = it->members;
        invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.guild == guid; }), invites_.end());
        guilds_.erase(it);
        if (store_.remove) store_.remove(guid);
        std::vector<Guid> gone;
        for (const GuildMember& m : members) gone.push_back(m.guid);
        start_cooldowns(gone, now);
        for (const GuildMember& m : members) {
            if (auto p = member(m.guid)) {
                p->view_dirty = true;
                p->replies.push_back({kReqDisband, line, true});
            }
        }
    }

    std::function<Guid()> mint_;
    Store store_;
    std::vector<Guild> guilds_;
    std::vector<Pending> invites_;
    std::vector<std::weak_ptr<Member>> live_;
    std::map<Guid, double> cooldowns_;
    std::set<Guid> experience_dirty_;
};

}  // namespace aoc::worldd::guild
