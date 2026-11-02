// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_gm.cpp
//
//  GM access: the GM tag of a verified GM's pawn.  (The commands are session/gm_commands.inc; the switch every GM call asks is PlayerSession::gm_rpc_admitted.)
// ============================================================================
#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

void PlayerSession::gm_show_tag(Connection& conn, bool show, Clock::time_point now) {
    if (gm_level_ != 3 || show == gm_tag_) return;
    if (!pb_.set_gm_tag(show)) {
        spdlog::warn("[gm] {}: the pawn does not take BaseCharacterInfo.bShowGMTag: the client is not told", name());
        return;
    }
    gm_tag_ = show;
    // (with no room the state stays set, and the pawn's next update carries it)
    if (room_for(1)) queue_later("BaseCharacterInfo (GM tag)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("GM tag"); }, conn, now, /*must_build=*/false);
    spdlog::info("[gm] {}: the GM tag is {}", name(), show ? "on" : "off");
}

void PlayerSession::on_show_gm_tag(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto show = rpc.call.decoded ? repl::rpcf::bool_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "bEnable"), false) : std::nullopt;
    char params[64];
    std::snprintf(params, sizeof params, "bEnable=%s", !show ? "?; the parameters did not decode" : *show ? "true" : "false");
    if (!gm_rpc_admitted("ServerShowGMTag", params, now) || !show) return;
    gm_show_tag(conn, *show, now);
    ++gm_rpcs_answered_;
}

/// The client's call of the GM tag (session/rpc_table.h).
void PlayerSession::register_gm_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerShowGMTag", [](PlayerSession& s, const RpcContext& c) { s.on_show_gm_tag(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
