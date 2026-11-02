// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/group.inc: the members of PlayerSession for parties and raids (session/player_group.cpp, group.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// Tests: the shared book of parties and raids.
    std::shared_ptr<group::Book> group_book_for_test() const { return group_book_; }
private:
    void connect_group(PlayerWorld& world);
    void leave_group_world(Clock::time_point now);
    /// True when the call is one of the group's.
    bool on_group_rpc(Connection& conn, const protocol::repl::ClientRpc& rpc, std::string_view what, Clock::time_point now);
    void run_group(Connection& conn, Clock::time_point now);
    std::shared_ptr<group::Book> group_book_;
    std::shared_ptr<group::Member> group_member_;
    RequestBudget group_budget_{8, std::chrono::milliseconds(500)};
    size_t group_logged_ = 0;
    Clock::time_point next_group_tick_{};
    bool group_vitals_due_ = false;
    crowd::State crowd_sent_{};   // what the client was last told of CrowdControlState
