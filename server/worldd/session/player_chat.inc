// SPDX-License-Identifier: AGPL-3.0-only
// Included inside namespace aoc::worldd in player_session.cpp.

void PlayerSession::on_chat(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!chat_peer_ || !chat_room_ || !within_budget(chat_budget_, "SendChatMessage", now)) return;
    if (!rpc.call.decoded) {
        spdlog::log(level_for(chat_logged_), "[chat] {}: SendChatMessage parameters could not be decoded ({} bits)", name(), rpc.call.param_bits);
        return;
    }
    auto value = [&](std::string_view path) -> const repl::Value* {
        const auto* command = repl::rpc_command(*rpc.call.rpc, path);
        if (!command) return nullptr;
        const auto it = rpc.call.params.values.find(command->handle);
        return it == rpc.call.params.values.end() ? nullptr : &it->second;
    };
    const auto* quote_value = value("ChatData.Quote");
    const auto* quote = quote_value ? quote_value->get_if<std::string>() : nullptr;
    const auto* category_value = value("ChatData.ChatCategory");
    const auto* category = category_value ? category_value->get_if<uint64_t>() : nullptr;
    const auto* target_value = value("ChatData.TargetName");
    const auto* target = target_value ? target_value->get_if<std::string>() : nullptr;
    if (!quote || !category || *category > 255) return;
    // Dot commands are private to their authenticated sender, in every channel.
    std::istringstream gm_input(*quote);
    std::string gm_prefix;
    gm_input >> gm_prefix;
    if (chat::same_name(gm_prefix, ".gm")) {
        auto* gm = world_ ? dynamic_cast<GmTravel*>(world_->gm_port()) : nullptr;
        const std::string reply = gm ? gm->command(*this, conn, *quote).message : "GM tools are unavailable.";
        std::istringstream lines(reply);
        for (std::string line; std::getline(lines, line) && chat_peer_->inbox.size() < chat::kMaxInbox;) {
            if (!line.empty()) chat_peer_->inbox.push_back({line, "GM", 4});
        }
        run_chat(conn);
        return;
    }
    const auto at = checker_.good();
    chat_peer_->x = at.x; chat_peer_->y = at.y; chat_peer_->z = at.z;
    // SubjectName, location, m_bIsSelf, GM flags and emblems supplied by the client
    // are deliberately ignored. Routing and the displayed sender come from the session.
    const auto error = chat_room_->send(*chat_peer_, *quote, static_cast<uint8_t>(*category), target ? *target : std::string());
    if (!error.empty() && room_for(1)) {
        queue("chat status", [error](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(error, 4); }, conn);
    } else if (error.empty()) {
        spdlog::log(level_for(chat_logged_, 10), "[chat] {}: accepted category {}, {} bytes", name(), *category, quote->size());
    }
    run_chat(conn);
}

void PlayerSession::run_chat(Connection& conn) {
    if (!chat_peer_ || finished_ || failed_) return;
    const auto at = checker_.good();
    chat_peer_->x = at.x; chat_peer_->y = at.y; chat_peer_->z = at.z;
    // Wait for the join before building ClientReceiveChatMessages. Drain at most
    // four lines per tick so chat cannot starve other reliable replies.
    if (!join_.empty()) return;
    for (size_t i = 0; i < 4 && !chat_peer_->inbox.empty() && room_for(1); ++i) {
        auto message = std::move(chat_peer_->inbox.front());
        chat_peer_->inbox.pop_front();
        queue("ClientReceiveChatMessages", [message](bootstrap::PlayerBootstrap& pb) {
            return pb.chat_line(message.text, message.category, message.from);
        }, conn, /*must_build=*/true);
    }
}
