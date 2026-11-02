// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/session/group.h
//
//  Parties and raids, with no network: who is in which group, who leads it, and
//  who has to hear about a change.  Accessed on worldd's endpoint thread, as
//  chat::Room is; each session holds a Member and drains it in its tick
//  (session/player_group.cpp).
//
//  From the client's code: a group is the whole of FGroupClientData each time
//  (UIntrepidSocialComponent::UpdateGroup copies it over what it held, 0x146630090)
//  and a zero GroupGuid is no group.  A party is one squad.  Squad numbers from 0,
//  the sizes (8 a squad, 5 squads) and every rule below are modelled: the retail
//  server's rules are stripped from the client and no recording shows a group.
// ============================================================================
#pragma once

#include "session/chat.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::group {

using Guid = std::array<uint32_t, 4>;
inline constexpr Guid kNoGuid{};

// EAoCGroupType, ERaidShoutPrivilege (layouts/aoc.usmap)
inline constexpr uint8_t kParty = 0, kRaid = 1;
inline constexpr uint8_t kShoutAll = 0, kShoutSquadLeaders = 1, kShoutRaidLeader = 2;

inline constexpr size_t kSquadSize = 8, kRaidSquads = 5;
inline constexpr double kInviteSeconds = 60;    // how long an unanswered invitation holds
inline constexpr double kOfflineSeconds = 300;  // how long a member who left the world keeps his place

/// EChatCategory (layouts/aoc.usmap)
inline constexpr uint8_t kGroupInfo = 16, kGroupLeaderSay = 17, kGroupSay = 18, kGroupShout = 19;
inline constexpr uint8_t kRaidInfo = 20, kRaidLeaderSay = 21, kRaidSay = 22, kRaidShout = 23;
inline constexpr uint8_t kSquadLeaderSay = 24, kSquadSay = 25, kSquadShout = 26;

/// FAoCPartyMemberStats as the session last published it.
struct Vitals {
    float health_percent = 1, max_health = 0, mana_percent = 1, max_mana = 0;
    int32_t level = 1;
    bool dead = false;
    double x = 0, y = 0, z = 0;
    std::optional<std::array<double, 3>> marker;
    float heading = 0;
    bool operator==(const Vitals&) const = default;
};

/// FLootAllocationMethods: the default (EDefaultLootAllocationMethod) and the seven tiers (ELootAllocationMethod), Common to Artifact.
using LootMethods = std::array<uint8_t, 8>;
inline constexpr LootMethods kDefaultLoot{0, 0, 2, 2, 2, 2, 2, 2};   // pkt#606
inline bool valid_loot(const LootMethods& m) {
    if (m[0] > 1) return false;
    return std::all_of(m.begin() + 1, m.end(), [](uint8_t v) { return v <= 3; });
}

struct Settings {
    LootMethods party_loot = kDefaultLoot, raid_loot = kDefaultLoot;
    bool override_party_loot = false;
    bool member_shouts = true;       // FAoCPartySettings.bEnableMemberShouts
    uint8_t shout_privilege = kShoutRaidLeader;   // the empty group of a retail login says so, and bEnableMemberShouts
    bool operator==(const Settings&) const = default;
};

struct SquadView {
    int32_t number = 0;
    std::vector<size_t> members;     // indices into View::people, in joining order
    std::vector<Guid> leaders;
};

struct Person {
    std::string name;
    Guid guid = kNoGuid;
    uint8_t character_class = 0;
    bool online = true;
    Vitals vitals;
};

/// What FGroupClientData says of a group.  `guid` zero: no group.
struct View {
    Guid guid = kNoGuid, leader = kNoGuid, master_looter = kNoGuid;
    uint8_t type = kParty;
    Settings settings;
    std::vector<Person> people;
    std::vector<SquadView> squads;
};

struct Invitation {
    Guid group = kNoGuid;
    std::string inviter;
    uint8_t type = kParty;
};

/// One character's end of the Book; the session holds it for as long as it is in the world.
struct Member {
    std::string name;
    Guid guid = kNoGuid;
    uint8_t character_class = 0;
    Vitals vitals;
    bool view_dirty = true;                    // the group itself changed: ClientUpdateGroup is owed now
    bool vitals_dirty = false;                 // only someone's numbers changed: ClientUpdateGroup is owed at the next round
    std::optional<Invitation> invitation;      // ClientRequestJoinGroup is owed
    std::vector<std::string> status;           // lines only this player sees
    std::vector<chat::Message> chat;           // group chat lines to deliver
    std::optional<std::string> convert_prompt; // ClientRequestConvertToRaid is owed: the player the leader was inviting
};

/// A group's guid: random, never zero (a zero GroupGuid is no group).
inline Guid mint_guid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (;;) {
        const uint64_t a = rng(), b = rng();
        const Guid g{static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a), static_cast<uint32_t>(b >> 32), static_cast<uint32_t>(b)};
        if (g != kNoGuid) return g;
    }
}

class Book {
public:
    /// `mint` makes a group's guid; the Book never invents randomness itself.
    explicit Book(std::function<Guid()> mint) : mint_(std::move(mint)) {}

    /// A character entered the world: he takes back his place if he still has one.
    std::shared_ptr<Member> connect(const std::string& name, const Guid& guid, uint8_t character_class) {
        auto m = std::make_shared<Member>();
        m->name = name;
        m->guid = guid;
        m->character_class = character_class;
        live_.erase(std::remove_if(live_.begin(), live_.end(), [&](const std::weak_ptr<Member>& w) {
            auto p = w.lock();
            return !p || p->guid == guid;
        }), live_.end());
        live_.push_back(m);
        if (Group* g = group_of(guid)) {
            Seat* s = seat_of(*g, guid);
            s->left_at.reset();
            s->name = name;
            touch(*g);
        }
        return m;
    }

    /// A character left the world, or the session ended: his place is held for kOfflineSeconds.  A session a newer login of the same
    /// character replaced says nothing: the place is the new session's.
    void disconnect(const Member& who, double now) {
        const Guid guid = who.guid;
        if (member(guid).get() != &who) return;
        std::vector<Guid> asked;
        invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Pending& p) {
            if (p.invitee != guid) return false;
            asked.push_back(p.group);
            return true;
        }), invites_.end());
        for (const Guid& g : asked) drop_if_lonely(g);
        if (Group* g = group_of(guid)) {
            seat_of(*g, guid)->left_at = now;
            if (g->leader == guid) pass_leadership(*g);
            touch(*g);
        }
    }

    /// Expires invitations and places held too long.  Called from any session's tick, at most once a second by the caller.
    void expire(double now) {
        std::vector<Guid> unanswered;
        invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Pending& p) {
            if (now - p.at < kInviteSeconds) return false;
            if (auto inviter = member(p.inviter)) inviter->status.push_back(p.name + " did not answer your invitation.");
            unanswered.push_back(p.group);
            return true;
        }), invites_.end());
        for (const Guid& g : unanswered) drop_if_lonely(g);
        std::vector<Guid> gone;
        for (const Group& g : groups_) {
            for (const Squad& q : g.squads) {
                for (const Seat& s : q.seats) {
                    if (s.left_at && now - *s.left_at >= kOfflineSeconds) gone.push_back(s.guid);
                }
            }
        }
        for (const Guid& guid : gone) remove_from_group(guid, " left the group.");
    }

    // ---- what a client asks ------------------------------------------------------------------------------------------------------------
    // Each returns the refusal to show the asker, empty when it was accepted.

    std::string invite(Member& from, const std::string& target_name, double now) {
        auto target = member_named(target_name);
        if (!target) return "That player is not online.";
        if (target->guid == from.guid) return "You cannot invite yourself.";
        Group* mine = group_of(from.guid);
        if (mine && mine->leader != from.guid) return "Only the group's leader can invite.";
        if (group_of(target->guid)) return target->name + " is already in a group.";
        for (const Pending& p : invites_) {
            if (p.invitee == target->guid) return target->name + " is considering another invitation.";
        }
        if (mine && capacity(*mine) <= count(*mine)) {
            // a full party: the client's ServerAcceptConvertToRaid turns it into a raid, then invites again
            if (mine->type == kParty && count(*mine) < kSquadSize * kRaidSquads) {
                from.convert_prompt = target->name;
                return {};
            }
            return "Your group is full.";
        }
        if (!mine) {
            groups_.push_back(new_group(from));
            mine = &groups_.back();
        }
        invites_.push_back({mine->guid, from.guid, target->guid, target->name, now});
        target->invitation = Invitation{mine->guid, from.name, mine->type};
        touch(*mine);
        return {};
    }

    std::string accept(Member& who, const Guid& group_guid) {
        const auto it = std::find_if(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.invitee == who.guid && p.group == group_guid; });
        if (it == invites_.end()) return "That invitation is no longer open.";
        invites_.erase(it);
        if (group_of(who.guid)) return "You are already in a group.";
        Group* g = find(group_guid);
        if (!g) return "That group no longer exists.";
        if (count(*g) >= capacity(*g)) return "That group is full.";
        Squad* squad = nullptr;
        for (Squad& q : g->squads) {
            if (q.seats.size() < kSquadSize) { squad = &q; break; }
        }
        if (!squad) return "That group is full.";
        squad->seats.push_back(Seat{who.guid, who.name, std::nullopt});
        announce(*g, who.name + " joined the group.");
        touch(*g);
        return {};
    }

    void decline(Member& who, const Guid& group_guid) {
        const auto it = std::find_if(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.invitee == who.guid && p.group == group_guid; });
        if (it == invites_.end()) return;
        const Guid inviter = it->inviter;
        invites_.erase(it);
        if (auto from = member(inviter)) from->status.push_back(who.name + " declined your invitation.");
        drop_if_lonely(group_guid);
    }

    std::string leave(Member& who) {
        if (!group_of(who.guid)) return "You are not in a group.";
        remove_from_group(who.guid, " left the group.");
        who.view_dirty = true;
        return {};
    }

    std::string kick(Member& from, const std::string& target_name) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can remove a member.";
        const Seat* s = seat_named(*g, target_name);
        if (!s) return target_name + " is not in your group.";
        if (s->guid == from.guid) return "Leave the group instead.";
        const Guid guid = s->guid;
        remove_from_group(guid, " was removed from the group.");
        if (auto target = member(guid)) target->view_dirty = true;
        return {};
    }

    std::string promote(Member& from, const std::string& target_name) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        const Seat* s = seat_named(*g, target_name);
        if (!s) return target_name + " is not in your group.";
        if (s->left_at) return s->name + " is not online.";
        g->leader = s->guid;
        announce(*g, s->name + " now leads the group.");
        touch(*g);
        return {};
    }

    std::string disband(Member& from) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can disband it.";
        disband_group(g->guid, "The group was disbanded.");
        return {};
    }

    std::string convert_to_raid(Member& from) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        if (g->type == kRaid) return {};
        g->type = kRaid;
        for (int32_t n = 1; n < static_cast<int32_t>(kRaidSquads); ++n) g->squads.push_back(Squad{n, {}});
        announce(*g, "The party is now a raid.");
        touch(*g);
        return {};
    }

    std::string convert_to_party(Member& from) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        if (g->type == kParty) return {};
        if (count(*g) > kSquadSize) return "A party holds at most 8 players.";
        std::vector<Seat> everyone;
        for (Squad& q : g->squads) {
            for (Seat& s : q.seats) everyone.push_back(std::move(s));
        }
        g->squads.clear();
        g->squads.push_back(Squad{0, std::move(everyone)});
        g->type = kParty;
        announce(*g, "The raid is now a party.");
        touch(*g);
        return {};
    }

    std::string move_to_squad(Member& from, const std::string& target_name, int32_t squad) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->type != kRaid) return "Only a raid has squads.";
        if (g->leader != from.guid) return "Only the raid's leader can do that.";
        const auto to = std::find_if(g->squads.begin(), g->squads.end(), [&](const Squad& q) { return q.number == squad; });
        if (to == g->squads.end()) return "There is no such squad.";
        for (Squad& q : g->squads) {
            const auto it = std::find_if(q.seats.begin(), q.seats.end(), [&](const Seat& s) { return chat::same_name(s.name, target_name); });
            if (it == q.seats.end()) continue;
            if (&q == &*to) return {};
            if (to->seats.size() >= kSquadSize) return "That squad is full.";
            to->seats.push_back(std::move(*it));
            q.seats.erase(it);
            touch(*g);
            return {};
        }
        return target_name + " is not in your raid.";
    }

    std::string set_member_shouts(Member& from, bool enabled) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        g->settings.member_shouts = enabled;
        touch(*g);
        return {};
    }

    std::string set_shout_privilege(Member& from, uint8_t privilege) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        if (privilege > kShoutRaidLeader) return "That setting does not exist.";
        g->settings.shout_privilege = privilege;
        touch(*g);
        return {};
    }

    /// The leader's loot rules: shown to the group and kept.  Whoever may loot a corpse takes what he likes: no rule is enforced yet.  Retail's approval by the
    /// members (ClientRequestGroupLootAllocationMethodsApproval) is not asked for.
    std::string set_party_loot(Member& from, const LootMethods& methods) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        if (!valid_loot(methods)) return "That loot rule does not exist.";
        g->settings.party_loot = methods;
        touch(*g);
        return {};
    }
    std::string set_raid_loot(Member& from, const LootMethods& methods) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        if (g->type != kRaid) return "You are not in a raid.";
        if (!valid_loot(methods)) return "That loot rule does not exist.";
        g->settings.raid_loot = methods;
        touch(*g);
        return {};
    }
    std::string set_override_party_loot(Member& from, bool enabled) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        if (g->type != kRaid) return "You are not in a raid.";
        g->settings.override_party_loot = enabled;
        touch(*g);
        return {};
    }
    /// An empty name takes the master looter away.
    std::string set_master_looter(Member& from, const std::string& name) {
        Group* g = group_of(from.guid);
        if (!g) return "You are not in a group.";
        if (g->leader != from.guid) return "Only the group's leader can do that.";
        if (name.empty()) {
            g->master_looter = kNoGuid;
        } else {
            const Seat* s = seat_named(*g, name);
            if (!s) return name + " is not in your group.";
            g->master_looter = s->guid;
        }
        touch(*g);
        return {};
    }

    /// The session published new vitals: whoever shares its group hears at their next update.
    void publish(Member& who, const Vitals& vitals) {
        if (who.vitals == vitals) return;
        who.vitals = vitals;
        if (const Group* g = group_of(who.guid)) {
            for (const Squad& q : g->squads) {
                for (const Seat& s : q.seats) {
                    if (auto m = member(s.guid)) m->vitals_dirty = true;
                }
            }
        }
    }

    /// The client held another invitation when `inviter`'s came, so it never showed this one.
    void tell_busy(const Member& who, const std::string& inviter) {
        if (auto from = member_named(inviter)) from->status.push_back(who.name + " is considering another invitation.");
    }

    /// Chat in a group channel.  True when `category` is a group channel, so the caller stops routing it; `refusal` holds what to tell the sender.
    bool say(Member& from, const std::string& text, uint8_t category, std::string& refusal) {
        const bool party_channel = category == kGroupLeaderSay || category == kGroupSay || category == kGroupShout;
        const bool raid_channel = category == kRaidLeaderSay || category == kRaidSay || category == kRaidShout;
        const bool squad_channel = category == kSquadLeaderSay || category == kSquadSay || category == kSquadShout;
        if (!party_channel && !raid_channel && !squad_channel) return false;
        if (!chat::valid_text(text)) { refusal = "Chat messages must contain 1 to 256 characters on one line."; return true; }
        Group* g = group_of(from.guid);
        if (!g) { refusal = "You are not in a group."; return true; }
        if ((raid_channel || squad_channel) && g->type != kRaid) { refusal = "You are not in a raid."; return true; }
        const bool leader = g->leader == from.guid;
        if ((category == kGroupLeaderSay || category == kRaidLeaderSay || category == kSquadLeaderSay) && !leader) { refusal = "Only the leader can use that channel."; return true; }
        if (category == kGroupShout && !leader && !g->settings.member_shouts) { refusal = "The leader has turned off member shouts."; return true; }
        if (category == kRaidShout && !leader && g->settings.shout_privilege == kShoutRaidLeader) { refusal = "Only the raid's leader can shout."; return true; }
        // a party is a squad, so the party channels in a raid reach the sender's squad; the raid channels reach everyone
        const Squad* only = nullptr;
        if (!raid_channel && g->type == kRaid) {
            for (const Squad& q : g->squads) {
                if (std::any_of(q.seats.begin(), q.seats.end(), [&](const Seat& s) { return s.guid == from.guid; })) only = &q;
            }
        }
        for (const Squad& q : g->squads) {
            if (only && &q != only) continue;
            for (const Seat& s : q.seats) {
                auto to = member(s.guid);
                if (to && to->chat.size() < chat::kMaxInbox) to->chat.push_back({text, from.name, category});
            }
        }
        return true;
    }

    /// FGroupClientData for `who`.
    View view(const Member& who) const {
        View v;
        const Group* g = group_of(who.guid);
        if (!g) return v;
        v.guid = g->guid;
        v.leader = g->leader;
        v.master_looter = g->master_looter;
        v.type = g->type;
        v.settings = g->settings;
        for (const Squad& q : g->squads) {
            SquadView sv;
            sv.number = q.number;
            for (const Seat& s : q.seats) {
                Person p;
                p.name = s.name;
                p.guid = s.guid;
                p.online = !s.left_at;
                if (auto m = member(s.guid)) { p.character_class = m->character_class; p.vitals = m->vitals; }
                sv.members.push_back(v.people.size());
                v.people.push_back(std::move(p));
                if (s.guid == g->leader) sv.leaders.push_back(s.guid);
            }
            v.squads.push_back(std::move(sv));
        }
        return v;
    }

    /// Whether two characters share a group (Party or Raid in an ability's mask), and a squad.
    bool same_group(const Guid& a, const Guid& b) const {
        const Group* g = group_of(a);
        return g && seat_of(*g, b);
    }
    bool same_squad(const Guid& a, const Guid& b) const {
        const Group* g = group_of(a);
        if (!g) return false;
        for (const Squad& q : g->squads) {
            const auto holds = [&](const Guid& w) { return std::any_of(q.seats.begin(), q.seats.end(), [&](const Seat& s) { return s.guid == w; }); };
            if (holds(a)) return holds(b);
        }
        return false;
    }
    bool in_group(const Guid& a) const { return group_of(a) != nullptr; }
    size_t groups() const { return groups_.size(); }

private:
    struct Seat {
        Guid guid = kNoGuid;
        std::string name;
        std::optional<double> left_at;  // no value: online
    };
    struct Squad {
        int32_t number = 0;
        std::vector<Seat> seats;
    };
    struct Group {
        Guid guid = kNoGuid, leader = kNoGuid, master_looter = kNoGuid;
        uint8_t type = kParty;
        Settings settings;
        std::vector<Squad> squads;
    };
    struct Pending {
        Guid group = kNoGuid, inviter = kNoGuid, invitee = kNoGuid;
        std::string name;
        double at = 0;
    };

    Group new_group(const Member& leader) {
        Group g;
        g.guid = mint_();
        g.leader = leader.guid;
        g.squads.push_back(Squad{0, {Seat{leader.guid, leader.name, std::nullopt}}});
        return g;
    }
    static size_t count(const Group& g) {
        size_t n = 0;
        for (const Squad& q : g.squads) n += q.seats.size();
        return n;
    }
    static size_t capacity(const Group& g) { return g.squads.size() * kSquadSize; }

    Group* find(const Guid& guid) {
        const auto it = std::find_if(groups_.begin(), groups_.end(), [&](const Group& g) { return g.guid == guid; });
        return it == groups_.end() ? nullptr : &*it;
    }
    const Group* group_of(const Guid& guid) const {
        for (const Group& g : groups_) {
            if (seat_of(g, guid)) return &g;
        }
        return nullptr;
    }
    Group* group_of(const Guid& guid) { return const_cast<Group*>(static_cast<const Book*>(this)->group_of(guid)); }
    static const Seat* seat_of(const Group& g, const Guid& guid) {
        for (const Squad& q : g.squads) {
            for (const Seat& s : q.seats) {
                if (s.guid == guid) return &s;
            }
        }
        return nullptr;
    }
    static Seat* seat_of(Group& g, const Guid& guid) { return const_cast<Seat*>(seat_of(static_cast<const Group&>(g), guid)); }
    static const Seat* seat_named(const Group& g, const std::string& name) {
        for (const Squad& q : g.squads) {
            for (const Seat& s : q.seats) {
                if (chat::same_name(s.name, name)) return &s;
            }
        }
        return nullptr;
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

    void touch(const Group& g) {
        for (const Squad& q : g.squads) {
            for (const Seat& s : q.seats) {
                if (auto m = member(s.guid)) m->view_dirty = true;
            }
        }
    }
    void announce(const Group& g, const std::string& line) {
        for (const Squad& q : g.squads) {
            for (const Seat& s : q.seats) {
                if (auto m = member(s.guid)) m->status.push_back(line);
            }
        }
    }

    /// The first member still online takes the lead; a group with nobody online keeps its leader until the places expire.
    void pass_leadership(Group& g) {
        for (const Squad& q : g.squads) {
            for (const Seat& s : q.seats) {
                if (!s.left_at && s.guid != g.leader) {
                    g.leader = s.guid;
                    announce(g, s.name + " now leads the group.");
                    return;
                }
            }
        }
    }

    void remove_from_group(const Guid& guid, const char* how) {
        Group* g = group_of(guid);
        if (!g) return;
        std::string name;
        for (Squad& q : g->squads) {
            const auto it = std::find_if(q.seats.begin(), q.seats.end(), [&](const Seat& s) { return s.guid == guid; });
            if (it != q.seats.end()) {
                name = it->name;
                q.seats.erase(it);
                break;
            }
        }
        if (g->leader == guid) {
            g->leader = kNoGuid;
            for (const Squad& q : g->squads) {
                for (const Seat& s : q.seats) {
                    if (g->leader == kNoGuid || (!s.left_at && seat_of(*g, g->leader)->left_at)) g->leader = s.guid;
                }
            }
        }
        announce(*g, name + how);
        touch(*g);
        const Guid group_guid = g->guid;
        drop_if_lonely(group_guid);
    }

    /// A group of one has nobody to be grouped with, unless an invitation is still open to fill it.
    void drop_if_lonely(const Guid& group_guid) {
        Group* g = find(group_guid);
        if (!g || count(*g) > 1) return;
        if (std::any_of(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.group == group_guid; })) return;
        disband_group(group_guid, count(*g) == 1 ? "Your group has no other members and was disbanded." : "");
    }

    void disband_group(const Guid& group_guid, const std::string& line) {
        const auto it = std::find_if(groups_.begin(), groups_.end(), [&](const Group& g) { return g.guid == group_guid; });
        if (it == groups_.end()) return;
        std::vector<Guid> seated;
        for (const Squad& q : it->squads) {
            for (const Seat& s : q.seats) seated.push_back(s.guid);
        }
        invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Pending& p) { return p.group == group_guid; }), invites_.end());
        groups_.erase(it);
        for (const Guid& guid : seated) {
            if (auto m = member(guid)) {
                m->view_dirty = true;
                if (!line.empty()) m->status.push_back(line);
            }
        }
    }

    std::function<Guid()> mint_;
    std::vector<Group> groups_;
    std::vector<Pending> invites_;
    std::vector<std::weak_ptr<Member>> live_;
};

}  // namespace aoc::worldd::group
