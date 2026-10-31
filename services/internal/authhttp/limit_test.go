// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package authhttp

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestLimit(t *testing.T) {
	now := time.Unix(1000, 0)
	l := DefaultLimits()
	l.Now = func() time.Time { return now }
	h := Limit(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusOK) }), l)
	send := func(path, addr string) int {
		r := httptest.NewRequest(http.MethodPost, path, nil)
		r.RemoteAddr = addr
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w.Code
	}
	const login, register = "/api/v1/auth/login", "/api/v1/auth/register"

	for i := 0; i < 10; i++ {
		if c := send(login, "1.1.1.1:5"); c != http.StatusOK {
			t.Fatalf("login %d: %d", i, c)
		}
	}
	if c := send(login, "1.1.1.1:6"); c != http.StatusTooManyRequests {
		t.Fatalf("the 11th login, from another port of the address: %d", c)
	}
	if c := send(login, "2.2.2.2:5"); c != http.StatusOK {
		t.Fatalf("another address is not limited: %d", c)
	}
	now = now.Add(6 * time.Second)
	if c := send(login, "1.1.1.1:5"); c != http.StatusOK {
		t.Fatalf("one login comes back after six seconds: %d", c)
	}
	if c := send(login, "1.1.1.1:5"); c != http.StatusTooManyRequests {
		t.Fatalf("and only one: %d", c)
	}

	for i := 0; i < 5; i++ {
		if c := send(register, "3.3.3.3:5"); c != http.StatusOK {
			t.Fatalf("register %d: %d", i, c)
		}
	}
	if c := send(register, "3.3.3.3:5"); c != http.StatusTooManyRequests {
		t.Fatalf("the 6th register: %d", c)
	}
	if c := send(login, "3.3.3.3:5"); c != http.StatusOK {
		t.Fatalf("registers do not use up logins: %d", c)
	}
	if c := send("/health", "1.1.1.1:5"); c != http.StatusOK {
		t.Fatalf("health is not limited: %d", c)
	}
}

func TestLimitBusy(t *testing.T) {
	l := DefaultLimits()
	l.BusyWait = 20 * time.Millisecond
	entered, release := make(chan struct{}), make(chan struct{})
	h := Limit(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		entered <- struct{}{}
		<-release
		w.WriteHeader(http.StatusOK)
	}), l)
	send := func(addr string) *httptest.ResponseRecorder {
		r := httptest.NewRequest(http.MethodPost, "/api/v1/auth/login", nil)
		r.RemoteAddr = addr
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w
	}
	first := make(chan int)
	go func() { first <- send("1.1.1.1:5").Code }()
	<-entered
	if c := send("2.2.2.2:5").Code; c != http.StatusServiceUnavailable {
		t.Fatalf("a second password check while one runs: %d", c)
	}
	close(release)
	if c := <-first; c != http.StatusOK {
		t.Fatalf("the first: %d", c)
	}
}
