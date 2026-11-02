// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/auto_attack.inc: the members of PlayerSession for the auto attack options saved with the character (session/player_requests.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    std::map<std::string, bool> auto_attack_settings_, auto_attack_updates_;
    bool combat_settings_initialized_ = false;
