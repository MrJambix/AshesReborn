// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/citizenship.h  (node citizenship, stage 1)
//
//  The rules of node citizenship, from content.db (contentc's citizenship_generic, citizenship_node and citizenship_dues_def, from the design data's
//  CitizenshipGenericRecordBase, CitizenshipNodeRecordBase and CitizenshipDuesDefRecordBase):
//
//    * a node takes citizens at the levels citizenship_node lists (Village 3 and Town 4 in the shipped data), each with its CitizenshipCooldownTimeSeconds (0 in the data);
//    * joining costs InitialDueCost Tax Receipts (20): the join dialog's CitizenshipCost, which the client checks against the TaxReceipts the player holds (the TaxVoucher
//      items, sold by the town hall's administrator for 4 Glint each).  Modelled: the record's InitialFeeCost (100) is not charged;
//    * dues: CitizenshipDues, one Tax Receipt, every FrequencyDuesPaidWeeks (2) weeks on DueDayOfTheWeek (Thursday); LateFee (5) more after OverduePeriodDays (5);
//    * the Node Administrator's "become a citizen" line asks GetCharacterLevel(GetPlayer()) >= 10.
//
//    * dues unpaid past their overdue date put the citizenship at risk (CitizenshipAtRisk); after the node's CitizenshipAtRiskTimeLimitSeconds (7 days for nearly every node) more
//      it lapses (NumberOfMissableDuesPayments is 1: the first missed payment).  Modelled: the lapse is judged by the server's wall clock while a character of the account plays.
//
//  What the rules do not do yet: the dues' expressions (CitizenshipRankImpact, LevelImpact, NonPropertyOwnerImpact) are not run (each counts as 1).
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace aoc::worldd::world {

/// The TaxReceipts currency tier (CurrencyTierDefRecordBase "TaxReceipts"): its one kind is the TaxVoucher item.
constexpr int64_t kTaxReceiptsTier = 6064631941190189059;

struct CitizenshipRules {
    int64_t dues_tier = kTaxReceiptsTier;   // what joining and the dues are paid in
    int32_t join_cost = 20;                 // InitialDueCost: Tax Receipts to join
    int32_t dues_cost = 1;                  // CitizenshipDues: Tax Receipts each period
    int32_t dues_weeks = 2;                 // FrequencyDuesPaidWeeks
    int     due_weekday = 4;                // DueDayOfTheWeek (0 Sunday .. 6 Saturday): Thursday
    int32_t late_fee = 5;                   // LateFee: Tax Receipts more once the dues are overdue
    int32_t overdue_days = 5;               // OverduePeriodDays
    int     min_level = 10;                 // the Node Administrator's line
    std::map<int, int64_t> cooldown_secs;   // the node levels that take citizens, and their renounce cooldown
    std::map<int64_t, int64_t> at_risk_secs;   // city_node.citizenship_at_risk_secs by node (a node not listed, or 0: kDefaultAtRiskSecs)
    static constexpr int64_t kDefaultAtRiskSecs = 604800;

    /// Whether a node at `level` takes citizens.
    bool takes_citizens(int level) const { return cooldown_secs.count(level) != 0; }
    /// The renounce cooldown of a node at `level` (ms; 0 for none or a level that takes no citizens).
    int64_t cooldown_ms(int level) const;
    /// The first due day (00:00 UTC of a due_weekday) at least dues_weeks weeks after `from_ms` (Unix ms).
    int64_t next_due_ms(int64_t from_ms) const;
    /// When dues due at `due_ms` become overdue (the late fee is added from then on).
    int64_t overdue_ms(int64_t due_ms) const { return due_ms + static_cast<int64_t>(overdue_days) * 86400000; }
    /// What paying the dues costs at `now_ms` when they are due at `due_ms` (the late fee once overdue).
    int32_t dues_owed(int64_t due_ms, int64_t now_ms) const { return dues_cost + (due_ms > 0 && now_ms >= overdue_ms(due_ms) ? late_fee : 0); }
    /// How long a citizenship of `node` stays at risk before it lapses (ms).
    int64_t at_risk_ms(int64_t node) const {
        const auto it = at_risk_secs.find(node);
        return (it == at_risk_secs.end() || it->second <= 0 ? kDefaultAtRiskSecs : it->second) * 1000;
    }
    /// When a citizenship of `node` whose dues were due at `due_ms` lapses if they stay unpaid (0: never, no dues date).
    int64_t lapse_ms(int64_t node, int64_t due_ms) const { return due_ms > 0 ? overdue_ms(due_ms) + at_risk_ms(node) : 0; }

    /// Reads content.db's citizenship tables (read-only).  nullopt with `error` when the file has none (older than contentc version 12) or they do not read.
    static std::optional<CitizenshipRules> load(const std::string& content_db_path, std::string& error);
};

}  // namespace aoc::worldd::world
