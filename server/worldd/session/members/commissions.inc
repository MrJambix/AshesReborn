// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/commissions.inc: the members of PlayerSession for the commission boards (session/player_commissions.cpp, quest/commission_table.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// A use of a commission board (quest/session_objects.cpp finish_prop_use): the board's commissions for this refresh window as CurrentCommissionBoard, then
    /// ClientOpenCommissionBoardDialog.
    void open_commission_board(Connection& conn, const world::WorldActor& board, Clock::time_point now);
    /// The board this session has open (0: none) and what it lists: for a test.
    uint64_t commission_board_actor() const { return commission_actor_; }
    const repl::CommissionBoardList& commission_board() const { return commission_list_; }

private:
    /// ServerNCSBoardAcceptCommission(CommissionGuid): the card's InstanceGuid; the commission is taken as a quest of the log.
    void on_commission_accept(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now);
    uint64_t                  commission_actor_ = 0;   // the board whose window was last opened
    repl::CommissionBoardList commission_list_;        // ... and what it listed
    size_t                    commission_logged_ = 0;
    uint32_t                  commission_opens_ = 0, commission_taken_ = 0, commission_refused_ = 0;
    /// The board cards (their InstanceGuid) whose commission this character finished and claimed: the card shows bCompletedAlready for the rest of its refresh window
    /// (the card's guid is made from the board and the window, so a new window's cards are new).  Not saved.
    std::set<quest::Guid128>  commission_done_;
public:
    /// A test: the character claimed the commission of the card `card`.
    void note_commission_done_for_test(const quest::Guid128& card) { commission_done_.insert(card); }
private:
