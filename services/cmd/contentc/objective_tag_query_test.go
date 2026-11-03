// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Contentc version 43: a Kill objective's TagQuery and a quest item's NPCTagQuery are written as the query itself.

import (
	"encoding/json"
	"testing"

	"aocrop/services/internal/designdb"
)

func TestObjectiveTagQuery(t *testing.T) {
	query := func(tok []int64, tags ...string) designdb.Struct {
		var ts, dict []designdb.Value
		for _, x := range tok {
			ts = append(ts, x)
		}
		for _, n := range tags {
			dict = append(dict, designdb.Struct{"TagName": n})
		}
		return designdb.Struct{"TokenStreamVersion": int64(0), "TagDictionary": dict, "QueryTokenStream": ts}
	}
	text := func(v any) string {
		b, err := json.Marshal(v)
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	if got := text(tagQueryJSON(query([]int64{0, 1, 2, 2, 0, 1}, "npc.stature.rareSpawn", "HarbingerEvent.LobbyA"))); got != `{"tok":[0,1,2,2,0,1],"tags":["npc.stature.rareSpawn","HarbingerEvent.LobbyA"]}` {
		t.Errorf("a query of two tags = %s", got)
	}
	if got := tagQueryJSON(query([]int64{0, 1, 2, 0})); got != nil {
		t.Errorf("an empty AllTags query = %v, want none (it would match every creature)", got)
	}
	if got := tagQueryJSON(designdb.Struct{}); got != nil {
		t.Errorf("no query = %v, want none", got)
	}
	if got := text(tagQueryJSON(query([]int64{0}, "Character"))); got != `1` {
		t.Errorf("a query with tags and no stream = %s, want the flag 1", got)
	}
}
