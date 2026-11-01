package gmhttp

import (
	"context"
	"errors"
	"net/http/httptest"
	"strings"
	"testing"

	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/account"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

type fakeTokens struct{}

func (fakeTokens) ValidateToken(_ context.Context, token string) (account.Account, error) {
	if token != "valid-login" {
		return account.Account{}, errors.New("expired")
	}
	return account.Account{ID: 42, Username: "Jambix"}, nil
}

type fakeGrants int32

func (g fakeGrants) GMLevel(_ context.Context, id int64) (int32, error) {
	if id != 42 {
		panic("untrusted account ID")
	}
	return int32(g), nil
}

type fakeWorld struct {
	called bool
	err    error
}

func (f *fakeWorld) AccountCommand(_ context.Context, r *adminv1.AccountCommandRequest, _ ...grpc.CallOption) (*adminv1.AccountCommandResponse, error) {
	if r.AccountId != 42 {
		panic("account ID must come from the login token")
	}
	f.called = true
	return &adminv1.AccountCommandResponse{Enabled: true, Character: "AltName", Level: 3, Message: "done"}, f.err
}

func TestGMHTTPAccountBoundary(t *testing.T) {
	for _, tc := range []struct {
		name, method, token, body string
		level                     int32
		want                      int
		called                    bool
		err                       error
	}{
		{name: "missing login", method: "GET", level: 3, want: 401},
		{name: "expired login", method: "GET", token: "expired", level: 3, want: 401},
		{name: "matching username without grant", method: "POST", token: "valid-login", body: `{"command":"on"}`, want: 403},
		{name: "account ID spoof", method: "POST", token: "valid-login", body: `{"command":"on","account_id":1}`, level: 3, want: 400},
		{name: "trailing request", method: "POST", token: "valid-login", body: `{"command":"on"} {}`, level: 3, want: 400},
		{name: "overlong command", method: "POST", token: "valid-login", body: `{"command":"` + strings.Repeat("a", 1025) + `"}`, level: 3, want: 400},
		{name: "owner poll", method: "GET", token: "valid-login", level: 3, want: 200, called: true},
		{name: "owner command", method: "POST", token: "valid-login", body: `{"command":".GM speed self 2"}`, level: 3, want: 200, called: true},
		{name: "world checks disabled mode", method: "POST", token: "valid-login", body: `{"command":"money self 100"}`, level: 3, want: 403, called: true, err: status.Error(codes.PermissionDenied, "off")},
	} {
		t.Run(tc.name, func(t *testing.T) {
			world := &fakeWorld{err: tc.err}
			handler := Handler(fakeTokens{}, fakeGrants(tc.level), world)
			req := httptest.NewRequest(tc.method, "/api/v1/gm", strings.NewReader(tc.body))
			if tc.token != "" {
				req.Header.Set("Authorization", "Bearer "+tc.token)
			}
			reply := httptest.NewRecorder()
			handler.ServeHTTP(reply, req)
			if reply.Code != tc.want || world.called != tc.called {
				t.Fatalf("status %d, world called %v: %s", reply.Code, world.called, reply.Body.String())
			}
			if reply.Header().Get("Cache-Control") != "no-store" {
				t.Fatal("GM state must not be cached")
			}
		})
	}
}
