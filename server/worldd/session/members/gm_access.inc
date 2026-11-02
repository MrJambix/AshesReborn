// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/gm_access.inc: the members of PlayerSession for GM access: the account's grant, GM mode and the GM tag (session/player_gm.cpp, session/gm_commands.inc)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    int64_t account_id() const { return account_id_; }
    int gm_level() const { return gm_level_; }
    bool gm_enabled() const { return gm_level_ == 3 && gm_enabled_; }
    bool gm_rpc_admitted_for_panel(Clock::time_point now) { return gm_level_ == 3 && gm_budget_.take(now); }
    uint64_t gm_panel_revision() const { return gm_panel_revision_; }
    bool set_gm_enabled(bool enabled) {
        if (gm_level_ != 3) return false;
        gm_enabled_ = enabled;
        if (enabled) ++gm_panel_revision_;
        return true;
    }
    /// Shows or hides the GM tag (BaseCharacterInfo.bShowGMTag) on a verified GM's pawn: .GM On and .GM Off, and the client's ServerShowGMTag.  A new session starts without it.
    void          gm_show_tag(Connection& conn, bool show, Clock::time_point now);
    bool          gm_tag_shown() const { return gm_tag_; }

private:
    int64_t account_id_ = 0;
    int gm_level_ = 0;
    bool gm_enabled_ = false;
    bool gm_tag_ = false;
    uint64_t gm_panel_revision_ = 0;
    /// ServerShowGMTag (925, bEnable) on the PlayerController: logged whatever the switch says; answered only with GM mode on.
    void on_show_gm_tag(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
