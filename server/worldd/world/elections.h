// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/elections.h  (node citizenship, stage 2: mayors)
//
//  The mayoral elections of the nodes that take citizens, as one store for the whole world (every session reads it; PlayerWorld owns it and keeps it in a file beside
//  the ground crates, the supervisor's run folder, so an update does not lose it).
//
//    the cycle   EMayorElectionState: AwaitingFirstElection (first_registration_secs after the node first takes citizens) -> RegistrationPeriod (registration_days; the client's
//                "prevote") -> ActiveElection (election_days) -> AwaitingElectionResults (first_wait_for_results_secs) -> PostElection (the mayor's term, then registration
//                again).  From content.db's election_def (Scientific_Election_Definition: 1 day, 5 days, 5 days, 10 s).  Modelled: the term (kTermDays: the data's
//                min_term_days is 0), a registration with no candidate runs again, and the calendar (start_day, start_hour ...) is not followed: the phases follow
//                each other by their lengths.
//    who         the node's citizens (kept here from the sessions' citizenship changes: an account that joins, renounces or lapses), one vote per account; a candidate is a
//                citizen who registered in the registration period.  The mayor's account loses the office with its citizenship.
//    the count   ranked choice (ServerRankedChoiceVoteForMayor sends the ranking; the window sends one): an instant runoff, the candidate with the fewest first choices out
//                each round; a tie goes to the one who registered first.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::world {

enum class ElectionState : uint8_t { AwaitingFirst = 0, Registration = 1, Active = 2, AwaitingResults = 3, Post = 4 };
const char* election_state_name(ElectionState s);

struct ElectionRules {
    int64_t first_registration_secs = 86400;   // AwaitingFirstElection
    int32_t registration_days = 5;
    int32_t election_days = 5;
    int64_t results_secs = 10;                 // first_wait_for_results_secs
    int32_t term_days = 20;                    // modelled
    uint8_t election_type = 0;                 // EMayorElectionType: Scientific 0
    /// content.db's election_def: the first definition that is no test one.  nullopt with `error` for a file without the table.
    static std::optional<ElectionRules> load(const std::string& content_db_path, std::string& error);
};

struct ElectionPerson {
    int64_t     account = 0;
    std::string character_id;   // 32 lower-case hex digits
    std::string name;
    int64_t     at_ms = 0;      // when they joined, registered or voted
    bool operator==(const ElectionPerson&) const = default;
};

struct NodeElection {
    ElectionState state = ElectionState::AwaitingFirst;
    int64_t       phase_started_ms = 0, phase_ends_ms = 0;
    std::vector<ElectionPerson> candidates;                        // in the order they registered
    std::map<int64_t, std::vector<std::string>> votes;             // account -> the ranked candidates' character ids
    std::map<int64_t, ElectionPerson> voters;                      // account -> who voted (the character that voted)
    std::map<int64_t, ElectionPerson> citizens;                    // account -> its primary character
    ElectionPerson mayor;                                          // account 0: no mayor
    std::string    mayor_message;
    bool operator==(const NodeElection&) const = default;
};

class Elections {
public:
    explicit Elections(ElectionRules rules = {}) : rules_(rules) {}
    const ElectionRules& rules() const { return rules_; }

    /// What a phase change did, for the sessions: the node, the new state and, at the results, the winner (empty: no mayor).
    struct Change {
        int64_t       node = 0;
        ElectionState state = ElectionState::AwaitingFirst;
        bool          results = false;
        std::string   winner;
    };
    /// Brings every node to `now_ms`: a node that `takes_citizens` and has no election yet starts AwaitingFirst; each phase whose time ran out gives way to the next
    /// (several, after a long stop).  The changes in order.
    std::vector<Change> advance(int64_t now_ms, const std::function<bool(int64_t node)>& takes_citizens, const std::vector<int64_t>& nodes);
    /// The next phase now, whatever time is left (a game master's).
    std::optional<Change> skip(int64_t node, int64_t now_ms);

    const NodeElection* node(int64_t guid) const;
    const std::map<int64_t, NodeElection>& all() const { return nodes_; }

    /// The account is a citizen of `node` (its primary character); of no other node (it leaves the citizens, candidates and votes of every other, and the office).
    void set_citizen(int64_t node, const ElectionPerson& who);
    /// The account is a citizen of no node.
    void remove_citizen(int64_t account);

    /// A citizen stands for mayor (the registration period).  False with `why`.
    bool register_candidate(int64_t node, const ElectionPerson& who, int64_t now_ms, std::string& why);
    /// A citizen's ranked vote (the election): `ranked` are candidates' character ids, best first (unknown ones and repeats are dropped; empty: refused).
    bool vote(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why);
    /// The mayor's message (at most 512 bytes).
    bool set_message(int64_t node, int64_t account, const std::string& message, std::string& why);
    /// The node's mayor is `who` now (a game master's); account 0 removes the mayor.
    void set_mayor(int64_t node, const ElectionPerson& who, int64_t now_ms);

    bool is_candidate(int64_t node, int64_t account) const;
    /// The node whose mayor the account is (0: none).
    int64_t mayor_node(int64_t account) const;

    /// The instant runoff over `e`'s votes: the winner's index in e.candidates, nullopt for no candidate.
    static std::optional<size_t> tally(const NodeElection& e);

    /// Bumped by every change: readers that copied something compare it.
    uint64_t version() const { return version_; }

    std::string to_json() const;
    /// Replaces the store with `json`'s (what to_json wrote).  False with `error`, nothing changed.
    bool from_json(const std::string& json, std::string& error);

private:
    void enter(int64_t node, NodeElection& e, ElectionState s, int64_t start_ms, std::vector<Change>& out);
    ElectionRules                   rules_;
    std::map<int64_t, NodeElection> nodes_;
    uint64_t                        version_ = 0;
};

}  // namespace aoc::worldd::world
