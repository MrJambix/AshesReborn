// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_commissions.cpp
//
//  The commission boards.  A board (native class CommissionBulletinBoard, opened as a plain actor: spawns/generic_npc.h plain_actor_open) has the interactable
//  ICD_PID_CommissionBoard, whose PID_CommissionBoard completes with AR_CommissionBoard: EActionResponse::OpenCommissionBoard.  Its use sets the PlayerController's
//  CurrentCommissionBoard (FCommissionBoardList: the board's record, its guid, when it last refreshed and its commissions as FQuestPreview) and calls
//  ClientOpenCommissionBoardDialog; the card's button sends ServerNCSBoardAcceptCommission with the card's InstanceGuid, which takes the commission as a quest.  The
//  quest pays when the client claims it (ServerNCSCompleteCommission, quest/session_destiny.cpp on_destiny_claim; quest_log.cpp pays_on_claim).
//
//  Read from the client: the struct's layout (protocol/repl/quest_codec.h), that OnRep_CurrentCommissionBoard (0x145e33f70) looks the board record up (a record the
//  client lacks leaves the window empty), the countdown from TimeOfLastRefresh against the GameState's server time, the accept's parameter.
//  From the data: each board record's filter, size and refresh (quest/commission_table.h, tools/worldd/gen_commissions.py).
//  Modelled (no recording): the board record of a Blueprint whose CommissionBoardId names none; a board offers the Generic commissions of its filter whose biome is the
//  board's zone's (or none), the same for every player in one refresh window (picked by the board's guid and the window); an empty reward preview; the reach.
// ============================================================================
#include "session/player_session.h"

#include "quest/commission_table.h"
#include "session/player_session_util.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

constexpr int64_t kNarrativeQuestTypeId = 6732126019658962568ll;       // NarrativeQuestV2
constexpr int64_t kNarrativeObjectiveTypeId = 2174867120698124769ll;   // NarrativeObjectiveV2
constexpr int64_t kGenericBoard = 6064631047155351553ll;               // CommissionBoard_Generic: a Blueprint the table does not list

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

const quest::commissions::Board* board_record(int64_t record) {
    for (const quest::commissions::Board& b : quest::commissions::kBoards) {
        if (b.record == record) return &b;
    }
    return nullptr;
}

/// The board record a board Blueprint opens (quest/commission_table.h kBlueprintBoards), CommissionBoard_Generic for one it does not list.
const quest::commissions::Board* board_of(const std::string& archetype) {
    std::string leaf = spawns::blueprint_leaf(archetype);   // (without the class's "_C", which the table's names keep)
    if (leaf.ends_with("_C")) leaf.resize(leaf.size() - 2);
    for (const quest::commissions::BlueprintBoard& bb : quest::commissions::kBlueprintBoards) {
        std::string_view name = bb.blueprint;
        if (name.ends_with("_C")) name.remove_suffix(2);
        if (leaf == name) return board_record(bb.record);
    }
    return board_record(kGenericBoard);
}

/// The board actor's BoardGuid (its open writes it from the point's seed: spawns/captured_spawns.cpp), else its network GUID's words.
repl::GuidValue board_guid(const world::WorldActor& a) {
    repl::GuidValue g;
    uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
    const char* const paths[4] = {"BoardGuid.A", "BoardGuid.B", "BoardGuid.C", "BoardGuid.D"};
    const repl::ObjectState& o = a.actor.obj;
    for (int i = 0; i < 4 && o.schema; ++i) {
        const repl::Value* v = o.state.get(*o.schema, paths[i]);
        const auto* n = v ? v->get_if<int64_t>() : nullptr;
        if (n) *words[i] = static_cast<uint32_t>(*n);
    }
    if ((g.a | g.b | g.c | g.d) == 0) g = repl::GuidValue{static_cast<uint32_t>(a.actor.guid.ObjectId), static_cast<uint32_t>(a.actor.guid.ObjectId >> 32), 0x434F4D4Du, 1u};
    return g;
}

quest::Guid128 guid128(const repl::GuidValue& g) { return quest::Guid128{g.a, g.b, g.c, g.d}; }

}  // namespace

void PlayerSession::open_commission_board(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const auto refuse = [&](const char* why) {
        ++commission_refused_;
        spdlog::log(level_for(commission_logged_), "[session] {}: the commission board does not open: {}", name(), why);
    };
    if (!qb || !quests_on_) return refuse("the world has no quests, or quests are off for this character");
    if (!room_for(3)) return;
    const quest::commissions::Board* board = board_of(a.npc.archetype);
    if (!board) return refuse("no board record");
    // the board's node and biome: the zone of influence it stands in
    int64_t node = 0;
    int biome = 0;
    std::string node_name;
    if (const world::CityNodes* nodes = world_->city_nodes()) {
        const world::NodeSite site = nodes->node_at(a.x, a.y);
        biome = site.biome;
        if (const world::CityNodeInfo* info = nodes->by_rank(site.node_rank)) {
            node = info->guid;
            node_name = info->display_name;
        }
    }
    // the refresh window: the same for everyone (the wall clock's), shown on the client's clock (the session's world time)
    const double refresh = std::max(60.0, board->refresh_minutes * 60.0);
    const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const double window = std::floor(wall / refresh);
    const repl::GuidValue guid = board_guid(a);
    // the commissions: the board's filter's, of the zone's biome or none, that the quest book holds, picked by the board and the window
    std::vector<const quest::commissions::Commission*> pool;
    for (const uint16_t i : board->offers) {
        if (i >= std::size(quest::commissions::kCommissions)) continue;
        const quest::commissions::Commission& c = quest::commissions::kCommissions[i];
        if (c.biome != 0 && biome != 0 && c.biome != biome) continue;
        const content::QuestRow* def = qb->quest(c.quest);
        if (!def || def->disabled) continue;
        pool.push_back(&c);
    }
    uint64_t seed = (static_cast<uint64_t>(guid.a) << 32 | guid.b) ^ (static_cast<uint64_t>(guid.c) << 32 | guid.d) ^ static_cast<uint64_t>(window) * 0x9E3779B97F4A7C15ull ^
                    static_cast<uint64_t>(board->record);
    for (size_t i = pool.size(); i > 1; --i) std::swap(pool[i - 1], pool[splitmix(seed) % i]);
    if (pool.size() > static_cast<size_t>(board->per_board)) pool.resize(static_cast<size_t>(board->per_board));
    repl::CommissionBoardList list;
    list.node = node;
    list.guid = guid;
    list.last_refresh = quest_clock(now) - (wall - window * refresh);
    list.board = board->record;
    for (const quest::commissions::Commission* c : pool) {
        const content::QuestRow* def = qb->quest(c->quest);
        repl::QuestPreview p;
        p.quest = c->quest;
        p.quest_type = kNarrativeQuestTypeId;
        for (const content::QuestObjectiveRow& o : def->objectives) {   // the objectives that start awake (the first when none is marked)
            if (o.awake) p.objectives.emplace_back(o.guid, kNarrativeObjectiveTypeId);
        }
        if (p.objectives.empty() && !def->objectives.empty()) p.objectives.emplace_back(def->objectives.front().guid, kNarrativeObjectiveTypeId);
        p.node = node;
        uint64_t s = seed ^ static_cast<uint64_t>(c->quest);
        p.instance = repl::GuidValue{static_cast<uint32_t>(splitmix(s)), static_cast<uint32_t>(splitmix(s)), static_cast<uint32_t>(splitmix(s)), static_cast<uint32_t>(splitmix(s))};
        // taken already by this character from this board in this window: the card shows it so
        const quest::Guid128 pg = guid128(p.instance);
        for (const quest::QuestInstance& q : quest_log_.instances()) p.completed = p.completed || q.preview == pg;
        list.commissions.push_back(std::move(p));
    }
    commission_actor_ = a.id;
    commission_list_ = list;
    ++commission_opens_;
    spdlog::log(level_for(commission_logged_), "[session] {}: opens the commission board {} ({}, {}): {} of {} commissions, refreshes every {:.0f} min", name(), board->name,
                spawns::blueprint_leaf(a.npc.archetype), node_name.empty() ? std::string("no node") : node_name, list.commissions.size(), pool.size(), refresh / 60.0);
    queue("CurrentCommissionBoard", [list](bootstrap::PlayerBootstrap& pb) { return pb.commission_board_update(list); }, conn, /*must_build=*/true);
    queue("ClientOpenCommissionBoardDialog", [](bootstrap::PlayerBootstrap& pb) { return pb.open_commission_board(); }, conn, /*must_build=*/true);
}

void PlayerSession::on_commission_accept(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const auto refuse = [&](const std::string& why) {
        ++commission_refused_;
        spdlog::log(level_for(commission_logged_), "[session] {}: a commission is not taken: {}", name(), why);
    };
    if (!rpc.call.decoded || !qb || !book || !quests_on_) return refuse("the world has no quests, or quests are off for this character");
    repl::GuidValue g;
    uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
    const char* const letters[4] = {"CommissionGuid.A", "CommissionGuid.B", "CommissionGuid.C", "CommissionGuid.D"};
    for (int i = 0; i < 4; ++i) *words[i] = static_cast<uint32_t>(quest_int_value(quest_rpc_value(rpc, letters[i])));
    const auto entry = std::find_if(commission_list_.commissions.begin(), commission_list_.commissions.end(), [&](const repl::QuestPreview& p) { return p.instance == g; });
    if (entry == commission_list_.commissions.end()) return refuse("it is on no board this character has open");
    const world::WorldActor* board = registry_ ? registry_->find(commission_actor_) : nullptr;
    if (!board) return refuse("the board is gone");
    const GoodPosition at = checker_.good();
    if (std::hypot(board->x - at.x, board->y - at.y) > 360.0 + 700.0) return refuse("the player walked away from the board");   // its reach and the window's slack (modelled)
    const content::QuestRow* def = qb->quest(entry->quest);
    if (!def) return refuse("its quest is not in the data");
    const auto facts = dialogue_facts(nullptr);
    dialogue::Evaluator ev(*book, *facts, [this](const std::string& what) {
        if (dialogue_told_.insert(what).second) spdlog::info("[session] {}: a condition of a quest could not be answered and does not hold: {} (told once)", name(), what);
    });
    if (const quest::Create c = quest_log_.can_start(def, ev, /*bypass_rules=*/false); c != quest::Create::Ok) return refuse(def->name + ": " + quest::create_name(c));
    start_quest(conn, *def, 0, "a commission board", now);
    for (quest::QuestInstance& q : quest_log_.instances_for_wire()) {   // the instance just taken: the card's guid on it, so the card shows it taken
        if (q.record != def->guid || !q.preview.zero()) continue;
        q.preview = guid128(g);
        q.preview_node = entry->node;
        quest_log_.touch(q);
        entry->completed = true;
        break;
    }
    ++commission_taken_;
    quest_sync(conn, "a commission was taken");
    const repl::CommissionBoardList list = commission_list_;
    queue("CurrentCommissionBoard", [list](bootstrap::PlayerBootstrap& pb) { return pb.commission_board_update(list); }, conn, /*must_build=*/true);
}

void PlayerSession::register_commissions_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerNCSBoardAcceptCommission", [](PlayerSession& s, const RpcContext& c) { s.on_commission_accept(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
