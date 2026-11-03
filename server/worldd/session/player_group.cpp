// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// session/player_group.cpp
// A session's end of session/group.h: the client's calls go in, what the group owes this player comes out.

#include "session/player_session.h"
#include "session/player_session_util.h"
#include "session/gm_travel.h"

#include <chrono>

#include <spdlog/spdlog.h>

namespace aoc::worldd {

namespace {
/// Vitals go out this often at most; what changes the group itself goes at once.  EGameConstant has ASHES_GROUP_UPDATE_RATE, whose value is not read: modelled.
constexpr std::chrono::seconds kGroupVitalsEvery{1};

double group_clock(Clock::time_point now) { return std::chrono::duration<double>(now.time_since_epoch()).count(); }
}  // namespace

void PlayerSession::connect_group(PlayerWorld& world) {
    group_book_ = world.group_book();
    group_member_ = group_book_->connect(name(), pb_.setup().character_guid, static_cast<uint8_t>(class_id_));
    if (!group_book_->in_group(pb_.setup().character_guid)) group_member_->view_dirty = false;   // the login's own empty group says it
}

void PlayerSession::leave_group_world(Clock::time_point now) {
    if (group_book_ && group_member_) group_book_->disconnect(*group_member_, group_clock(now));
    group_member_.reset();
}

bool PlayerSession::on_group_rpc(Connection& conn, const repl::ClientRpc& rpc, std::string_view what, Clock::time_point now) {
    enum class Call { Invite, Accept, Decline, Busy, Leave, Kick, Promote, Disband, ToRaid, ToParty, Move, Shouts, Privilege, PartyLoot, RaidLoot, OverrideLoot, MasterLooter, Summon, Ignore, Unignore, IgnoreList };
    static const std::unordered_map<std::string_view, Call> kCalls = {
        {"ServerInviteToGroup", Call::Invite}, {"ServerAcceptJoinGroup", Call::Accept}, {"ServerDeclineJoinGroup", Call::Decline},
        {"ServerNotifyInviterOfPendingGroupInvitation", Call::Busy}, {"ServerLeaveGroup", Call::Leave}, {"ServerKickFromGroup", Call::Kick},
        {"ServerTransferGroupLeadership", Call::Promote}, {"ServerDisbandGroup", Call::Disband}, {"ServerConvertPartyToRaid", Call::ToRaid},
        {"ServerConvertRaidToParty", Call::ToParty}, {"ServerMoveRaidMember", Call::Move}, {"ServerSetPartyEnableMemberShouts", Call::Shouts},
        {"ServerSetRaidShoutPrivilege", Call::Privilege}, {"ServerTrySetPartyLootAllocationMethods", Call::PartyLoot},
        {"ServerTrySetRaidLootAllocationMethods", Call::RaidLoot}, {"ServerSetOverridePartyLootSettings", Call::OverrideLoot},
        {"ServerSetGroupMasterLooter", Call::MasterLooter}, {"ServerSummonParty", Call::Summon}, {"ServerSummonRaid", Call::Summon},
        {"ServerIgnore", Call::Ignore}, {"ServerIgnoreCharacter", Call::Ignore}, {"ServerIgnoreRemove", Call::Unignore}, {"ServerRemoveIgnoreCharacter", Call::Unignore},
        {"ServerIgnoreList", Call::IgnoreList},
    };
    const auto call = kCalls.find(what);
    if (call == kCalls.end()) return false;
    if (!group_book_ || !group_member_ || !within_budget(group_budget_, "group request", now)) return true;
    if (!rpc.call.decoded) {
        spdlog::log(level_for(group_logged_), "[group] {}: {} does not read ({} bits)", name(), what, rpc.call.param_bits);
        return true;
    }
    auto text = [&](std::string_view path) {   // a string left empty is not on the wire
        const auto* s = repl::rpcf::as<std::string>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path));
        return s ? *s : std::string();
    };
    auto named_group = [&] {
        group::Guid g{};
        static constexpr const char* kWords[] = {"GroupGuid.A", "GroupGuid.B", "GroupGuid.C", "GroupGuid.D"};
        for (size_t i = 0; i < 4; ++i) {
            const auto* v = repl::rpcf::as<int64_t>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, kWords[i]));
            g[i] = v ? static_cast<uint32_t>(*v) : 0;
        }
        return g;
    };
    auto methods_named = [&] {
        group::LootMethods m{};
        size_t at = 0;
        for (const char* path : {"Methods.DefaultLootAllocationMethod", "Methods.CommonLootAllocationMethod", "Methods.UncommonLootAllocationMethod", "Methods.RareLootAllocationMethod",
                                 "Methods.HeroicLootAllocationMethod", "Methods.EpicLootAllocationMethod", "Methods.LegendaryLootAllocationMethod", "Methods.ArtifactLootAllocationMethod"}) {
            const auto* v = repl::rpcf::as<uint64_t>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path));
            m[at++] = v && *v < 256 ? static_cast<uint8_t>(*v) : uint8_t{255};
        }
        return m;
    };
    group::Book& book = *group_book_;
    group::Member& me = *group_member_;
    std::string refusal;
    switch (call->second) {
        case Call::Invite: refusal = book.invite(me, text("CharacterName"), group_clock(now)); break;
        case Call::Accept: refusal = book.accept(me, named_group()); break;
        case Call::Decline: book.decline(me, named_group()); break;
        case Call::Busy: book.tell_busy(me, text("InviterName")); break;
        case Call::Leave: refusal = book.leave(me); break;
        case Call::Kick: refusal = book.kick(me, text("CharacterNameToKick")); break;
        case Call::Promote: refusal = book.promote(me, text("CharacterNameToPromote")); break;
        case Call::Disband: refusal = book.disband(me); break;
        case Call::ToRaid: refusal = book.convert_to_raid(me); break;
        case Call::ToParty: refusal = book.convert_to_party(me); break;
        case Call::Move: {
            const auto* to = repl::rpcf::as<int64_t>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "NewSquadNum"));
            refusal = book.move_to_squad(me, text("CharacterName"), to ? static_cast<int32_t>(*to) : 0);
            break;
        }
        case Call::Shouts: {   // (a bool that is false is not on the wire)
            const auto* on = repl::rpcf::as<bool>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "bEnableMemberShouts"));
            refusal = book.set_member_shouts(me, on && *on);
            break;
        }
        case Call::Ignore: {
            const std::string who = what == "ServerIgnore" ? text("PlayerName") : text("InCharacterName");
            if (!chat_room_ || !chat_peer_) break;
            refusal = chat_room_->ignore(*chat_peer_, who);
            if (refusal.empty()) {
                me.status.push_back("You now ignore " + who + ".");
                show_ignore_list(conn);
                progress_changed_ = true;   // the list is saved with the progress, at once
                hand_over_progress("the ignore list");
            }
            break;
        }
        case Call::Unignore: {
            const std::string who = what == "ServerIgnoreRemove" ? text("PlayerName") : text("InCharacterName");
            if (chat_room_ && chat_peer_) {
                const bool was = chat_room_->unignore(*chat_peer_, who);
                me.status.push_back(was ? "You no longer ignore " + who + "." : who + " is not on your ignore list.");
                if (was) {
                    show_ignore_list(conn);
                    progress_changed_ = true;
                    hand_over_progress("the ignore list");
                }
            }
            break;
        }
        case Call::IgnoreList: {
            if (!chat_room_ || !chat_peer_) break;
            std::string line = "You ignore:";
            const auto list = chat_room_->ignored_by(*chat_peer_);
            for (const std::string& n : list) line += " " + n;
            me.status.push_back(list.empty() ? "You ignore nobody." : line);
            break;
        }
        case Call::PartyLoot: refusal = book.set_party_loot(me, methods_named()); break;
        case Call::RaidLoot: refusal = book.set_raid_loot(me, methods_named()); break;
        case Call::OverrideLoot: {   // (a bool that is false is not on the wire)
            const auto* on = repl::rpcf::as<bool>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "bOverridePartyLootSettings"));
            refusal = book.set_override_party_loot(me, on && *on);
            break;
        }
        case Call::MasterLooter: refusal = book.set_master_looter(me, text("MasterLooterName")); break;
        case Call::Summon: {
            // The client's cheat manager holds SummonParty and SummonRaid, so this is a game master's: modelled.
            auto* gm = world_ ? dynamic_cast<GmTravel*>(world_->gm_port()) : nullptr;
            const group::View view = book.view(me);
            if (!gm_enabled() || !gm) refusal = "Only a game master can summon a group.";
            else if (view.guid == group::kNoGuid) refusal = "You are not in a group.";
            else {
                const auto at = checker_.good();
                size_t moved = 0;
                for (const group::Person& p : view.people) {
                    if (p.guid == me.guid || !p.online) continue;
                    GmTeleportRequest r;
                    r.character = p.name;
                    r.x = at.x;
                    r.y = at.y;
                    r.z = at.z;
                    r.has_z = true;
                    moved += gm->teleport(r, name()).status == GmStatus::Ok ? 1 : 0;
                }
                me.status.push_back("Summoned " + std::to_string(moved) + " member(s).");
            }
            break;
        }
        case Call::Privilege: {
            const auto* v = repl::rpcf::as<uint64_t>(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "ShoutPrivilege"));
            refusal = book.set_shout_privilege(me, v ? static_cast<uint8_t>(*v) : group::kShoutAll);
            break;
        }
    }
    spdlog::log(level_for(group_logged_, 20), "[group] {}: {} {}", name(), what, refusal.empty() ? std::string("accepted") : "refused: " + refusal);
    if (!refusal.empty()) me.status.push_back(std::move(refusal));
    run_group(conn, now);
    return true;
}

void PlayerSession::run_group(Connection& conn, Clock::time_point now) {
    if (!group_book_ || !group_member_ || finished_ || failed_) return;
    group::Member& me = *group_member_;
    if (now >= next_group_tick_) {
        next_group_tick_ = now + kGroupVitalsEvery;
        group_book_->expire(group_clock(now));
        group::Vitals v;
        const auto value = [&](int64_t guid) { const stats::StatValue* s = stats_ ? stats_->find(guid) : nullptr; return s ? s->value : 0.0f; };
        v.max_health = value(content::kGuidMaxHealth);
        v.max_mana = value(content::kGuidMaxMana);
        v.health_percent = v.max_health > 0 ? value(content::kGuidHealth) / v.max_health : 0.0f;
        v.mana_percent = v.max_mana > 0 ? value(content::kGuidMana) / v.max_mana : 0.0f;
        v.level = progress_.level;
        v.dead = dead_;
        const auto at = checker_.good();
        v.x = at.x; v.y = at.y; v.z = at.z;
        v.heading = static_cast<float>(at.yaw_degrees);
        if (map_marker_) v.marker = std::array<double, 3>{map_marker_->x, map_marker_->y, map_marker_->z};
        group_book_->publish(me, v);
        group_vitals_due_ = me.vitals_dirty;
    }
    if (!join_.empty()) return;   // the login's own frames first
    if (me.invitation && room_for(1)) {
        const group::Invitation invitation = *me.invitation;
        me.invitation.reset();
        queue("ClientRequestJoinGroup", [invitation](bootstrap::PlayerBootstrap& pb) { return pb.group_invitation(invitation); }, conn);
    }
    if (me.convert_prompt && room_for(1)) {
        const std::string invited = *me.convert_prompt;
        me.convert_prompt.reset();
        queue("ClientRequestConvertToRaid", [invited](bootstrap::PlayerBootstrap& pb) { return pb.group_convert_prompt(invited); }, conn);
    }
    if ((me.view_dirty || group_vitals_due_) && room_for(1)) {
        me.view_dirty = me.vitals_dirty = group_vitals_due_ = false;
        const group::View view = group_book_->view(me);
        queue("ClientUpdateGroup", [view](bootstrap::PlayerBootstrap& pb) { return pb.group_update(view); }, conn);
    }
    for (size_t i = 0; i < 4 && !me.status.empty() && room_for(1); ++i) {
        const std::string line = std::move(me.status.front());
        me.status.erase(me.status.begin());
        queue("group status", [line](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(line, group::kGroupInfo); }, conn);
    }
    for (size_t i = 0; i < 4 && !me.chat.empty() && room_for(1); ++i) {
        const chat::Message message = std::move(me.chat.front());
        me.chat.erase(me.chat.begin());
        queue("ClientReceiveChatMessages", [message](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(message.text, message.category, message.from); }, conn, /*must_build=*/true);
    }
}

}  // namespace aoc::worldd
