// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/consumables.inc: the members of PlayerSession for consumables: item-bearing ability requests (session/player_consumables.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    bool prepare_consumable(Connection& conn, int64_t ability, int32_t instance, int slot, uint32_t location,
                            const protocol::repl::ItemInstance& claimed, TriggerInfo& info, Clock::time_point now);
    /// The abilities of the items carried (bootstrap::carried_item_abilities): asked every tick, so each record's cast is read from content.db once.
    std::vector<int64_t> carried_abilities();
    std::map<int64_t, int64_t> item_casts_;
    std::deque<items::Outcome> pending_consumables_;
