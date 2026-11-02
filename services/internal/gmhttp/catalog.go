package gmhttp

import (
	"database/sql"
	"errors"
	"fmt"
	"os"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"unicode"

	_ "modernc.org/sqlite" // database/sql driver "sqlite"
)

// CatalogEntry is one thing a GM command can name. Value is what the command takes.
type CatalogEntry struct {
	Value string `json:"value"`
	Name  string `json:"name"`            // the record's name
	Label string `json:"label,omitempty"` // its display name, for the few records that have a real one
	Level int    `json:"level,omitempty"` // an item's or quest's level, a recipe's tier
	Stack int    `json:"stack,omitempty"` // an item's stack size

	rank   int // lower comes first: records the game has retired come last
	search string
}

var ErrUnknownCatalog = errors.New("unknown catalog")

const catalogLimit = 50

// Catalog answers name searches over content.db's records. It reads them once, at the first search, so a server nobody
// opens the GM tools on holds none of it. It reads the columns it needs directly and does not ask for a schema version:
// a content.db this cannot read costs the GM tools their pickers, and must not stop the login service.
type Catalog struct {
	path  string
	once  sync.Once
	kinds map[string][]CatalogEntry
	err   error
}

func NewCatalog(path string) *Catalog { return &Catalog{path: path} }

// retired matches the prefixes the data gives records that are out of the game, and records with no name of their own.
var retired = regexp.MustCompile(`(?i)^(obsolete|notused|retired|zlegacy|test_)|^item_\d+$`)

// Items are named by Guid, because three names are shared and a name would be refused. Quests are named by name: QuestBook::find
// reads a Guid only below 19 digits, and every Guid in the data has 19. Everything else is unique by name.
var catalogQueries = map[string]string{
	"item":    `SELECT guid, name, '', level, max_stack_size, 0 FROM item`,
	"recipe":  `SELECT name, name, '', tier, 0, 0 FROM craft_recipe`,
	"quest":   `SELECT name, name, '', level, 0, disabled FROM quest`,
	"node":    `SELECT name, name, node_name, 0, 0, NOT levelable FROM city_node`,
	"station": `SELECT name, name, CASE WHEN display_name LIKE '%\_Label' ESCAPE '\' THEN '' ELSE display_name END, 0, 0, blacklisted FROM craft_station_def`,
	"plot":    `SELECT DISTINCT name, name, '', 0, 0, 0 FROM service_plot WHERE name LIKE '%Service%'`,
	"line":    `SELECT DISTINCT skill_line_name, skill_line_name, '', 0, 0, 0 FROM item_weapon WHERE skill_line_name <> ''`,
}

func (c *Catalog) load() {
	if _, err := os.Stat(c.path); err != nil { // SQLite would make an empty file of a missing path
		c.err = err
		return
	}
	db, err := sql.Open("sqlite", c.path+"?_pragma=query_only(1)")
	if err != nil {
		c.err = err
		return
	}
	defer db.Close()
	c.kinds = map[string][]CatalogEntry{}
	for kind, query := range catalogQueries {
		entries, err := readCatalog(db, query)
		if err != nil {
			c.err = fmt.Errorf("catalog %s: %w", kind, err)
			return
		}
		c.kinds[kind] = entries
	}
}

func readCatalog(db *sql.DB, query string) ([]CatalogEntry, error) {
	rows, err := db.Query(query)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var entries []CatalogEntry
	seen := map[string]bool{} // a shared quest name is one thing to the command that takes it
	for rows.Next() {
		var e CatalogEntry
		var value any
		if err := rows.Scan(&value, &e.Name, &e.Label, &e.Level, &e.Stack, &e.rank); err != nil {
			return nil, err
		}
		switch v := value.(type) {
		case int64:
			e.Value = strconv.FormatInt(v, 10)
		case string:
			e.Value = v
		default:
			return nil, fmt.Errorf("%s: value of type %T", e.Name, value)
		}
		if seen[e.Value] {
			continue
		}
		seen[e.Value] = true
		if retired.MatchString(e.Name) {
			e.rank = max(e.rank, 2)
		}
		e.search = strings.ToLower(e.Name + " " + e.Label + " " + e.Value)
		entries = append(entries, e)
	}
	slices.SortFunc(entries, func(a, b CatalogEntry) int {
		if a.rank != b.rank {
			return a.rank - b.rank
		}
		return strings.Compare(a.search, b.search)
	})
	return entries, rows.Err()
}

// Lookup finds up to catalogLimit entries of a kind holding every word of query, and whose names start with prefix when
// one is given, and says whether there were more. Case does not count, and "_" separates words as a space does. A name
// that is exactly the query comes first.
func (c *Catalog) Lookup(kind, prefix, query string) ([]CatalogEntry, bool, error) {
	c.once.Do(c.load)
	if c.err != nil {
		return nil, false, c.err
	}
	all, ok := c.kinds[kind]
	if !ok {
		return nil, false, ErrUnknownCatalog
	}
	exact := strings.ToLower(strings.TrimSpace(query))
	prefix = strings.ToLower(prefix)
	words := strings.FieldsFunc(exact, func(r rune) bool { return unicode.IsSpace(r) || r == '_' })
	found := []CatalogEntry{}
	more := false
next:
	for i := range all {
		e := &all[i]
		if !strings.HasPrefix(e.search, prefix) {
			continue
		}
		for _, word := range words {
			if !strings.Contains(e.search, word) {
				continue next
			}
		}
		if exact != "" && strings.EqualFold(e.Name, exact) {
			found = slices.Insert(found, 0, *e)
		} else {
			found = append(found, *e)
		}
		if len(found) > catalogLimit {
			more = true
			if exact == "" {
				break
			}
		}
	}
	return found[:min(len(found), catalogLimit)], more, nil
}
