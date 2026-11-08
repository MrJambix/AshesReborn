// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_citizenship.cpp
//
//  Node citizenship, stage 1: an account joins one city node at its Node Administrator, pays its dues and can renounce.  The client's side, read from its code:
//
//    * the administrator's line runs the action response AddNodeCitizenship: the server opens the join dialog, ClientShowAddNodeCitizenshipDialog(NodeGuid, CitizenshipCost,
//      CitizenshipDuesDueTime, InteractionNPCGuid, VendorDefRecordGuid).  The dialog checks CitizenshipCost against the dues currency the player holds (the TaxReceipts, the
//      TaxVoucher items: UAddNodeCitizenshipDialog::HandleCurrencyUpdated 0x146740b60); its confirm sends ServerSendCitizenshipApplication(NodeGuid, NumDuesToFulfill)
//      (UDialogueWidget::HandleAddNodeCitizenshipConfirmed 0x14673eac0 -> 0x145d23fc0), its purchase button ServerOpenVendor(InteractionNPCGuid, VendorDefRecordGuid)
//      (0x145d22460), the town hall's shop of the vouchers;
//    * the answer is ClientCitizenshipApplicationSuccessful(NumDuesFulfilled) or ClientCitizenshipApplicationRejection(), then ClientShowCitizenshipToast(NodeGuid, false);
//      the account's node is OwnerInfo's CitizenNodeGuid and every CharactersNodeData entry;
//    * RemoveNodeCitizenship opens ClientShowCitizenshipRenouncementWarning(); its accept sends ServerRemoveNodeCitizen(NodeGuid, PlayerName) with the player's own name
//      (AAoCHUDBase::HandleCitizenshipRenouncementAccepted 0x14665bf00 -> 0x145d22f40), answered with ClientShowCitizenshipToast(NodeGuid, true);
//    * the dues: ServerFulfillCitizenshipDues(DuesRequired) -> ClientCitizenshipDuesFulfillmentSuccessful(NumFulfilled) or ...Rejection();
//    * the window: ServerSetEssentialCitizenshipWindowData() (AAoCPlayerController::RequestCitizenshipWindowData 0x146ae61a0, at most once per the window's update frequency)
//      -> ClientPopulateCitizenshipData(FEssentialCitizenshipWindowData) (protocol/repl/citizenship_codec.h).
//
//  Modelled (no retail recording of any of it is here): the cost (InitialDueCost, world/citizenship.h), the dues schedule, the window's rank, property state and the parts of
//  the window data that are not read yet (sent empty), and the offer's lifetime.  Times on the wire are Unix seconds (the window compares them with the seconds since 1970
//  the client keeps at its receipt, 0x146a9e580).
// ============================================================================
#include "session/player_session_util.h"

#include "items/currency.h"
#include "items/inventory.h"
#include "protocol/repl/citizenship_codec.h"
#include "world/citizenship.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
using protocol::emit::FIntrepidNetworkGUID;

namespace {

/// An integer parameter of a decoded call: 0 for one left at its default (not on the wire); nullopt for an undecoded call or another type.
std::optional<int64_t> int_param(const repl::ClientRpc& rpc, std::string_view path) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path);
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return std::nullopt;
}

std::optional<std::string> str_param(const repl::ClientRpc& rpc, std::string_view path) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path);
    if (!v) return std::string();
    if (const std::string* s = v->get_if<std::string>()) return *s;
    return std::nullopt;
}

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::pair<int64_t, int> PlayerSession::citizenship_node_at(double x, double y) const {
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    if (!nodes) return {0, -1};
    const world::CityNodeInfo* n = nodes->by_rank(nodes->node_at(x, y).node_rank);
    if (!n) return {0, -1};
    return {n->guid, nodes->level_of(n->guid)};
}

void PlayerSession::send_citizenship_call(Connection& conn, const char* rpc, std::function<bool(const schema::RpcSchema&, repl::RpcParams&)> fill) {
    if (!room_for(1)) return;
    queue(
        rpc,
        [rpc, fill = std::move(fill)](bootstrap::PlayerBootstrap& pb) {
            return pb.market_call(rpc, [&](const schema::RpcSchema& s) -> std::optional<repl::RpcParams> {
                repl::RpcParams p;
                if (!fill(s, p)) return std::nullopt;
                return p;
            });
        },
        conn, /*must_build=*/true);
}

bool PlayerSession::pay_tax_receipts(Connection& conn, int32_t cost, std::string& why) {
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const items::CurrencyTier* tier = world_ && world_->currencies() && rules ? world_->currencies()->tier(rules->dues_tier) : nullptr;
    if (!tier) {
        why = "content.db has no TaxReceipts currency";
        return false;
    }
    if (cost <= 0) return true;
    const items::Inventory kept = inventory_;
    items::Outcome paid;
    if (!pay_glint(cost, paid, why, rules->dues_tier)) {
        inventory_ = kept;
        why = "the bags hold " + std::to_string(items::carried_value(*tier, inventory_)) + " Tax Receipt(s) and " + std::to_string(cost) + " are asked";
        return false;
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(paid);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    hand_over_inventory("the citizenship's Tax Receipts", /*periodic=*/false);
    return true;
}

void PlayerSession::set_citizenship(Connection& conn, const svc::Citizenship& c, const char* why) {
    citizenship_ = c;
    citizenship_changed_ = true;
    if (world_) world_->set_account_citizenship(account_id_, c);
    if (pb_.set_citizen_node(c.node_guid) && room_for(1)) queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn);
    hand_over_progress(why);
}

void PlayerSession::open_citizenship_dialog(Connection& conn, const world::WorldActor& npc, bool join, int64_t vendor, Clock::time_point now) {
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const auto [node, level] = citizenship_node_at(npc.x, npc.y);
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        citizenship_offer_.reset();
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: {}'s citizenship line: {}", name(), npc.npc.name, why);
        if (join) send_citizenship_call(conn, "ClientCitizenshipApplicationRejection", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
    };
    if (!rules) return refuse("this world has no citizenship rules (content.db)");
    if (node == 0) return refuse("the NPC stands in no node's zone of influence");
    const int64_t now_ms = wall_ms();
    if (join) {
        if (citizenship_.node_guid != 0) return refuse("the account is a citizen of node " + std::to_string(citizenship_.node_guid) + " already");
        if (citizenship_.renounce_cooldown_until_ms > now_ms) return refuse("the account renounced lately: it may join again in " + std::to_string((citizenship_.renounce_cooldown_until_ms - now_ms) / 1000) + " s");
        if (!rules->takes_citizens(level)) return refuse("node " + std::to_string(node) + " is at level " + std::to_string(level) + ", which takes no citizens");
        if (progress_.level < rules->min_level) return refuse("the character is level " + std::to_string(progress_.level) + " (" + std::to_string(rules->min_level) + " is asked)");
        citizenship_offer_ = CitizenshipOffer{npc.id, node, vendor, rules->join_cost, true, now};
        ++citizenship_dialogs_;
        const int32_t cost = rules->join_cost;
        const int64_t due_s = rules->next_due_ms(now_ms) / 1000;
        const FIntrepidNetworkGUID g = npc.actor.guid;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: {} offers the citizenship of node {} for {} Tax Receipts (dues next on {} s)", name(), npc.npc.name, node, cost, due_s);
        send_citizenship_call(conn, "ClientShowAddNodeCitizenshipDialog", [node, cost, due_s, g, vendor](const schema::RpcSchema& s, repl::RpcParams& p) {
            return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(node)) && repl::set_rpc_param(s, p, "CitizenshipCost", repl::Value::i64(cost)) &&
                   repl::set_rpc_param(s, p, "CitizenshipDuesDueTime", repl::Value::i64(due_s)) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.A", repl::Value::i64(static_cast<int32_t>(static_cast<uint32_t>(g.ObjectId)))) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.B", repl::Value::i64(static_cast<int32_t>(static_cast<uint32_t>(g.ObjectId >> 32)))) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.C", repl::Value::i64(static_cast<int32_t>(g.ServerId))) &&
                   repl::set_rpc_param(s, p, "InteractionNPCGuid.D", repl::Value::i64(static_cast<int32_t>(g.Randomizer))) &&
                   repl::set_rpc_param(s, p, "VendorDefRecordGuid", repl::Value::i64(vendor));
        });
        return;
    }
    if (citizenship_.node_guid != node) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: {}'s renounce line: the account is not a citizen of node {}", name(), npc.npc.name, node);
        return;
    }
    citizenship_offer_ = CitizenshipOffer{npc.id, node, 0, 0, false, now};
    ++citizenship_dialogs_;
    send_citizenship_call(conn, "ClientShowCitizenshipRenouncementWarning", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
}

const world::WorldActor* PlayerSession::citizenship_offer_npc(int64_t vendor, Clock::time_point now) const {
    if (!citizenship_offer_ || !citizenship_offer_->join || vendor == 0 || citizenship_offer_->vendor != vendor || now - citizenship_offer_->at > kCitizenshipOfferFor) return nullptr;
    const world::WorldActor* npc = registry_ ? registry_->find(citizenship_offer_->npc) : nullptr;
    if (!npc) return nullptr;
    const GoodPosition at = checker_.good();
    return std::hypot(npc->x - at.x, npc->y - at.y) <= kDialogueRangeCm ? npc : nullptr;
}

void PlayerSession::on_citizenship_application(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerSendCitizenshipApplication", now)) return;
    const auto node = int_param(rpc, "NodeGuid");
    const auto dues = int_param(rpc, "NumDuesToFulfill");
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: the citizenship application is refused: {}", name(), why);
        send_citizenship_call(conn, "ClientCitizenshipApplicationRejection", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
    };
    if (!node || !dues) return refuse("its parameters do not read");
    if (dead_) return refuse("the character is dead");
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules) return refuse("this world has no citizenship rules");
    if (!citizenship_offer_ || !citizenship_offer_->join || now - citizenship_offer_->at > kCitizenshipOfferFor) return refuse("no Node Administrator offered a citizenship lately");
    if (*node != citizenship_offer_->node) return refuse("it names node " + std::to_string(*node) + ", the offer was node " + std::to_string(citizenship_offer_->node));
    const world::WorldActor* npc = registry_ ? registry_->find(citizenship_offer_->npc) : nullptr;
    const GoodPosition at = checker_.good();
    if (!npc || std::hypot(npc->x - at.x, npc->y - at.y) > kDialogueRangeCm) return refuse("the Node Administrator is gone or out of reach");
    const int64_t now_ms = wall_ms();
    if (citizenship_.node_guid != 0) return refuse("the account is a citizen of node " + std::to_string(citizenship_.node_guid) + " already");
    if (citizenship_.renounce_cooldown_until_ms > now_ms) return refuse("the account renounced lately");
    const auto [here, level] = citizenship_node_at(npc->x, npc->y);
    if (here != *node || !rules->takes_citizens(level)) return refuse("node " + std::to_string(*node) + " takes no citizens at its level now (" + std::to_string(level) + ")");
    if (progress_.level < rules->min_level) return refuse("the character is below level " + std::to_string(rules->min_level));
    const int32_t cost = citizenship_offer_->cost;
    if (*dues != cost) spdlog::log(level_for(citizenship_log_), "[nodes] {}: the application names {} dues, the offer asked {}: the offer's are taken", name(), *dues, cost);
    std::string why;
    if (!pay_tax_receipts(conn, cost, why)) return refuse(why);
    svc::Citizenship c;
    c.node_guid = *node;
    c.primary_character_id = character_id_;
    c.joined_ms = now_ms;
    c.dues_due_ms = rules->next_due_ms(now_ms);
    citizenship_offer_.reset();
    ++citizenship_joins_;
    spdlog::info("[nodes] {}: the account becomes a citizen of node {} ({} Tax Receipts paid; dues next due at {} ms)", name(), c.node_guid, cost, c.dues_due_ms);
    set_citizenship(conn, c, "the citizenship");
    send_citizenship_call(conn, "ClientCitizenshipApplicationSuccessful", [cost](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NumDuesFulfilled", repl::Value::i64(cost));
    });
    const int64_t joined = c.node_guid;
    send_citizenship_call(conn, "ClientShowCitizenshipToast", [joined](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(joined)) && repl::set_rpc_param(s, p, "bRenouncing", repl::Value::boolean(false));
    });
}

void PlayerSession::on_citizenship_renounce(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerRemoveNodeCitizen", now)) return;
    const auto node = int_param(rpc, "NodeGuid");
    const auto who = str_param(rpc, "PlayerName");
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerRemoveNodeCitizen is refused: {}", name(), why);
    };
    if (!node || !who) return refuse("its parameters do not read");
    // the player's own renouncement names the player; another name is the mayor's or a game master's removal of a citizen, which is not built
    if (!same_name(*who, name())) return refuse("it names " + *who + ", not the player (removing another citizen is not built)");
    if (citizenship_.node_guid == 0 || citizenship_.node_guid != *node) return refuse("the account is not a citizen of node " + std::to_string(*node));
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const int level = nodes ? nodes->level_of(*node) : -1;
    svc::Citizenship c;
    c.renounce_cooldown_until_ms = rules && rules->cooldown_ms(level) > 0 ? wall_ms() + rules->cooldown_ms(level) : 0;
    citizenship_offer_.reset();
    ++citizenship_renounces_;
    spdlog::info("[nodes] {}: the account renounces its citizenship of node {}{}", name(), *node,
                 c.renounce_cooldown_until_ms > 0 ? " (it may join again in " + std::to_string(rules->cooldown_ms(level) / 1000) + " s)" : "");
    set_citizenship(conn, c, "the citizenship's renouncement");
    const int64_t left = *node;
    send_citizenship_call(conn, "ClientShowCitizenshipToast", [left](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NodeGuid", repl::Value::i64(left)) && repl::set_rpc_param(s, p, "bRenouncing", repl::Value::boolean(true));
    });
}

void PlayerSession::on_citizenship_dues(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerFulfillCitizenshipDues", now)) return;
    const auto refuse = [&](const std::string& why) {
        ++citizenship_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: the citizenship dues are refused: {}", name(), why);
        send_citizenship_call(conn, "ClientCitizenshipDuesFulfillmentRejection", [](const schema::RpcSchema&, repl::RpcParams&) { return true; });
    };
    const auto asked = int_param(rpc, "DuesRequired");
    if (!asked) return refuse("its parameters do not read");
    if (dead_) return refuse("the character is dead");
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    if (!rules) return refuse("this world has no citizenship rules");
    if (citizenship_.node_guid == 0) return refuse("the account is a citizen of no node");
    const int64_t now_ms = wall_ms();
    const int64_t period = static_cast<int64_t>(rules->dues_weeks) * 7 * 86400000;
    if (citizenship_.dues_due_ms - now_ms > period) return refuse("the dues are paid until " + std::to_string(citizenship_.dues_due_ms) + " ms already");
    const int32_t owed = rules->dues_owed(citizenship_.dues_due_ms, now_ms);
    if (*asked != owed) spdlog::log(level_for(citizenship_log_), "[nodes] {}: the client asks to pay {} dues, {} are owed: what is owed is taken", name(), *asked, owed);
    std::string why;
    if (!pay_tax_receipts(conn, owed, why)) return refuse(why);
    svc::Citizenship c = citizenship_;
    // the next period: one period on from the date that was due, or from now for dues paid after they were overdue
    c.dues_due_ms = c.dues_due_ms > 0 && now_ms < rules->overdue_ms(c.dues_due_ms) ? c.dues_due_ms + period : rules->next_due_ms(now_ms);
    ++citizenship_dues_paid_;
    spdlog::info("[nodes] {}: pays {} Tax Receipt(s) of citizenship dues for node {}: next due at {} ms", name(), owed, c.node_guid, c.dues_due_ms);
    set_citizenship(conn, c, "the citizenship dues");
    send_citizenship_call(conn, "ClientCitizenshipDuesFulfillmentSuccessful", [owed](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "NumFulfilled", repl::Value::i64(owed));
    });
}

repl::CitizenshipWindowData PlayerSession::citizenship_window(int64_t now_ms) const {
    repl::CitizenshipWindowData d;
    if (citizenship_.node_guid == 0) return d;   // no citizenship: the window shows the non-citizen's panel
    const world::CitizenshipRules* rules = world_ ? world_->citizenship_rules() : nullptr;
    repl::CitizenshipStatus& s = d.status;
    const std::string primary = upper(citizenship_.primary_character_id.empty() ? character_id_ : citizenship_.primary_character_id);
    for (const bootstrap::AccountCharacterNode& c : pb_.setup().account_characters) {
        const auto g = items::guid_of(c.guid_hex);
        if (!g) continue;
        if (c.guid_hex == primary) s.primary = {*g, c.name};
        else if (s.alternates.size() < repl::kMaxCitizenshipAlternates) s.alternates.push_back({*g, c.name});
    }
    if (s.primary.name.empty()) {   // (the account's list is not known: the player is the primary character)
        if (const auto g = items::guid_of(character_id_)) s.primary = {*g, name()};
    }
    const int64_t due = citizenship_.dues_due_ms;
    s.dues_required = rules ? rules->dues_owed(due, now_ms) : 1;
    s.dues_paid = due > now_ms ? s.dues_required : 0;
    s.dues_level_impact = s.dues_rank_impact = s.dues_non_owner_impact = 1.0;   // modelled: the dues' expressions are not run
    s.dues_overdue = due > 0 && now_ms >= due;
    s.rank = 1;                                                               // modelled
    s.player_level = progress_.level;
    s.highest_level = progress_.level;
    s.property_state = 1;                                                     // ApartmentRenter (modelled: no housing)
    d.dues_due_time = due / 1000;
    d.late_fees_due_time = rules && due > 0 ? rules->overdue_ms(due) / 1000 : 0;
    d.dues_state = due > 0 && now_ms >= due ? 1 : 0;                          // AwaitingOverdueDate once due, else AwaitingDueDate
    return d;
}

void PlayerSession::send_citizenship_window(Connection& conn) {
    const repl::CitizenshipWindowData d = citizenship_window(wall_ms());
    ++citizenship_windows_;
    send_citizenship_call(conn, "ClientPopulateCitizenshipData", [d](const schema::RpcSchema& s, repl::RpcParams& p) {
        return repl::set_rpc_param(s, p, "EssentialCitizenshipData", repl::Value(d));
    });
}

void PlayerSession::on_citizenship_window_request() {
    ++citizenship_requests_;   // answered by the RPC table's entry below, which has the connection
}

/// The client calls of node citizenship (session/rpc_table.h).
void PlayerSession::register_citizenship_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSetEssentialCitizenshipWindowData", [](PlayerSession& s, const RpcContext& c) {
        s.on_citizenship_window_request();
        if (s.within_budget(s.citizenship_budget_, "ServerSetEssentialCitizenshipWindowData", c.now)) s.send_citizenship_window(c.conn);
    });
    t.add(RpcScope::PlayerController, "ServerSendCitizenshipApplication", [](PlayerSession& s, const RpcContext& c) { s.on_citizenship_application(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRemoveNodeCitizen", [](PlayerSession& s, const RpcContext& c) { s.on_citizenship_renounce(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerFulfillCitizenshipDues", [](PlayerSession& s, const RpcContext& c) { s.on_citizenship_dues(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
