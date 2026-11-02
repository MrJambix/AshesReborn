// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/currency.inc: the members of PlayerSession for paying with items and node currency, crate selling (vendor/player_currency.cpp, session/player_vendors.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// Takes a price in Glint (`cost` Glint T1) out of the bags, with any of its kinds and with change: `paid` holds the Remove events and then the Adds of the change.  False with `why` when it
    /// cannot be paid; the caller keeps a copy of the bags and puts it back then, as on any later failure.
    bool pay_glint(int64_t cost, items::Outcome& paid, std::string& why, int64_t tier_guid = items::kGlintTier);
    /// The currency lines of a reward go to the purse (vendor/player_currency.cpp).
    void pay_rolled_currency(Connection& conn, const std::vector<combat::RolledCurrency>& paid, const char* why);
    /// A shopkeeper who buys crates opens the crate window.  False when this NPC buys none.
    bool open_crate_sell(Connection& conn, const world::WorldActor& npc);
    /// The crate on the back goes to the open shop, for what its record is worth there.
    void sell_crate(Connection& conn, const svc::InventoryItem& crate, Clock::time_point now);
    /// What the bags hold of Glint, in Glint T1.
    int64_t glint_held() const;
