// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/ground_crate.inc: the members of PlayerSession for the crates on the ground (session/player_ground_crate.cpp, items/ground_crates.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// Drop Crate's notify: the crate on the back is laid on the ground in front of the character.  False, with the crate still on the back, when the world takes no crate (the caller
    /// then puts it in the item bag, as before the crates on the ground).
    bool lay_crate_down(Connection& conn, Clock::time_point now);
    /// A use of a crate on the ground: it is picked up after the interaction's channel (ICD_CargoActor's second), when the cargo slot is free.
    void on_crate_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now);
    /// Every tick: the pick-up that waits for its channel, and (every few seconds) the character's own crates whose time on the ground is over, back to the cargo slot when it is free.
    void run_ground_crates(Connection& conn, Clock::time_point now);
    /// The crate of `actor` goes into the cargo slot (`why` for the log; `answer`: the crate answers a use, ClientOnDoInteract).  False when it is gone or does not fit (it then lies where it
    /// was).
    bool pick_up_crate(Connection& conn, uint64_t actor, const char* why, bool answer, Clock::time_point now);
    uint64_t          crate_pick_actor_ = 0;          // a pick-up runs: the crate's actor (0: none)
    Clock::time_point crate_pick_end_{};
    double            crate_pick_x_ = 0, crate_pick_y_ = 0;
    Clock::time_point crate_back_check_{};
    uint32_t          crates_laid_ = 0, crates_picked_ = 0;
public:
    uint32_t crates_laid() const { return crates_laid_; }
    uint32_t crates_picked() const { return crates_picked_; }
    /// A test's Drop Crate notify (the cast itself is the ability tests').
    void drop_crate_for_test(Connection& conn, Clock::time_point now) { drop_crate(conn, now); }
private:
