// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/mayor_commissions.inc: the members of PlayerSession for the mayor's commissions (session/player_mayor_commissions.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    void on_mayor_hand_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_commission_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    void on_mayor_commission_activate(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// The console's commission calls (ServerGMAddCommissionToMayorHand, ...AddAndStartMayoralCommission, ...ClearMayoralCommissions(Cooldowns),
    /// ...AddParticipationCompletionTo(Nodes)MayoralCommission(s), ...Update(All)MayoralCommission(s)ExpirationTime, ServerRerollMayoralCommissions): GM only.
    void on_gm_mayor_commissions(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    /// ClientReceiveMayoralCommissionHand: the node's hand (filled up first), its running and cooling commissions and its slots.
    void send_mayor_hand(Connection& conn, int64_t node, Clock::time_point now);
    /// The use of the mayor's board of a Town Hall (CommissionBoard_NodeMayoral; its response is OpenMayoralCommissionBoard): the mayoral commissions of the node it stands in.
    void open_mayoral_board(Connection& conn, const world::WorldActor& board, Clock::time_point now);
    /// ClientReceiveMayoralCommissions: the node's running commissions (the client opens its board window with them).  With none running nothing is sent, unless `even_empty`
    /// (the mayor's own board, which opens its window with an empty list).
    void send_mayor_board(Connection& conn, int64_t node, Clock::time_point now, bool even_empty = false);
    /// A claimed commission quest that a node's mayoral commission gave counts for it (quest/session_destiny.cpp).
    void note_mayor_commission_done(const quest::QuestInstance& q);
    /// A wall-clock time as the GameState's world seconds (the clock of the commissions' times on the wire).
    int64_t to_world_seconds(int64_t wall_at_ms, Clock::time_point now);
    world::LevelGrants node_grants(int64_t node) const;
    std::string mayor_node_key(int64_t node) const;
    protocol::repl::RewardInstance mayor_commission_reward(int64_t record);
    /// The node a game master's call means: the one it names, else the one the player stands in (0: none).
    int64_t gm_commission_node(int64_t asked) const;
    int64_t  mayor_board_node_ = 0;   // the node whose commissions the board this session opened last lists
    uint32_t mayor_board_opens_ = 0, mayor_hands_sent_ = 0, mayor_boards_sent_ = 0, mayor_purchases_ = 0, mayor_purchases_refused_ = 0, mayor_activations_ = 0, mayor_activations_refused_ = 0,
             mayor_completions_ = 0;
public:
    uint32_t mayor_hands_sent() const { return mayor_hands_sent_; }
    uint32_t mayor_boards_sent() const { return mayor_boards_sent_; }
    uint32_t mayor_board_opens() const { return mayor_board_opens_; }
    /// A test: the use of the mayor's board `board` (as finish_prop_use does for an object whose interaction is Mayoral_Commission_Board_PID).
    void open_mayoral_board_for_test(Connection& conn, const world::WorldActor& board, Clock::time_point now) { open_mayoral_board(conn, board, now); }
    uint32_t mayor_purchases() const { return mayor_purchases_; }
    uint32_t mayor_purchases_refused() const { return mayor_purchases_refused_; }
    uint32_t mayor_activations() const { return mayor_activations_; }
    uint32_t mayor_activations_refused() const { return mayor_activations_refused_; }
    uint32_t mayor_completions() const { return mayor_completions_; }
    /// A test: the hand and running slots of every node (a test's world has no node layouts to read them from).
    void set_mayor_slots_for_test(int hand, int slots) { mayor_slots_for_test_ = std::make_pair(hand, slots); }
    /// A test: the board of `node` opens (as open_commission_board does for a town board).
    void send_mayor_board_for_test(Connection& conn, int64_t node, Clock::time_point now, bool even_empty = false) { send_mayor_board(conn, node, now, even_empty); }
    /// A test: the claim of the commission quest `q` (as session_destiny.cpp does).
    void note_mayor_commission_done_for_test(const quest::QuestInstance& q) { note_mayor_commission_done(q); }
private:
    std::optional<std::pair<int, int>> mayor_slots_for_test_;
