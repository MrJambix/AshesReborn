package gmhttp

import (
	"context"
	"encoding/json"
	"io"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/metadata"
)

// fakeStream sends its frames, then blocks until the stream's context ends.
type fakeStream struct {
	frames []*adminv1.SubscribeResponse
	asked  *adminv1.SubscribeRequest
	opened int
}

func (f *fakeStream) Subscribe(ctx context.Context, r *adminv1.SubscribeRequest, _ ...grpc.CallOption) (grpc.ServerStreamingClient[adminv1.SubscribeResponse], error) {
	f.asked = r
	f.opened++
	return &frameReader{ctx: ctx, frames: f.frames}, nil
}

type frameReader struct {
	grpc.ClientStream
	ctx    context.Context
	frames []*adminv1.SubscribeResponse
}

func (r *frameReader) Recv() (*adminv1.SubscribeResponse, error) {
	if len(r.frames) == 0 {
		<-r.ctx.Done()
		return nil, io.EOF
	}
	f := r.frames[0]
	r.frames = r.frames[1:]
	return f, nil
}
func (r *frameReader) Header() (metadata.MD, error) { return nil, nil }

func snapshot(seq uint64, entities ...*adminv1.Entity) *adminv1.SubscribeResponse {
	return &adminv1.SubscribeResponse{Seq: seq, Body: &adminv1.SubscribeResponse_Snapshot{Snapshot: &adminv1.EntitySnapshot{Entities: entities}}}
}
func delta(seq uint64, removed []uint64, changed ...*adminv1.Entity) *adminv1.SubscribeResponse {
	return &adminv1.SubscribeResponse{Seq: seq, Body: &adminv1.SubscribeResponse_Delta{Delta: &adminv1.EntityDelta{Changed: changed, Removed: removed}}}
}

func waitFor(t *testing.T, m *LiveMap, ok func(MapFrame) bool) MapFrame {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		f := m.Read(nil, nil)
		if ok(f) {
			return f
		}
		if time.Now().After(deadline) {
			t.Fatalf("map never got there: %+v", f)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestLiveMapFollowsTheStream(t *testing.T) {
	player := adminv1.EntityKind_ENTITY_KIND_PLAYER
	npc := adminv1.EntityKind_ENTITY_KIND_NPC
	stream := &fakeStream{frames: []*adminv1.SubscribeResponse{
		snapshot(1, &adminv1.Entity{Id: 10, Kind: player, XCm: 100, YCm: 200, Name: "YurHealz"}, &adminv1.Entity{Id: 11, Kind: npc, XCm: -50, YCm: 5}),
		// the player moves (no name in an update), the NPC leaves, a node appears
		delta(2, []uint64{11}, &adminv1.Entity{Id: 10, Kind: player, XCm: 400, YCm: 200}, &adminv1.Entity{Id: 12, Kind: adminv1.EntityKind_ENTITY_KIND_RESOURCE_NODE, XCm: 9, YCm: 9}),
	}}
	m := NewLiveMap(stream)
	f := waitFor(t, m, func(f MapFrame) bool { return f.Seq == 2 })
	if !f.Live || len(f.Entities) != 2 {
		t.Fatalf("frame %+v", f)
	}
	if stream.asked.GetArea().GetMaxXCm() != verraArea.MaxXCm {
		t.Fatalf("subscribed to %v", stream.asked.GetArea())
	}
	players := m.Read(map[int32]bool{1: true}, nil)
	if len(players.Entities) != 1 || players.Entities[0].X != 400 || players.Entities[0].Name != "YurHealz" {
		t.Fatalf("the player should have moved and kept the name: %+v", players.Entities)
	}
	inside := m.Read(nil, &adminv1.AreaBox{MinXCm: 0, MinYCm: 0, MaxXCm: 100, MaxYCm: 100})
	if len(inside.Entities) != 1 || inside.Entities[0].ID != 12 {
		t.Fatalf("box: %+v", inside.Entities)
	}
}

func TestLiveMapStartsAgainAfterAGap(t *testing.T) {
	stream := &fakeStream{frames: []*adminv1.SubscribeResponse{snapshot(1), delta(5, nil)}}
	m := NewLiveMap(stream)
	f := waitFor(t, m, func(f MapFrame) bool { return f.Problem != "" })
	if f.Live {
		t.Fatal("a stream that skipped a frame is not live")
	}
}

type fakeNodes struct{ err error }

func (f fakeNodes) ListNodes(context.Context, *citynodev1.ListNodesRequest, ...grpc.CallOption) (*citynodev1.ListNodesResponse, error) {
	return &citynodev1.ListNodesResponse{Nodes: []*citynodev1.Node{
		{Guid: 1, Name: "Verra_RVR_Joeva", DisplayName: "Joeva", Level: 3, LevelName: "Village", Percent: 40},
		{Guid: 2, Name: "Verra_RVR_Halcyon", DisplayName: "Halcyon", Level: 1, ParentGuid: 1},
	}}, f.err
}

type nodePlaces struct{ fakeReads }

func (nodePlaces) ListPlaces(context.Context, *adminv1.ListPlacesRequest, ...grpc.CallOption) (*adminv1.ListPlacesResponse, error) {
	return &adminv1.ListPlacesResponse{Places: []*adminv1.Place{
		{Name: "Verra_RVR_Joeva", DisplayName: "Joeva", Kind: "node", Position: &adminv1.Position{X: 1, Y: 2}},
		{Name: "verra_rvr_halcyon", DisplayName: "Halcyon", Kind: "node", Position: &adminv1.Position{X: 3, Y: 4}},
		{Name: "login", Kind: "login"},
	}}, nil
}

func TestMapRoutes(t *testing.T) {
	handler := Map(fakeTokens{}, fakeGrants(3), NewLiveMap(&fakeStream{frames: []*adminv1.SubscribeResponse{snapshot(1)}}), &nodePlaces{}, fakeNodes{}, t.TempDir(), nil)
	get := func(path, token string) *httptest.ResponseRecorder {
		req := httptest.NewRequest("GET", path, nil)
		if token != "" {
			req.Header.Set("Authorization", "Bearer "+token)
		}
		reply := httptest.NewRecorder()
		handler.ServeHTTP(reply, req)
		return reply
	}
	if r := get("/api/v1/gm/map", ""); r.Code != 401 {
		t.Fatalf("map without a login: %d", r.Code)
	}
	if r := Map(fakeTokens{}, fakeGrants(0), NewLiveMap(&fakeStream{}), &nodePlaces{}, fakeNodes{}, t.TempDir(), nil); true {
		req := httptest.NewRequest("GET", "/api/v1/gm/nodes", nil)
		req.Header.Set("Authorization", "Bearer valid-login")
		reply := httptest.NewRecorder()
		r.ServeHTTP(reply, req)
		if reply.Code != 403 {
			t.Fatalf("nodes without the grant: %d", reply.Code)
		}
	}
	for _, bad := range []string{"?kinds=9", "?kinds=x", "?box=1,2,3", "?box=a,b,c,d"} {
		if r := get("/api/v1/gm/map"+bad, "valid-login"); r.Code != 400 {
			t.Fatalf("%s: %d", bad, r.Code)
		}
	}
	if r := get("/api/v1/gm/map?kinds=1,3&box=-100,-100,100,100", "valid-login"); r.Code != 200 || !strings.Contains(r.Body.String(), `"entities":[`) {
		t.Fatalf("map: %d %s", r.Code, r.Body.String())
	}
	r := get("/api/v1/gm/nodes", "valid-login")
	var body struct{ Nodes []MapNode }
	if r.Code != 200 || json.Unmarshal(r.Body.Bytes(), &body) != nil || len(body.Nodes) != 2 {
		t.Fatalf("nodes: %d %s", r.Code, r.Body.String())
	}
	if body.Nodes[0].Level != 3 || body.Nodes[0].LevelName != "Village" || body.Nodes[1].Parent != "Joeva" || body.Nodes[1].X != 3 {
		t.Fatalf("nodes: %+v", body.Nodes)
	}
}

func TestMinimapFiles(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "overview.jpg"), []byte("JPEG fake"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "secret.txt"), []byte("no"), 0o644); err != nil {
		t.Fatal(err)
	}
	handler := Map(fakeTokens{}, fakeGrants(3), NewLiveMap(&fakeStream{}), &nodePlaces{}, fakeNodes{}, dir, nil)
	get := func(path, token string) *httptest.ResponseRecorder {
		req := httptest.NewRequest("GET", path, nil)
		if token != "" {
			req.Header.Set("Authorization", "Bearer "+token)
		}
		reply := httptest.NewRecorder()
		handler.ServeHTTP(reply, req)
		return reply
	}
	if r := get("/api/v1/gm/minimap/overview.jpg", ""); r.Code != 401 {
		t.Fatalf("a picture without a login: %d", r.Code)
	}
	r := get("/api/v1/gm/minimap/overview.jpg", "valid-login")
	if r.Code != 200 || r.Header().Get("Content-Type") != "image/jpeg" || !strings.HasPrefix(r.Header().Get("Cache-Control"), "private") || r.Body.String() != "JPEG fake" {
		t.Fatalf("overview: %d %q %q", r.Code, r.Header().Get("Content-Type"), r.Header().Get("Cache-Control"))
	}
	for _, bad := range []string{"secret.txt", "..%2Fsecret.txt", "s250000_x1_y1.png", "overview.png", "l_x1_y1.png", "n_../x_L1.jpg", "i_a.b.png", "minimap.json"} {
		if r := get("/api/v1/gm/minimap/"+bad, "valid-login"); r.Code != 404 {
			t.Fatalf("%s: %d", bad, r.Code)
		}
	}
}

func TestMapNodesWithoutTheNodeService(t *testing.T) {
	got := mergeNodes(context.Background(), []*adminv1.Place{{Name: "Verra_RVR_Joeva", Kind: "node", Position: &adminv1.Position{}}}, fakeNodes{err: io.EOF})
	if len(got) != 1 || got[0].Level != -1 {
		t.Fatalf("%+v", got)
	}
}
