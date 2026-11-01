// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
#pragma once

#include <cstdint>

namespace aoc::worldd::pets {

// Ability, effect and SpawnNPCRecordBase identities from the design database.
// The summon montage has no branch request. Its captured end is about 8.38 s;
// completion at 8 s is modelled, leaving the montage's short recovery afterward.
inline constexpr double kSummonSeconds = 8.0;
struct Definition {
    int64_t ability, effect;
    const char* name;
    const char* archetype;
};
inline constexpr Definition kDefinitions[] = {
    {6064633584217554944ll, 6064633584224960514ll, "Spirit Guardian",
     "/Game/DesignAssets/Population/NPC/TestNPCs/Pets/Guardian/Pet_Summoner_Guardian.Default__Pet_Summoner_Guardian_C"},
    {6064633854969511983ll, 6064633826102476801ll, "Spirit Hunter",
     "/Game/DesignAssets/Population/NPC/TestNPCs/Pets/Hunter/Pet_Summoner_Hunter.Default__Pet_Summoner_Hunter_C"},
    {6064633809144250565ll, 6064633809137500355ll, "Spirit Mystic",
     "/Game/DesignAssets/Population/NPC/TestNPCs/Pets/Mystic/Pet_Summoner_Mystic.Default__Pet_Summoner_Mystic_C"},
};
inline const Definition* for_ability(int64_t guid) {
    for (const auto& d : kDefinitions) if (d.ability == guid) return &d;
    return nullptr;
}
inline const Definition* for_effect(int64_t guid) {
    for (const auto& d : kDefinitions) if (d.effect == guid) return &d;
    return nullptr;
}
inline constexpr int64_t kDismiss = 6064633780525466222ll;
inline constexpr int64_t kFollow = 6064633690279903233ll;
inline constexpr int64_t kStay = 6064633774860402690ll;

} // namespace aoc::worldd::pets
