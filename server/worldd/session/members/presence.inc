// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/presence.inc: the members of PlayerSession for the player's public actors for the other players (session/player_presence.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    std::unique_ptr<PlayerPresence> presence_;
