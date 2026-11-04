// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
//
// The Destiny board: six tracks (quest/destiny_table.h, the DestinySystem record), each an ordinary quest whose SubQuest objectives start the track's quests.  Every character has them: there is no
// giver, so a track starts by itself the first time the character's quests run.  [modelled: no recording shows the board; the client's calls and their fields are the schema's]
#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

namespace {

void guid_words(const quest::Guid128& guid, uint32_t (&out)[4]) {
    out[0] = guid.a;
    out[1] = guid.b;
    out[2] = guid.c;
    out[3] = guid.d;
}

/// The quest a track's root runs: its first SubQuest objective's.
int64_t track_inner_quest(const content::QuestRow& root) {
    for (const content::QuestObjectiveRow& o : root.objectives) {
        for (const quest::destiny::SubQuestLink& l : quest::destiny::kSubQuests) {
            if (l.objective == o.guid) return l.quest;
        }
    }
    return 0;
}

bool is_milestone(const quest::destiny::Track& track, int64_t objective) {
    return std::find(std::begin(track.milestones), std::end(track.milestones), objective) != std::end(track.milestones);
}

}  // namespace

void PlayerSession::start_destiny_tracks(Connection& conn, Clock::time_point now) {
    destiny_started_ = true;
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!qb) return;
    if (quest_log_.variable(quest::kDestinyLinksVariable) == 0) quest_log_.set_variable(quest::kDestinyLinksVariable, 1);   // (a track started from here on is linked from its first update)
    size_t started = 0;
    for (const quest::destiny::Track& track : quest::destiny::kTracks) {
        const content::QuestRow* root = qb->quest(track.quest);
        if (!root || quest_log_.find(track.quest) || quest_log_.completions(track.quest) > 0) continue;
        start_quest(conn, *root, 0, "the Destiny board", now);
        ++started;
    }
    if (started) spdlog::info("[quests] {}: the Destiny board starts {} track(s)", name(), started);
}

repl::RewardInstance PlayerSession::reward_preview(const std::vector<int64_t>& tables) {
    repl::RewardInstance reward;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || tables.empty()) return reward;
    std::string error;
    const auto loaded = db->loot_tables(tables, error);
    if (!loaded) return reward;
    const abilities::Catalog* catalog = world_->ability_catalog();
    for (const int64_t guid : tables) {
        const auto found = loaded->find(guid);
        if (found == loaded->end()) continue;
        for (const content::LootContainer& container : found->second.containers) {
            for (const content::LootReward& line : container.rewards) {
                for (const content::LootItem& r : line.items) {
                    const auto record = db->item(r.item, error);
                    if (!record) continue;
                    const content::ExprRow* quantity = catalog ? catalog->expr(r.qty) : nullptr;
                    svc::InventoryItem it;
                    it.container = svc::ItemContainer::Backpack;
                    it.item_record_id = r.item;
                    it.instance_guid = fresh_guid();
                    it.quantity = std::max(1, quantity ? std::atoi(quantity->text.c_str()) : 1);
                    it.quality = record->default_quality;
                    it.durability = record->max_durability_at(it.quality);
                    reward.items.push_back(items::to_instance(it));
                }
            }
        }
    }
    return reward;
}

void PlayerSession::on_destiny_claim(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !qb || !rpc.call.decoded) return;
    // the claim button (UDestinies::HandleClaimButtonClicked) sends this once for each step that is Active and Completed.  [its rule is the client's; what the retail server does on it is modelled]
    quest::Guid128 g;
    uint32_t* words[4] = {&g.a, &g.b, &g.c, &g.d};
    const char* const letters[4] = {"QuestInstanceGuid.A", "QuestInstanceGuid.B", "QuestInstanceGuid.C", "QuestInstanceGuid.D"};
    for (int i = 0; i < 4; ++i) *words[i] = static_cast<uint32_t>(quest_int_value(quest_rpc_value(rpc, letters[i])));
    quest::QuestInstance* q = nullptr;
    for (quest::QuestInstance& candidate : quest_log_.instances_for_wire()) {
        if (candidate.guid == g) q = &candidate;
    }
    const content::QuestRow* def = q ? qb->quest(q->record) : nullptr;
    if (!def || !q->awaiting_claim) {
        ++quests_refused_;
        spdlog::log(level_for(quests_logged_, 100), "[session] {}: the claim of quest instance {} is refused: {}", name(), g.hex(), !q ? "it is not in the log" : "nothing waits on it");
        return;
    }
    const int64_t record = q->record;
    const std::vector<int64_t> tables = quest::quest_reward_tables(*def, *q);
    q->awaiting_claim = false;   // (tick_quests takes it out of the log, a finish, once the client has been told)
    q->end_time = quest_clock(now);
    quest_log_.touch(*q);
    spdlog::info("[quests] {}: claimed the rewards of {}", name(), def->name);
    const bool commission = def->flavor == "Commission";
    grant_quest_rewards(conn, tables, commission ? "a claimed commission reward" : "a claimed Destiny reward", now, record);
    quest_sync(conn, commission ? "a commission reward was claimed" : "a Destiny reward was claimed");
}

void PlayerSession::on_destiny_client_data(Connection& conn, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !qb || !room_for(1)) return;
    (void)now;
    std::vector<bootstrap::PlayerBootstrap::DestinyTrackOut> out;
    for (const quest::destiny::Track& track : quest::destiny::kTracks) {
        bootstrap::PlayerBootstrap::DestinyTrackOut t;
        t.quest = track.quest;
        const quest::QuestInstance* root = quest_log_.find(track.quest);
        if (root) guid_words(root->guid, t.instance);
        t.active = root != nullptr;
        // what the track counts: the objectives of the quest it runs, its milestones (rewards for the count, not steps) left out
        const content::QuestRow* root_def = qb->quest(track.quest);
        const int64_t inner = root_def ? track_inner_quest(*root_def) : 0;
        const content::QuestRow* inner_def = inner ? qb->quest(inner) : nullptr;
        const quest::QuestInstance* running = inner ? quest_log_.find(inner) : nullptr;
        if (inner_def) {
            for (size_t i = 0; i < inner_def->objectives.size(); ++i) {
                if (is_milestone(track, inner_def->objectives[i].guid)) continue;
                ++t.total;
                const bool done = quest_log_.completions(inner) > 0 || (running && i < running->objectives.size() && running->objectives[i].completed);
                if (done) ++t.completed;
            }
        }
        out.push_back(t);
    }
    queue("ClientReceiveDestinyClientData", [out](bootstrap::PlayerBootstrap& pb) { return pb.destiny_client_data(out); }, conn, /*must_build=*/true);
}

void PlayerSession::on_destiny_milestones(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const quest::QuestBook* qb = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !qb || !rpc.call.decoded || !room_for(1)) return;
    (void)now;
    const auto asked = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "DestinyTrackData.DestinyTrackQuest.Guid"), 0);
    if (!asked || *asked == 0) return;
    const int64_t quest_id = *asked;
    const quest::destiny::Track* track = nullptr;
    for (const quest::destiny::Track& t : quest::destiny::kTracks) {
        if (t.quest == quest_id) track = &t;
    }
    if (!track) {
        spdlog::info("[quests] {}: the client asked for the milestones of quest {}, which is no Destiny track", name(), quest_id);
        return;
    }
    const content::QuestRow* root_def = qb->quest(track->quest);
    const int64_t inner = root_def ? track_inner_quest(*root_def) : 0;
    const quest::QuestInstance* running = inner ? quest_log_.find(inner) : nullptr;
    std::vector<bootstrap::PlayerBootstrap::DestinyMilestoneOut> out;
    for (const int64_t objective : track->milestones) {
        if (objective == 0) continue;
        bootstrap::PlayerBootstrap::DestinyMilestoneOut m;
        m.objective = objective;
        for (const quest::destiny::SubQuestLink& l : quest::destiny::kSubQuests) {
            if (l.objective == objective) m.quest = l.quest;
        }
        if (running) guid_words(running->guid, m.instance);
        m.completed = m.quest != 0 && quest_log_.completions(m.quest) > 0;
        if (const content::QuestRow* paying = m.quest ? qb->quest(m.quest) : nullptr) m.reward = reward_preview(paying->rewards);
        out.push_back(m);
    }
    queue("ClientReceiveDestinyTrackMilestoneRewards", [quest_id, out](bootstrap::PlayerBootstrap& pb) { return pb.destiny_milestones(quest_id, out); }, conn, /*must_build=*/true);
}

/// The client calls of this feature (session/rpc_table.h).
void PlayerSession::register_destiny_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerNCSCompleteCommission", [](PlayerSession& s, const RpcContext& c) { s.on_destiny_claim(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGetDestinyClientData", [](PlayerSession& s, const RpcContext& c) { s.on_destiny_client_data(c.conn, c.now); });
    t.add(RpcScope::PlayerController, "ServerGetDestinyTrackMilestoneRewards", [](PlayerSession& s, const RpcContext& c) { s.on_destiny_milestones(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
