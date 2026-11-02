// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/tests/test_worldd_group.cpp
//
//  session/group.h with no network: parties and raids, their leaders, their
//  chat, and what happens when a member goes away.
// ============================================================================
#include "session/group.h"

#include <cstdio>
#include <functional>
#include <string>

using namespace aoc::worldd;
using group::Book;
using group::Guid;
using group::Member;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

Book make_book() {
    return Book([n = uint32_t{1000}]() mutable { ++n; return Guid{n, 1, 2, 3}; });
}
Guid guid_of(uint32_t i) { return Guid{i, 0, 0, 7}; }

void test_party() {
    std::printf("a party forms, changes hands and ends\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 1), c = b.connect("Cat", guid_of(2), 2), d = b.connect("Dee", guid_of(3), 3);
    a->view_dirty = c->view_dirty = false;

    CHECK(b.invite(*a, "Ann", 0) != "", "a player cannot invite himself");
    CHECK(b.invite(*a, "Nobody", 0) == "That player is not online.", "inviting someone offline is refused");
    CHECK(b.invite(*a, "cAt", 0).empty() && c->invitation && c->invitation->inviter == "Ann", "an invitation finds a name in any case and names the inviter");
    CHECK(c->invitation && c->invitation->type == group::kParty, "a new group is a party");
    CHECK(b.invite(*a, "Cat", 0) != "", "someone already considering an invitation is not invited again");
    CHECK(b.view(*c).guid == group::kNoGuid, "an invitee is not in the group until he accepts");

    const Guid party = c->invitation->group;
    CHECK(b.accept(*c, Guid{9, 9, 9, 9}) != "", "accepting a group that did not invite him is refused");
    CHECK(b.accept(*c, party).empty(), "accepting joins the group");
    CHECK(b.accept(*c, party) != "", "an invitation can only be used once");
    CHECK(a->view_dirty && c->view_dirty, "both members are owed a new view");
    const auto v = b.view(*a);
    CHECK(v.guid == party && v.leader == guid_of(1) && v.type == group::kParty, "the view names the group and its leader");
    CHECK(v.squads.size() == 1 && v.squads[0].number == 0 && v.squads[0].members.size() == 2, "a party is one squad of its members");
    CHECK(b.same_group(guid_of(1), guid_of(2)) && !b.same_group(guid_of(1), guid_of(3)), "same_group knows who is together");

    CHECK(b.invite(*c, "Dee", 0) != "", "a member who does not lead cannot invite");
    CHECK(b.kick(*c, "Ann") != "", "a member who does not lead cannot kick");
    CHECK(b.promote(*c, "Cat") != "", "a member who does not lead cannot promote");
    CHECK(b.promote(*a, "Cat").empty() && b.view(*a).leader == guid_of(2), "the leader can hand over the lead");
    CHECK(b.promote(*a, "Ann") != "", "the old leader no longer can");

    CHECK(b.invite(*c, "Dee", 0).empty() && b.accept(*d, d->invitation->group).empty(), "the new leader can invite");
    CHECK(b.kick(*c, "Dee").empty() && !b.in_group(guid_of(3)) && d->view_dirty, "a kicked member leaves and is told");
    CHECK(b.kick(*c, "Dee") != "", "kicking someone not in the group is refused");
    CHECK(b.leave(*a).empty(), "a member can leave");
    CHECK(!b.in_group(guid_of(2)) && b.groups() == 0, "a group left with one member ends");
    CHECK(a->view_dirty && c->view_dirty && b.view(*c).guid == group::kNoGuid, "everyone hears it, with a zero guid: no group");
}

void test_decline_and_expiry() {
    std::printf("invitations are declined and run out\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 1), c = b.connect("Cat", guid_of(2), 2);
    CHECK(b.invite(*a, "Cat", 0).empty(), "invited");
    const Guid g1 = c->invitation->group;
    b.decline(*c, g1);
    CHECK(b.groups() == 0 && !a->status.empty(), "a declined invitation tells the inviter and leaves no empty group");
    a->status.clear();
    CHECK(b.invite(*a, "Cat", 100).empty(), "invited again");
    const Guid g2 = c->invitation->group;
    b.expire(100 + group::kInviteSeconds - 1);
    CHECK(b.accept(*c, g2).empty(), "an invitation holds until it runs out");
    Book e = make_book();
    auto x = e.connect("Xan", guid_of(5), 1), y = e.connect("Yul", guid_of(6), 1);
    CHECK(e.invite(*x, "Yul", 100).empty(), "invited in a second book");
    const Guid g3 = y->invitation->group;
    e.expire(100 + group::kInviteSeconds);
    CHECK(e.accept(*y, g3) != "" && !x->status.empty(), "an unanswered invitation runs out and the inviter hears");
    CHECK(e.groups() == 0, "and the group made for it ends");
    CHECK(e.invite(*x, "Yul", 200).empty() && e.groups() == 1, "invited once more");
    e.disconnect(*y, 201);
    CHECK(e.groups() == 0, "an invitee who leaves the world ends a group made for him");
}

void test_offline() {
    std::printf("a member who leaves the world keeps a place for a while\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 1), c = b.connect("Cat", guid_of(2), 2);
    b.invite(*a, "Cat", 0);
    b.accept(*c, c->invitation->group);
    b.disconnect(*a, 10);
    a.reset();
    auto v = b.view(*c);
    CHECK(v.leader == guid_of(2), "the lead passes to someone online");
    CHECK(v.people.size() == 2 && !v.people[0].online && v.people[1].online, "the absent member is listed offline");
    a = b.connect("Ann", guid_of(1), 1);
    v = b.view(*a);
    CHECK(v.people.size() == 2 && v.people[0].online && a->view_dirty, "coming back takes the place again, online");
    auto replaced = a;
    a = b.connect("Ann", guid_of(1), 1);
    b.disconnect(*replaced, 15);
    CHECK(b.view(*a).people[0].online, "a session a newer login replaced does not take the place offline");
    replaced.reset();
    b.disconnect(*a, 20);
    a.reset();
    b.expire(20 + group::kOfflineSeconds - 1);
    CHECK(b.in_group(guid_of(1)), "the place holds until its time is up");
    b.expire(20 + group::kOfflineSeconds);
    CHECK(!b.in_group(guid_of(1)) && !b.in_group(guid_of(2)) && b.groups() == 0, "then it goes, and a group of one ends");
}

void test_raid() {
    std::printf("a full party becomes a raid\n");
    Book b = make_book();
    std::vector<std::shared_ptr<Member>> m;
    for (uint32_t i = 0; i < 41; ++i) m.push_back(b.connect("P" + std::to_string(i), guid_of(i + 1), 1));
    for (size_t i = 1; i < group::kSquadSize; ++i) {
        CHECK(b.invite(*m[0], m[i]->name, 0).empty(), "invited to the party");
        b.accept(*m[i], m[i]->invitation->group);
    }
    CHECK(b.invite(*m[0], m[8]->name, 0).empty() && m[0]->convert_prompt == m[8]->name && !m[8]->invitation, "a ninth asks the leader whether to make a raid");
    CHECK(b.move_to_squad(*m[0], m[1]->name, 1) != "", "a party has no squads to move to");
    CHECK(b.convert_to_raid(*m[1]) != "", "only the leader can convert");
    CHECK(b.convert_to_raid(*m[0]).empty() && b.view(*m[0]).type == group::kRaid && b.view(*m[0]).squads.size() == group::kRaidSquads, "converting gives five squads");
    for (size_t i = 8; i < 40; ++i) {
        const std::string why = b.invite(*m[0], m[i]->name, 0);
        if (!why.empty() || !m[i]->invitation || !b.accept(*m[i], m[i]->invitation->group).empty()) { CHECK(false, "filling the raid"); break; }
    }
    CHECK(b.view(*m[0]).people.size() == 40, "a raid holds forty");
    CHECK(b.invite(*m[0], m[40]->name, 0) == "Your group is full.", "the forty-first is refused");
    CHECK(b.move_to_squad(*m[0], "P1", 7) != "", "a squad that does not exist is refused");
    CHECK(b.move_to_squad(*m[0], "P1", 1) == "That squad is full.", "a full squad takes nobody");
    CHECK(b.same_squad(guid_of(1), guid_of(2)) && !b.same_squad(guid_of(1), guid_of(9)), "same_squad follows the squads");
    CHECK(b.convert_to_party(*m[0]) != "", "a raid over eight cannot become a party");
    for (size_t i = 39; i >= 8; --i) b.leave(*m[i]);
    CHECK(b.view(*m[0]).people.size() == 8, "the raid shrinks");
    CHECK(b.convert_to_party(*m[0]).empty() && b.view(*m[0]).type == group::kParty && b.view(*m[0]).squads.size() == 1, "eight can be a party again");
}

void test_moves() {
    std::printf("raid members move between squads\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 1), c = b.connect("Cat", guid_of(2), 2);
    b.invite(*a, "Cat", 0);
    b.accept(*c, c->invitation->group);
    b.convert_to_raid(*a);
    CHECK(b.move_to_squad(*a, "cat", 3).empty() && !b.same_squad(guid_of(1), guid_of(2)) && b.same_group(guid_of(1), guid_of(2)), "a move changes the squad, not the group");
    CHECK(b.view(*a).squads[3].members.size() == 1, "the view lists him there");
    CHECK(b.move_to_squad(*a, "Nobody", 1) != "", "a stranger cannot be moved");
    CHECK(b.move_to_squad(*c, "Ann", 2) != "", "only the leader moves people");
}

void test_chat() {
    std::printf("group chat reaches the group and no one else\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 1), c = b.connect("Cat", guid_of(2), 2), d = b.connect("Dee", guid_of(3), 3);
    std::string why;
    CHECK(!b.say(*a, "hi", chat::kSay, why), "other channels are not the group's");
    CHECK(b.say(*a, "hi", group::kGroupSay, why) && !why.empty(), "with no group the sender is told");
    why.clear();
    b.invite(*a, "Cat", 0);
    b.accept(*c, c->invitation->group);
    CHECK(b.say(*c, "hello", group::kGroupSay, why) && why.empty(), "a member can speak");
    CHECK(a->chat.size() == 1 && c->chat.size() == 1 && d->chat.empty(), "members hear it, others do not");
    CHECK(a->chat.size() == 1 && a->chat[0].from == "Cat" && a->chat[0].category == group::kGroupSay, "with the sender's name and the channel");
    a->chat.clear(); c->chat.clear();
    CHECK(b.say(*c, "lead", group::kGroupLeaderSay, why) && !why.empty(), "the leader channel is the leader's");
    why.clear();
    CHECK(b.say(*a, "raid", group::kRaidSay, why) && !why.empty(), "raid channels need a raid");
    why.clear();
    CHECK(b.set_member_shouts(*c, false) != "", "only the leader sets shouts");
    CHECK(b.set_member_shouts(*a, false).empty() && b.view(*a).settings.member_shouts == false, "the leader can turn them off");
    CHECK(b.say(*c, "oi", group::kGroupShout, why) && !why.empty() && a->chat.empty(), "a member cannot shout then");
    why.clear();
    CHECK(b.say(*a, "oi", group::kGroupShout, why) && why.empty() && c->chat.size() == 1, "the leader still can");
    a->chat.clear(); c->chat.clear();

    b.convert_to_raid(*a);
    b.move_to_squad(*a, "Cat", 1);
    CHECK(b.say(*a, "squad", group::kGroupSay, why) && a->chat.size() == 1 && c->chat.empty(), "in a raid the party channel stays in the squad");
    a->chat.clear();
    CHECK(b.say(*a, "all", group::kRaidSay, why) && a->chat.size() == 1 && c->chat.size() == 1, "the raid channel reaches everyone");
    CHECK(b.set_shout_privilege(*a, 9) != "", "a privilege that does not exist is refused");
    CHECK(b.set_shout_privilege(*a, group::kShoutRaidLeader).empty(), "raid shouts can be the leader's alone");
    why.clear();
    CHECK(b.say(*c, "oi", group::kRaidShout, why) && !why.empty(), "and then a member cannot");
    why.clear();
    CHECK(b.say(*a, std::string(300, 'x'), group::kGroupSay, why) && !why.empty(), "an oversized line is refused");
}

void test_loot_settings() {
    std::printf("the leader's loot rules\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 1), c = b.connect("Cat", guid_of(2), 2);
    b.invite(*a, "Cat", 0);
    b.accept(*c, c->invitation->group);
    CHECK(b.view(*a).settings.party_loot == group::kDefaultLoot, "a group starts with retail's rules");
    const group::LootMethods rules{1, 1, 1, 3, 3, 3, 3, 3};
    CHECK(b.set_party_loot(*c, rules) != "", "a member cannot set them");
    CHECK(b.set_party_loot(*a, group::LootMethods{2, 0, 0, 0, 0, 0, 0, 0}) != "", "a default that does not exist is refused");
    CHECK(b.set_party_loot(*a, group::LootMethods{0, 4, 0, 0, 0, 0, 0, 0}) != "", "a tier's rule that does not exist is refused");
    a->view_dirty = c->view_dirty = false;
    CHECK(b.set_party_loot(*a, rules).empty() && b.view(*c).settings.party_loot == rules && c->view_dirty, "the leader can, and everyone hears");
    CHECK(b.set_raid_loot(*a, rules) != "" && b.set_override_party_loot(*a, true) != "", "raid rules need a raid");
    b.convert_to_raid(*a);
    CHECK(b.set_raid_loot(*a, rules).empty() && b.view(*a).settings.raid_loot == rules, "then they stick");
    CHECK(b.set_override_party_loot(*a, true).empty() && b.view(*c).settings.override_party_loot, "as does overriding the parties'");
    CHECK(b.set_master_looter(*c, "Ann") != "", "a member cannot name a master looter");
    CHECK(b.set_master_looter(*a, "Nobody") != "", "nor can a stranger be named");
    CHECK(b.set_master_looter(*a, "cat").empty() && b.view(*a).master_looter == guid_of(2), "the leader can name a member");
    CHECK(b.set_master_looter(*a, "").empty() && b.view(*a).master_looter == group::kNoGuid, "and take it away");
}

void test_vitals() {
    std::printf("vitals travel to the group\n");
    Book b = make_book();
    auto a = b.connect("Ann", guid_of(1), 8), c = b.connect("Cat", guid_of(2), 3), d = b.connect("Dee", guid_of(3), 4);
    b.invite(*a, "Cat", 0);
    b.accept(*c, c->invitation->group);
    a->vitals_dirty = c->vitals_dirty = d->vitals_dirty = false;
    group::Vitals v;
    v.health_percent = 0.5f;
    v.level = 12;
    b.publish(*a, v);
    CHECK(a->vitals_dirty && c->vitals_dirty && !d->vitals_dirty, "a change is owed to the group only");
    const auto seen = b.view(*c);
    CHECK(seen.people[0].vitals.health_percent == 0.5f && seen.people[0].vitals.level == 12 && seen.people[0].character_class == 8, "the view carries it, and the class");
    a->vitals_dirty = c->vitals_dirty = false;
    b.publish(*a, v);
    CHECK(!a->vitals_dirty && !c->vitals_dirty, "the same vitals owe nothing");
    b.publish(*d, v);
    CHECK(!a->vitals_dirty, "someone outside the group changes nothing for it");
}

}  // namespace

int main() {
    test_party();
    test_decline_and_expiry();
    test_offline();
    test_raid();
    test_moves();
    test_chat();
    test_loot_settings();
    test_vitals();
    std::printf("\ntest_worldd_group: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
