// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/resurrect.inc: the members of PlayerSession for another player's Resurrect (respawn/session_embersprings.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    void on_resurrect_response(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    bool resurrect_offered_ = false;   // another player's Resurrect landed, and the client's alert has not answered
