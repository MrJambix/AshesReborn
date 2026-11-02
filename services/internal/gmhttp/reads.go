package gmhttp

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strconv"
	"sync"
	"time"

	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"
)

// WorldReads and StreamReads are the read-only calls of worldd's admin API and world stream that the GM web tools show.
type WorldReads interface {
	ListPlayers(context.Context, *adminv1.ListPlayersRequest, ...grpc.CallOption) (*adminv1.ListPlayersResponse, error)
	ListPlaces(context.Context, *adminv1.ListPlacesRequest, ...grpc.CallOption) (*adminv1.ListPlacesResponse, error)
}
type StreamReads interface {
	GetStatus(context.Context, *adminv1.GetStatusRequest, ...grpc.CallOption) (*adminv1.GetStatusResponse, error)
	Search(context.Context, *adminv1.SearchRequest, ...grpc.CallOption) (*adminv1.SearchResponse, error)
	Inspect(context.Context, *adminv1.InspectRequest, ...grpc.CallOption) (*adminv1.InspectResponse, error)
}

const (
	readBurst     = 20.0
	readPerSecond = 10.0
)

// Lookups is the search over the game's records that fills the tools' pickers.
type Lookups interface {
	Lookup(kind, prefix, query string) ([]CatalogEntry, bool, error)
}

// Reads serves GET /api/v1/gm/{status,players,places,search,inspect,catalog}. Nothing here changes the world, so it needs the
// account's grant but not a character in the world. Everything that changes the world stays on Handler, where worldd
// checks the joined session and GM mode as well.
func Reads(tokens Tokens, grants Grants, world WorldReads, stream StreamReads, lookups Lookups) http.Handler {
	var mu sync.Mutex
	tokensLeft, at := readBurst, time.Now()
	take := func() bool { // only one GM account exists, so one budget; a read hops to the network thread, a poll must not flood it
		mu.Lock()
		defer mu.Unlock()
		now := time.Now()
		tokensLeft = min(readBurst, tokensLeft+now.Sub(at).Seconds()*readPerSecond)
		at = now
		if tokensLeft < 1 {
			return false
		}
		tokensLeft--
		return true
	}
	admit := func(ctx context.Context, w http.ResponseWriter, r *http.Request) bool {
		if _, code, message := authorize(ctx, r, tokens, grants); code != 0 {
			readError(w, code, message)
			return false
		}
		if !take() {
			readError(w, 429, "Please wait before the next GM request.")
			return false
		}
		return true
	}
	route := func(mux *http.ServeMux, path string, read func(context.Context, *http.Request) (proto.Message, error)) {
		mux.HandleFunc("GET "+path, func(w http.ResponseWriter, r *http.Request) {
			ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
			defer cancel()
			if !admit(ctx, w, r) {
				return
			}
			result, err := read(ctx, r)
			if err != nil {
				code, message := 503, "The world server is unavailable."
				switch status.Code(err) {
				case codes.InvalidArgument:
					code, message = 400, status.Convert(err).Message()
				case codes.NotFound:
					code, message = 404, "That is no longer in the world."
				}
				readError(w, code, message)
				return
			}
			data, err := (protojson.MarshalOptions{UseProtoNames: true, EmitUnpopulated: true}).Marshal(result)
			if err != nil {
				readError(w, 500, "Invalid world response.")
				return
			}
			_, _ = w.Write(data)
		})
	}
	mux := http.NewServeMux()
	route(mux, "/api/v1/gm/status", func(ctx context.Context, _ *http.Request) (proto.Message, error) {
		return stream.GetStatus(ctx, &adminv1.GetStatusRequest{})
	})
	route(mux, "/api/v1/gm/players", func(ctx context.Context, _ *http.Request) (proto.Message, error) {
		return world.ListPlayers(ctx, &adminv1.ListPlayersRequest{})
	})
	route(mux, "/api/v1/gm/places", func(ctx context.Context, _ *http.Request) (proto.Message, error) {
		return world.ListPlaces(ctx, &adminv1.ListPlacesRequest{})
	})
	route(mux, "/api/v1/gm/search", func(ctx context.Context, r *http.Request) (proto.Message, error) {
		query := r.URL.Query().Get("q")
		if len(query) > 128 {
			return nil, status.Error(codes.InvalidArgument, "Search for at most 128 characters.")
		}
		return stream.Search(ctx, &adminv1.SearchRequest{Query: query, Limit: 50}) // worldd refuses a query under 2 characters, with its reason
	})
	route(mux, "/api/v1/gm/inspect", func(ctx context.Context, r *http.Request) (proto.Message, error) {
		id, err := strconv.ParseUint(r.URL.Query().Get("id"), 10, 64)
		if err != nil || id == 0 {
			return nil, status.Error(codes.InvalidArgument, "Inspect needs an actor id.")
		}
		return stream.Inspect(ctx, &adminv1.InspectRequest{Id: id})
	})
	mux.HandleFunc("GET /api/v1/gm/catalog", func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		if !admit(ctx, w, r) {
			return
		}
		q := r.URL.Query()
		if len(q.Get("q")) > 128 || len(q.Get("prefix")) > 128 {
			readError(w, 400, "Search for at most 128 characters.")
			return
		}
		results, truncated, err := lookups.Lookup(q.Get("kind"), q.Get("prefix"), q.Get("q"))
		if errors.Is(err, ErrUnknownCatalog) {
			readError(w, 400, "Unknown catalog.")
			return
		}
		if err != nil {
			slog.Error("GM catalog unavailable", "err", err)
			readError(w, 503, "The game's records are unavailable. Type the name instead.")
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"results": results, "truncated": truncated})
	})
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Type", "application/json")
		if h, pattern := mux.Handler(r); pattern != "" {
			h.ServeHTTP(w, r)
			return
		}
		readError(w, 404, "Unknown GM endpoint.")
	})
}

func readError(w http.ResponseWriter, code int, message string) {
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"message": message})
}
