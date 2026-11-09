// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/mayor_commissions.h  (node citizenship: the mayor's commissions)
//
//  A node's mayoral commissions, as plain rules on the node's state (kept with its election, world/elections.h).  The data is quest/commission_table.h's
//  kMayoralCommissions (the PlayerSponsored commissions: MoneyCost, MayoralCompletionCount, Expiration, Cooldown, AllowMultipleCompletion, the node reward).
//
//    the hand      the commissions the mayor may buy: as many as the node level's CommissionHandSlots (10 for a Village), drawn from the node's own commissions
//                  and its region's shared ones (their names: zLegacy_MCOM_<region>_<node or ANY>_...), none that runs or cools down; a bought one is replaced.
//    buying        from the hand, while fewer than the level's CommissionSlots (6) run, for MoneyCost copper of the node's treasury.  It runs for its Expiration
//                  (48 h for nearly all) on the node's boards, where any player takes it as a quest.
//    completing    each claimed completion of its quest counts once (a character once, unless AllowMultipleCompletion); at MayoralCompletionCount (100) it is
//                  done: the node gets its reward's NodeModification for the reward's Duration (24 h), and the commission cools down for its Cooldown (6 h) before
//                  the mayor may buy it again.  One that expires first cools down too, not done.
//  Modelled: the hand's draw (deterministic from the node and a counter; the retail draw is not known), and that the shared commissions of a region are the
//  ones whose name's node word is ANY, Kill, Gather, Scout or Cull.  A commission with a ResourceCost (3, outside the Riverlands) takes that resource from the
//  node's stores as well (NodeElection::stored; not sold while they lack it).
// ============================================================================
#pragma once

#include "quest/commission_table.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aoc::worldd::world {

/// An FGuid: a running commission's InstanceGuid.
struct MayorGuid {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    bool zero() const { return (a | b | c | d) == 0; }
    bool operator==(const MayorGuid&) const = default;
};

/// A commission the mayor bought, on the node's boards.
struct MayorActiveCommission {
    MayorGuid   guid;
    int64_t     record = 0;                    // the commission's quest record
    int64_t     started_ms = 0, expires_ms = 0;
    int32_t     completions = 0;
    std::vector<std::string> completed_by;     // the characters that completed it (their ids as the session keeps them)
    bool operator==(const MayorActiveCommission&) const = default;
};

/// A commission that ended (done or expired), until the mayor may buy it again.
struct MayorCoolingCommission {
    int64_t record = 0;
    int64_t until_ms = 0;
    bool    completed = false;      // done (else expired)
    bool    acknowledged = false;
    bool operator==(const MayorCoolingCommission&) const = default;
};

/// A NodeModification a done commission gave the node, until it ends.
struct NodeRewardMod {
    int64_t mod = 0;
    int64_t commission = 0;
    int64_t until_ms = 0;
    bool operator==(const NodeRewardMod&) const = default;
};

struct MayorCommissions {
    std::vector<int64_t>                hand;      // the commission records the mayor may buy
    std::vector<MayorActiveCommission>  active;
    std::vector<MayorCoolingCommission> cooling;
    std::vector<NodeRewardMod>          rewards;
    uint64_t                            serial = 0;   // the draws and the guids so far (they follow from it)
    bool operator==(const MayorCommissions&) const = default;
};

namespace mayor {

using quest::commissions::MayoralCommission;

/// The table's commission of quest record `record` (null: none).
const MayoralCommission* commission(int64_t record);
/// A node's name as the commissions' names write it: lower case, letters and digits only ("New Aela" -> "newaela").
std::string node_key(const std::string& display_name);
/// The commissions a node's mayor may draw: its own and its region's shared ones.
std::vector<const MayoralCommission*> pool(const std::string& node_key);

/// Fills the hand up to `hand_slots` from the pool (and drops a record that is no commission, runs or cools down).  True when it changed.
bool refill_hand(MayorCommissions& s, const std::string& node_key, int hand_slots, int64_t node);

enum class Buy : uint8_t { Ok, Unknown, NotInHand, NoSlot, Cooling, ResourceCost, Treasury };
const char* buy_name(Buy b);
/// The mayor buys `record` from the hand: on Ok it runs (`*started`, when given); `money_cost` is what the treasury pays and its ResourceCost what the node's
/// stores give (the caller takes both).  `stored`: the node's stores (null: none, and a commission with a ResourceCost is not sold).
Buy purchase(MayorCommissions& s, int64_t node, int64_t record, int slots, int64_t treasury, int64_t now_ms, MayorActiveCommission* started = nullptr,
             const std::map<int64_t, int32_t>* stored = nullptr);
/// Starts `c` at once (a game master's: no hand, slot or money).
MayorActiveCommission& start(MayorCommissions& s, int64_t node, const MayoralCommission& c, int64_t now_ms);

const MayorActiveCommission* find(const MayorCommissions& s, const MayorGuid& g);
bool completed_by(const MayorActiveCommission& a, const std::string& character);

enum class Done : uint8_t { Counted, Finished, Already, NotActive };
const char* done_name(Done d);
/// `character` completed the running commission `g` (`count` completions: 1 for a player's, more for a game master's, who is no character: "").  At the
/// commission's count it is done: it cools down and the node gets its reward.
Done complete(MayorCommissions& s, const MayorGuid& g, const std::string& character, int64_t now_ms, int32_t count = 1);

/// What advance() ended: a commission that expired, a cooldown or a node reward that ran out.
struct Ended {
    enum Kind : uint8_t { Expired, Cooled, RewardEnded } kind = Expired;
    int64_t record = 0;
};
std::vector<Ended> advance(MayorCommissions& s, int64_t now_ms);

}  // namespace mayor
}  // namespace aoc::worldd::world
