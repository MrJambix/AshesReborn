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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
constexpr int64_t kJan5At23 = 1799190000000;    // 2027-01-05 23:00: the election's end
constexpr int64_t kJan6At1 = 1799197200000;     // 2027-01-06 01:00: the results
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
    CHECK(e.node(kNode)->state == ElectionState::Active && e.node(kNode)->phase_ends_ms == kJan5At23, "the election from 1 January 00:00 to the 5th at 23:00");
    e.advance(kJan5At23, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingResults && e.node(kNode)->phase_ends_ms == kJan6At1, "the results at 01:00 on the 6th");
    // a long stop: the phases catch up
    t = kJan6At1 + 3 * kHour;
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
    CHECK(r.election_end_ms(kJan1) == kJan5At23 && r.results_ms(kJan5At23) == kJan6At1, "five days to 23:00 of the last, the results at the next 01:00");
    ElectionRules late = r;
    late.results_hour = 23;
    late.end_hour = 20;
    CHECK(late.results_ms(kJan1 + 20 * kHour) == kJan1 + 23 * kHour, "a results hour later on the same day is that day's");
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
}

void test_file() {
    std::printf("the elections file\n");
    Elections e;
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.advance(1000, [](int64_t) { return true; }, {kNode});
    std::string why;
    e.register_candidate(kNode, who(1, kAnn, "Ann"), 2000, why);
    Elections back;
    CHECK(back.from_json(e.to_json(), why) && back.all() == e.all() && back.node(kNode)->first, "what is written reads back the same, the first election's mark too");
    CHECK(!back.from_json("{nope", why) && back.all() == e.all(), "a damaged file is refused and changes nothing");
}

void test_dues() {
    std::printf("the dues of the data's formulas\n");
    CitizenshipRules r;
    r.population_scalars = {{50, 1.0}, {150, 1.3}, {300, 2.0}};
    r.missed_dues_increase = {{3, 0.5}, {4, 0.75}};
    r.late_fee = 5;
    auto f = r.dues_factors(1, 10, 3, 3);
    CHECK(f.rank == 1 && f.level == 2 && f.population == 1 && r.dues_amount(f) == 2, "the 1st citizen at level 10: level / 5 = 2, so 2 Tax Receipts");
    f = r.dues_factors(1, 4, 3, 3);
    CHECK(f.level == 1 && r.dues_amount(f) == 1, "below level 5 the level impact is 1");
    f = r.dues_factors(60, 25, 4, 160);
    CHECK(std::abs(f.rank - 2.4) < 1e-9 && f.level == 5 && std::abs(f.population - 1.3) < 1e-9 && r.dues_amount(f) == 16,
          "the 60th citizen at level 25 in a node of 160: 1 x 2.4 x 5 x 1.3 = 15.6, rounded up to 16");
    f = r.dues_factors(10, 25, 4, 300);
    CHECK(f.rank == 1 && f.population == 2.0 && r.dues_amount(f) == 10, "300 citizens double the dues");
    const int64_t due = 1'800'000'000'000;
    CHECK(r.dues_owed(due, due - 1, 4, 3) == 4 && r.dues_owed(due, r.overdue_ms(due) - 1, 4, 3) == 4, "before the overdue date the dues are the period's");
    CHECK(r.dues_owed(due, r.overdue_ms(due), 4, 3) == 6 + 5 && r.dues_owed(due, r.overdue_ms(due), 4, 4) == 7 + 5,
          "once overdue: the node level's increase (Village half, Town three quarters, rounded up) and the late fee");
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
    CHECK(real && real->population_scalars.size() == 3 && real->missed_dues_increase.count(3) && std::abs(real->missed_dues_increase.at(3) - 0.5) < 1e-9 &&
              real->dues_level_scalar.count(4) && real->dues_level_scalar.at(4) == 1,
          "... the citizens' count scalars, the Village's 50% for missed dues, the node level scalars");
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
    CHECK(r && r->start_day == 1 && r->start_hour == 0 && r->end_hour == 23 && r->results_hour == 1 && r->min_term_days == 0,
          "... the election on the 1st at 00:00 to 23:00 of its last day, the results at 01:00, no minimum term (the test definition is not taken)");
    CHECK(r && r->ranked_limit == 3, "a ballot ranks at most 3 candidates (ElectionConstants)");
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
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
