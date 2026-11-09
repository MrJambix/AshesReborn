// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/test_worldd_elections.cpp
//
//  The mayoral elections (world/elections.h): the cycle from the first wait to the term and back to registration, a registration with no candidate running again, who
//  may stand and vote, the instant runoff, the office lost with the citizenship, the mayor's message, a game master's skip, the file round trip, and the rules of the real
//  content.db (AOC_CONTENT_DB or dist/content/content.db; skipped without it).
// ============================================================================
#include "world/citizenship.h"
#include "world/elections.h"
#include "vendor/node_tax.h"
#include "vendor/town_shops.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd::world;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

constexpr int64_t kNode = 5444963087871838208;
constexpr int64_t kDay = 86400000;
constexpr int64_t kHour = 3600000;

// UTC times of the calendar the tests walk through
constexpr int64_t kNov10Noon = 1794312000000;   // 2026-11-10 12:00
constexpr int64_t kNov26 = 1795651200000;       // 2026-11-26 00:00: December's registration
constexpr int64_t kDec1 = 1796083200000;        // 2026-12-01 00:00: December's election
constexpr int64_t kDec27 = 1798329600000;       // 2026-12-27 00:00: January's registration
constexpr int64_t kJan1 = 1798761600000;        // 2027-01-01 00:00
constexpr int64_t kJan6 = 1799193600000;        // 2027-01-06 00:00: the election's end (through the end of hour 23 of the 5th)
constexpr int64_t kJan6Results = kJan6 + 1000;  // 2027-01-06 00:00:01: the results
constexpr int64_t kJan27 = 1801008000000;       // 2027-01-27 00:00: February's registration

ElectionPerson who(int64_t account, const char* id, const char* name, int64_t at = 0) { return ElectionPerson{account, id, name, at}; }

const char* const kAnn = "0000000000000000000000000000000a";
const char* const kBob = "0000000000000000000000000000000b";
const char* const kCy = "0000000000000000000000000000000c";
const char* const kDee = "0000000000000000000000000000000d";

void test_cycle() {
    std::printf("the cycle of a node's elections\n");
    const auto takes = [](int64_t n) { return n == kNode; };
    {
        Elections empty;
        empty.advance(kNov10Noon, takes, {kNode});
        empty.advance(kNov10Noon + 3 * kDay, takes, {kNode});
        CHECK(empty.node(kNode) && empty.node(kNode)->state == ElectionState::AwaitingFirst, "a node with no citizen waits for its first election");
    }
    Elections e;
    int64_t t = kNov10Noon;
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.set_citizen(kNode, who(2, kBob, "Bob"));
    e.set_citizen(kNode, who(3, kCy, "Cy"));
    auto ch = e.advance(t, takes, {kNode, 42});
    CHECK(e.node(kNode) && e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->first && !e.node(42),
          "a node that takes citizens and has some starts its first registration at once; another node does not");
    CHECK(e.node(kNode)->phase_ends_ms == t + kDay && e.node(kNode)->citizens.size() == 3, "... for the first registration's day, with the citizens it had");
    std::string why;
    // the first registration with no candidate runs again
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->phase_started_ms == t, "a first registration with no candidate runs again");
    CHECK(e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why) && e.register_candidate(kNode, who(2, kBob, "Bob"), t + 1, why), "two citizens stand");
    CHECK(!e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why), "... once each");
    CHECK(!e.register_candidate(kNode, who(9, "0000000000000000000000000000000f", "Stranger"), t, why), "a non-citizen does not stand");
    CHECK(!e.vote(kNode, who(3, kCy, "Cy"), {kAnn}, t, why), "nobody votes in the registration period");
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Active && e.node(kNode)->phase_ends_ms == t + kDay, "then the first election, for a day");
    CHECK(e.vote(kNode, who(3, kCy, "Cy"), {kBob, kAnn}, t, why) && e.vote(kNode, who(1, kAnn, "Ann"), {kAnn}, t, why) && e.vote(kNode, who(2, kBob, "Bob"), {kBob}, t, why),
          "three citizens vote, one with a ranking");
    CHECK(!e.vote(kNode, who(3, kCy, "Cy"), {"ffffffffffffffffffffffffffffffff"}, t, why), "a vote for no candidate is refused");
    CHECK(e.vote(kNode, who(3, kCy, "Cy"), {kBob}, t, why) && e.node(kNode)->votes.at(3).size() == 1, "a second vote replaces the first");
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingResults && e.node(kNode)->phase_ends_ms == t + 10 * 1000, "then the first wait for the results, 10 s");
    t += 10 * 1000;
    ch = e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && !ch.empty() && ch.back().results && ch.back().winner == "Bob", "the results: Bob, two votes to one");
    CHECK(e.node(kNode)->mayor.account == 2 && e.mayor_node(2) == kNode && e.mayor_node(1) == 0 && !e.node(kNode)->first, "Bob is the node's mayor; the first election is over");
    CHECK(e.node(kNode)->phase_ends_ms == kNov26, "the term runs to the next month's registration: 26 November, five days before the election of 1 December");
    CHECK(e.set_message(kNode, 2, "Welcome to Halcyon", why) && e.node(kNode)->mayor_message == "Welcome to Halcyon", "the mayor sets a message");
    CHECK(!e.set_message(kNode, 1, "I am mayor", why), "... nobody else");
    // the monthly cycle
    e.advance(kNov26, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->phase_ends_ms == kDec1 && e.node(kNode)->candidates.empty() &&
              e.node(kNode)->mayor.account == 2,
          "the month's registration: until 1 December 00:00, the old candidates gone, the mayor still serving");
    ch = e.advance(kDec1, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 2 && !ch.empty() && !ch.back().results &&
              e.node(kNode)->phase_ends_ms == kDec27,
          "nobody stood in December: no vote, Bob stays mayor until January's registration (27 December)");
    e.advance(kDec27, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.register_candidate(kNode, who(1, kAnn, "Ann"), kDec27, why), "January's registration: Ann stands");
    e.advance(kJan1, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Active && e.node(kNode)->phase_ends_ms == kJan6, "the election from 1 January 00:00 through the 5th's hour 23 (to 00:00 on the 6th)");
    e.advance(kJan6, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingResults && e.node(kNode)->phase_ends_ms == kJan6Results, "the results one second later, at 00:00:01");
    e.advance(kJan6Results, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 1 && e.node(kNode)->mayor.at_ms == kJan6Results,
          "at 00:00:01 the post-election state and the elected mayor");
    // a long stop: the phases catch up
    t = kJan6Results + 3 * kHour;
    ch = e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 1 && e.node(kNode)->phase_ends_ms == kJan27,
          "Ann, the only candidate, is elected unopposed; her term runs to February's registration");
    CHECK(e.node(kNode)->mayor_message.empty(), "... with no message (the office changed hands)");
    // the office goes with the citizenship
    e.remove_citizen(1);
    CHECK(e.node(kNode)->mayor.account == 0 && e.node(kNode)->citizens.count(1) == 0, "the mayor who loses the citizenship loses the office");
    // another node's citizenship moves the account
    e.set_citizen(77, who(3, kCy, "Cy"));
    CHECK(e.node(kNode)->citizens.count(3) == 0 && e.node(77) && e.node(77)->citizens.count(3) == 1, "a citizen of another node is no citizen of this one");
    // a game master's skip
    const auto skipped = e.skip(kNode, t);
    CHECK(skipped && e.node(kNode)->state == ElectionState::Registration, "a game master skips the term: the registration starts");
}

void test_calendar() {
    std::printf("the election calendar of the data\n");
    ElectionRules r;
    CHECK(r.election_start_ms(kNov10Noon) == kDec1, "from 10 November the next election starts on 1 December (its registration on the 26th has not begun)");
    CHECK(r.election_start_ms(kNov26) == kDec1 && r.election_start_ms(kNov26 + 1) == kJan1, "... on the registration's first instant still that one, after it the next month's");
    CHECK(r.election_end_ms(kJan1) == kJan6 && r.results_ms(kJan6) == kJan6Results, "five days through hour 23 of the last (00:00 after it), the results at 00:00:01");
    ElectionRules early = r;
    early.end_hour = 20;
    CHECK(early.election_end_ms(kJan1) == kJan6 - 3 * kHour, "an end hour of 20 ends at 21:00");
}

void test_ballot_and_rank() {
    std::printf("the ballot's limit and the citizens' rank\n");
    Elections e;
    const auto takes = [](int64_t) { return true; };
    e.set_citizen(kNode, who(1, kAnn, "Ann", 300));
    e.set_citizen(kNode, who(2, kBob, "Bob", 100));
    e.set_citizen(kNode, who(3, kCy, "Cy", 200));
    e.set_citizen(kNode, who(4, kDee, "Dee", 200));
    CHECK(e.citizen_rank(kNode, 2) == 1 && e.citizen_rank(kNode, 3) == 2 && e.citizen_rank(kNode, 4) == 3 && e.citizen_rank(kNode, 1) == 4,
          "the rank is the place by when each joined (a tie by account)");
    CHECK(e.citizen_rank(kNode, 9) == 0 && e.citizen_count(kNode) == 4 && e.citizen_count(42) == 0, "no rank for a stranger; four citizens");
    int64_t t = kNov10Noon;
    e.advance(t, takes, {kNode});
    std::string why;
    for (int64_t a = 1; a <= 4; ++a) e.register_candidate(kNode, who(a, a == 1 ? kAnn : a == 2 ? kBob : a == 3 ? kCy : kDee, "x"), t, why);
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Active && e.vote(kNode, who(1, kAnn, "Ann"), {kDee, kCy, kBob, kAnn}, t, why),
          "a ballot ranking four candidates is taken");
    CHECK(e.node(kNode)->votes.at(1) == (std::vector<std::string>{kDee, kCy, kBob}), "... with its first three: RankedChoiceVoteCountLimit");
}

void test_runoff() {
    std::printf("the instant runoff\n");
    NodeElection n;
    n.candidates = {who(1, kAnn, "Ann"), who(2, kBob, "Bob"), who(3, kCy, "Cy")};
    // Ann 2, Bob 2, Cy 1 whose second choice is Bob: Cy goes, Bob wins 3 to 2
    n.votes[10] = {kAnn};
    n.votes[11] = {kAnn};
    n.votes[12] = {kBob};
    n.votes[13] = {kBob};
    n.votes[14] = {kCy, kBob};
    auto w = Elections::tally(n);
    CHECK(w && *w == 1, "the weakest goes and its ballots count for their next choice: Bob");
    // a candidate who withdrew: the ballots naming him count for their next choice still standing (5 x [B, A], 3 x [A], 4 x [C], B gone: A 8 to 4)
    NodeElection gone;
    gone.candidates = {who(1, kAnn, "A"), who(3, kCy, "C")};
    for (int64_t v = 0; v < 5; ++v) gone.votes[20 + v] = {kBob, kAnn};
    for (int64_t v = 0; v < 3; ++v) gone.votes[30 + v] = {kAnn};
    for (int64_t v = 0; v < 4; ++v) gone.votes[40 + v] = {kCy};
    w = Elections::tally(gone);
    CHECK(w && *w == 0, "a candidate who withdrew passes his ballots on: A wins 8 to 4, not C");
    // an exhausted ballot: its only choice is counted out, it counts no more (A 3, B 2, C 2 whose ballots name no one else: C goes, A 3 of 5 wins)
    NodeElection spent;
    spent.candidates = {who(1, kAnn, "A"), who(2, kBob, "B"), who(3, kCy, "C")};
    spent.votes[50] = {kAnn};
    spent.votes[51] = {kAnn};
    spent.votes[52] = {kAnn};
    spent.votes[53] = {kBob};
    spent.votes[54] = {kBob};
    spent.votes[55] = {kCy};
    spent.votes[56] = {kCy};
    w = Elections::tally(spent);
    CHECK(w && *w == 0, "the ballots of the one counted out with no next choice are spent: A, 3 of the 5 still counting");
    // a three-way tie: the later registered go first, the first registered wins
    NodeElection three;
    three.candidates = {who(1, kAnn, "A"), who(2, kBob, "B"), who(3, kCy, "C")};
    three.votes[60] = {kAnn};
    three.votes[61] = {kBob};
    three.votes[62] = {kCy};
    w = Elections::tally(three);
    CHECK(w && *w == 0, "a three-way tie: C goes, then B, A (registered first) wins");
    NodeElection tie;
    tie.candidates = {who(1, kAnn, "Ann"), who(2, kBob, "Bob")};
    tie.votes[10] = {kAnn};
    tie.votes[11] = {kBob};
    w = Elections::tally(tie);
    CHECK(w && *w == 0, "a tie goes to the one who registered first");
    NodeElection none;
    none.candidates = {who(1, kAnn, "Ann")};
    w = Elections::tally(none);
    CHECK(w && *w == 0, "a candidate with no vote is elected unopposed");
    CHECK(!Elections::tally(NodeElection{}), "no candidate: no mayor");
    // a withdrawal during the vote: the candidate's citizenship ends, the ballots naming him go to their next choice
    Elections e;
    const auto takes = [](int64_t) { return true; };
    for (int64_t a = 1; a <= 3; ++a) e.set_citizen(kNode, who(a, a == 1 ? kAnn : a == 2 ? kBob : kCy, a == 1 ? "Ann" : a == 2 ? "Bob" : "Cy"));
    for (int64_t a = 10; a <= 14; ++a) e.set_citizen(kNode, who(a, ("voter" + std::to_string(a)).c_str(), "v"));
    int64_t t = kNov10Noon;
    e.advance(t, takes, {kNode});
    std::string why;
    e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why);
    e.register_candidate(kNode, who(2, kBob, "Bob"), t, why);
    e.register_candidate(kNode, who(3, kCy, "Cy"), t, why);
    t += kDay;
    e.advance(t, takes, {kNode});
    e.vote(kNode, who(10, "voter10", "v"), {kBob, kAnn}, t, why);
    e.vote(kNode, who(11, "voter11", "v"), {kBob, kAnn}, t, why);
    e.vote(kNode, who(12, "voter12", "v"), {kCy}, t, why);
    e.vote(kNode, who(13, "voter13", "v"), {kAnn}, t, why);
    e.vote(kNode, who(14, "voter14", "v"), {kCy}, t, why);
    e.remove_citizen(2);   // Bob leaves the node while the vote runs
    t += kDay;
    e.advance(t + 10 * 1000, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 1, "Bob leaves during the vote: his two ballots go to Ann, who wins 3 to 2");
}

void test_file() {
    std::printf("the elections file\n");
    Elections e;
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.advance(1000, [](int64_t) { return true; }, {kNode});
    std::string why;
    e.register_candidate(kNode, who(1, kAnn, "Ann"), 2000, why);
    Elections back;
    e.set_dues_anchor(kNode, 123456);
    e.set_dues_anchor(kNode, 999);
    CHECK(e.dues_anchor(kNode) == 123456, "a node's dues schedule is set once");
    CHECK(back.from_json(e.to_json(), why) && back.all() == e.all() && back.node(kNode)->first && back.dues_anchor(kNode) == 123456,
          "what is written reads back the same, the first election's mark and the dues schedule too");
    CHECK(!back.from_json("{nope", why) && back.all() == e.all(), "a damaged file is refused and changes nothing");
}

void test_dues() {
    std::printf("the dues of the data's formulas\n");
    CitizenshipRules r;
    r.missed_dues_increase = {{3, 0.5}, {4, 0.75}};
    r.late_fee = 5;
    auto f = r.dues_factors(1, 10, 3);
    CHECK(f.rank == 1 && f.level == 2 && r.dues_amount(f) == 2, "the 1st citizen at level 10: level / 5 = 2, so 2 Tax Receipts");
    f = r.dues_factors(1, 4, 3);
    CHECK(f.level == 1 && r.dues_amount(f) == 1, "below level 5 the level impact is 1");
    // rounded down
    f = r.dues_factors(123, 21, 3);
    CHECK(std::abs(f.level - 4.2) < 1e-9 && std::abs(f.rank - 4.92) < 1e-9 && r.dues_amount(f) == 20, "4.2 x 4.92 = 20.66 is 20 (rounded down)");
    f = r.dues_factors(2221, 14, 3);
    CHECK(std::abs(f.level - 2.8) < 1e-9 && std::abs(f.rank - 88.84) < 1e-9 && r.dues_amount(f) == 248, "2.8 x 88.84 = 248.75 is 248");
    f = r.dues_factors(10, 25, 4);
    CHECK(f.rank == 1 && r.dues_amount(f) == 5, "no factor for the node's citizen count");
    CHECK(r.dues_amount(CitizenshipRules::DuesFactors{0.5, 1, 1, 1}) == 1, "at least 1");
    const int64_t due = 1'800'000'000'000;
    CHECK(r.dues_owed(due, due - 1, 4, 3) == 4 && r.dues_owed(due, r.overdue_ms(due) - 1, 4, 3) == 4, "before the overdue date the dues are the period's");
    CHECK(r.dues_owed(due, r.overdue_ms(due), 4, 3) == 6 + 5 && r.dues_owed(due, r.overdue_ms(due), 3, 4) == 5 + 5,
          "once overdue: the node level's increase (Village half, Town three quarters: 3 x 1.75 = 5.25 is 5) and the late fee");
    // the node's schedule: one set of dates for its citizens, every period from its start, with its time of day
    const int64_t anchor = kNov10Noon + 17 * 60000 + 3000;   // 12:17:03
    CHECK(r.period_ms() == 14 * kDay && r.next_dues_ms(anchor, anchor) == anchor + 14 * kDay, "the first citizen's first date: a period on");
    CHECK(r.next_dues_ms(anchor, anchor + 12 * kDay) == anchor + 14 * kDay, "a citizen who joins 12 days on has the node's date, 2 days away, to the second");
    CHECK(r.next_dues_ms(anchor, anchor + 14 * kDay) == anchor + 28 * kDay && r.next_dues_ms(anchor, anchor + 14 * kDay - 1) == anchor + 14 * kDay,
          "once a date passes, the next period's date");
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    std::string why;
    const auto real = CitizenshipRules::load(path, why);
    CHECK(real && real->base_dues == 1 && real->rank_threshold == 25 && real->level_threshold == 5 && real->non_owner_impact == 1,
          "the real dues: 1 Tax Receipt, the rank's threshold 25 and the level's 5 read from the formulas, no non-owner impact");
    CHECK(real && real->missed_dues_increase.count(3) && std::abs(real->missed_dues_increase.at(3) - 0.5) < 1e-9 && real->dues_level_scalar.count(4) &&
              real->dues_level_scalar.at(4) == 1 && real->join_cost == 1 && real->dues_weeks == 2,
          "... the Village's 50% for missed dues, the node level scalars, one Tax Receipt to join, every 2 weeks");
}

void test_min_citizens() {
    std::printf("the citizens a node needs to elect a mayor\n");
    ElectionRules r;
    r.min_citizens[kNode] = 3;
    Elections e(r);
    const auto takes = [](int64_t n) { return n == kNode; };
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.set_citizen(kNode, who(2, kBob, "Bob"));
    e.advance(kNov10Noon, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingFirst, "two citizens of a node that needs three: no election yet");
    e.set_citizen(kNode, who(3, kCy, "Cy"));
    e.advance(kNov10Noon + kHour, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->first, "the third citizen starts the first election");
    e.remove_citizen(3);
    e.advance(kNov10Noon + kHour + kDay, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingFirst, "too few citizens when the first registration ends: it waits for them again");
    CHECK(r.citizens_for_election(42) == 1, "a node the data gives no minimum needs one citizen");
    const auto skipped = e.skip(kNode, kNov10Noon + 2 * kDay);
    CHECK(skipped && e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->first, "a game master's skip starts the first election even with too few citizens");
}

void test_taxes() {
    std::printf("the mayor's taxes and the node's treasury\n");
    Elections e;
    CHECK(!e.taxes(kNode) && e.treasury(kNode) == 0, "a node starts with the data's rates and an empty treasury");
    e.set_tax(kNode, "NodeTax.ArtisanTax", TaxOverride{0.2f, 0});
    e.add_treasury(kNode, 150);
    e.add_treasury(kNode, -500);
    CHECK(e.taxes(kNode) && e.taxes(kNode)->at("NodeTax.ArtisanTax").percent == 0.2f && e.treasury(kNode) == 0, "a rate is kept; the treasury never goes below nothing");
    e.add_treasury(kNode, 75);
    std::string why;
    Elections back;
    CHECK(back.from_json(e.to_json(), why) && back.all() == e.all() && back.treasury(kNode) == 75, "the rates and the treasury are written to the file and read back");
    // a shop's rate: the mayor's for its tag or the nearest tag above it, else the data's default
    const auto* overrides = e.taxes(kNode);
    const aoc::worldd::vendor::TaxRate deep = aoc::worldd::vendor::node_tax_rate("NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax", overrides);
    const aoc::worldd::vendor::TaxRate other = aoc::worldd::vendor::node_tax_rate("NodeTax.GoodsTax", overrides);
    const aoc::worldd::vendor::TaxRate plain = aoc::worldd::vendor::node_tax_rate("NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax", nullptr);
    CHECK(deep.found && deep.percent == 0.2f, "a tag under the one the mayor set takes the mayor's rate");
    CHECK(plain.found && plain.percent == aoc::worldd::vendor::default_tax_rate("NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax").percent && other.percent == plain.percent,
          "the other tags keep the data's default");
    const auto table = aoc::worldd::vendor::node_table(kNode, overrides);
    size_t mine = 0;
    for (const auto& en : table.entries) mine += en.tag == "NodeTax.ArtisanTax" && en.tax_percent == 0.2f && en.flag_a && en.flag_b;
    CHECK(mine == 1 && table.entries.size() == aoc::worldd::vendor::default_table(kNode).entries.size(), "the node's table carries the mayor's rate, marked as changed");
}

void test_tax_limits() {
    std::printf("the tax limits of the real content.db\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    constexpr int64_t kJoeva = 108646121231993;
    std::string why;
    const auto c = CitizenshipRules::load(path, why);
    const TaxLimit* l = c ? c->tax_limit(kJoeva, "NodeTax.ArtisanTax.AgriculturalSupplyBaseTax.SalesTax") : nullptr;
    CHECK(l && std::abs(l->min_percent - 0.1f) < 1e-6f && std::abs(l->max_percent - 1.0f) < 1e-6f && std::abs(l->default_percent - 0.5f) < 1e-6f,
          "Joeva's tax table: a mayor sets a rate from 10% to 100% (the default 50%)");
    CHECK(c && !c->tax_limit(kJoeva, "Something.Else"), "a tag outside the NodeTax tree has no limits");
    const auto r = ElectionRules::load(path, why);
    CHECK(r && r->citizens_for_election(kJoeva) == 10, "Joeva needs 10 citizens to elect a mayor (MayorMinimumCitizens)");
}

void test_rules() {
    std::printf("the election rules of the real content.db\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    std::string why;
    const auto r = ElectionRules::load(path, why);
    CHECK(r && r->registration_days == 5 && r->election_days == 5 && r->first_registration_secs == 86400 && r->first_election_secs == 86400 && r->results_secs == 10 &&
              r->election_type == 0,
          "Scientific_Election_Definition: the first registration and election a day each, 10 s for their results; then 5 days of registration and 5 of voting");
    CHECK(r && r->start_day == 1 && r->start_hour == 0 && r->end_hour == 23 && r->min_term_days == 0,
          "... the election on the 1st at 00:00 through hour 23 of its last day, no minimum term (the test definition is not taken)");
    CHECK(r && r->ranked_limit == 3, "a ballot ranks at most 3 candidates (ElectionConstants)");
}

void test_mayor_commissions() {
    std::printf("the mayor's commissions: the hand, buying, completions, the cooldown and the node reward\n");
    namespace mc = aoc::worldd::world::mayor;
    CHECK(mc::node_key("New Aela") == "newaela" && mc::node_key("Joeva") == "joeva", "a node's name as the commissions write it");
    const auto joeva = mc::pool("joeva");
    size_t own = 0, shared = 0;
    for (const auto* c : joeva) (std::string(c->node) == "joeva" ? own : shared) += 1;
    CHECK(own >= 10 && shared >= 10 && mc::pool("testhold").empty(), "Joeva draws its own commissions and the Riverlands' shared ones; a node no commission names draws none");
    constexpr int64_t kJoevaNode = 108646121231993;
    MayorCommissions s;
    CHECK(mc::refill_hand(s, "joeva", 10, kJoevaNode) && s.hand.size() == 10, "a Village's hand: 10 commissions");
    std::set<int64_t> distinct(s.hand.begin(), s.hand.end());
    bool all_ours = true;
    for (const int64_t r : s.hand) all_ours = all_ours && std::any_of(joeva.begin(), joeva.end(), [&](const auto* c) { return c->quest == r; });
    CHECK(distinct.size() == 10 && all_ours, "... ten different ones, all Joeva's to draw");
    const int64_t t0 = kNov10Noon;
    const int64_t pick = s.hand[0];
    const mc::MayoralCommission* c = mc::commission(pick);
    CHECK(c && c->money_cost == 50000 && c->completions == 100 && c->expiration_ms == 2 * kDay && c->cooldown_ms == 6 * kHour && c->node_mod != 0 && c->node_mod_ms == kDay,
          "a commission of the data: 50,000 copper, 100 completions, 48 h on the board, 6 h cooling, a node reward for 24 h");
    CHECK(mc::purchase(s, kJoevaNode, 42, 6, 1'000'000, t0) == mc::Buy::Unknown, "a record that is no mayor's commission is not bought");
    CHECK(mc::purchase(s, kJoevaNode, pick, 6, 49'999, t0) == mc::Buy::Treasury && s.active.empty(), "the treasury must hold its cost");
    MayorActiveCommission started;
    CHECK(mc::purchase(s, kJoevaNode, pick, 6, 50'000, t0, &started) == mc::Buy::Ok && s.active.size() == 1 && !started.guid.zero() && started.expires_ms == t0 + 2 * kDay,
          "bought: it runs for 48 h with a guid of its own");
    CHECK(s.hand.size() == 9 && std::find(s.hand.begin(), s.hand.end(), pick) == s.hand.end() && mc::refill_hand(s, "joeva", 10, kJoevaNode) && s.hand.size() == 10 &&
              std::find(s.hand.begin(), s.hand.end(), pick) == s.hand.end(),
          "it leaves the hand, which is filled again without it");
    CHECK(mc::purchase(s, kJoevaNode, pick, 6, 1'000'000, t0) == mc::Buy::NotInHand, "a running commission is not in the hand");
    while (s.active.size() < 6) {
        const int64_t r = s.hand[0];
        if (mc::purchase(s, kJoevaNode, r, 6, 1'000'000, t0) != mc::Buy::Ok) break;
    }
    CHECK(s.active.size() == 6 && mc::purchase(s, kJoevaNode, s.hand[0], 6, 1'000'000, t0) == mc::Buy::NoSlot, "a Village runs 6 at once: a seventh is refused");
    // completions
    const MayorGuid g = started.guid;
    CHECK(mc::complete(s, g, "AAAA", t0 + kHour) == mc::Done::Counted && mc::find(s, g)->completions == 1 && mc::completed_by(*mc::find(s, g), "AAAA"),
          "a character's completion counts");
    CHECK(c->multiple || mc::complete(s, g, "AAAA", t0 + kHour) == mc::Done::Already, "the same character again does not (AllowMultipleCompletion false)");
    CHECK(mc::complete(s, MayorGuid{9, 9, 9, 9}, "BBBB", t0) == mc::Done::NotActive, "a guid that runs nowhere counts for nothing");
    CHECK(mc::complete(s, g, "", t0 + kHour, 98) == mc::Done::Counted && mc::find(s, g)->completions == 99, "a game master's completions count too");
    CHECK(mc::complete(s, g, "CCCC", t0 + 2 * kHour) == mc::Done::Finished && !mc::find(s, g) && s.active.size() == 5, "the hundredth completes it: it stops running");
    CHECK(s.cooling.size() == 1 && s.cooling[0].record == pick && s.cooling[0].completed && s.cooling[0].until_ms == t0 + 2 * kHour + 6 * kHour,
          "it cools down 6 h, done");
    CHECK(s.rewards.size() == 1 && s.rewards[0].mod == c->node_mod && s.rewards[0].until_ms == t0 + 2 * kHour + kDay, "the node has its reward's modification for 24 h");
    s.hand.push_back(pick);
    CHECK(mc::purchase(s, kJoevaNode, pick, 6, 1'000'000, t0 + 3 * kHour) == mc::Buy::Cooling, "it may not be bought while it cools down");
    s.hand.pop_back();
    // the clock
    auto ended = mc::advance(s, t0 + 2 * kHour + 6 * kHour);
    CHECK(ended.size() == 1 && ended[0].kind == mc::Ended::Cooled && s.cooling.empty() && s.rewards.size() == 1, "after 6 h it may be bought again; the reward still runs");
    ended = mc::advance(s, t0 + 2 * kDay);
    size_t expired = 0;
    for (const auto& e : ended) expired += e.kind == mc::Ended::Expired;
    CHECK(expired == 5 && s.active.empty() && s.cooling.size() == 5 && !s.cooling[0].completed, "the others expire after 48 h, not done, and cool down");
    ended = mc::advance(s, t0 + 2 * kHour + kDay + 1);
    CHECK(s.rewards.empty(), "the node reward ends after its 24 h");
    // the store keeps them
    Elections e;
    e.set_citizen(kJoevaNode, who(1, kAnn, "Ann"));
    MayorActiveCommission kept;
    e.change_commissions(kJoevaNode, [&](MayorCommissions& m, int64_t& treasury) {
        treasury = 120'000;
        mc::refill_hand(m, "joeva", 10, kJoevaNode);
        const int64_t r = m.hand[0];
        mc::purchase(m, kJoevaNode, r, 6, treasury, t0, &kept);
        treasury -= 50'000;
        mc::complete(m, kept.guid, "AAAA", t0 + 1);
        return true;
    });
    CHECK(e.treasury(kJoevaNode) == 70'000 && e.commissions(kJoevaNode) && e.commissions(kJoevaNode)->active.size() == 1, "the store pays from the node's treasury");
    std::string why;
    Elections back;
    CHECK(back.from_json(e.to_json(), why) && back.commissions(kJoevaNode) && *back.commissions(kJoevaNode) == *e.commissions(kJoevaNode),
          "the hand, the running commission with its completions and the counter are written to the file and read back");
}

}  // namespace

int main() {
    test_cycle();
    test_calendar();
    test_ballot_and_rank();
    test_runoff();
    test_file();
    test_rules();
    test_dues();
    test_min_citizens();
    test_taxes();
    test_tax_limits();
    test_mayor_commissions();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
