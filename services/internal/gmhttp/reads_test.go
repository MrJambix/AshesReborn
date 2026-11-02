package gmhttp

import (
	"context"
	"net/http/httptest"
	"strings"
	"testing"

	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

type fakeReads struct {
	called    bool
	inspected uint64
	query     string
	err       error
}

func (f *fakeReads) ListPlayers(context.Context, *adminv1.ListPlayersRequest, ...grpc.CallOption) (*adminv1.ListPlayersResponse, error) {
	f.called = true
	return &adminv1.ListPlayersResponse{Players: []*adminv1.Player{{Name: "Alt"}}}, f.err
}
func (f *fakeReads) ListPlaces(context.Context, *adminv1.ListPlacesRequest, ...grpc.CallOption) (*adminv1.ListPlacesResponse, error) {
	f.called = true
	return &adminv1.ListPlacesResponse{}, f.err
}
func (f *fakeReads) GetStatus(context.Context, *adminv1.GetStatusRequest, ...grpc.CallOption) (*adminv1.GetStatusResponse, error) {
	f.called = true
	return &adminv1.GetStatusResponse{Tick: 9}, f.err
}
func (f *fakeReads) Search(_ context.Context, r *adminv1.SearchRequest, _ ...grpc.CallOption) (*adminv1.SearchResponse, error) {
	f.called, f.query = true, r.Query
	return &adminv1.SearchResponse{}, f.err
}
func (f *fakeReads) Inspect(_ context.Context, r *adminv1.InspectRequest, _ ...grpc.CallOption) (*adminv1.InspectResponse, error) {
	f.called, f.inspected = true, r.Id
	return &adminv1.InspectResponse{}, f.err
}

func TestReadsAccountBoundary(t *testing.T) {
	for _, tc := range []struct {
		name, method, path, token string
		level                     int32
		want                      int
		called                    bool
		err                       error
		body                      string
	}{
		{name: "missing login", method: "GET", path: "/api/v1/gm/players", level: 3, want: 401},
		{name: "expired login", method: "GET", path: "/api/v1/gm/status", token: "expired", level: 3, want: 401},
		{name: "no grant", method: "GET", path: "/api/v1/gm/players", token: "valid-login", want: 403},
		{name: "no grant on inspect", method: "GET", path: "/api/v1/gm/inspect?id=7", token: "valid-login", want: 403},
		{name: "players", method: "GET", path: "/api/v1/gm/players", token: "valid-login", level: 3, want: 200, called: true, body: `"Alt"`},
		{name: "places", method: "GET", path: "/api/v1/gm/places", token: "valid-login", level: 3, want: 200, called: true},
		{name: "status", method: "GET", path: "/api/v1/gm/status", token: "valid-login", level: 3, want: 200, called: true, body: `"tick"`},
		{name: "search", method: "GET", path: "/api/v1/gm/search?q=wolf", token: "valid-login", level: 3, want: 200, called: true},
		{name: "overlong search", method: "GET", path: "/api/v1/gm/search?q=" + strings.Repeat("a", 129), token: "valid-login", level: 3, want: 400},
		{name: "inspect", method: "GET", path: "/api/v1/gm/inspect?id=7", token: "valid-login", level: 3, want: 200, called: true},
		{name: "bad id", method: "GET", path: "/api/v1/gm/inspect?id=wolf", token: "valid-login", level: 3, want: 400},
		{name: "zero id", method: "GET", path: "/api/v1/gm/inspect?id=0", token: "valid-login", level: 3, want: 400},
		{name: "gone", method: "GET", path: "/api/v1/gm/inspect?id=7", token: "valid-login", level: 3, want: 404, called: true, err: status.Error(codes.NotFound, "internal detail")},
		{name: "world down", method: "GET", path: "/api/v1/gm/players", token: "valid-login", level: 3, want: 503, called: true, err: status.Error(codes.Unavailable, "internal detail")},
		{name: "no writes", method: "POST", path: "/api/v1/gm/players", token: "valid-login", level: 3, want: 404},
		{name: "unknown read", method: "GET", path: "/api/v1/gm/secrets", token: "valid-login", level: 3, want: 404},
		{name: "catalog needs a login", method: "GET", path: "/api/v1/gm/catalog?kind=item", level: 3, want: 401},
		{name: "catalog needs the grant", method: "GET", path: "/api/v1/gm/catalog?kind=item", token: "valid-login", want: 403},
		{name: "catalog", method: "GET", path: "/api/v1/gm/catalog?kind=item&q=sword", token: "valid-login", level: 3, want: 200, body: `"value":"6064633867184242918"`},
		{name: "unknown catalog", method: "GET", path: "/api/v1/gm/catalog?kind=accounts", token: "valid-login", level: 3, want: 400},
	} {
		t.Run(tc.name, func(t *testing.T) {
			fake := &fakeReads{err: tc.err}
			req := httptest.NewRequest(tc.method, tc.path, nil)
			if tc.token != "" {
				req.Header.Set("Authorization", "Bearer "+tc.token)
			}
			reply := httptest.NewRecorder()
			Reads(fakeTokens{}, fakeGrants(tc.level), fake, fake, testCatalog(t)).ServeHTTP(reply, req)
			if reply.Code != tc.want || fake.called != tc.called {
				t.Fatalf("status %d, world called %v: %s", reply.Code, fake.called, reply.Body.String())
			}
			if reply.Header().Get("Cache-Control") != "no-store" {
				t.Fatal("GM state must not be cached")
			}
			if !strings.Contains(reply.Body.String(), tc.body) {
				t.Fatalf("body %s lacks %s", reply.Body.String(), tc.body)
			}
			if strings.Contains(reply.Body.String(), "internal detail") {
				t.Fatalf("world error leaked: %s", reply.Body.String())
			}
		})
	}
}

func TestReadsBudget(t *testing.T) {
	handler := Reads(fakeTokens{}, fakeGrants(3), &fakeReads{}, &fakeReads{}, testCatalog(t))
	limited := false
	for range 100 {
		req := httptest.NewRequest("GET", "/api/v1/gm/players", nil)
		req.Header.Set("Authorization", "Bearer valid-login")
		reply := httptest.NewRecorder()
		handler.ServeHTTP(reply, req)
		limited = limited || reply.Code == 429
	}
	if !limited {
		t.Fatal("a flood of reads must be refused")
	}
}

func TestSite(t *testing.T) {
	for _, path := range []string{"/gm/", "/gm/commands.js", "/gm/app.js", "/gm/tools.js", "/gm/style.css"} {
		reply := httptest.NewRecorder()
		Site().ServeHTTP(reply, httptest.NewRequest("GET", path, nil))
		if reply.Code != 200 || reply.Body.Len() == 0 {
			t.Fatalf("%s: status %d", path, reply.Code)
		}
		csp := reply.Header().Get("Content-Security-Policy")
		if !strings.Contains(csp, "script-src 'self'") || !strings.Contains(csp, "frame-ancestors 'none'") {
			t.Fatalf("%s: weak policy %q", path, csp)
		}
	}
	reply := httptest.NewRecorder()
	Site().ServeHTTP(reply, httptest.NewRequest("POST", "/gm/", nil))
	if reply.Code != 405 {
		t.Fatalf("POST: status %d", reply.Code)
	}
}
