// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Contentc version 43: the effects of the gear sets (set_effect_bonus).

import (
	"testing"

	"aocrop/services/internal/designdb"
)

const tSetEffects = 9000000000000000003

func effectBonus(effect int64, lo, hi string, stacks int64) designdb.Struct {
	return designdb.Struct{"Effect": idStruct(effect), "MinimumRarity": "EItemRarity::" + lo, "MaximumRarity": "EItemRarity::" + hi, "Stacks": stacks}
}

func TestSetEffectBonuses(t *testing.T) {
	src := gearSource()
	// an ArmorType set: an empty stat tier and an effect per rarity band; an entry that names no effect is left out
	src.rows["SetBonusRecordBase"] = append(src.rows["SetBonusRecordBase"], designdb.Row{Guid: tSetEffects, Name: "ArmorType_Test", Fields: designdb.Struct{
		"SetStatBonuses": designdb.Pairs{setTier(5)},
		"SetEffectBonuses": designdb.Pairs{{Key: int64(5), Value: designdb.Struct{"EffectBonuses": []designdb.Value{
			effectBonus(7001, "Common", "Common", 1), effectBonus(0, "Rare", "Rare", 1), effectBonus(7006, "Legendary", "Artifact", 2)}}}},
	}})
	db, meta, err := buildGearSynthetic(t, src)
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.set_effect_bonus"] != "2" || meta["typed.set_bonus"] != "4" {
		t.Errorf("meta = %v", meta)
	}
	rows, err := db.SQL().Query(`SELECT tier_ord, pieces, ord, effect_guid, min_rarity, max_rarity, stacks FROM set_effect_bonus WHERE set_guid = ? ORDER BY tier_ord, ord`, int64(tSetEffects))
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	type row struct{ tier, pieces, ord, effect, lo, hi, stacks int64 }
	var got []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.tier, &r.pieces, &r.ord, &r.effect, &r.lo, &r.hi, &r.stacks); err != nil {
			t.Fatal(err)
		}
		got = append(got, r)
	}
	want := []row{{0, 5, 0, 7001, 0, 0, 1}, {0, 5, 2, 7006, 5, 6, 2}}
	if len(got) != len(want) || got[0] != want[0] || got[1] != want[1] {
		t.Errorf("set_effect_bonus = %+v, want %+v", got, want)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM set_bonus WHERE set_guid = ?`, int64(tSetEffects)); n != 0 {
		t.Errorf("the set has %d set_bonus rows, want none (its stat tier is empty)", n)
	}

	// a tier of no pieces is refused
	bad := gearSource()
	bad.rows["SetBonusRecordBase"] = append(bad.rows["SetBonusRecordBase"], designdb.Row{Guid: tSetEffects, Name: "Bad", Fields: designdb.Struct{
		"SetStatBonuses":   designdb.Pairs{},
		"SetEffectBonuses": designdb.Pairs{{Key: int64(0), Value: designdb.Struct{"EffectBonuses": []designdb.Value{effectBonus(7001, "Common", "Common", 1)}}}},
	}})
	if _, _, err := buildGearSynthetic(t, bad); err == nil {
		t.Error("a tier of 0 pieces was taken")
	}
}
