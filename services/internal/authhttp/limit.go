// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package authhttp

import (
	"net"
	"net/http"
	"sync"
	"time"
)

// Limits for a login API that faces the internet. A bcrypt comparison is costly on purpose, so Busy
// also keeps a flood from taking every processor of the machine.
type Limits struct {
	LoginsPerMinute  float64 // per address, with this many at once
	RegistersPerHour float64 // per address, with this many at once
	Busy             int     // password checks at the same time
	BusyWait         time.Duration
	Now              func() time.Time // for tests
}

// DefaultLimits suit a small server: a typo or two never trips them, and guessing passwords is slow.
func DefaultLimits() Limits {
	return Limits{LoginsPerMinute: 10, RegistersPerHour: 5, Busy: 1, BusyWait: 5 * time.Second, Now: time.Now}
}

type bucket struct {
	tokens float64
	at     time.Time
}

// maxTracked is where the buckets are thrown away: a flood from many addresses must not grow the map for ever.
const maxTracked = 100000

// Limit wraps a Handler for the public listener: over the limit is a 429 in the API's own error format.
func Limit(next http.Handler, l Limits) http.Handler {
	if l.Now == nil {
		l.Now = time.Now
	}
	var mu sync.Mutex
	logins := map[string]*bucket{}
	registers := map[string]*bucket{}
	busy := make(chan struct{}, max(l.Busy, 1))

	take := func(m map[string]*bucket, ip string, burst, perSecond float64) bool {
		mu.Lock()
		defer mu.Unlock()
		if len(m) >= maxTracked {
			clear(m)
		}
		now := l.Now()
		b := m[ip]
		if b == nil {
			b = &bucket{tokens: burst, at: now}
			m[ip] = b
		}
		b.tokens = min(burst, b.tokens+now.Sub(b.at).Seconds()*perSecond)
		b.at = now
		if b.tokens < 1 {
			return false
		}
		b.tokens--
		return true
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ip, _, err := net.SplitHostPort(r.RemoteAddr)
		if err != nil {
			ip = r.RemoteAddr
		}
		var allowed bool
		switch r.URL.Path {
		case "/api/v1/auth/login":
			allowed = take(logins, ip, l.LoginsPerMinute, l.LoginsPerMinute/60)
		case "/api/v1/auth/register":
			allowed = take(registers, ip, l.RegistersPerHour, l.RegistersPerHour/3600)
		default:
			next.ServeHTTP(w, r)
			return
		}
		if !allowed {
			writeError(w, http.StatusTooManyRequests, "rate_limited", "Too many tries. Please wait a while and try again.")
			return
		}
		timer := time.NewTimer(l.BusyWait)
		defer timer.Stop()
		select {
		case busy <- struct{}{}:
			defer func() { <-busy }()
			next.ServeHTTP(w, r)
		case <-timer.C:
			writeError(w, http.StatusServiceUnavailable, "server_error", "The login server is busy; try again.")
		case <-r.Context().Done():
		}
	})
}
