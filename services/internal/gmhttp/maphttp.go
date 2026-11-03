package gmhttp

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
)

const (
	mapBurst     = 10.0
	mapPerSecond = 4.0
)

// MapNode is a city node on the map: where the places list puts it, and its level from the node service (Level -1 when that is not answering).
type MapNode struct {
	Name        string  `json:"name"`
	DisplayName string  `json:"display_name"`
	X           float64 `json:"x"`
	Y           float64 `json:"y"`
	Z           float64 `json:"z"`
	Level       int32   `json:"level"`
	LevelName   string  `json:"level_name,omitempty"`
	Percent     uint32  `json:"percent"`
	Experience  int64   `json:"experience,string"`
	ToNext      int64   `json:"to_next,string"`
	Parent      string  `json:"parent,omitempty"` // the display name of the node it is a vassal of
}

// minimapFile is what GET /api/v1/gm/minimap/ may name: mapc minimap's files and nothing else (tiles r_ and l_, node backdrops n_, icons i_).
var minimapFile = regexp.MustCompile(`^(minimap\.json|overview\.jpg|[rl]_x-?\d+_y-?\d+\.jpg|n_[A-Za-z]+_L\d\.jpg|i_[A-Za-z0-9_]+\.png)$`)

// Map serves GET /api/v1/gm/map (the dots of the live map), GET /api/v1/gm/nodes (the city nodes with their levels),
// GET /api/v1/gm/mapdata (the game's map records: POIs, places, shrines, the nodes' icons and backdrops, the legend) and
// GET /api/v1/gm/minimap/{file} (the game's own map pictures, which `mapc minimap` writes to minimapDir from the player's install). Reads
// only, so the account's grant is enough. The map and the nodes have their own budget, so that a map polling every second leaves
// the other reads theirs; the pictures and the records have none, since a view asks for a few dozen pictures at once and the records are read once.
func Map(tokens Tokens, grants Grants, live *LiveMap, world WorldReads, nodes NodeReads, minimapDir string, records *MapRecords) http.Handler {
	var mu sync.Mutex
	left, at := mapBurst, time.Now()
	take := func() bool {
		mu.Lock()
		defer mu.Unlock()
		now := time.Now()
		left = min(mapBurst, left+now.Sub(at).Seconds()*mapPerSecond)
		at = now
		if left < 1 {
			return false
		}
		left--
		return true
	}
	mux := http.NewServeMux()
	admit := func(w http.ResponseWriter, r *http.Request) (context.Context, context.CancelFunc, bool) {
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		if _, code, message := authorize(ctx, r, tokens, grants); code != 0 {
			readError(w, code, message)
			return ctx, cancel, false
		}
		if !take() {
			readError(w, 429, "Please wait before the next GM request.")
			return ctx, cancel, false
		}
		return ctx, cancel, true
	}
	mux.HandleFunc("GET /api/v1/gm/map", func(w http.ResponseWriter, r *http.Request) {
		_, cancel, ok := admit(w, r)
		defer cancel()
		if !ok {
			return
		}
		kinds, box, problem := mapQuery(r)
		if problem != "" {
			readError(w, 400, problem)
			return
		}
		_ = json.NewEncoder(w).Encode(live.Read(kinds, box))
	})
	mux.HandleFunc("GET /api/v1/gm/minimap/{file}", func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		if _, code, message := authorize(ctx, r, tokens, grants); code != 0 {
			readError(w, code, message)
			return
		}
		name := r.PathValue("file")
		if !minimapFile.MatchString(name) {
			readError(w, 404, "Unknown map file.")
			return
		}
		f, err := os.Open(filepath.Join(minimapDir, name))
		if err != nil {
			readError(w, 404, "The game's map is not on this server. Run mapc minimap.")
			return
		}
		defer f.Close()
		info, err := f.Stat()
		if err != nil {
			readError(w, 404, "The game's map is not on this server. Run mapc minimap.")
			return
		}
		switch {
		case strings.HasSuffix(name, ".png"):
			w.Header().Set("Content-Type", "image/png")
		case strings.HasSuffix(name, ".jpg"):
			w.Header().Set("Content-Type", "image/jpeg")
		}
		w.Header().Set("Cache-Control", "private, max-age=86400") // the pictures change only with a new export
		http.ServeContent(w, r, name, info.ModTime(), f)
	})
	mux.HandleFunc("GET /api/v1/gm/mapdata", func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		if _, code, message := authorize(ctx, r, tokens, grants); code != 0 {
			readError(w, code, message)
			return
		}
		if records == nil {
			readError(w, 503, "The game's map records are not on this server.")
			return
		}
		data, err := records.Read()
		if err != nil {
			readError(w, 503, "The game's map records could not be read: "+err.Error())
			return
		}
		_ = json.NewEncoder(w).Encode(data)
	})
	mux.HandleFunc("GET /api/v1/gm/nodes", func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel, ok := admit(w, r)
		defer cancel()
		if !ok {
			return
		}
		places, err := world.ListPlaces(ctx, &adminv1.ListPlacesRequest{})
		if err != nil {
			readError(w, 503, "The world server is unavailable.")
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"nodes": mergeNodes(ctx, places.GetPlaces(), nodes)})
	})
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Type", "application/json")
		if _, pattern := mux.Handler(r); pattern != "" {
			mux.ServeHTTP(w, r) // through the mux, which fills in r.PathValue
			return
		}
		readError(w, 404, "Unknown GM endpoint.")
	})
}

// mapQuery reads ?kinds=1,3 (EntityKind numbers) and ?box=minx,miny,maxx,maxy (cm).
func mapQuery(r *http.Request) (map[int32]bool, *adminv1.AreaBox, string) {
	q := r.URL.Query()
	kinds := map[int32]bool{}
	if k := q.Get("kinds"); k != "" {
		for _, word := range strings.Split(k, ",") {
			n, err := strconv.Atoi(word)
			if err != nil || n < 1 || n > 5 {
				return nil, nil, "kinds are 1 to 5 (player, NPC, resource node, mount, loot)."
			}
			kinds[int32(n)] = true
		}
	}
	if b := q.Get("box"); b != "" {
		parts := strings.Split(b, ",")
		if len(parts) != 4 {
			return nil, nil, "box is min x, min y, max x, max y in cm."
		}
		var v [4]int32
		for i, p := range parts {
			n, err := strconv.ParseInt(p, 10, 32)
			if err != nil {
				return nil, nil, "box is min x, min y, max x, max y in cm."
			}
			v[i] = int32(n)
		}
		return kinds, &adminv1.AreaBox{MinXCm: v[0], MinYCm: v[1], MaxXCm: v[2], MaxYCm: v[3]}, ""
	}
	return kinds, nil, ""
}

// mergeNodes puts the node service's levels on the places that are nodes. A node service that does not answer leaves the levels at -1.
func mergeNodes(ctx context.Context, places []*adminv1.Place, nodes NodeReads) []MapNode {
	byName := map[string]*citynodev1.Node{}
	byGuid := map[int64]*citynodev1.Node{}
	if nodes != nil {
		if list, err := nodes.ListNodes(ctx, &citynodev1.ListNodesRequest{}); err == nil {
			for _, n := range list.GetNodes() {
				byName[strings.ToLower(n.GetName())] = n
				byGuid[n.GetGuid()] = n
			}
		}
	}
	out := []MapNode{}
	for _, p := range places {
		if p.GetKind() != "node" {
			continue
		}
		pos := p.GetPosition()
		m := MapNode{Name: p.GetName(), DisplayName: p.GetDisplayName(), X: pos.GetX(), Y: pos.GetY(), Z: pos.GetZ(), Level: -1}
		if n, ok := byName[strings.ToLower(p.GetName())]; ok {
			m.Level, m.LevelName, m.Percent, m.Experience, m.ToNext = int32(n.GetLevel()), n.GetLevelName(), n.GetPercent(), n.GetExperience(), n.GetExperienceToNextLevel()
			if parent, ok := byGuid[n.GetParentGuid()]; ok && n.GetParentGuid() != 0 {
				m.Parent = parent.GetDisplayName()
			}
		}
		out = append(out, m)
	}
	return out
}
