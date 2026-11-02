// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package statushttp serves what the launcher shows before anyone signs in: GET /api/v1/status (each
// server's name, connected players and time since its last restart) and GET /api/v1/changelog (the patch notes).
// Neither needs a login, so neither names a player.
package statushttp

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"google.golang.org/grpc"

	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
)

// cacheFor keeps a flood of status requests from reaching worldd's network thread.
const (
	cacheFor     = 15 * time.Second
	worldTimeout = 3 * time.Second
	maxNotes     = 256 * 1024
)

// Players is the part of worldd's admin API the status reads.
type Players interface {
	ListPlayers(ctx context.Context, in *adminv1.ListPlayersRequest, opts ...grpc.CallOption) (*adminv1.ListPlayersResponse, error)
}

type Options struct {
	WorldName   string
	Players     Players
	StartedFile string           // holds the time the supervisor last started worldd; empty when unknown
	NotesFile   string           // PATCH_NOTES.md, read once: a new one comes with a package, which restarts this service
	Now         func() time.Time // for tests
}

type server struct {
	Name                string `json:"name"`
	Online              bool   `json:"online"`
	Players             *int   `json:"players,omitempty"`
	SinceRestartSeconds *int64 `json:"since_restart_seconds,omitempty"`
}

func Handler(o Options) http.Handler {
	if o.Now == nil {
		o.Now = time.Now
	}
	notes := readNotes(o.NotesFile)
	var mu sync.Mutex
	var checked time.Time
	var online bool
	var players int

	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/status", func(w http.ResponseWriter, _ *http.Request) {
		mu.Lock()
		now := o.Now()
		if checked.IsZero() || now.Sub(checked) >= cacheFor {
			// Not the request's context: one caller hanging up must not mark the world down for the others.
			ctx, cancel := context.WithTimeout(context.Background(), worldTimeout)
			list, err := o.Players.ListPlayers(ctx, &adminv1.ListPlayersRequest{})
			cancel()
			online = err == nil
			players = len(list.GetPlayers())
			checked = now
		}
		s := server{Name: o.WorldName, Online: online}
		if online {
			count := players
			s.Players = &count
			s.SinceRestartSeconds = sinceRestart(o.StartedFile, now)
		}
		mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string][]server{"servers": {s}})
	})
	mux.HandleFunc("GET /api/v1/changelog", func(w http.ResponseWriter, _ *http.Request) {
		if notes == "" {
			http.NotFound(w, nil)
			return
		}
		w.Header().Set("Content-Type", "text/markdown; charset=utf-8")
		_, _ = io.WriteString(w, notes)
	})
	return mux
}

func sinceRestart(file string, now time.Time) *int64 {
	if file == "" {
		return nil
	}
	text, err := os.ReadFile(file)
	if err != nil {
		return nil
	}
	started, err := time.Parse(time.RFC3339Nano, strings.TrimSpace(string(text)))
	if err != nil {
		return nil
	}
	seconds := int64(max(now.Sub(started), 0) / time.Second)
	return &seconds
}

func readNotes(file string) string {
	if file == "" {
		return ""
	}
	f, err := os.Open(file)
	if err != nil {
		slog.Warn("no patch notes for the launcher", "err", err)
		return ""
	}
	defer f.Close()
	text, err := io.ReadAll(io.LimitReader(f, maxNotes+1))
	if err != nil || len(text) > maxNotes {
		slog.Warn("patch notes not served: unreadable or too long", "file", file, "err", err)
		return ""
	}
	return string(text)
}
