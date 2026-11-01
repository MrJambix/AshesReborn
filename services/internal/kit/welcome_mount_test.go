package kit

import (
	"aocrop/services/internal/character"
	"database/sql"
	"testing"
)

func TestWelcomeMountIsFreshAndFitsEveryClass(t *testing.T) {
	db := build(t, func(tx *sql.Tx) {
		_, err := tx.Exec(`INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots, default_quality, max_stack_size, stat_block_guid, appearance_guid, max_durability, quality_curve_guid)
            VALUES (9001, 'Mount_Grounded_Flailrunner_0', 1, 0, 0, 1, 1, 0, 0, 0, 0)`)
		if err != nil {
			t.Fatal(err)
		}
	})
	maker, err := Load(db)
	if err != nil {
		t.Fatal(err)
	}
	if err = maker.EnableWelcomeMount(db); err != nil {
		t.Fatal(err)
	}
	seen := map[string]bool{}
	for class := int32(1); class <= 8; class++ {
		for range 2 {
			items, err := maker.Kit(class)
			if err != nil {
				t.Fatal(err)
			}
			if err := character.ValidateItems(items); err != nil {
				t.Fatal(err)
			}
			count := 0
			for _, it := range items {
				if it.ItemRecordID != 9001 {
					continue
				}
				count++
				if it.Container != character.Backpack || it.Quantity != 1 || seen[it.InstanceGUID] {
					t.Fatalf("invalid welcome mount: %+v", it)
				}
				seen[it.InstanceGUID] = true
			}
			if count != 1 {
				t.Fatalf("class %d: got %d welcome mounts", class, count)
			}
		}
	}
}

func TestMissingWelcomeMountRefusesConfiguration(t *testing.T) {
	db := build(t, nil)
	maker, err := Load(db)
	if err != nil {
		t.Fatal(err)
	}
	if maker.EnableWelcomeMount(db) == nil {
		t.Fatal("missing welcome mount must fail")
	}
}
