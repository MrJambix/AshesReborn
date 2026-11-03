// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"slices"
	"strings"
	"testing"
)

func TestValidateIgnoredNames(t *testing.T) {
	ok := [][]string{nil, {"Cyb"}, {"Cyb", "YurHealz", "Ærin"}, {strings.Repeat("a", 64)}}
	for _, names := range ok {
		if err := ValidateIgnoredNames(names); err != nil {
			t.Errorf("%q: %v, want nil", names, err)
		}
	}
	many := make([]string, MaxIgnoredNames+1)
	for i := range many {
		many[i] = "n" + strings.Repeat("x", i%50) + string(rune('a'+i%26)) + string(rune('A'+i/26))
	}
	bad := [][]string{{""}, {"Cyb", "cyb"}, {strings.Repeat("a", 65)}, {"a\nb"}, {string([]byte{0xff, 0xfe})}, many}
	for _, names := range bad {
		if err := ValidateIgnoredNames(names); !errors.Is(err, ErrInvalid) {
			t.Errorf("%d names starting %.20q: %v, want ErrInvalid", len(names), names[0], err)
		}
	}
}

func TestIgnoredNamesPersistPerCharacter(t *testing.T) {
	s, owner := setup(t)
	ctx := context.Background()
	first, err := s.Create(ctx, New{AccountID: owner, Name: "First", ClassID: 3})
	if err != nil {
		t.Fatal(err)
	}
	second, err := s.Create(ctx, New{AccountID: owner, Name: "Second", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	if got, err := s.IgnoredNames(ctx, first.ID); err != nil || len(got) != 0 {
		t.Fatalf("a new character ignores %v, %v; want none", got, err)
	}
	want := []string{"Spammer", "Griefer", "Ærin"}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: want}); err != nil {
		t.Fatal(err)
	}
	fresh := NewStore(s.pool)
	if got, err := fresh.IgnoredNames(ctx, first.ID); err != nil || !slices.Equal(got, want) {
		t.Fatalf("reloaded list = %v, %v; want %v", got, err, want)
	}
	if got, _ := fresh.IgnoredNames(ctx, second.ID); len(got) != 0 {
		t.Fatalf("the other character's list = %v; want none (the list is the character's own)", got)
	}
	// a save without replace_ignored_names leaves the list alone; one with it replaces it, in order
	if _, err := fresh.SaveProgress(ctx, first.ID, Progress{Level: 2}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.IgnoredNames(ctx, first.ID); !slices.Equal(got, want) {
		t.Fatalf("an unrelated save changed the list to %v", got)
	}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: []string{"Griefer"}}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.IgnoredNames(ctx, first.ID); !slices.Equal(got, []string{"Griefer"}) {
		t.Fatalf("after the replace: %v, want [Griefer]", got)
	}
	// a refused list changes nothing, the level included
	if _, err := s.SaveProgress(ctx, first.ID, Progress{Level: 3, ReplaceIgnoredNames: true, IgnoredNames: []string{"a", "A"}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a name twice = %v, want ErrInvalid", err)
	}
	if got, _ := s.Get(ctx, first.ID); got.Level != 2 {
		t.Fatal("a refused ignore list changed the progress")
	}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{IgnoredNames: []string{"x"}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("names without replace_ignored_names = %v, want ErrInvalid", err)
	}
	if _, err := s.IgnoredNames(ctx, "0123456789abcdef0123456789abcdef"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an unknown character = %v, want ErrNotFound", err)
	}
}
