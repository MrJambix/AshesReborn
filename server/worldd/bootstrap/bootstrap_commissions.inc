// SPDX-License-Identifier: AGPL-3.0-only
// Included inside namespace aoc::worldd::bootstrap in bootstrap.cpp.
// The commission boards on the wire (session/player_commissions.cpp): the PlayerController's CurrentCommissionBoard, an FCommissionBoardList with its own
// NetSerialize (protocol/repl/quest_codec.h), whose OnRep (0x145e33f70) fills the board's window, and ClientOpenCommissionBoardDialog (no parameters, 0x145e20f10),
// which opens it.

std::optional<Send> PlayerBootstrap::commission_board_update(const repl::CommissionBoardList& board) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    if (!pc_.obj.state.set(*pc_.obj.schema, "CurrentCommissionBoard", Value(board))) return std::nullopt;
    return update(Role::PlayerController, "commission board", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::open_commission_board() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    if (!pc_.obj.schema->rpc("ClientOpenCommissionBoardDialog")) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientOpenCommissionBoardDialog";
    return rpcs(Role::PlayerController, "ClientOpenCommissionBoardDialog", std::span<const repl::RpcRequest>(&call, 1));
}
