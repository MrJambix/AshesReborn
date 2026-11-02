// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/tests/test_worldd_guild.cpp
//
//  session/guild.h with no network and no database: founding, invitations, ranks,
//  the leader's succession, the cooldown, chat, and what is handed to the store.
// ============================================================================
#include "session/guild.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace aoc::worldd;
using guild::Book;
using guild::Guid;
using guild::Member;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

Book make_book() {
    return Book([n = uint32_t{1000}]() mutable { ++n; return Guid{n, 1, 2, 3}; });
}
Guid guid_of(uint32_t i) { return Guid{i, 0, 0, 9}; }
bool last_ok(const Member& m) { return !m.replies.empty() && m.replies.back().success; }

void test_names() {
    std::printf("the rules of a name\n");
    CHECK(guild::name_problem("Iron Wolves").empty(), "letters and one space are good");
    CHECK(!guild::name_problem("").empty(), "empty is not");
    CHECK(!guild::name_problem("ab").empty(), "too short is not");
    CHECK(!guild::name_problem(std::string(25, 'a')).empty(), "too long is not");
    CHECK(!guild::name_problem("Iron  Wolves").empty(), "two spaces in a row are not");
    CHECK(!guild::name_problem(" Iron").empty() && !guild::name_problem("Iron ").empty(), "nor a space at an end");
    CHECK(!guild::name_problem("Iron1").empty() && !guild::name_problem("Ir\xc3\xb6n").empty(), "nor digits or anything beyond letters");
}

void test_founding() {
    std::printf("founding a guild\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 10, 1, 100), c = b.connect("Cat", guid_of(2), 12, 2, 100);
    CHECK(!a->view_dirty, "a character with no guild owes nothing at the login");
    b.create(*a, "x!", 100);
    CHECK(!last_ok(*a) && b.guilds() == 0, "a bad name founds nothing");
    b.create(*a, "Iron Wolves", 100);
    CHECK(last_ok(*a) && a->replies.back().request == guild::kReqCreate && b.guilds() == 1 && a->view_dirty, "a good one does, and the founder hears");
    const auto v = b.view(*a, 100);
    CHECK(v.name == "Iron Wolves" && v.level == 1 && v.capacity == 50 && v.level_cap == 10, "as the data's defaults say");
    CHECK(v.ranks.size() == 3 && v.ranks[0].name == "Guild Leader" && v.ranks[1].name == "Officer" && v.ranks[2].name == "Member", "with its three ranks");
    size_t leader = 0, officer = 0, plain = 0;
    for (const bool p : v.ranks[0].permissions) leader += p;
    for (const bool p : v.ranks[1].permissions) officer += p;
    for (const bool p : v.ranks[2].permissions) plain += p;
    CHECK(leader == 35 && officer == 26 && plain == 8, "whose permissions count as the data's do");
    CHECK(v.self && v.self->member.rank == 0 && v.self->online && v.self->member.level == 10, "the founder leads it");
    b.create(*a, "Second", 100);
    CHECK(!last_ok(*a), "nobody founds a second");
    b.create(*c, "iron wolves", 100);
    CHECK(!last_ok(*c) && c->replies.back().message.find("taken") != std::string::npos, "a name taken in any case is refused");
}

/// Ann leads Iron Wolves; Cat is a member.
struct Pair {
    Book b = make_book();
    std::shared_ptr<Member> a = b.connect("Ann", guid_of(1), 10, 1, 100), c = b.connect("Cat", guid_of(2), 12, 2, 100);
    Pair() {
        b.create(*a, "Iron Wolves", 100);
        b.invite(*a, "Cat", 100);
        b.accept(*c, 100);
        a->replies.clear();
        c->replies.clear();
        c->invitation.reset();
        a->view_dirty = c->view_dirty = false;
    }
};

void test_invites() {
    std::printf("invitations\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 10, 1, 100), c = b.connect("Cat", guid_of(2), 12, 2, 100), d = b.connect("Dee", guid_of(3), 1, 3, 100);
    b.invite(*a, "Cat", 100);
    CHECK(!last_ok(*a), "someone with no guild cannot invite");
    b.create(*a, "Iron Wolves", 100);
    b.invite(*a, "Nobody", 100);
    CHECK(!last_ok(*a), "nor can a player offline be invited");
    b.invite(*a, "cAt", 100);
    CHECK(last_ok(*a) && c->invitation && c->invitation->inviter == "Ann" && c->invitation->guild_name == "Iron Wolves" && !c->invitation->uuid.empty(), "the invitee is asked, by whom and to what");
    CHECK(!b.in_guild(guid_of(2)), "he is not in until he accepts");
    b.invite(*a, "Cat", 100);
    CHECK(!last_ok(*a), "a second invitation waits for the first");
    b.decline(*c);
    CHECK(last_ok(*a) && a->replies.back().request == guild::kReqDecline && !b.in_guild(guid_of(2)), "declining tells the inviter");
    b.accept(*c, 100);
    CHECK(!last_ok(*c), "a declined invitation cannot be accepted");
    b.invite(*a, "Cat", 200);
    b.expire(200 + guild::kInviteSeconds);
    b.accept(*c, 300);
    CHECK(!last_ok(*c) && !b.in_guild(guid_of(2)), "one that ran out cannot either");
    b.invite(*a, "Cat", 400);
    b.accept(*c, 401);
    CHECK(b.same_guild(guid_of(1), guid_of(2)) && a->view_dirty && c->view_dirty, "one that holds joins, and both hear");
    const auto v = b.view(*c, 401);
    CHECK(v.self && v.self->member.rank == 2 && v.members.size() == 2, "at the lowest rank");
    b.invite(*c, "Dee", 402);
    CHECK(!last_ok(*c), "a Member's rank cannot invite");
    b.invite(*a, "Dee", 402);
    b.disconnect(*d, 403);
    b.accept(*d, 403);
    CHECK(!last_ok(*d), "an invitee who left the world has no invitation left");
}

void test_leaving() {
    std::printf("leaving, removing and the cooldown\n");
    {
        Pair p;
        p.b.kick(*p.c, guid_of(1), 200);
        CHECK(!last_ok(*p.c) && p.b.same_guild(guid_of(1), guid_of(2)), "a Member cannot remove anyone");
        p.b.kick(*p.a, guid_of(1), 200);
        CHECK(!last_ok(*p.a), "nobody removes himself");
        p.b.kick(*p.a, guid_of(9), 200);
        CHECK(!last_ok(*p.a), "nor a stranger");
        p.b.kick(*p.a, guid_of(2), 200);
        CHECK(!p.b.in_guild(guid_of(2)) && p.c->view_dirty && last_ok(*p.c), "the leader can, and he hears");
        CHECK(p.b.cooldown_until(guid_of(2), 200) == 200 + guild::kCooldownSeconds, "a removed member has the cooldown");
        p.b.invite(*p.a, "Cat", 201);
        CHECK(!last_ok(*p.a) && !p.c->invitation, "so he cannot be invited");
        p.b.create(*p.c, "Other", 201);
        CHECK(!last_ok(*p.c), "or found another");
        p.b.create(*p.c, "Other", 200 + guild::kCooldownSeconds);
        CHECK(last_ok(*p.c), "until it runs out");
    }
    {
        Pair p;
        auto d = p.b.connect("Dee", guid_of(3), 1, 3, 100);
        p.b.invite(*p.a, "Dee", 100);
        p.b.accept(*d, 150);
        p.b.leave(*p.a, 200);
        const auto v = p.b.view(*p.c, 200);
        CHECK(!p.b.in_guild(guid_of(1)) && v.self && v.self->member.rank == 0, "when the leader leaves, the longest-serving of the next rank leads");
        p.b.leave(*p.c, 201);
        p.b.leave(*d, 202);
        CHECK(p.b.guilds() == 0 && d->view_dirty, "the last one out takes the guild with him");
    }
    {
        Pair p;
        p.b.disband(*p.c, 200);
        CHECK(!last_ok(*p.c) && p.b.guilds() == 1, "a Member cannot disband");
        p.b.disband(*p.a, 200);
        CHECK(p.b.guilds() == 0 && p.a->view_dirty && p.c->view_dirty, "the leader can, and all hear");
        CHECK(p.b.view(*p.c, 200).name.empty(), "and the view says no guild");
    }
}

void test_ranks() {
    std::printf("ranks\n");
    Pair p;
    p.b.set_ranks(*p.c, {{guid_of(2), 1}});
    CHECK(!last_ok(*p.c), "a Member cannot change ranks");
    p.b.set_ranks(*p.a, {{guid_of(2), 9}});
    CHECK(!last_ok(*p.a), "a rank that does not exist is refused");
    p.b.set_ranks(*p.a, {{guid_of(2), 0}});
    CHECK(!last_ok(*p.a), "nobody is made leader this way");
    p.b.set_ranks(*p.a, {{guid_of(2), 1}});
    CHECK(last_ok(*p.a) && p.b.view(*p.c, 100).self->member.rank == 1 && p.c->view_dirty, "the leader can promote");
    auto d = p.b.connect("Dee", guid_of(3), 1, 3, 100);
    p.b.invite(*p.c, "Dee", 100);
    CHECK(last_ok(*p.c), "and an Officer can invite");
    p.b.accept(*d, 100);
    p.b.set_ranks(*p.c, {{guid_of(1), 2}});
    CHECK(!last_ok(*p.c), "an Officer cannot touch the leader");
    p.b.set_ranks(*p.c, {{guid_of(3), 1}});
    CHECK(!last_ok(*p.c), "nor raise anyone to his own rank");

    auto ranks = guild::default_ranks();
    p.b.set_hierarchy(*d, ranks);
    CHECK(!last_ok(*d), "a Member cannot manage ranks");
    ranks.push_back({"Recruit", {}});
    ranks[0].permissions.fill(false);
    p.b.set_hierarchy(*p.a, ranks);
    auto v = p.b.view(*p.a, 100);
    CHECK(last_ok(*p.a) && v.ranks.size() == 4 && v.ranks[3].name == "Recruit", "the leader can add a rank");
    CHECK(v.ranks[0].permissions[guild::kDisbandGuild], "but cannot lock the leader's rank");
    p.b.set_ranks(*p.a, {{guid_of(3), 3}});
    ranks = guild::default_ranks();
    p.b.set_hierarchy(*p.a, ranks);
    CHECK(p.b.view(*d, 100).self->member.rank == 2, "members of a deleted rank go to the lowest");
    p.b.set_hierarchy(*p.a, std::vector<guild::Rank>(11, guild::Rank{"Rank", {}}));
    CHECK(!last_ok(*p.a), "ten ranks at most");
    p.b.set_hierarchy(*p.a, {guild::Rank{"Only", {}}});
    CHECK(!last_ok(*p.a), "two at least");

    p.b.assign_leader(*p.c, guid_of(3));
    CHECK(!last_ok(*p.c), "an Officer cannot hand over the guild");
    p.b.assign_leader(*p.a, guid_of(2));
    v = p.b.view(*p.a, 100);
    CHECK(v.self->member.rank == 1 && p.b.view(*p.c, 100).self->member.rank == 0, "the leader can, becoming an Officer");
}

void test_chat() {
    std::printf("guild chat\n");
    Pair p;
    auto out = p.b.connect("Out", guid_of(7), 1, 1, 100);
    std::string why;
    CHECK(!p.b.say(*p.a, "hi", chat::kSay, why), "other channels are not the guild's");
    CHECK(p.b.say(*out, "hi", guild::kGuildSay, why) && !why.empty(), "with no guild the sender is told");
    why.clear();
    CHECK(p.b.say(*p.c, "hello", guild::kGuildSay, why) && why.empty(), "a member can speak");
    CHECK(p.a->chat.size() == 1 && p.c->chat.size() == 1 && out->chat.empty() && p.a->chat[0].from == "Cat" && p.a->chat[0].category == guild::kGuildSay, "members hear it, others do not");
    p.a->chat.clear(); p.c->chat.clear();
    CHECK(p.b.say(*p.c, "secret", guild::kGuildOfficerSay, why) && !why.empty(), "a Member cannot use the officers' channel");
    why.clear();
    CHECK(p.b.say(*p.a, "secret", guild::kGuildOfficerSay, why) && why.empty() && p.a->chat.size() == 1 && p.c->chat.empty(), "officers hear it, Members do not");
    why.clear();
    CHECK(p.b.say(*p.a, std::string(300, 'x'), guild::kGuildSay, why) && !why.empty(), "an oversized line is refused");
}

void test_experience() {
    std::printf("guild experience and levels\n");
    CHECK(guild::level_for_experience(0) == 1 && guild::level_for_experience(guild::kLevelExperience[0] - 1) == 1, "a new guild is level 1 until the table's first step");
    CHECK(guild::level_for_experience(guild::kLevelExperience[0]) == 2, "then level 2");
    CHECK(guild::level_for_experience(0xffffffffu) == guild::kLevelCap, "and stops at the level cap, whatever the table goes on to");
    std::vector<guild::Guild> saved;
    Book b = make_book();
    b.set_store({[&](const guild::Guild& g) { saved.push_back(g); }, nullptr, nullptr});
    auto a = b.connect("Ann", guid_of(1), 10, 1, 100), out = b.connect("Out", guid_of(2), 10, 1, 100);
    b.add_experience(*out, 1000);
    b.flush_experience();
    CHECK(saved.empty(), "someone in no guild earns none for anyone");
    b.create(*a, "Iron Wolves", 100);
    saved.clear();
    a->view_dirty = false;
    a->replies.clear();
    b.add_experience(*a, 100);
    CHECK(saved.empty() && !a->view_dirty, "a little experience writes nothing yet");
    b.flush_experience();
    CHECK(saved.size() == 1 && saved[0].experience == static_cast<uint32_t>(100 * guild::kExperienceShare) && a->view_dirty, "the round writes it and shows it");
    b.flush_experience();
    CHECK(saved.size() == 1, "and a round with nothing new writes nothing");
    a->view_dirty = false;
    b.add_experience(*a, static_cast<int64_t>(guild::kLevelExperience[0] / guild::kExperienceShare) + 10);
    CHECK(saved.size() == 2 && saved[1].level == 2 && a->view_dirty && last_ok(*a) && a->replies.back().request == guild::kReqNotifyLevelUp, "a new level goes at once, and the guild hears");
    b.add_experience(*a, 0x7fffffffffffll);
    b.flush_experience();
    CHECK(b.view(*a, 100).level == guild::kLevelCap && b.view(*a, 100).experience == guild::kLevelExperience[guild::kLevelCap - 2], "experience stops with the cap");
}

void test_store() {
    std::printf("what reaches the store, and what comes back\n");
    std::vector<guild::Guild> saved;
    std::vector<Guid> removed;
    Book b = make_book();
    b.set_store({[&](const guild::Guild& g) { saved.push_back(g); }, [&](const Guid& g) { removed.push_back(g); }});
    auto a = b.connect("Ann", guid_of(1), 10, 1, 100), c = b.connect("Cat", guid_of(2), 12, 2, 100);
    b.create(*a, "Iron Wolves", 100);
    CHECK(saved.size() == 1 && saved[0].name == "Iron Wolves", "founding saves");
    b.invite(*a, "Cat", 100);
    CHECK(saved.size() == 1, "an invitation saves nothing");
    b.accept(*c, 100);
    CHECK(saved.size() == 2 && saved[1].members.size() == 2, "joining saves");
    b.publish(*c, 13, 2);
    CHECK(saved.size() == 3 && saved[2].members[1].level == 13 && c->view_dirty, "a level saves and is shown");
    b.publish(*c, 13, 2);
    CHECK(saved.size() == 3, "the same level saves nothing");
    const guild::Guild kept = saved.back();
    b.disband(*a, 200);
    CHECK(removed.size() == 1 && removed[0] == kept.guid, "disbanding removes it");

    Book restarted = make_book();
    restarted.load({kept});
    CHECK(restarted.guilds() == 1, "a restart loads what was saved");
    auto c2 = restarted.connect("Cat", guid_of(2), 14, 2, 500);
    CHECK(c2->view_dirty && restarted.in_guild(guid_of(2)), "a member logging in is owed the guild");
    const auto v = restarted.view(*c2, 500);
    CHECK(v.name == "Iron Wolves" && v.self && v.self->member.level == 14 && v.members.size() == 2, "with what he is now");
    CHECK(!v.members[0].online && v.self->online, "the absent listed as offline");
    {
        std::vector<std::map<Guid, double>> cooldowns_saved;
        Book c = make_book();
        c.set_store({nullptr, nullptr, [&](const std::map<Guid, double>& m) { cooldowns_saved.push_back(m); }});
        auto x = c.connect("Xan", guid_of(5), 1, 1, 100);
        c.create(*x, "Salt Lions", 100);
        c.leave(*x, 200);
        CHECK(cooldowns_saved.size() == 1 && cooldowns_saved[0].at(guid_of(5)) == 200 + guild::kCooldownSeconds, "a cooldown is handed to the store");
        Book after = make_book();
        after.load({}, cooldowns_saved[0]);
        CHECK(after.cooldown_until(guid_of(5), 300) == 200 + guild::kCooldownSeconds, "and comes back after a restart");
        auto x2 = after.connect("Xan", guid_of(5), 1, 1, 300);
        CHECK(x2->view_dirty, "the character is told of it at his login");
        after.create(*x2, "Salt Lions", 300);
        CHECK(!last_ok(*x2), "so he still cannot found a guild");
    }
    auto ghost = restarted.connect("Cat", guid_of(2), 14, 2, 501);
    restarted.disconnect(*c2, 502);
    CHECK(restarted.view(*ghost, 502).self->online, "a session a newer login replaced does not take him offline");
    restarted.disconnect(*ghost, 503);
    auto a2 = restarted.connect("Ann", guid_of(1), 10, 1, 504);
    const auto v2 = restarted.view(*a2, 504);
    CHECK(v2.members.size() == 2 && !v2.members[1].online && v2.members[1].member.last_online_unix == 503, "one who left is offline, and when is known");
}

}  // namespace

int main() {
    test_names();
    test_founding();
    test_invites();
    test_leaving();
    test_ranks();
    test_chat();
    test_experience();
    test_store();
    std::printf("\ntest_worldd_guild: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
