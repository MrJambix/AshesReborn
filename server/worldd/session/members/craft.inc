// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/craft.inc: the members of PlayerSession that the crafting feature (session/player_craft.cpp) added after the split
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// The budget of the crafting requests (a start, the end of a craft) and the flag of the "does not read" line of a start. The shops have a budget and a flag of their own:
    /// buying a lot does not slow the crafts down, and a start that does not read is told whatever the shops told before.
    RequestBudget craft_budget_{kVendorBurst, kVendorInterval};
    bool          craft_unreadable_logged_ = false;
    /// A recipe item used from the bag (activation LearnRecipes): the recipes it names that the character does not know yet join the taught list, which the client
    /// reads from OwnerInfo.PlayerKnownRecipeIds and the inventory save keeps, and one of the stack goes. An item whose recipes are all known stays, refused.
    void learn_recipes(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, const content::ItemUse& activation, Clock::time_point now);
