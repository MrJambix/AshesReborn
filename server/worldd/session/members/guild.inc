// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/guild.inc: the members of PlayerSession for guilds (session/player_guild.cpp, guild.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// Tests: the shared book of guilds, and the id of this pawn's guild component.
    std::shared_ptr<guild::Book> guild_book_for_test() const { return guild_book_; }
    const protocol::emit::FIntrepidNetworkGUID& guild_component_for_test() const { return guild_component_; }
    std::shared_ptr<guild::Member> guild_member_for_test() const { return guild_member_; }
private:
    void connect_guild(PlayerWorld& world);
    void leave_guild_world();
    /// The create-guild window: the guild scroll's use, and the `.guild` chat command.
    void open_guild_panel(Connection& conn);
    /// True when the call is one of the guild's.
    bool on_guild_rpc(Connection& conn, const protocol::repl::ClientRpc& rpc, std::string_view what, Clock::time_point now);
    void run_guild(Connection& conn, Clock::time_point now);
    std::shared_ptr<guild::Book> guild_book_;
    std::shared_ptr<guild::Member> guild_member_;
    protocol::emit::FIntrepidNetworkGUID guild_component_{};
    RequestBudget guild_budget_{8, std::chrono::milliseconds(500)};
    size_t guild_logged_ = 0;
    Clock::time_point next_guild_tick_{}, guild_open_at_{}, next_guild_experience_flush_{};
    bool guild_opened_ = false, guild_list_owed_ = false;
