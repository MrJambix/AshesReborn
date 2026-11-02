// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
)

func newGuildStore(t *testing.T) (*Store, context.Context) {
	s, _ := setup(t)
	return s, context.Background()
}

func TestGuildValidate(t *testing.T) {
	good := Guild{GUID: "0123456789abcdef0123456789abcdef", Name: "Iron Wolves", Data: `{"level":1}`}
	if err := good.Validate(); err != nil {
		t.Fatalf("a good guild: %v", err)
	}
	for name, g := range map[string]Guild{
		"short guid":     {GUID: "abc", Name: "x", Data: `{}`},
		"upper guid":     {GUID: "0123456789ABCDEF0123456789abcdef", Name: "x", Data: `{}`},
		"empty name":     {GUID: good.GUID, Name: "", Data: `{}`},
		"long name":      {GUID: good.GUID, Name: string(make([]byte, 65)), Data: `{}`},
		"not json":       {GUID: good.GUID, Name: "x", Data: `nope`},
		"json no object": {GUID: good.GUID, Name: "x", Data: `[1]`},
	} {
		if err := g.Validate(); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: got %v, want ErrInvalid", name, err)
		}
	}
}

func TestSaveListDeleteGuild(t *testing.T) {
	s, ctx := newGuildStore(t)
	const a, b = "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210"
	if got, err := s.ListGuilds(ctx); err != nil || len(got) != 0 {
		t.Fatalf("no guilds at first: %v, %v", got, err)
	}
	if err := s.SaveGuild(ctx, Guild{GUID: a, Name: "Iron Wolves", Data: `{"level":1}`}); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveGuild(ctx, Guild{GUID: a, Name: "Iron Wolves", Data: `{"level":2}`}); err != nil {
		t.Fatalf("saving again replaces: %v", err)
	}
	if err := s.SaveGuild(ctx, Guild{GUID: b, Name: "iron WOLVES", Data: `{}`}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a name taken in another case: got %v, want ErrInvalid", err)
	}
	if err := s.SaveGuild(ctx, Guild{GUID: b, Name: "Stone Bears", Data: `{}`}); err != nil {
		t.Fatal(err)
	}
	got, err := s.ListGuilds(ctx)
	if err != nil || len(got) != 2 || got[0].GUID != a || got[1].GUID != b {
		t.Fatalf("list: %v, %v", got, err)
	}
	var data struct {
		Level int `json:"level"`
	}
	if err := json.Unmarshal([]byte(got[0].Data), &data); err != nil || data.Level != 2 {
		t.Fatalf("the latest save wins: %q, %v", got[0].Data, err)
	}
	if err := s.DeleteGuild(ctx, a); err != nil {
		t.Fatal(err)
	}
	if err := s.DeleteGuild(ctx, a); err != nil {
		t.Fatalf("deleting again is no error: %v", err)
	}
	if err := s.DeleteGuild(ctx, "nope"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a bad guid: got %v, want ErrInvalid", err)
	}
	if got, err := s.ListGuilds(ctx); err != nil || len(got) != 1 || got[0].GUID != b {
		t.Fatalf("after the delete: %v, %v", got, err)
	}
	if err := s.SaveGuild(ctx, Guild{GUID: a, Name: "Iron Wolves", Data: `{"level":1}`}); err != nil {
		t.Fatalf("a deleted guild's name is free again: %v", err)
	}
}
