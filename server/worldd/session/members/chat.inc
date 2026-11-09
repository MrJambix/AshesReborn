// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/chat.inc: the members of PlayerSession for chat (session/player_chat.cpp, chat.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    void on_chat(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void run_chat(Connection& conn);
    /// The client's ignore list window: OwnerInfo.IgnoredCharacterNames becomes the names the chat room says the character ignores, sent with the next OwnerInfo update.
    void show_ignore_list(Connection& conn);
    /// The welcome line, once a session and only after the phase-in was built (queued behind it, so it cannot overtake it).
    void send_welcome(Connection& conn);
    bool welcome_sent_ = false;
public:
    /// True once the session's welcome line was queued.
    bool welcome_line_sent() const { return welcome_sent_; }
private:
    std::shared_ptr<chat::Room> chat_room_;
    std::shared_ptr<chat::Peer> chat_peer_;
    RequestBudget chat_budget_{4, std::chrono::milliseconds(750)};
    size_t chat_logged_ = 0;
