// SPDX-License-Identifier: AGPL-3.0-only
// session/player_guild.cpp
// A session's end of session/guild.h: the calls on the pawn's GuildComponent go in, what the guild owes this player comes out.

#include "session/player_session.h"
#include "session/player_session_util.h"

#include <chrono>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace aoc::worldd {

namespace {
constexpr std::chrono::seconds kGuildTickEvery{1};
constexpr std::chrono::seconds kGuildExperienceEvery{30};
/// How long after the join the component opens: the join's frames stay retail's, and nobody reaches the guild window sooner.
constexpr std::chrono::seconds kGuildOpenAfterJoin{2};

/// Guilds keep real dates, which outlive a session and the server.
double guild_clock() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

guild::Guid guid_in(const repl::Value& a, const repl::Value& b, const repl::Value& c, const repl::Value& d) {
    const auto word = [](const repl::Value& v) { const auto* i = v.get_if<int64_t>(); return i ? static_cast<uint32_t>(*i) : 0u; };
    return {word(a), word(b), word(c), word(d)};
}
}  // namespace

void PlayerSession::connect_guild(PlayerWorld& world) {
    if (!world.guilds_enabled()) return;   // no book and no member: run_guild does nothing, so the component never opens
    guild_book_ = world.guild_book();
    guild_member_ = guild_book_->connect(name(), pb_.setup().character_guid, static_cast<uint8_t>(progress_.level), static_cast<uint8_t>(class_id_), guild_clock());
    guild_component_ = world.mint_guid();
}

void PlayerSession::leave_guild_world() {
    if (guild_book_ && guild_member_) guild_book_->disconnect(*guild_member_, guild_clock());
    guild_member_.reset();
}

void PlayerSession::open_guild_panel(Connection& conn) {
    if (!guild_book_) {
        if (chat_peer_ && chat_peer_->inbox.size() < chat::kMaxInbox) chat_peer_->inbox.push_back({"Guilds are switched off for now.", "", 4});
        return;
    }
    if (guild_book_ && guild_book_->in_guild(pb_.setup().character_guid)) {
        if (guild_member_) guild_member_->replies.push_back({guild::kReqCreate, "You are already in a guild.", false});
        return;
    }
    if (!room_for(1)) return;
    queue("ClientOpenCreateGuildPanel", [](bootstrap::PlayerBootstrap& pb) { return pb.open_create_guild_panel(); }, conn);
}

bool PlayerSession::on_guild_rpc(Connection& conn, const repl::ClientRpc& rpc, std::string_view what, Clock::time_point now) {
    enum class Call { Create, Invite, Accept, Decline, Leave, Disband, Kick, Leader, Ranks, Hierarchy, Say, Refresh, List, Quiet };
    static const std::unordered_map<std::string_view, Call> kCalls = {
        {"ServerCreateGuild", Call::Create}, {"ServerInvitePlayerToGuild", Call::Invite}, {"ServerAcceptInviteToGuild", Call::Accept},
        {"ServerDeclineInviteToGuild", Call::Decline}, {"ServerLeaveGuild", Call::Leave}, {"ServerDisbandGuild", Call::Disband},
        {"ServerKickPlayerFromGuild", Call::Kick}, {"ServerAssignGuildLeader", Call::Leader}, {"ServerReassignMemberRanks", Call::Ranks},
        {"ServerUpdateRankHierarchy", Call::Hierarchy}, {"ServerSendMessageToGuild", Call::Say}, {"ServerGetUpdatedGuildData", Call::Refresh},
        {"ServerGetUpdatedGuildsList", Call::List}, {"ServerRemoveGuildDataListener", Call::Quiet},
    };
    const auto call = kCalls.find(what);
    if (call == kCalls.end()) return false;   // wars, skills, storage and the GM calls: dispatch logs them once
    if (!guild_book_ || !guild_member_ || !within_budget(guild_budget_, "guild request", now)) return true;
    if (!rpc.call.decoded) {
        spdlog::log(level_for(guild_logged_), "[guild] {}: {} does not read ({} bits)", name(), what, rpc.call.param_bits);
        return true;
    }
    const auto param = [&](std::string_view path) { return repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path); };
    const auto text = [&](std::string_view path) {   // a string left empty is not on the wire
        const auto* s = repl::rpcf::as<std::string>(param(path));
        return s ? *s : std::string();
    };
    const auto guid_param = [&](const std::string& base) {
        guild::Guid g{};
        static constexpr const char* kWords[] = {".A", ".B", ".C", ".D"};
        for (size_t i = 0; i < 4; ++i) {
            const auto* v = repl::rpcf::as<int64_t>(param(base + kWords[i]));
            g[i] = v ? static_cast<uint32_t>(*v) : 0;
        }
        return g;
    };
    const double clock = guild_clock();
    guild::Book& book = *guild_book_;
    guild::Member& me = *guild_member_;
    switch (call->second) {
        case Call::Create: book.create(me, text("InGuildName"), clock); break;
        case Call::Invite: book.invite(me, text("InviteeName"), clock); break;
        case Call::Accept: book.accept(me, clock); break;
        case Call::Decline: book.decline(me); break;
        case Call::Leave: book.leave(me, clock); break;
        case Call::Disband: book.disband(me, clock); break;
        case Call::Kick: book.kick(me, guid_param("MemberGuidToKick"), clock); break;
        case Call::Leader: book.assign_leader(me, guid_param("TargetCharacterGuid")); break;
        case Call::Ranks: {
            std::vector<std::pair<guild::Guid, uint8_t>> changes;
            if (const auto* list = repl::rpcf::as<repl::ArrayValue>(param("MemberRanksToUpdate"))) {
                for (const repl::ElementState& e : list->elements) {
                    const auto at = [&](uint16_t h) { const auto it = e.values.find(h); return it == e.values.end() ? repl::Value::i64(0) : it->second; };
                    const auto rank = at(5);
                    const auto* r = rank.get_if<uint64_t>();
                    changes.emplace_back(guid_in(at(1), at(2), at(3), at(4)), r ? static_cast<uint8_t>(*r) : uint8_t{0});
                }
            }
            book.set_ranks(me, changes);
            break;
        }
        case Call::Hierarchy: {
            std::vector<guild::Rank> ranks;
            if (const auto* list = repl::rpcf::as<repl::ArrayValue>(param("UpdatedRankData"))) {
                for (const repl::ElementState& e : list->elements) {
                    guild::Rank r;
                    if (const auto it = e.values.find(1); it != e.values.end()) {
                        if (const auto* s = it->second.get_if<std::string>()) r.name = *s;
                    }
                    for (size_t i = 0; i < guild::kPermissions; ++i) {
                        const auto it = e.values.find(static_cast<uint16_t>(i + 2));
                        const auto* b = it == e.values.end() ? nullptr : it->second.get_if<bool>();
                        r.permissions[i] = b && *b;
                    }
                    ranks.push_back(std::move(r));
                }
            }
            book.set_hierarchy(me, std::move(ranks));
            break;
        }
        case Call::Say: {
            const auto* category = repl::rpcf::as<uint64_t>(param("Category"));
            std::string refusal;
            if (category && *category <= 255 && book.say(me, text("Message"), static_cast<uint8_t>(*category), refusal) && !refusal.empty()) {
                me.replies.push_back({0, std::move(refusal), false});
            }
            break;
        }
        case Call::Refresh: me.view_dirty = true; break;
        case Call::List: guild_list_owed_ = true; break;
        case Call::Quiet: break;
    }
    spdlog::log(level_for(guild_logged_, 20), "[guild] {}: {}", name(), what);
    run_guild(conn, now);
    return true;
}

void PlayerSession::run_guild(Connection& conn, Clock::time_point now) {
    if (!guild_book_ || !guild_member_ || finished_ || failed_) return;
    guild::Member& me = *guild_member_;
    if (now >= next_guild_tick_) {
        next_guild_tick_ = now + kGuildTickEvery;
        note_node_entered();
        if (now >= next_guild_experience_flush_) {
            next_guild_experience_flush_ = now + kGuildExperienceEvery;
            guild_book_->flush_experience();
        }
        guild_book_->expire(guild_clock());
        guild_book_->publish(me, static_cast<uint8_t>(progress_.level), static_cast<uint8_t>(class_id_));
    }
    if (!join_.empty()) return;   // the login's own frames first
    // The client cannot call the component before it knows its id.
    if (!guild_opened_) {
        if (guild_open_at_ == Clock::time_point{}) guild_open_at_ = now + kGuildOpenAfterJoin;
        if (now < guild_open_at_ || !room_for(1)) return;
        if (!pb_.open_guild_component(guild_component_)) return;
        guild_opened_ = true;
        me.view_dirty = me.view_dirty || guild_book_->in_guild(pb_.setup().character_guid);
        queue("the guild component", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("guild component"); }, conn);
    }
    if (me.view_dirty && room_for(1)) {
        me.view_dirty = false;
        if (pb_.set_guild_view(guild_book_->view(me, guild_clock()))) {
            queue("the guild's data", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("guild data"); }, conn);
        }
    }
    if (me.invitation && room_for(1)) {
        const guild::Invitation invitation = *me.invitation;
        me.invitation.reset();
        queue("ClientInvitePlayerToGuild", [invitation](bootstrap::PlayerBootstrap& pb) { return pb.guild_invitation(invitation); }, conn);
    }
    if (guild_list_owed_ && room_for(1)) {
        guild_list_owed_ = false;
        const auto list = guild_book_->names();
        queue("ClientGetUpdatedGuildsList", [list](bootstrap::PlayerBootstrap& pb) { return pb.guild_list(list); }, conn);
    }
    for (size_t i = 0; i < 4 && !me.replies.empty() && room_for(1); ++i) {
        const guild::Reply reply = std::move(me.replies.front());
        me.replies.erase(me.replies.begin());
        queue("a guild reply", [reply](bootstrap::PlayerBootstrap& pb) { return pb.guild_reply(reply); }, conn);
    }
    for (size_t i = 0; i < 4 && !me.chat.empty() && room_for(1); ++i) {
        const chat::Message message = std::move(me.chat.front());
        me.chat.erase(me.chat.begin());
        queue("ClientReceiveChatMessages", [message](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(message.text, message.category, message.from); }, conn, /*must_build=*/true);
    }
}

}  // namespace aoc::worldd
