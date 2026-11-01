// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package kit

import (
	"database/sql"
	"fmt"
	"testing"
)

const startsRow = `{"PlayerStarts": {"StartLocations": [
	{"Location": {"X": -100.5, "Y": -200.5, "Z": 300.5}, "Rotation": {"Pitch": 0, "Roll": 0, "Yaw": 90}},
	{"Location": {"X": -101.5, "Y": -201.5, "Z": 301.5}, "Rotation": {"Pitch": 0, "Roll": 0, "Yaw": 90}}], "Tag": {"TagName": "%s"}}}`

func withStarts(t *testing.T, tags ...string) *Starts {
	t.Helper()
	db := build(t, func(tx *sql.Tx) {
		for i, tag := range tags {
			js := []byte(fmt.Sprintf(startsRow, tag))
			if _, err := tx.Exec(`INSERT INTO record (table_index, type_id, schema, guid, parent_guid, name, dev_only, data) VALUES (1, 2197448624304014653, 'PlayerStartsRecordBase', ?, 0, ?, 0, ?)`,
				int64(i+1), tag+" Player Starts", js); err != nil {
				t.Fatal(err)
			}
		}
	})
	s, err := LoadStarts(db)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func TestAnvilsStartsAtAPlaceOfItsZone(t *testing.T) {
	s := withStarts(t, "Riverlands", "Anvils")
	seen := map[float64]bool{}
	for range 200 {
		p, ok := s.Start("Anvils")
		if !ok || p == nil {
			t.Fatalf("Start(Anvils) = %v, %v", p, ok)
		}
		if (p.X != -100.5 || p.Y != -200.5 || p.Z != 300.5) && (p.X != -101.5 || p.Y != -201.5 || p.Z != 301.5) || p.Yaw != 90 {
			t.Fatalf("Start(Anvils) = %+v, not a place of the zone", p)
		}
		seen[p.X] = true
	}
	if len(seen) != 2 {
		t.Errorf("200 new characters stood on %d places, want both", len(seen))
	}
	if p, ok := s.Start("ANVILS"); !ok || p == nil {
		t.Errorf("the tag's case counts: %v, %v", p, ok)
	}
}

func TestRiverlandsAndNoTagKeepTheDefaultSpawn(t *testing.T) {
	s := withStarts(t, "Riverlands", "Anvils")
	for _, tag := range []string{"Riverlands", "riverlands", ""} {
		if p, ok := s.Start(tag); !ok || p != nil {
			t.Errorf("Start(%q) = %v, %v; want the default spawn", tag, p, ok)
		}
	}
}

func TestAnUnknownTagIsSaidToBeUnknown(t *testing.T) {
	s := withStarts(t, "Riverlands", "Anvils")
	if p, ok := s.Start("Moonlands"); ok || p != nil {
		t.Errorf("Start(Moonlands) = %v, %v", p, ok)
	}
}

func TestLoadStartsRefusesAContentDBWithoutThem(t *testing.T) {
	if _, err := LoadStarts(build(t, nil)); err == nil {
		t.Error("no player starts: no error")
	}
}
