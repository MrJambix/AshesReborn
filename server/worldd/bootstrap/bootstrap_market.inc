// SPDX-License-Identifier: AGPL-3.0-only
// Included inside namespace aoc::worldd::bootstrap in bootstrap.cpp.
// The player marketplace on the wire: the PlayerController's ClientOpenMarketplaceUI, ClientReceiveMarketplaceHeaders, ClientReceiveMarketplaceData and the four responses.
// Each is one reliable call of the controller with no state of its own here; protocol/repl/marketplace_rpc.h builds the parameters.

std::optional<Send> PlayerBootstrap::market_call(const char* rpc_name, const std::function<std::optional<repl::RpcParams>(const schema::RpcSchema&)>& build) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc(rpc_name);
    if (!rpc) return std::nullopt;
    auto params = build(*rpc);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = rpc_name;
    call.params = std::move(*params);
    return rpcs(Role::PlayerController, rpc_name, std::span<const repl::RpcRequest>(&call, 1));
}
