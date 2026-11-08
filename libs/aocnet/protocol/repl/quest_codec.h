// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/quest_codec.h
//
//  The two NetSerialize structs of the quest log (quests milestone, slice 1), as the client's own code runs them, in the order it runs them.  Bit stream, LSB first;
//  a count or number is a raw 32-bit word ("Ar << int32"), an int64 64 bits.
//
//  FRewardInstance (what a quest pays: the preview of the offer, the reward of a quest in the log)
//
//    32 + n x FItemInstance    Items: an int32 count, then each item as item_codec.h writes it (the exe refuses a count above 0x11111)
//    32                        NodeActivityExperience (int32)
//    32                        GuildActivityExperience (int32)
//    32 + ...                  the experience map: an int32 count of entries
//    32 + ...                  the currency map: an int32 count of entries
//    32 + ...                  SourceRewardTableIds: an int32 count of ids
//
//    Every captured reward (the item-started quest of the treasure map, the quest log of a login) has the two maps and the id list EMPTY (six zero words in all): only that form is
//    modelled.  A non-empty map or id list is refused by the reader, and the writer cannot be asked for one (the struct has no member for it).
//
//  FHashedVariable (the variables of one quest instance: RepQuestVariables' map element value)
//
//    32 + n x (64 + 32)        an int32 count, then each variable as its key (an int64: CityHash64 of the variable's name, quest_log.h) and its value (int32)
//
//    Proven bit for bit on the retail quest accept (11 variables in 1,056 bits, every key equal to the hash of its name).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/item_codec.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <cstring>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// The most items one reward holds (the exe's own limit is 0x11111) and the most variables one quest instance holds.
constexpr size_t kMaxRewardItems = 256;
constexpr size_t kMaxHashedVariables = 4096;

inline bool reward_instance_fits(const RewardInstance& r) {
    if (r.items.size() > kMaxRewardItems || r.source_reward_tables.size() > kMaxRewardItems) return false;
    for (const ItemInstance& i : r.items) {
        if (!item_instance_fits(i)) return false;
    }
    return true;
}

inline std::optional<RewardInstance> read_reward_instance(wire::PacketReader& r) {
    RewardInstance out;
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<size_t>(n) > kMaxRewardItems || static_cast<size_t>(n) > r.remaining_bits()) return std::nullopt;
    for (int32_t i = 0; i < n; ++i) {
        auto item = read_item_instance(r);
        if (!item) return std::nullopt;
        out.items.push_back(std::move(*item));
    }
    out.node_activity_experience = static_cast<int32_t>(r.read_uint32());
    out.guild_activity_experience = static_cast<int32_t>(r.read_uint32());
    const uint32_t experience = r.read_uint32(), currency = r.read_uint32(), tables = r.read_uint32();
    if (overran(r) || experience != 0 || currency != 0) return std::nullopt;   // the two maps stay unmodelled
    if (tables > kMaxRewardItems || static_cast<size_t>(tables) * 128 > r.remaining_bits()) return std::nullopt;
    for (uint32_t i = 0; i < tables; ++i) {
        const uint64_t id = r.read_uint64(), paired = r.read_uint64();
        out.source_reward_tables.emplace_back(id, paired);
    }
    if (overran(r)) return std::nullopt;
    return out;
}

inline bool write_reward_instance(emit::BunchWriter& out, const RewardInstance& r) {
    if (!reward_instance_fits(r)) return false;
    out.write_uint32(static_cast<uint32_t>(r.items.size()));
    for (const ItemInstance& i : r.items) {
        if (!write_item_instance(out, i)) return false;
    }
    out.write_uint32(static_cast<uint32_t>(r.node_activity_experience));
    out.write_uint32(static_cast<uint32_t>(r.guild_activity_experience));
    out.write_uint32(0);   // the experience map
    out.write_uint32(0);   // the currency map
    out.write_uint32(static_cast<uint32_t>(r.source_reward_tables.size()));
    for (const auto& [id, paired] : r.source_reward_tables) {
        out.write_uint64(id);
        out.write_uint64(paired);
    }
    return true;
}

// FCommissionBoardList (commission boards): NetSerialize 0x1459be2b0 (an archive: raw words, LSB first), read from the client:
//
//    64                        NodeGuid (int64)
//    4 x 32                    Guid (FGuid: A, B, C, D)
//    64                        TimeOfLastRefresh (double)
//    64                        CommissionBoardRecordId (int64)
//    32 + n x FQuestPreview    Commissions: an int32 count (the exe refuses one above 0xaaaa), then each as 0x1459c37c0 runs it:
//        64 + 64                   QuestId: Guid, TypeId (0x1413df110: no Name)
//        32 + m x (64 + 64)        InitialObjectives: an int32 count, then each Guid, TypeId (0x1459c21c0)
//        ...                       BaseQuestRewards: FRewardInstance as above (0x14643ec60)
//        64                        NodeId (int64)
//        4 x 32                    InstanceGuid (FGuid)
//        32                        bCompletedAlready: a uint32 (non-zero: true)
constexpr size_t kMaxCommissions = 64;            // a board lists CommissionsPerBoard (8); the exe's own cap is 0xaaaa
constexpr size_t kMaxCommissionObjectives = 64;

inline bool commission_board_fits(const CommissionBoardList& b) {
    if (b.commissions.size() > kMaxCommissions) return false;
    for (const QuestPreview& q : b.commissions) {
        if (q.objectives.size() > kMaxCommissionObjectives || !reward_instance_fits(q.rewards)) return false;
    }
    return true;
}

inline void write_guid_words(emit::BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

inline GuidValue read_guid_words(wire::PacketReader& r) {
    GuidValue g;
    g.a = r.read_uint32();
    g.b = r.read_uint32();
    g.c = r.read_uint32();
    g.d = r.read_uint32();
    return g;
}

inline bool write_commission_board(emit::BunchWriter& out, const CommissionBoardList& b) {
    if (!commission_board_fits(b)) return false;
    out.write_uint64(static_cast<uint64_t>(b.node));
    write_guid_words(out, b.guid);
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(b.last_refresh));
    std::memcpy(&bits, &b.last_refresh, sizeof(bits));
    out.write_uint64(bits);
    out.write_uint64(static_cast<uint64_t>(b.board));
    out.write_uint32(static_cast<uint32_t>(b.commissions.size()));
    for (const QuestPreview& q : b.commissions) {
        out.write_uint64(static_cast<uint64_t>(q.quest));
        out.write_uint64(static_cast<uint64_t>(q.quest_type));
        out.write_uint32(static_cast<uint32_t>(q.objectives.size()));
        for (const auto& [guid, type] : q.objectives) {
            out.write_uint64(static_cast<uint64_t>(guid));
            out.write_uint64(static_cast<uint64_t>(type));
        }
        if (!write_reward_instance(out, q.rewards)) return false;
        out.write_uint64(static_cast<uint64_t>(q.node));
        write_guid_words(out, q.instance);
        out.write_uint32(q.completed ? 1u : 0u);
    }
    return true;
}

inline std::optional<CommissionBoardList> read_commission_board(wire::PacketReader& r) {
    CommissionBoardList b;
    b.node = static_cast<int64_t>(r.read_uint64());
    b.guid = read_guid_words(r);
    const uint64_t bits = r.read_uint64();
    std::memcpy(&b.last_refresh, &bits, sizeof(bits));
    b.board = static_cast<int64_t>(r.read_uint64());
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<size_t>(n) > kMaxCommissions) return std::nullopt;
    for (int32_t i = 0; i < n; ++i) {
        QuestPreview q;
        q.quest = static_cast<int64_t>(r.read_uint64());
        q.quest_type = static_cast<int64_t>(r.read_uint64());
        const int32_t m = static_cast<int32_t>(r.read_uint32());
        if (overran(r) || m < 0 || static_cast<size_t>(m) > kMaxCommissionObjectives) return std::nullopt;
        for (int32_t k = 0; k < m; ++k) {
            const int64_t guid = static_cast<int64_t>(r.read_uint64());
            const int64_t type = static_cast<int64_t>(r.read_uint64());
            q.objectives.emplace_back(guid, type);
        }
        auto rewards = read_reward_instance(r);
        if (!rewards) return std::nullopt;
        q.rewards = std::move(*rewards);
        q.node = static_cast<int64_t>(r.read_uint64());
        q.instance = read_guid_words(r);
        const uint32_t done = r.read_uint32();
        if (overran(r) || done > 1) return std::nullopt;   // (the exe takes any non-zero; this writer only writes 0 or 1)
        q.completed = done != 0;
        b.commissions.push_back(std::move(q));
    }
    if (overran(r)) return std::nullopt;
    return b;
}

inline bool hashed_variable_fits(const HashedVariable& v) { return v.variables.size() <= kMaxHashedVariables; }

inline std::optional<HashedVariable> read_hashed_variable(wire::PacketReader& r) {
    HashedVariable out;
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<size_t>(n) > kMaxHashedVariables || static_cast<size_t>(n) * 96 > r.remaining_bits()) return std::nullopt;
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t key = r.read_uint64();
        const int32_t value = static_cast<int32_t>(r.read_uint32());
        out.variables.emplace_back(key, value);
    }
    if (overran(r)) return std::nullopt;
    return out;
}

inline bool write_hashed_variable(emit::BunchWriter& out, const HashedVariable& v) {
    if (!hashed_variable_fits(v)) return false;
    out.write_uint32(static_cast<uint32_t>(v.variables.size()));
    for (const auto& [key, value] : v.variables) {
        out.write_uint64(key);
        out.write_uint32(static_cast<uint32_t>(value));
    }
    return true;
}

}}}  // namespace aoc::protocol::repl
