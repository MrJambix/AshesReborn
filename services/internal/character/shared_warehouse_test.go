// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"testing"
)

// The shared warehouse (migration 0026) against a real database: one warehouse for the characters of an account, written only by a save of a newer revision.

func saveShared(s *Store, id string, bag []Item, stored []WarehouseItem, bought []WarehouseContainer, revision int64, merged ...string) error {
	return s.SaveInventoryShared(context.Background(), id, bag, nil, false, nil, false, nil, false, stored, bought, SharedWarehouseSave{Revision: revision, MergedLegacy: merged}, nil)
}

func TestSharedWarehouseIsTheAccountsAndTakesOnlyNewerRevisions(t *testing.T) {
	ctx := context.Background()
	s, acc := setup(t)
	a, err := s.Create(ctx, New{AccountID: acc, Name: "maina", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	b, err := s.Create(ctx, New{AccountID: acc, Name: "altb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	bagA, err := s.Inventory(ctx, a.ID)
	if err != nil {
		t.Fatal(err)
	}
	bagB, err := s.Inventory(ctx, b.ID)
	if err != nil {
		t.Fatal(err)
	}

	// before the warehouse was shared: each character kept its own, and b bought a tab
	oldA := []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 0, 101, 9001, 1), wh(WarehouseMaterials, joevaNode, baseTab, 3, 202, 9002, 5)}
	oldB := []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 0, 303, 9003, 1)}
	if err := saveWarehouse(s, a.ID, bagA, oldA); err != nil {
		t.Fatal(err)
	}
	tab := WarehouseContainer{Area: WarehouseMaterials, NodeGUID: halcyonNode, ContainerGUID: baseTab + 1}
	if err := s.SaveInventoryEverything(ctx, b.ID, bagB, nil, false, nil, false, nil, false, oldB, true, []WarehouseContainer{tab}); err != nil {
		t.Fatal(err)
	}

	got, err := s.SharedWarehouseOf(ctx, a.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.Revision != 0 || len(got.Items) != 0 {
		t.Fatalf("a warehouse never saved: revision %d, %d items", got.Revision, len(got.Items))
	}
	sameWarehouse(t, "the legacy items (a's, the older character, first)", got.Legacy, append(append([]WarehouseItem{}, oldA...), oldB...))
	if len(got.Containers) != 1 || got.Containers[0] != tab {
		t.Fatalf("the containers are %+v, want b's tab", got.Containers)
	}
	fromB, err := s.SharedWarehouseOf(ctx, b.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(fromB.Legacy) != 3 {
		t.Fatalf("b sees %d legacy items, want the account's 3", len(fromB.Legacy))
	}

	// a's session merges two of them (b's item moves to slot 1, its own place being taken) and saves revision 1
	merged := []WarehouseItem{oldA[0], wh(WarehouseItems, 0, baseChunk, 1, 303, 9003, 1), oldA[1]} // (in the order the store returns them: by area, town, container, slot)
	chunk := WarehouseContainer{Area: WarehouseItems, ContainerGUID: 77}
	if err := saveShared(s, a.ID, bagA, merged, []WarehouseContainer{chunk}, 1, guid(9001), guid(9002), guid(9003)); err != nil {
		t.Fatal(err)
	}
	got, err = s.SharedWarehouseOf(ctx, b.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.Revision != 1 || len(got.Legacy) != 0 {
		t.Fatalf("after the merge: revision %d, %d legacy items left", got.Revision, len(got.Legacy))
	}
	sameWarehouse(t, "the shared items", got.Items, merged)
	if len(got.Containers) != 2 {
		t.Fatalf("the containers are %+v, want the chunk a bought and b's tab", got.Containers)
	}
	if old, err := s.WarehouseItems(ctx, a.ID); err != nil || len(old) != 0 {
		t.Fatalf("a's own warehouse after the merge: %d items, %v", len(old), err)
	}

	// b's save of an older (or the same) revision arrives later: its bag is saved, the warehouse is not
	if err := saveShared(s, b.ID, bagB, nil, nil, 1); err != nil {
		t.Fatal(err)
	}
	got, err = s.SharedWarehouseOf(ctx, a.ID)
	if err != nil {
		t.Fatal(err)
	}
	sameWarehouse(t, "after a stale save", got.Items, merged)

	// a newer one replaces it whole
	if err := saveShared(s, b.ID, bagB, merged[:1], nil, 5); err != nil {
		t.Fatal(err)
	}
	got, err = s.SharedWarehouseOf(ctx, a.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.Revision != 5 {
		t.Fatalf("revision %d, want 5", got.Revision)
	}
	sameWarehouse(t, "after a newer save", got.Items, merged[:1])

	// the rules
	if err := saveShared(s, a.ID, bagA, nil, nil, -1); !errors.Is(err, ErrInvalid) {
		t.Errorf("a negative revision: %v, want ErrInvalid", err)
	}
	if err := saveShared(s, a.ID, bagA, nil, nil, 9, "not a guid"); !errors.Is(err, ErrInvalid) {
		t.Errorf("a merged guid that is none: %v, want ErrInvalid", err)
	}
	onBoth := wh(WarehouseItems, 0, baseChunk, 2, bagA[0].ItemRecordID, 0, 1)
	onBoth.InstanceGUID = bagA[0].InstanceGUID
	if err := saveShared(s, a.ID, bagA, []WarehouseItem{onBoth}, nil, 9); !errors.Is(err, ErrInvalid) {
		t.Errorf("an instance in the bag and the warehouse: %v, want ErrInvalid", err)
	}
	if _, err := s.SharedWarehouseOf(ctx, "0123456789abcdef0123456789abcdef"); !errors.Is(err, ErrNotFound) {
		t.Errorf("an unknown character: %v, want ErrNotFound", err)
	}
}
