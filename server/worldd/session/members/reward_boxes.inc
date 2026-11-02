// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/reward_boxes.inc: the members of PlayerSession for reward boxes (session/player_items.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// A reward box was used: its choose-one window opens.  The box stays until a choice is made.
    void open_reward_box(Connection& conn, const protocol::repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now);
    /// ServerSelectReward: the box goes and the item at the index comes.
    void on_select_reward(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The reward box whose window is open, and what it lists in the window's order.
    struct RewardChoice {
        std::string                   box_guid;
        int                           slot = 0;
        std::vector<svc::InventoryItem> items;
    };
    std::optional<RewardChoice> reward_choice_;
