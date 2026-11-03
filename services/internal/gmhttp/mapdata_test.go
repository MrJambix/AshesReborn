package gmhttp

import (
	"encoding/json"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"testing"
)

// With the real content.db (AOC_CONTENT_DB, as scripts\services.ps1 test sets it) the game's map records read as the client's map shows them.
func TestMapRecordsOfTheRealContent(t *testing.T) {
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		t.Skip("AOC_CONTENT_DB is not set")
	}
	data, err := NewMapRecords(path).Read()
	if err != nil {
		t.Fatal(err)
	}
	var joeva *MapNodeArt
	for i := range data.Nodes {
		if data.Nodes[i].Name == "Verra_RVR_Joeva" {
			joeva = &data.Nodes[i]
		}
	}
	if joeva == nil || joeva.Backdrop != "Joeva" || joeva.Title != "Joeva" || math.Abs(joeva.Yaw+67.374) > 0.01 || len(joeva.Icons) < 4 || joeva.Icons[0] != "TUI_Icon_Node_LVL0" {
		t.Fatalf("Joeva: %+v", joeva)
	}
	kinds := map[string]int{}
	found := map[string]MapPoint{}
	for _, p := range data.Points {
		kinds[p.Kind]++
		found[p.Name] = p
		if p.X == 0 && p.Y == 0 {
			t.Errorf("%s %s stands at the origin", p.Kind, p.Name)
		}
	}
	for _, k := range []string{"poi_major", "poi_minor", "location", "shrine", "dungeon"} {
		if kinds[k] == 0 {
			t.Errorf("no %s: %v", k, kinds)
		}
	}
	if p := found["RVR_HighwaymenHills"]; p.Title != "Highwaymen Hills" || p.Icon == "" || math.Abs(p.X+629043.94) > 1 || p.Box[0] != -657408 {
		t.Errorf("Highwaymen Hills: %+v", p)
	}
	if p := found["RVR_Aelan_Divine_Gate"]; p.Kind != "shrine" || p.Title != "Aelan Divine Gate" || p.Icon != "TUI_Icon_Map_ReviveShrine_Active" {
		t.Errorf("Aelan Divine Gate: %+v", p)
	}
	if p := found["NodePlayground_Devon_Test_POI"]; p.Name != "" {
		t.Errorf("a POI of another level is on the map: %+v", p)
	}
	titles := []string{}
	for _, g := range data.Legend {
		titles = append(titles, g.Title)
	}
	if len(data.Legend) < 6 || data.Legend[1].Title != "Points of Interest" || data.Legend[1].Entries[0].Kind != "node" {
		t.Errorf("legend: %v", titles)
	}
}

func TestMapDataRoute(t *testing.T) {
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		t.Skip("AOC_CONTENT_DB is not set")
	}
	h := Map(fakeTokens{}, fakeGrants(3), NewLiveMap(&fakeStream{}), &nodePlaces{}, fakeNodes{}, t.TempDir(), NewMapRecords(path))
	r := httptest.NewRequest("GET", "/api/v1/gm/mapdata", nil)
	r.Header.Set("Authorization", "Bearer valid-login")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != http.StatusOK {
		t.Fatalf("status %d: %s", w.Code, w.Body)
	}
	var got MapData
	if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil || len(got.Nodes) == 0 || len(got.Points) == 0 {
		t.Fatalf("%v: %d nodes, %d points", err, len(got.Nodes), len(got.Points))
	}
}
