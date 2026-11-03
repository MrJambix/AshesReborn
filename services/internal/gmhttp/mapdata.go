package gmhttp

import (
	"bytes"
	"compress/gzip"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"regexp"
	"sort"
	"strings"
	"sync"
)

// MapPoint is a mark of the game's own map: a POI, a dungeon, a named place, a shrine, a quest giver or a vendor, where the game's records put it.
type MapPoint struct {
	Kind  string     `json:"k"`              // legend key: poi_major, poi_minor, dungeon, landmark, location, special, shrine, quest, vendor
	Name  string     `json:"name"`           // the record's name
	Title string     `json:"title"`          // what the player sees
	Note  string     `json:"note,omitempty"` // the game's description, or the vendor's or quest's record
	X     float64    `json:"x"`              // cm
	Y     float64    `json:"y"`              // cm
	Z     float64    `json:"z"`              // cm
	Icon  string     `json:"icon,omitempty"` // the icon's texture (mapc minimap writes it as i_<icon>.png)
	Size  int        `json:"size,omitempty"` // the icon's size on the game's map, px
	Box   [4]float64 `json:"box,omitzero"`   // min x, min y, max x, max y (cm): a named place's extent
}

// MapNodeArt is what the game's map draws for a city node: its icon at each level, and the backdrop of its town (mapc minimap: n_<Backdrop>_L<level>.jpg),
// centred on the node and turned by its yaw.
type MapNodeArt struct {
	Name     string   `json:"name"`
	Title    string   `json:"title"`
	Type     string   `json:"type"`
	X        float64  `json:"x"`
	Y        float64  `json:"y"`
	Yaw      float64  `json:"yaw"`                // degrees
	Icons    []string `json:"icons"`              // by node level, from 0
	Backdrop string   `json:"backdrop,omitempty"` // the town's picture set, by level
}

// MapLegendEntry is one line of the game's map legend; Kind is the MapPoint.Kind it shows, or "" for a kind this map has no places of.
type MapLegendEntry struct {
	Key   string `json:"key"`
	Title string `json:"title"`
	Icon  string `json:"icon"`
	Kind  string `json:"kind,omitempty"`
}

type MapLegendGroup struct {
	Title   string           `json:"title"`
	Entries []MapLegendEntry `json:"entries"`
}

// MapData is the game's map as its records hold it (content.db, read once at the first call): GET /api/v1/gm/mapdata.
type MapData struct {
	Nodes  []MapNodeArt     `json:"nodes"`
	Points []MapPoint       `json:"points"`
	Legend []MapLegendGroup `json:"legend"`
}

// MapRecords reads MapData from content.db once. Like Catalog, it asks for no schema version: a content.db it cannot read costs the map its marks, not the
// login service.
type MapRecords struct {
	path string
	once sync.Once
	data MapData
	err  error
}

func NewMapRecords(path string) *MapRecords { return &MapRecords{path: path} }

func (m *MapRecords) Read() (MapData, error) {
	m.once.Do(func() { m.data, m.err = readMapRecords(m.path) })
	return m.data, m.err
}

const verraLevel = "Verra_World_Master"

type recRef struct {
	Guid int64 `json:"Guid"`
}
type recText struct {
	Source string `json:"Source"`
}
type recVec struct{ X, Y, Z float64 }
type recBox struct {
	IsValid bool   `json:"IsValid"`
	Min     recVec `json:"Min"`
	Max     recVec `json:"Max"`
}
type iconVisual struct {
	Name string
	Icon string
	Size int
}

// leaf is the texture of "/Game/UI/Icons/Map/TUI_X.TUI_X": TUI_X.
func leaf(path string) string {
	if i := strings.LastIndexByte(path, '.'); i >= 0 {
		return path[i+1:]
	}
	return path[strings.LastIndexByte(path, '/')+1:]
}

func readMapRecords(path string) (MapData, error) {
	db, err := sql.Open("sqlite", "file:"+strings.ReplaceAll(path, `\`, "/")+"?mode=ro")
	if err != nil {
		return MapData{}, err
	}
	defer db.Close()
	records := func(schema string) (map[int64]json.RawMessage, map[int64]string, error) {
		rows, err := db.Query(`SELECT r.guid, r.name, r.data FROM record r JOIN record_table t ON t.table_index = r.table_index WHERE t.schema = ?`, schema)
		if err != nil {
			return nil, nil, err
		}
		defer rows.Close()
		data, names := map[int64]json.RawMessage{}, map[int64]string{}
		for rows.Next() {
			var guid int64
			var name string
			var blob []byte
			if err := rows.Scan(&guid, &name, &blob); err != nil {
				return nil, nil, err
			}
			if len(blob) > 1 && blob[0] == 0x1f && blob[1] == 0x8b {
				z, err := gzip.NewReader(bytes.NewReader(blob))
				if err != nil {
					return nil, nil, fmt.Errorf("%s %s: %w", schema, name, err)
				}
				if blob, err = io.ReadAll(z); err != nil {
					return nil, nil, fmt.Errorf("%s %s: %w", schema, name, err)
				}
			}
			data[guid], names[guid] = blob, name
		}
		return data, names, rows.Err()
	}

	out := MapData{Nodes: []MapNodeArt{}, Points: []MapPoint{}, Legend: []MapLegendGroup{}}

	// the icons
	visData, visNames, err := records("MapIconVisualRecordBase")
	if err != nil {
		return out, err
	}
	visuals := map[int64]iconVisual{}
	for g, raw := range visData {
		var v struct {
			Icon     string              `json:"Icon"`
			IconSize struct{ X float64 } `json:"IconSize"`
		}
		if json.Unmarshal(raw, &v) == nil {
			visuals[g] = iconVisual{Name: visNames[g], Icon: leaf(v.Icon), Size: int(v.IconSize.X)}
		}
	}
	visual := func(r recRef) iconVisual { return visuals[r.Guid] }

	// the level the marks must be on
	levelData, levelNames, err := records("LevelDefinitionRecordBase")
	if err != nil {
		return out, err
	}
	verra := map[int64]bool{}
	for g := range levelData {
		if strings.Contains(levelNames[g], verraLevel) {
			verra[g] = true
		}
	}

	// the legend, and the node and shrine icons (MapIconMappings)
	mapData, _, err := records("MapIconMappingsRecordBase")
	if err != nil {
		return out, err
	}
	legData, legNames, err := records("MapLegendEntryRecordBase")
	if err != nil {
		return out, err
	}
	nodeIcons := map[string][]string{}
	shrine := iconVisual{}
	for _, raw := range mapData {
		var m struct {
			IconMappingData struct {
				LegendCategories []struct {
					CategoryName recText  `json:"CategoryName"`
					Entries      []recRef `json:"Entries"`
				} `json:"LegendCategories"`
				NodeIconMappings []struct {
					Key   string `json:"Key"`
					Value struct {
						Icons []recRef `json:"Icons"`
					} `json:"Value"`
				} `json:"NodeIconMappings"`
				ShrineUnlocked recRef `json:"Shrine_Unlocked"`
			} `json:"IconMappingData"`
		}
		if err := json.Unmarshal(raw, &m); err != nil {
			return out, fmt.Errorf("MapIconMappings: %w", err)
		}
		for _, n := range m.IconMappingData.NodeIconMappings {
			for _, ic := range n.Value.Icons {
				nodeIcons[strings.TrimPrefix(n.Key, "ENodeType::")] = append(nodeIcons[strings.TrimPrefix(n.Key, "ENodeType::")], visual(ic).Icon)
			}
		}
		shrine = visual(m.IconMappingData.ShrineUnlocked)
		for _, c := range m.IconMappingData.LegendCategories {
			group := MapLegendGroup{Title: c.CategoryName.Source, Entries: []MapLegendEntry{}}
			for _, e := range c.Entries {
				var l struct {
					VisualsIds []recRef `json:"VisualsIds"`
				}
				_ = json.Unmarshal(legData[e.Guid], &l)
				key := strings.TrimPrefix(legNames[e.Guid], "MapLegendEntry_")
				icon := ""
				for _, v := range l.VisualsIds {
					if icon = visual(v).Icon; icon != "" {
						break
					}
				}
				group.Entries = append(group.Entries, MapLegendEntry{Key: key, Title: legendTitle(key), Icon: icon, Kind: legendKinds[key]})
			}
			out.Legend = append(out.Legend, group)
		}
	}
	// the marks the legend has no line for
	out.Legend = append(out.Legend, MapLegendGroup{Title: "Places", Entries: []MapLegendEntry{
		{Key: "Shrine", Title: "Respawn shrines", Icon: shrine.Icon, Kind: "shrine"},
		{Key: "Vendor", Title: "Vendors", Icon: visualByName(visuals, "MapIconVisual_Vendor_Default").Icon, Kind: "vendor"},
		{Key: "Landmark", Title: "Landmarks", Icon: visualByName(visuals, "MapIconVisual_Town").Icon, Kind: "landmark"},
		{Key: "Location", Title: "Place names", Kind: "location"},
	}})

	// the city nodes: icons by type, backdrops by guid (MapTileSet_Verra_World_Master's NodeBackdrops)
	tileData, tileNames, err := records("MapTileSetRecordBase")
	if err != nil {
		return out, err
	}
	backdrops := map[int64]string{}
	for g, raw := range tileData {
		if tileNames[g] != "MapTileSet_"+verraLevel {
			continue
		}
		var t struct {
			NodeBackdrops []struct {
				Key   recRef `json:"Key"`
				Value struct {
					BackdropsPerCulture []struct {
						Value struct {
							BackdropsPerLayer []struct {
								Value struct {
									Textures []string `json:"Textures"`
								} `json:"Value"`
							} `json:"BackdropsPerLayer"`
						} `json:"Value"`
					} `json:"BackdropsPerCulture"`
				} `json:"Value"`
			} `json:"NodeBackdrops"`
		}
		if err := json.Unmarshal(raw, &t); err != nil {
			return out, fmt.Errorf("%s: %w", tileNames[g], err)
		}
		for _, b := range t.NodeBackdrops {
			for _, c := range b.Value.BackdropsPerCulture {
				for _, l := range c.Value.BackdropsPerLayer {
					if len(l.Value.Textures) > 0 {
						backdrops[b.Key.Guid] = backdropName.ReplaceAllString(leaf(l.Value.Textures[0]), "$1")
					}
				}
			}
		}
	}
	rows, err := db.Query(`SELECT guid, name, node_name, node_type, x, y, yaw FROM city_node WHERE map_name = ? AND (x <> 0 OR y <> 0)`, verraLevel)
	if err != nil {
		return out, err
	}
	for rows.Next() {
		var n MapNodeArt
		var guid int64
		if err := rows.Scan(&guid, &n.Name, &n.Title, &n.Type, &n.X, &n.Y, &n.Yaw); err != nil {
			rows.Close()
			return out, err
		}
		n.Icons = nodeIcons[n.Type]
		if len(n.Icons) == 0 {
			n.Icons = nodeIcons["Default"]
		}
		if n.Icons == nil {
			n.Icons = []string{}
		}
		n.Backdrop = backdrops[guid]
		out.Nodes = append(out.Nodes, n)
	}
	rows.Close()

	// POIs and named places
	at := func(mapLoc, origin recVec, box recBox) (float64, float64, float64) {
		switch {
		case mapLoc.X != 0 || mapLoc.Y != 0:
			return mapLoc.X, mapLoc.Y, mapLoc.Z
		case origin.X != 0 || origin.Y != 0:
			return origin.X, origin.Y, origin.Z
		case box.IsValid:
			return (box.Min.X + box.Max.X) / 2, (box.Min.Y + box.Max.Y) / 2, (box.Min.Z + box.Max.Z) / 2
		}
		return 0, 0, 0
	}
	boxOf := func(b recBox) [4]float64 {
		if !b.IsValid {
			return [4]float64{}
		}
		return [4]float64{b.Min.X, b.Min.Y, b.Max.X, b.Max.Y}
	}
	poiData, poiNames, err := records("POIRecordBase")
	if err != nil {
		return out, err
	}
	for g, raw := range poiData {
		var p struct {
			Extents           recBox  `json:"Extents"`
			LevelDefinitionId recRef  `json:"LevelDefinitionId"`
			MapIconVisualId   recRef  `json:"MapIconVisualId"`
			MapLocation       recVec  `json:"MapLocation"`
			OriginLocation    recVec  `json:"OriginLocation"`
			PlayerFacingName  recText `json:"PlayerFacingName"`
		}
		if json.Unmarshal(raw, &p) != nil || !verra[p.LevelDefinitionId.Guid] || p.PlayerFacingName.Source == "" {
			continue
		}
		v := visual(p.MapIconVisualId)
		kind := "poi_minor"
		if strings.Contains(v.Name, "Major") {
			kind = "poi_major"
		} else if strings.Contains(v.Name, "Dungeon") {
			kind = "dungeon"
		}
		x, y, z := at(p.MapLocation, p.OriginLocation, p.Extents)
		if x == 0 && y == 0 {
			continue
		}
		out.Points = append(out.Points, MapPoint{Kind: kind, Name: poiNames[g], Title: p.PlayerFacingName.Source, X: x, Y: y, Z: z, Icon: v.Icon, Size: v.Size, Box: boxOf(p.Extents)})
	}
	infoData, infoNames, err := records("MapIconInfoRecordBase")
	if err != nil {
		return out, err
	}
	for g, raw := range infoData {
		var p struct {
			Description recText `json:"Description"`
			DisplayName recText `json:"DisplayName"`
			MapLocation recVec  `json:"MapLocation"`
			VisualsId   recRef  `json:"VisualsId"`
		}
		if json.Unmarshal(raw, &p) != nil || p.DisplayName.Source == "" || (p.MapLocation.X == 0 && p.MapLocation.Y == 0) {
			continue
		}
		if nearSameTitle(out.Points, p.DisplayName.Source, p.MapLocation.X, p.MapLocation.Y) {
			continue // the POI record of the same place is on the map already
		}
		v := visual(p.VisualsId)
		kind := "landmark"
		if strings.Contains(v.Name, "Dungeon") {
			kind = "dungeon"
		}
		out.Points = append(out.Points, MapPoint{Kind: kind, Name: infoNames[g], Title: p.DisplayName.Source, Note: p.Description.Source, X: p.MapLocation.X, Y: p.MapLocation.Y, Icon: v.Icon, Size: v.Size})
	}
	for _, schema := range []string{"NamedLocationRecord", "SpecialLocationRecord"} {
		data, names, err := records(schema)
		if err != nil {
			return out, err
		}
		kind := "location"
		if schema == "SpecialLocationRecord" {
			kind = "special"
		}
		for g, raw := range data {
			var p struct {
				Extents          recBox  `json:"Extents"`
				LevelDefinition  recRef  `json:"LevelDefinition"`
				MapLocation      recVec  `json:"MapLocation"`
				OriginLocation   recVec  `json:"OriginLocation"`
				PlayerFacingName recText `json:"PlayerFacingName"`
			}
			if json.Unmarshal(raw, &p) != nil || !verra[p.LevelDefinition.Guid] || p.PlayerFacingName.Source == "" {
				continue
			}
			x, y, z := at(p.MapLocation, p.OriginLocation, p.Extents)
			if x == 0 && y == 0 {
				continue
			}
			out.Points = append(out.Points, MapPoint{Kind: kind, Name: names[g], Title: p.PlayerFacingName.Source, X: x, Y: y, Z: z, Box: boxOf(p.Extents)})
		}
	}

	// respawn shrines (the attunement points)
	if rows, err = db.Query(`SELECT name, descriptor, x, y, z FROM attune_point WHERE level_name = ? AND (x <> 0 OR y <> 0)`, verraLevel); err != nil {
		return out, err
	}
	for rows.Next() {
		p := MapPoint{Kind: "shrine", Icon: shrine.Icon, Size: shrine.Size}
		if err := rows.Scan(&p.Name, &p.Title, &p.X, &p.Y, &p.Z); err != nil {
			rows.Close()
			return out, err
		}
		if p.Title == "" {
			p.Title = p.Name
		}
		out.Points = append(out.Points, p)
	}
	rows.Close()

	// quest givers and vendors placed in the world (those of the towns' buildings stand where the building puts them, which the data does not say)
	quest := visualByName(visuals, "MapIconVisual_QuestGiver")
	vendor := visualByName(visuals, "MapIconVisual_Vendor_Default")
	for _, q := range []struct {
		kind  string
		icon  iconVisual
		query string
	}{
		{"quest", quest, `WITH who AS (
			SELECT o.source_guid AS inst, MIN(q.name) AS what FROM npc_offered_quest o JOIN quest q ON q.guid = o.quest_guid WHERE o.source_kind = 'instance' GROUP BY o.source_guid
			UNION ALL
			SELECT ias.instance_guid, MIN(q.name) FROM npc_instance_asset_set ias JOIN npc_asset_set_member m ON m.set_guid = ias.set_guid
				JOIN npc_offered_quest o ON o.source_kind = 'asset' AND o.source_guid = m.asset_guid JOIN quest q ON q.guid = o.quest_guid GROUP BY ias.instance_guid)`},
		{"vendor", vendor, `WITH who AS (
			SELECT l.source_guid AS inst, MIN(v.name) AS what FROM npc_vendor_link l JOIN vendor_def v ON v.guid = l.vendor_guid WHERE l.source_kind = 'instance' GROUP BY l.source_guid
			UNION ALL
			SELECT ias.instance_guid, MIN(v.name) FROM npc_instance_asset_set ias JOIN npc_asset_set_member m ON m.set_guid = ias.set_guid
				JOIN npc_vendor_link l ON l.source_kind = 'asset' AND l.source_guid = m.asset_guid JOIN vendor_def v ON v.guid = l.vendor_guid GROUP BY ias.instance_guid)`},
	} {
		rows, err := db.Query(q.query + `
			SELECT i.name, MIN(w.what), p.x, p.y, p.z, p.yaw, sm.x, sm.y, sm.z FROM npc_placement p
			JOIN npc_set_member sm ON sm.set_guid = p.set_guid JOIN who w ON w.inst = sm.instance_guid JOIN npc_instance i ON i.guid = sm.instance_guid
			WHERE (p.x <> 0 OR p.y <> 0) AND i.dev_only = 0
			GROUP BY p.spawn_guid, p.key, sm.key`)
		if err != nil {
			return out, err
		}
		for rows.Next() {
			var px, py, pz, yaw, mx, my, mz float64
			p := MapPoint{Kind: q.kind, Icon: q.icon.Icon, Size: q.icon.Size}
			if err := rows.Scan(&p.Name, &p.Note, &px, &py, &pz, &yaw, &mx, &my, &mz); err != nil {
				rows.Close()
				return out, err
			}
			// the member's offset turned by the placement's yaw (spawns/population.cpp)
			c, s := math.Cos(yaw*math.Pi/180), math.Sin(yaw*math.Pi/180)
			p.X, p.Y, p.Z = px+mx*c-my*s, py+mx*s+my*c, pz+mz
			p.Title = npcTitle(p.Name)
			out.Points = append(out.Points, p)
		}
		rows.Close()
	}

	sort.Slice(out.Nodes, func(i, j int) bool { return out.Nodes[i].Name < out.Nodes[j].Name })
	sort.SliceStable(out.Points, func(i, j int) bool {
		if out.Points[i].Kind != out.Points[j].Kind {
			return out.Points[i].Kind < out.Points[j].Kind
		}
		return out.Points[i].Name < out.Points[j].Name
	})
	return out, nil
}

// nearSameTitle: a mark of this title within 200 m (the game has some places both as a POI and as a fixed map icon).
func nearSameTitle(points []MapPoint, title string, x, y float64) bool {
	for _, p := range points {
		if p.Title == title && math.Hypot(p.X-x, p.Y-y) < 20000 {
			return true
		}
	}
	return false
}

var backdropName =regexp.MustCompile(`^TUI_([A-Za-z]+)_L\d$`)

func visualByName(visuals map[int64]iconVisual, name string) iconVisual {
	for _, v := range visuals {
		if v.Name == name {
			return v
		}
	}
	return iconVisual{}
}

// legendKinds: the legend lines this map has places for.
var legendKinds = map[string]string{
	"Player": "player", "PartyMember": "player", "Node": "node", "MajorPOI": "poi_major", "MinorPOI": "poi_minor", "Dungeon": "dungeon", "QuestNPC": "quest",
}

var legendTitles = map[string]string{
	"Player": "Player", "PartyMember": "Party member", "Node": "Settlement", "MajorPOI": "Major point of interest", "MinorPOI": "Minor point of interest",
	"Dungeon": "Dungeon", "QuestNPC": "Quest", "CommissionNPC": "Commission", "QuestObjective": "Quest objective", "Event": "Event",
}

var camelWords = regexp.MustCompile(`([a-z])([A-Z])`)

// legendTitle: "CraftingStation_ArcaneEngineering" -> "Arcane Engineering" (the game's strings are in a string table this server does not ship).
func legendTitle(key string) string {
	if t, ok := legendTitles[key]; ok {
		return t
	}
	if i := strings.IndexByte(key, '_'); i >= 0 {
		key = key[i+1:]
	}
	return camelWords.ReplaceAllString(key, "$1 $2")
}

var npcNoise = regexp.MustCompile(`(?i)(_spawn|_npc|_\d+)$`)

// npcTitle: "RVR_QST_Fent_Spawn" -> "RVR QST Fent". The page shows the stream's name when the NPC is in view.
func npcTitle(name string) string {
	for {
		t := npcNoise.ReplaceAllString(name, "")
		if t == name {
			break
		}
		name = t
	}
	return strings.ReplaceAll(name, "_", " ")
}
