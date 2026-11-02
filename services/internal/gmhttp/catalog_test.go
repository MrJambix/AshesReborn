package gmhttp

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"testing"
)

const catalogFixture = `
CREATE TABLE item (guid INTEGER PRIMARY KEY, name TEXT, level INTEGER, max_stack_size INTEGER);
INSERT INTO item VALUES
  (6064633867184242918, 'Gear_Weapon_Sword_2H_Tideforged', 16, 1),
  (6064633867184242919, 'Obsolete_Gear_Weapon_Sword_2H_Old', 16, 1),
  (6064633867184242920, 'Resource_Raw_Metal_Iron', 10, 20),
  (6064633867184242921, 'Resource_Twin', 1, 20),
  (6064633867184242922, 'Resource_Twin', 1, 20),
  (6064633867184242923, 'Item_6064633867184242923', 1, 1);
CREATE TABLE craft_recipe (name TEXT, tier INTEGER);
INSERT INTO craft_recipe VALUES ('Carpentry_T2_Weapon_Longbow', 2);
CREATE TABLE quest (name TEXT, level INTEGER, disabled INTEGER);
INSERT INTO quest VALUES ('Shared_Quest', 5, 0), ('Shared_Quest', 5, 0), ('Off_Quest', 1, 1), ('On_Quest', 1, 0);
CREATE TABLE city_node (name TEXT, node_name TEXT, levelable INTEGER);
INSERT INTO city_node VALUES ('Verra_ANV_Harbor', 'Western Harbor', 0), ('Verra_RVR_Joeva', 'Joeva', 1);
CREATE TABLE craft_station_def (name TEXT, display_name TEXT, blacklisted INTEGER);
INSERT INTO craft_station_def VALUES ('CraftingStation_Carpentry_T1', 'Carpentry - Novice', 0), ('CraftingStation_Armorsmithing_T5', 'CraftingStation_Armorsmithing_T5_Label', 0);
CREATE TABLE service_plot (name TEXT);
INSERT INTO service_plot VALUES ('Plot_17_Service'), ('Plot_17_Service'), ('Plot_42_House_15x15');
CREATE TABLE item_weapon (skill_line_name TEXT);
INSERT INTO item_weapon VALUES ('Weapon_Ranged_Wand'), ('Weapon_Ranged_Wand'), ('');
`

func testCatalog(t *testing.T) *Catalog {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(catalogFixture); err != nil {
		t.Fatal(err)
	}
	return NewCatalog(path)
}

func lookupValues(t *testing.T, c *Catalog, kind, prefix, query string) []string {
	t.Helper()
	found, _, err := c.Lookup(kind, prefix, query)
	if err != nil {
		t.Fatal(err)
	}
	values := []string{}
	for _, e := range found {
		values = append(values, e.Value)
	}
	return values
}

func TestCatalogLookup(t *testing.T) {
	c := testCatalog(t)
	for _, tc := range []struct {
		name, kind, prefix, query string
		want                      []string
	}{
		{name: "words in any order, any case", kind: "item", query: "TIDE sword", want: []string{"6064633867184242918"}},
		{name: "underscore separates words", kind: "item", query: "weapon_tide", want: []string{"6064633867184242918"}},
		{name: "retired come last", kind: "item", query: "sword", want: []string{"6064633867184242918", "6064633867184242919"}},
		{name: "prefix filters", kind: "item", prefix: "Resource_Raw", want: []string{"6064633867184242920"}},
		{name: "shared item names stay apart", kind: "item", query: "twin", want: []string{"6064633867184242921", "6064633867184242922"}},
		{name: "a Guid finds its item", kind: "item", query: "6064633867184242920", want: []string{"6064633867184242920"}},
		{name: "no match", kind: "item", query: "nothing", want: []string{}},
		{name: "recipes by name", kind: "recipe", query: "longbow", want: []string{"Carpentry_T2_Weapon_Longbow"}},
		{name: "shared quest names are one, disabled last", kind: "quest", want: []string{"On_Quest", "Shared_Quest", "Off_Quest"}},
		{name: "a node by its display name, levelable first", kind: "node", query: "", want: []string{"Verra_RVR_Joeva", "Verra_ANV_Harbor"}},
		{name: "node display name searched", kind: "node", query: "western", want: []string{"Verra_ANV_Harbor"}},
		{name: "plots: service only", kind: "plot", want: []string{"Plot_17_Service"}},
		{name: "lines: no blanks or repeats", kind: "line", want: []string{"Weapon_Ranged_Wand"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := lookupValues(t, c, tc.kind, tc.prefix, tc.query); fmt.Sprint(got) != fmt.Sprint(tc.want) {
				t.Fatalf("got %v, want %v", got, tc.want)
			}
		})
	}
	stations, _, _ := c.Lookup("station", "", "armorsmithing")
	if len(stations) != 1 || stations[0].Label != "" {
		t.Fatalf("a placeholder display name must not show: %+v", stations)
	}
	if _, _, err := c.Lookup("accounts", "", ""); err != ErrUnknownCatalog {
		t.Fatalf("unknown kind: %v", err)
	}
}

func TestCatalogExactNameFirstAndLimit(t *testing.T) {
	path := filepath.Join(t.TempDir(), "content.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(catalogFixture); err != nil {
		t.Fatal(err)
	}
	for i := range catalogLimit + 5 {
		if _, err := db.Exec(`INSERT INTO item VALUES (?, ?, 1, 1)`, 1000+i, fmt.Sprintf("Aaa_Ore_%03d", i)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := db.Exec(`INSERT INTO item VALUES (999, 'Zzz_Ore', 1, 1)`); err != nil {
		t.Fatal(err)
	}
	db.Close()
	found, more, err := NewCatalog(path).Lookup("item", "", "ore")
	if err != nil || !more || len(found) != catalogLimit {
		t.Fatalf("got %d, more %v, err %v", len(found), more, err)
	}
	found, _, _ = NewCatalog(path).Lookup("item", "", "zzz_ore")
	if len(found) != 1 || found[0].Value != "999" {
		t.Fatalf("exact name: %+v", found)
	}
}

func TestCatalogMissingFileIsNotCreated(t *testing.T) {
	path := filepath.Join(t.TempDir(), "absent.db")
	if _, _, err := NewCatalog(path).Lookup("item", "", ""); err == nil {
		t.Fatal("a missing content.db must be an error")
	}
	if _, err := os.Stat(path); err == nil {
		t.Fatal("the lookup created a file")
	}
}

// The queries against the real data, when a build has one: a column the fixture invents would pass above and fail here.
func TestCatalogRealData(t *testing.T) {
	path := filepath.Join("..", "..", "..", "dist", "content", "content.db")
	if _, err := os.Stat(path); err != nil {
		t.Skip("no content.db here")
	}
	c := NewCatalog(path)
	for kind := range catalogQueries {
		found, _, err := c.Lookup(kind, "", "")
		if err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		if len(found) == 0 {
			t.Errorf("%s: empty", kind)
		}
		t.Logf("%s: first %q", kind, found[0].Name)
	}
	for _, tc := range []struct{ kind, prefix, query, wantName string }{
		{"item", "", "Consumable_Food_Regen_Rations_T1", "Consumable_Food_Regen_Rations_T1"},
		{"item", "Gear_Weapon", "sword tideforged", ""},
		{"item", "Mount_", "flailrunner", ""},
		{"recipe", "", "longbow", ""},
		{"node", "", "joeva", "Verra_RVR_Joeva"},
		{"station", "", "carpentry", ""},
		{"plot", "", "17", "Plot_17_Service"},
		{"line", "", "wand", "Weapon_Ranged_Wand"},
	} {
		found, _, err := c.Lookup(tc.kind, tc.prefix, tc.query)
		if err != nil || len(found) == 0 {
			t.Errorf("%s %q: %d found, %v", tc.kind, tc.query, len(found), err)
		} else if tc.wantName != "" && found[0].Name != tc.wantName {
			t.Errorf("%s %q: first is %q", tc.kind, tc.query, found[0].Name)
		}
	}
}

// Every command worldd serves must have a prebuilt form, with the same arguments, or the page would send what worldd refuses.
func TestEveryCommandHasAForm(t *testing.T) {
	source, err := os.ReadFile(filepath.Join("..", "..", "..", "server", "worldd", "session", "gm_commands.inc"))
	if err != nil {
		t.Skip("no worldd source here")
	}
	page, err := webFiles.ReadFile("web/commands.js")
	if err != nil {
		t.Fatal(err)
	}
	specs := regexp.MustCompile(`\{"([a-z-]+)", "[A-Za-z]+", "([^"]*)"\}`).FindAllStringSubmatch(string(source), -1)
	if len(specs) < 30 {
		t.Fatalf("read only %d commands from worldd's source", len(specs))
	}
	forms := map[string]string{}
	for _, m := range regexp.MustCompile(`(?m)^\s*cmd\("([a-z-]+)", "[A-Za-z]+", "([^"]*)"`).FindAllStringSubmatch(string(page), -1) {
		forms[m[1]] = m[2]
	}
	for _, spec := range specs {
		got, ok := forms[spec[1]]
		if !ok {
			t.Errorf("no form for %q", spec[1])
		} else if got != spec[2] {
			t.Errorf("%q takes %q in worldd but %q on the page", spec[1], spec[2], got)
		}
	}
}
