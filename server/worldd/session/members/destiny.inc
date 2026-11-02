// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/destiny.inc: the members of PlayerSession for the Destiny board (quest/session_destiny.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// Whether the Destiny tracks start by themselves (a test that counts the quests of its own turns this off).
    void set_destiny_board(bool on) { destiny_board_ = on; }

private:
    // ---- the Destiny board (quest/session_destiny.cpp)
    void start_destiny_tracks(Connection& conn, Clock::time_point now);
    void on_destiny_claim(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    protocol::repl::RewardInstance reward_preview(const std::vector<int64_t>& tables);
    void on_destiny_client_data(Connection& conn, Clock::time_point now);
    void on_destiny_milestones(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// A quest event noted where it happens and counted at the next quest tick, so that a burst of them does not run the engine once each.
    void note_quest_event(std::function<void(quest::Engine&)> event) {
        if (quests_on_ && quest_events_.size() < 256) quest_events_.push_back(std::move(event));
    }
    /// The EnteringNewNode objectives: called once a second; the first node of a session is where the character already is, not one it entered.
    void note_node_entered() {
        const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
        if (!nodes) return;
        const GoodPosition& at = checker_.good();
        const int node = nodes->node_at(at.x, at.y).node_rank;
        if (node == last_node_rank_) return;
        const bool entered = last_node_rank_ != kNoNodeSeen && node >= 0;
        last_node_rank_ = node;
        if (entered) note_quest_event([](quest::Engine& eng) { eng.notified("EnteringNewNode", 0); });
    }
    static constexpr int kNoNodeSeen = -2;
    int last_node_rank_ = kNoNodeSeen;
    std::vector<std::function<void(quest::Engine&)>> quest_events_;
    bool destiny_started_ = false;
    bool destiny_board_ = true;
