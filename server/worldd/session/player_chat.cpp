// SPDX-License-Identifier: AGPL-3.0-only
#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

void PlayerSession::show_ignore_list(Connection& conn) {
    if (!chat_room_ || !chat_peer_) return;
    const std::vector<std::string> names = chat_room_->ignored_by(*chat_peer_);
    if (!pb_.set_ignored_names(names)) {
        spdlog::log(level_for(chat_logged_), "[chat] {}: the ignore list ({} names) does not fit the player's state: the window is not told", name(), names.size());
        return;
    }
    if (room_for(1)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn);
}

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
    if (chat::same_name(gm_prefix, ".guild")) {   // no NPC of the data sells the guild scroll, so this is how a player founds a guild
        open_guild_panel(conn);
        run_guild(conn, now);
        return;
    }
    if (chat::same_name(gm_prefix, ".mount")) {   // the mayor flyers' item use never reaches the server (the client's ValidateItemUse): this summons any mount of the bag
        std::string rest;
        std::getline(gm_input, rest);
        const std::string reply = on_mount_command(conn, rest, now);
        std::istringstream lines(reply);
        for (std::string line; std::getline(lines, line) && chat_peer_->inbox.size() < chat::kMaxInbox;) {
            if (!line.empty()) chat_peer_->inbox.push_back({line, "", 4});
        }
        run_chat(conn);
        return;
    }
    if (chat::same_name(gm_prefix, ".election")) {   // node citizenship, stage 2: the election of the node the player stands in (a game master moves it on)
        std::string rest;
        std::getline(gm_input, rest);
        const std::string reply = on_election_command(conn, rest);
        std::istringstream lines(reply);
        for (std::string line; std::getline(lines, line) && chat_peer_->inbox.size() < chat::kMaxInbox;) {
            if (!line.empty()) chat_peer_->inbox.push_back({line, "", 4});
        }
        run_chat(conn);
        return;
    }
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
    if (guild_book_ && guild_member_) {
        std::string refusal;
        if (guild_book_->say(*guild_member_, *quote, static_cast<uint8_t>(*category), refusal)) {
            if (!refusal.empty()) guild_member_->replies.push_back({0, std::move(refusal), false});
            run_guild(conn, now);
            return;
        }
    }
    if (group_book_ && group_member_) {
        std::string refusal;
        if (group_book_->say(*group_member_, *quote, static_cast<uint8_t>(*category), refusal)) {
            if (!refusal.empty()) group_member_->status.push_back(std::move(refusal));
            run_group(conn, now);
            return;
        }
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

/// The client calls of this feature (session/rpc_table.h).
void PlayerSession::register_chat_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "SendChatMessage", [](PlayerSession& s, const RpcContext& c) { s.on_chat(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
