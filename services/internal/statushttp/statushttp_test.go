// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statushttp

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"

	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
)

type fakePlayers struct {
	count, calls int
	err          error
}

func (f *fakePlayers) ListPlayers(context.Context, *adminv1.ListPlayersRequest, ...grpc.CallOption) (*adminv1.ListPlayersResponse, error) {
	f.calls++
	if f.err != nil {
		return nil, f.err
	}
	return &adminv1.ListPlayersResponse{Players: make([]*adminv1.Player, f.count)}, nil
}

func status(t *testing.T, h http.Handler) (server, string) {
	t.Helper()
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/v1/status", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d", rec.Code)
	}
	body := rec.Body.String()
	var reply struct{ Servers []server }
	if err := json.Unmarshal([]byte(body), &reply); err != nil || len(reply.Servers) != 1 {
		t.Fatalf("reply %q: %v", body, err)
	}
	return reply.Servers[0], body
}

func TestStatusOnline(t *testing.T) {
	now := time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)
	file := filepath.Join(t.TempDir(), "worldd.started")
	// As the supervisor's ToString('o') writes it, with its line end.
	if err := os.WriteFile(file, []byte("2026-11-01T12:00:00.1234567Z\r\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	s, body := status(t, Handler(Options{WorldName: "Ashes Reborn", Players: &fakePlayers{count: 3}, StartedFile: file, Now: func() time.Time { return now }}))
	if s.Name != "Ashes Reborn" || !s.Online || s.Players == nil || *s.Players != 3 {
		t.Fatalf("server %+v", s)
	}
	if s.SinceRestartSeconds == nil || *s.SinceRestartSeconds != 2*86400-1 {
		t.Fatalf("since restart %v", s.SinceRestartSeconds)
	}
	if strings.Contains(body, "character") {
		t.Fatalf("names a player: %s", body)
	}
}

func TestStatusWorldDown(t *testing.T) {
	s, _ := status(t, Handler(Options{WorldName: "Ashes Reborn", Players: &fakePlayers{err: errors.New("down")}}))
	if s.Online || s.Players != nil || s.SinceRestartSeconds != nil {
		t.Fatalf("server %+v", s)
	}
}

func TestStatusUnknownRestart(t *testing.T) {
	for _, file := range []string{"", filepath.Join(t.TempDir(), "missing")} {
		s, _ := status(t, Handler(Options{Players: &fakePlayers{}, StartedFile: file}))
		if !s.Online || s.SinceRestartSeconds != nil || s.Players == nil || *s.Players != 0 {
			t.Fatalf("file %q: server %+v", file, s)
		}
	}
}

func TestStatusCachesTheWorld(t *testing.T) {
	now := time.Date(2026, 11, 3, 12, 0, 0, 0, time.UTC)
	players := &fakePlayers{count: 1}
	h := Handler(Options{Players: players, Now: func() time.Time { return now }})
	status(t, h)
	players.count = 2
	now = now.Add(cacheFor - time.Second)
	if s, _ := status(t, h); players.calls != 1 || *s.Players != 1 {
		t.Fatalf("calls %d, server %+v", players.calls, s)
	}
	now = now.Add(time.Second)
	if s, _ := status(t, h); players.calls != 2 || *s.Players != 2 {
		t.Fatalf("calls %d, server %+v", players.calls, s)
	}
}

func TestChangelog(t *testing.T) {
	file := filepath.Join(t.TempDir(), "PATCH_NOTES.md")
	if err := os.WriteFile(file, []byte("## 2026-11-01: Title\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	get := func(notes string) *httptest.ResponseRecorder {
		rec := httptest.NewRecorder()
		Handler(Options{Players: &fakePlayers{}, NotesFile: notes}).ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/v1/changelog", nil))
		return rec
	}
	if rec := get(file); rec.Code != http.StatusOK || rec.Body.String() != "## 2026-11-01: Title\n" {
		t.Fatalf("%d %q", rec.Code, rec.Body.String())
	}
	if rec := get(filepath.Join(t.TempDir(), "missing")); rec.Code != http.StatusNotFound {
		t.Fatalf("missing notes: %d", rec.Code)
	}
	long := filepath.Join(t.TempDir(), "long.md")
	if err := os.WriteFile(long, make([]byte, maxNotes+1), 0o600); err != nil {
		t.Fatal(err)
	}
	if rec := get(long); rec.Code != http.StatusNotFound {
		t.Fatalf("long notes: %d", rec.Code)
	}
}
