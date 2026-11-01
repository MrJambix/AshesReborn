// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors.
package character

import (
	"context"
	"errors"
	"reflect"
	"testing"

	"aocrop/services/internal/account"
)

func TestAutoAttackSettingsPersistPerAccount(t *testing.T) {
	s, owner := setup(t)
	ctx := context.Background()
	first, err := s.Create(ctx, New{AccountID: owner, Name: "First", ClassID: 5})
	if err != nil {
		t.Fatal(err)
	}
	second, err := s.Create(ctx, New{AccountID: owner, Name: "Second", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]bool{"bEnabled": true, "bToggle": false, "bStartsAfterMeleeAbility": true, "bStartsAfterRangedAbility": false, "bStopsAfterAbility": true}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{AutoAttackSettings: want}); err != nil {
		t.Fatal(err)
	}
	// A fresh store reads the persisted data, including explicit false values.
	fresh := NewStore(s.pool)
	if got, err := fresh.AutoAttackSettings(ctx, owner); err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("reloaded options = %v, %v; want %v", got, err, want)
	}
	if _, err := fresh.SaveProgress(ctx, second.ID, Progress{AutoAttackSettings: map[string]bool{"bEnabled": false}}); err != nil {
		t.Fatal(err)
	}
	want["bEnabled"] = false
	if _, err := fresh.SaveProgress(ctx, first.ID, Progress{Level: 2}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.AutoAttackSettings(ctx, owner); err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("account patch / unrelated save = %v, %v; want %v", got, err, want)
	}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{Level: 3, AutoAttackSettings: map[string]bool{"invalid": true}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("unknown option = %v", err)
	}
	if got, _ := s.Get(ctx, first.ID); got.Level != 2 {
		t.Fatal("invalid options changed character progress")
	}
	other, err := account.NewStore(s.pool, 0).Register(ctx, "another", "secret2")
	if err != nil {
		t.Fatal(err)
	}
	if got, err := s.AutoAttackSettings(ctx, other.ID); err != nil || len(got) != 0 {
		t.Fatalf("another account = %v, %v", got, err)
	}
	if _, err := s.AutoAttackSettings(ctx, other.ID+1000); !errors.Is(err, ErrNotFound) {
		t.Fatalf("unknown account = %v", err)
	}
	if err := s.Delete(ctx, owner, first.ID); err != nil {
		t.Fatal(err)
	}
	if got, err := s.AutoAttackSettings(ctx, owner); err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("deleting a character lost account settings: %v, %v", got, err)
	}
}
