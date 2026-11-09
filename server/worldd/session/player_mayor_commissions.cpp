// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_mayor_commissions.cpp
//
//  The mayor's commissions (world/mayor_commissions.h) between a session and the client.  Read from the client:
//
//    * the node window's mayor tab (UNodeMayorMenu::SetIsMayor for a mayor) sends ServerRequestMayoralCommissionHand() and listens for
//      ClientReceiveMayoralCommissionHand(FNodeMayorCommissionData Data: Node, Hand {CommissionRecordId, Reward}, ActiveCommissions {InstanceGuid, CommissionRecordId,
//      ExpirationTime, TargetNode, CharactersCompleted, Completions, bSupercharged, Reward}, CoolingCommissions {CommissionRecordId, CooldownTimeEnd, bCompleted,
//      bAcknowledged}, SlotCount); it asks again when a card's timer runs out and after a purchase;
//    * the purchase's confirmation (UNodeMayorMenu::HandlePurchaseMayoralCommissionConfirmed) sends ServerPurchaseMayoralCommission(CommissionRecordId);
//    * ClientReceiveMayoralCommissions(CommissionList {CommissionRecordId, CommissionInstanceGuid, bSupercharged, Completions, bPlayerAlreadyCompleted, Expiration,
//      Reward}, NodeGuid) opens the HUD's "CommissionBoard" menu and gives it the node's commissions (UCommissionBoardMaster::SetMayoralCommissions); a card is
//      open while its Expiration is ahead and its Completions are below the record's MayoralCompletionCount;
//    * a mayoral card's click (UActiveCommissionCard::HandleClicked) sends ServerActivateMayoralCommission(CommissionInstanceGuid);
//    * the times (Expiration, ExpirationTime, CooldownTimeEnd) are on the GameState's world clock, in seconds;
//    * ServerRerollMayoralCommissions has no sender in the client: it is the console's (a game master's here).
//
//  Modelled: this build places no mayoral board in a town (its population instance is in no set), so the node's town board (CommissionBoard_NodeDefault, the
//  Town Hall's billboard) sends the node's commissions when it opens, and so does a CommissionBoard_NodeMayoral should one stand; the commission is taken
//  as its quest; a claimed completion counts (session_destiny.cpp).
// ============================================================================
#include "session/player_session_util.h"

#include "world/citizenship.h"
#include "world/elections.h"
#include "world/mayor_commissions.h"
#include "world/node_wire.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

namespace {

const repl::Value* mparam(const repl::ClientRpc& rpc, std::string_view path) {
    return rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, path) : nullptr;
}

int64_t mint(const repl::ClientRpc& rpc, std::string_view path) {
    const repl::Value* v = mparam(rpc, path);
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    if (const bool* b = v->get_if<bool>()) return *b ? 1 : 0;
    return 0;
}

world::MayorGuid mguid(const repl::ClientRpc& rpc, const std::string& base) {
    return world::MayorGuid{static_cast<uint32_t>(mint(rpc, base + ".A")), static_cast<uint32_t>(mint(rpc, base + ".B")), static_cast<uint32_t>(mint(rpc, base + ".C")),
                            static_cast<uint32_t>(mint(rpc, base + ".D"))};
}

repl::Value i32(uint32_t w) { return repl::Value::i64(static_cast<int32_t>(w)); }

}  // namespace

int64_t PlayerSession::to_world_seconds(int64_t wall_at_ms, Clock::time_point now) {
    return static_cast<int64_t>(std::floor(quest_clock(now) + static_cast<double>(wall_at_ms - wall_ms()) / 1000.0));
}

world::LevelGrants PlayerSession::node_grants(int64_t node) const {
    const world::NodeWire* wire = world_ ? world_->node_wire() : nullptr;
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    world::LevelGrants g = wire && nodes ? wire->grants(node, nodes->level_of(node)) : world::LevelGrants{};
    if (mayor_slots_for_test_) {
        g.commission_hand_slots = mayor_slots_for_test_->first;
        g.commission_slots = mayor_slots_for_test_->second;
    }
    return g;
}

std::string PlayerSession::mayor_node_key(int64_t node) const {
    const world::CityNodeInfo* info = world_ && world_->city_nodes() ? world_->city_nodes()->by_guid(node) : nullptr;
    return info ? world::mayor::node_key(info->display_name) : std::string();
}

repl::RewardInstance PlayerSession::mayor_commission_reward(int64_t record) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const content::QuestRow* def = qb ? qb->quest(record) : nullptr;
    return def ? reward_preview(def->rewards) : repl::RewardInstance{};
}

void PlayerSession::send_mayor_hand(Connection& conn, int64_t node, Clock::time_point now) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el) return;
    const world::LevelGrants g = node_grants(node);
    const std::string key = mayor_node_key(node);
    const int64_t wall = wall_ms();
    el->advance_commissions(wall);
    if (el->change_commissions(node, [&](world::MayorCommissions& s, int64_t&) { return world::mayor::refill_hand(s, key, g.commission_hand_slots, node); })) world_->save_elections();
    const world::MayorCommissions* s = el->commissions(node);
    if (!s) return;
    repl::ArrayValue hand, active, cooling;
    for (const int64_t record : s->hand) {
        repl::ElementState e;
        e.values[1] = repl::Value::i64(record);
        e.values[2] = repl::Value(mayor_commission_reward(record));
        hand.elements.push_back(std::move(e));
    }
    for (const world::MayorActiveCommission& a : s->active) {
        repl::ElementState e;
        e.values[1] = i32(a.guid.a);
        e.values[2] = i32(a.guid.b);
        e.values[3] = i32(a.guid.c);
        e.values[4] = i32(a.guid.d);
        e.values[5] = repl::Value::i64(a.record);
        e.values[6] = repl::Value::i64(to_world_seconds(a.expires_ms, now));
        e.values[7] = repl::Value::i64(0);   // TargetNode (no commission of the data is aimed at another node)
        e.values[8] = repl::Value(repl::ArrayValue{});   // CharactersCompleted: the characters' guids are not the session's to give (counted in Completions)
        e.values[9] = repl::Value::i64(a.completions);
        e.values[10] = repl::Value::boolean(false);
        e.values[11] = repl::Value(mayor_commission_reward(a.record));
        active.elements.push_back(std::move(e));
    }
    for (const world::MayorCoolingCommission& c : s->cooling) {
        repl::ElementState e;
        e.values[1] = repl::Value::i64(c.record);
        e.values[2] = repl::Value::i64(to_world_seconds(c.until_ms, now));
        e.values[3] = repl::Value::boolean(c.completed);
        e.values[4] = repl::Value::boolean(c.acknowledged);
        cooling.elements.push_back(std::move(e));
    }
    const int32_t slots = g.commission_slots;
    ++mayor_hands_sent_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: the mayor's commissions of node {}: {} in the hand, {} running of {}, {} cooling", name(), node, s->hand.size(),
                s->active.size(), slots, s->cooling.size());
    send_citizenship_call(conn, "ClientReceiveMayoralCommissionHand", [node, hand, active, cooling, slots](const schema::RpcSchema& r, repl::RpcParams& p) {
        return repl::set_rpc_param(r, p, "Data.Node", repl::Value::i64(node)) && repl::set_rpc_param(r, p, "Data.Hand", repl::Value(hand)) &&
               repl::set_rpc_param(r, p, "Data.ActiveCommissions", repl::Value(active)) && repl::set_rpc_param(r, p, "Data.CoolingCommissions", repl::Value(cooling)) &&
               repl::set_rpc_param(r, p, "Data.SlotCount", repl::Value::i64(slots));
    });
}

void PlayerSession::send_mayor_board(Connection& conn, int64_t node, Clock::time_point now) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el || node == 0) return;
    el->advance_commissions(wall_ms());
    const world::MayorCommissions* s = el->commissions(node);
    mayor_board_node_ = node;
    if (!s || s->active.empty()) return;   // (nothing runs: the board shows its own commissions alone)
    repl::ArrayValue list;
    for (const world::MayorActiveCommission& a : s->active) {
        repl::ElementState e;
        e.values[1] = repl::Value::i64(a.record);
        e.values[2] = i32(a.guid.a);
        e.values[3] = i32(a.guid.b);
        e.values[4] = i32(a.guid.c);
        e.values[5] = i32(a.guid.d);
        e.values[6] = repl::Value::boolean(false);
        e.values[7] = repl::Value::i64(a.completions);
        e.values[8] = repl::Value::boolean(world::mayor::completed_by(a, character_id_));
        e.values[9] = repl::Value::i64(to_world_seconds(a.expires_ms, now));
        e.values[10] = repl::Value(mayor_commission_reward(a.record));
        list.elements.push_back(std::move(e));
    }
    ++mayor_boards_sent_;
    spdlog::log(level_for(citizenship_log_), "[nodes] {}: the board of node {} lists its {} mayoral commission(s)", name(), node, s->active.size());
    send_citizenship_call(conn, "ClientReceiveMayoralCommissions", [list, node](const schema::RpcSchema& r, repl::RpcParams& p) {
        return repl::set_rpc_param(r, p, "CommissionList", repl::Value(list)) && repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(node));
    });
}

void PlayerSession::on_mayor_hand_request(Connection& conn, const repl::ClientRpc&, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerRequestMayoralCommissionHand", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const int64_t node = el ? el->mayor_node(account_id_) : 0;
    if (node == 0) {
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerRequestMayoralCommissionHand is not answered: the account is no node's mayor", name());
        return;
    }
    send_mayor_hand(conn, node, now);
}

void PlayerSession::on_mayor_commission_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerPurchaseMayoralCommission", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const int64_t record = mint(rpc, "CommissionRecordId");
    const int64_t node = el ? el->mayor_node(account_id_) : 0;
    if (node == 0) {
        ++mayor_purchases_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: ServerPurchaseMayoralCommission({}) is refused: the account is no node's mayor", name(), record);
        if (el) send_must_be_mayor(conn);
        return;
    }
    const world::LevelGrants g = node_grants(node);
    const int64_t wall = wall_ms();
    el->advance_commissions(wall);
    world::mayor::Buy result = world::mayor::Buy::Unknown;
    int64_t paid = 0;
    el->change_commissions(node, [&](world::MayorCommissions& s, int64_t& treasury) {
        result = world::mayor::purchase(s, node, record, g.commission_slots, treasury, wall);
        if (result != world::mayor::Buy::Ok) return false;
        const world::mayor::MayoralCommission* c = world::mayor::commission(record);
        paid = c ? c->money_cost : 0;
        treasury -= paid;
        world::mayor::refill_hand(s, mayor_node_key(node), g.commission_hand_slots, node);
        return true;
    });
    if (result != world::mayor::Buy::Ok) {
        ++mayor_purchases_refused_;
        spdlog::info("[nodes] {}: the mayor of node {} does not buy commission {}: {}", name(), node, record, world::mayor::buy_name(result));
        send_mayor_hand(conn, node, now);
        return;
    }
    ++mayor_purchases_;
    world::NodeNews news{12, wall, name()};   // ENodeSystemNotificationType::NewMayoralCommission
    news.commission = record;
    el->add_news(node, std::move(news));
    world_->save_elections();
    const world::mayor::MayoralCommission* c = world::mayor::commission(record);
    spdlog::info("[nodes] {}: the mayor of node {} buys the commission {} for {} copper of the treasury (left {})", name(), node, c ? c->name : std::to_string(record), paid,
                 el->treasury(node));
    send_mayor_hand(conn, node, now);
}

void PlayerSession::on_mayor_commission_activate(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(citizenship_budget_, "ServerActivateMayoralCommission", now)) return;
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const world::MayorGuid g = mguid(rpc, "CommissionInstanceGuid");
    const auto refuse = [&](const std::string& why) {
        ++mayor_activations_refused_;
        spdlog::log(level_for(citizenship_log_), "[nodes] {}: a mayoral commission is not taken: {}", name(), why);
    };
    if (!el || !qb || !book || !quests_on_) return refuse("the world has no elections or quests, or quests are off for this character");
    el->advance_commissions(wall_ms());
    // the node whose board this session opened last lists it
    const world::MayorCommissions* s = el->commissions(mayor_board_node_);
    const world::MayorActiveCommission* a = s ? world::mayor::find(*s, g) : nullptr;
    if (!a) return refuse("it runs on no board this character has open");
    const world::WorldActor* board = registry_ ? registry_->find(commission_actor_) : nullptr;
    const GoodPosition at = checker_.good();
    if (!board || std::hypot(board->x - at.x, board->y - at.y) > 360.0 + 700.0) return refuse("the player walked away from the board");
    const world::mayor::MayoralCommission* c = world::mayor::commission(a->record);
    if (world::mayor::completed_by(*a, character_id_) && !(c && c->multiple)) return refuse("the character completed it already");
    const content::QuestRow* def = qb->quest(a->record);
    if (!def) return refuse("its quest is not in the data");
    const auto facts = dialogue_facts(nullptr);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a quest could not be answered and does not hold: {} (told once)", name(), what);
    });
    if (const quest::Create cr = quest_log_.can_start(def, ev, /*bypass_rules=*/false); cr != quest::Create::Ok) return refuse(def->name + ": " + quest::create_name(cr));
    const int64_t node = mayor_board_node_;
    start_quest(conn, *def, 0, "a mayoral commission", now);
    for (quest::QuestInstance& q : quest_log_.instances_for_wire()) {   // the instance just taken: the commission's guid and node on it
        if (q.record != def->guid || !q.preview.zero()) continue;
        q.preview = quest::Guid128{g.a, g.b, g.c, g.d};
        q.preview_node = node;
        quest_log_.touch(q);
        break;
    }
    ++mayor_activations_;
    spdlog::info("[nodes] {}: takes the mayoral commission {} of node {}", name(), def->name, node);
    quest_sync(conn, "a mayoral commission was taken");
}

void PlayerSession::note_mayor_commission_done(const quest::QuestInstance& q) {
    world::Elections* el = world_ ? world_->elections() : nullptr;
    if (!el || q.preview_node == 0 || q.preview.zero()) return;
    const world::MayorGuid g{q.preview.a, q.preview.b, q.preview.c, q.preview.d};
    const world::MayorCommissions* s = el->commissions(q.preview_node);
    if (!s || !world::mayor::find(*s, g)) return;   // (a board's own commission, or one that ended meanwhile)
    world::mayor::Done done = world::mayor::Done::NotActive;
    const int64_t wall = wall_ms();
    el->change_commissions(q.preview_node, [&](world::MayorCommissions& m, int64_t&) {
        done = world::mayor::complete(m, g, character_id_, wall);
        return done == world::mayor::Done::Counted || done == world::mayor::Done::Finished;
    });
    world_->save_elections();
    ++mayor_completions_;
    const world::mayor::MayoralCommission* c = world::mayor::commission(q.record);
    spdlog::info("[nodes] {}: a completion of the mayoral commission {} of node {}: {}", name(), c ? c->name : std::to_string(q.record), q.preview_node, world::mayor::done_name(done));
}

// ---- a game master's ----------------------------------------------------------------------------------------------------------------------

int64_t PlayerSession::gm_commission_node(int64_t asked) const {
    if (asked != 0) return world_ && world_->city_nodes() && world_->city_nodes()->by_guid(asked) ? asked : 0;
    const GoodPosition at = checker_.good();
    return citizenship_node_at(at.x, at.y).first;
}

void PlayerSession::on_gm_mayor_commissions(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const char* call = rpc.call.rpc ? rpc.call.rpc->name : "a game master's commission call";
    const std::string what = call;
    int64_t asked_node = mint(rpc, "NodeGuid");   // (the calls name the node NodeGuid, NodeRecordGuid or Node; 0: the node the player stands in)
    if (asked_node == 0) asked_node = mint(rpc, "NodeRecordGuid");
    if (asked_node == 0) asked_node = mint(rpc, "Node");
    const int64_t record = mint(rpc, "CommissionRecordId") != 0 ? mint(rpc, "CommissionRecordId") : mint(rpc, "CommissionRecordGuid");
    const int64_t count = mint(rpc, "NumIncreaseBy") != 0 ? mint(rpc, "NumIncreaseBy") : mint(rpc, "NumToIncreaseBy");
    const int64_t secs = mint(rpc, "SecondsUntilExpiration");
    const bool all = mint(rpc, "bApplyToAllInstances") != 0;
    if (!gm_rpc_admitted(call, "Node=" + std::to_string(asked_node) + " Commission=" + std::to_string(record) + " Count=" + std::to_string(count) + " Seconds=" + std::to_string(secs),
                         now) ||
        !rpc.call.decoded) {
        return;
    }
    world::Elections* el = world_ ? world_->elections() : nullptr;
    const int64_t node = el ? gm_commission_node(asked_node) : 0;
    if (node == 0) {
        spdlog::warn("[gm] {}: {} does nothing: no node {} (and the player stands in none)", name(), call, asked_node);
        return;
    }
    const int64_t wall = wall_ms();
    const world::LevelGrants g = node_grants(node);
    const std::string key = mayor_node_key(node);
    const world::mayor::MayoralCommission* c = world::mayor::commission(record);
    std::string said;
    el->change_commissions(node, [&](world::MayorCommissions& s, int64_t&) {
        if (!c && (what == "ServerGMAddCommissionToMayorHand" || what == "ServerGMAddAndStartMayoralCommission")) {
            said = "it is no mayor's commission";
            return false;
        }
        if (what == "ServerGMAddCommissionToMayorHand") {
            if (std::find(s.hand.begin(), s.hand.end(), record) == s.hand.end()) s.hand.push_back(record);
            said = std::string(c->name) + " is in the hand";
            return true;
        }
        if (what == "ServerGMAddAndStartMayoralCommission") {
            world::mayor::start(s, node, *c, wall);
            said = std::string(c->name) + " runs";
            return true;
        }
        if (what == "ServerGMClearMayoralCommissions") {
            s.active.clear();
            s.hand.clear();
            said = "no commission runs, the hand is empty";
            return true;
        }
        if (what == "ServerGMClearMayoralCommissionsCooldowns") {
            s.cooling.clear();
            said = "no commission cools down";
            return true;
        }
        if (what == "ServerRerollMayoralCommissions") {
            s.hand.clear();
            world::mayor::refill_hand(s, key, g.commission_hand_slots, node);
            said = "a new hand of " + std::to_string(s.hand.size());
            return true;
        }
        if (what == "ServerGMAddParticipationCompletionToMayoralCommission" || what == "ServerGMAddParticipationCompletionToNodesMayoralCommissions") {
            const bool every = what == "ServerGMAddParticipationCompletionToNodesMayoralCommissions";
            std::vector<world::MayorGuid> targets;
            for (const world::MayorActiveCommission& a : s.active) {
                if (every || a.record == record) targets.push_back(a.guid);
                if (!every && !all && !targets.empty()) break;
            }
            int done = 0;
            for (const world::MayorGuid& t : targets) done += world::mayor::complete(s, t, "", wall, static_cast<int32_t>(std::clamp<int64_t>(count, 1, 1000000))) == world::mayor::Done::Finished;
            said = std::to_string(targets.size()) + " commission(s) counted, " + std::to_string(done) + " done";
            return !targets.empty();
        }
        if (what == "ServerGMUpdateMayoralCommissionExpirationTime" || what == "ServerGMUpdateAllMayoralCommissionsExpirationTime") {
            const bool every = what == "ServerGMUpdateAllMayoralCommissionsExpirationTime";
            int n = 0;
            for (world::MayorActiveCommission& a : s.active) {
                if (!every && a.record != record) continue;
                a.expires_ms = wall + std::max<int64_t>(0, secs) * 1000;
                ++n;
            }
            said = std::to_string(n) + " commission(s) end in " + std::to_string(secs) + " s";
            return n > 0;
        }
        said = "not served";
        return false;
    });
    world_->save_elections();
    spdlog::info("[gm] {}: {} on node {}: {}", name(), call, node, said);
    if (el->mayor_node(account_id_) == node) send_mayor_hand(conn, node, now);
}

/// The client calls of the mayor's commissions (session/rpc_table.h).
void PlayerSession::register_mayor_commissions_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestMayoralCommissionHand", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_hand_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerPurchaseMayoralCommission", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_commission_purchase(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerActivateMayoralCommission", [](PlayerSession& s, const RpcContext& c) { s.on_mayor_commission_activate(c.conn, c.rpc, c.now); });
    for (const char* gm : {"ServerGMAddCommissionToMayorHand", "ServerGMAddAndStartMayoralCommission", "ServerGMClearMayoralCommissions", "ServerGMClearMayoralCommissionsCooldowns",
                           "ServerRerollMayoralCommissions", "ServerGMAddParticipationCompletionToMayoralCommission",
                           "ServerGMAddParticipationCompletionToNodesMayoralCommissions", "ServerGMUpdateMayoralCommissionExpirationTime",
                           "ServerGMUpdateAllMayoralCommissionsExpirationTime"}) {
        t.add(RpcScope::PlayerController, gm, [](PlayerSession& s, const RpcContext& c) { s.on_gm_mayor_commissions(c.conn, c.rpc, c.now); });
    }
}

}}  // namespace aoc::worldd
