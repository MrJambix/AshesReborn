// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/fall.inc: the members of PlayerSession for fall damage (session/player_fall.cpp, fall_damage.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// An accepted move of the new-move slot: the landing of a fall hurts.
    void note_fall(Connection& conn, const protocol::repl::NetworkMove& move, Clock::time_point now, bool on_mount);
    fall::Track       fall_;
    Clock::time_point fall_guard_until_{};   // no fall counts before this: the arrival in the world, and a teleport, whose client comes down by itself
