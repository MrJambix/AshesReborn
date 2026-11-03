// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/mount_command.inc: the members of PlayerSession for the `.mount` chat command (session/player_mounts.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// The `.mount` chat command: lists the bag's mounts, summons one by its number or name (the mayor flyers, whose use the client never sends), or dismisses
    /// the one that is out (`off`).  Returns the reply for the player.
    std::string on_mount_command(Connection& conn, const std::string& args, Clock::time_point now);
