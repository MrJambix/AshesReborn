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
#include "world/elections.h"

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

ElectionPerson who(int64_t account, const char* id, const char* name) { return ElectionPerson{account, id, name, 0}; }

const char* const kAnn = "0000000000000000000000000000000a";
const char* const kBob = "0000000000000000000000000000000b";
const char* const kCy = "0000000000000000000000000000000c";

void test_cycle() {
    std::printf("the cycle of a node's elections\n");
    Elections e;
    const auto takes = [](int64_t n) { return n == kNode; };
    int64_t t = 1'700'000'000'000;
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.set_citizen(kNode, who(2, kBob, "Bob"));
    e.set_citizen(kNode, who(3, kCy, "Cy"));
    auto ch = e.advance(t, takes, {kNode, 42});
    CHECK(ch.size() == 1 && e.node(kNode) && e.node(kNode)->state == ElectionState::AwaitingFirst && !e.node(42), "a node that takes citizens starts with the first wait; another does not");
    CHECK(e.node(kNode)->citizens.size() == 3, "... and keeps the citizens it had before its cycle started");
    std::string why;
    CHECK(!e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why), "nobody stands before the registration period");
    t += 86400 * 1000LL;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration, "after a day: the registration period");
    // no candidate: the registration runs again
    t += 5 * kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->phase_started_ms == t, "a registration with no candidate runs again");
    CHECK(e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why) && e.register_candidate(kNode, who(2, kBob, "Bob"), t + 1, why), "two citizens stand");
    CHECK(!e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why), "... once each");
    CHECK(!e.register_candidate(kNode, who(9, "0000000000000000000000000000000f", "Stranger"), t, why), "a non-citizen does not stand");
    CHECK(!e.vote(kNode, who(3, kCy, "Cy"), {kAnn}, t, why), "nobody votes in the registration period");
    t += 5 * kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Active, "then the election");
    CHECK(e.vote(kNode, who(3, kCy, "Cy"), {kBob, kAnn}, t, why) && e.vote(kNode, who(1, kAnn, "Ann"), {kAnn}, t, why) && e.vote(kNode, who(2, kBob, "Bob"), {kBob}, t, why),
          "three citizens vote, one with a ranking");
    CHECK(!e.vote(kNode, who(3, kCy, "Cy"), {"ffffffffffffffffffffffffffffffff"}, t, why), "a vote for no candidate is refused");
    CHECK(e.vote(kNode, who(3, kCy, "Cy"), {kBob}, t, why) && e.node(kNode)->votes.at(3).size() == 1, "a second vote replaces the first");
    t += 5 * kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingResults, "then the wait for the results");
    t += 10 * 1000;
    ch = e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && !ch.empty() && ch.back().results && ch.back().winner == "Bob", "the results: Bob, two votes to one");
    CHECK(e.node(kNode)->mayor.account == 2 && e.mayor_node(2) == kNode && e.mayor_node(1) == 0, "Bob is the node's mayor");
    CHECK(e.set_message(kNode, 2, "Welcome to Halcyon", why) && e.node(kNode)->mayor_message == "Welcome to Halcyon", "the mayor sets a message");
    CHECK(!e.set_message(kNode, 1, "I am mayor", why), "... nobody else");
    t += 20 * kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->candidates.empty() && e.node(kNode)->votes.empty() && e.node(kNode)->mayor.account == 2,
          "the term ends: a new registration, the old candidates and votes gone, the mayor still serving");
    // a long stop: several phases at once
    e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why);
    t += 11 * kDay;
    ch = e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 1, "after a long stop the phases catch up: Ann, the only candidate, is elected unopposed");
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
    e.skip(kNode, 2000);
    std::string why;
    e.register_candidate(kNode, who(1, kAnn, "Ann"), 2000, why);
    Elections back;
    CHECK(back.from_json(e.to_json(), why) && back.all() == e.all(), "what is written reads back the same");
    CHECK(!back.from_json("{nope", why) && back.all() == e.all(), "a damaged file is refused and changes nothing");
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
    CHECK(r && r->registration_days == 5 && r->election_days == 5 && r->first_registration_secs == 86400 && r->results_secs == 10 && r->election_type == 0,
          "Scientific_Election_Definition: 1 day, 5 days of registration, 5 of voting, 10 s for the results (the test definition is not taken)");
}

}  // namespace

int main() {
    test_cycle();
    test_runoff();
    test_file();
    test_rules();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
