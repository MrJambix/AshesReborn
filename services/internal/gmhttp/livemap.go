package gmhttp

import (
	"context"
	"log/slog"
	"sync"
	"time"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"google.golang.org/grpc"
)

// WorldStream is the world stream's Subscribe, which the live map follows.
type WorldStream interface {
	Subscribe(context.Context, *adminv1.SubscribeRequest, ...grpc.CallOption) (grpc.ServerStreamingClient[adminv1.SubscribeResponse], error)
}

// NodeReads is the city node service's list of nodes and their levels.
type NodeReads interface {
	ListNodes(context.Context, *citynodev1.ListNodesRequest, ...grpc.CallOption) (*citynodev1.ListNodesResponse, error)
}

// verraArea is worldd's world box around Verra (move_checker.h's kVerraBox): every actor is inside it, and it is under the stream's 100 km.
var verraArea = &adminv1.AreaBox{MinXCm: -2600000, MinYCm: -1500000, MaxXCm: 1000000, MaxYCm: 2000000}

const (
	mapIdle     = time.Minute     // the stream closes when nobody has asked for the map this long
	mapRetry    = 3 * time.Second // ... and opens again this long after it broke
	mapMaxDots  = 20000           // the most entities one answer carries (the rest are counted in Clipped)
	mapStaleFor = 6 * time.Second // a stream with no frame for this long (its heartbeat is 2 s) is not live
)

// mapEntity is one dot: [id, kind, x, y, z, yaw, state, level, health_pct], with the name apart (only players have one in the stream).
type mapEntity struct {
	ID     uint64 `json:"id,string"`
	Kind   int32  `json:"k"`
	X      int32  `json:"x"`
	Y      int32  `json:"y"`
	Z      int32  `json:"z"`
	Yaw    uint32 `json:"yaw"`
	State  uint32 `json:"s"`
	Level  uint32 `json:"l,omitempty"`
	Health uint32 `json:"hp"`
	Name   string `json:"n,omitempty"`
}

// LiveMap follows worldd's world stream for the whole of Verra and keeps the newest state of every entity. It subscribes only
// while the map is being looked at: the first read opens the stream, and it closes after mapIdle with no read.
type LiveMap struct {
	stream WorldStream

	mu       sync.Mutex
	entities map[uint64]*mapEntity
	names    map[uint64]string // a player's name comes only in the frame it first appears in
	seq      uint64
	at       time.Time // the last frame
	world    float64   // worldd's clock in that frame
	asked    time.Time // the last read
	running  bool
	problem  string // why the stream is not live, for the page
}

func NewLiveMap(stream WorldStream) *LiveMap {
	return &LiveMap{stream: stream, entities: map[uint64]*mapEntity{}, names: map[uint64]string{}}
}

// MapFrame is what a read of the map answers.
type MapFrame struct {
	Live      bool        `json:"live"`
	Problem   string      `json:"problem,omitempty"`
	Seq       uint64      `json:"seq,string"`
	WorldTime float64     `json:"world_time"`
	AgeMs     int64       `json:"age_ms"`
	Entities  []mapEntity `json:"entities"`
	Clipped   int         `json:"clipped,omitempty"`
}

// Read answers the entities of the kinds in `kinds` (EntityKind numbers; empty: all) inside the box, and starts the stream when it is not running.
func (m *LiveMap) Read(kinds map[int32]bool, box *adminv1.AreaBox) MapFrame {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.asked = time.Now()
	if !m.running {
		m.running = true
		go m.follow()
	}
	f := MapFrame{Seq: m.seq, WorldTime: m.world, Problem: m.problem, Entities: []mapEntity{}}
	if !m.at.IsZero() {
		f.AgeMs = time.Since(m.at).Milliseconds()
		f.Live = f.Problem == "" && time.Since(m.at) < mapStaleFor
	}
	for _, e := range m.entities {
		if len(kinds) > 0 && !kinds[e.Kind] {
			continue
		}
		if box != nil && (e.X < box.MinXCm || e.X >= box.MaxXCm || e.Y < box.MinYCm || e.Y >= box.MaxYCm) {
			continue
		}
		if len(f.Entities) >= mapMaxDots {
			f.Clipped++
			continue
		}
		d := *e
		d.Name = m.names[e.ID]
		f.Entities = append(f.Entities, d)
	}
	return f
}

// follow keeps one stream open while the map is read, and opens it again after a break.
func (m *LiveMap) follow() {
	for {
		m.mu.Lock()
		idle := time.Since(m.asked) > mapIdle
		if idle {
			m.running = false
			m.entities, m.names, m.at = map[uint64]*mapEntity{}, map[uint64]string{}, time.Time{}
		}
		m.mu.Unlock()
		if idle {
			return
		}
		err := m.once()
		m.mu.Lock()
		m.problem = "The world stream is not answering. Retrying."
		if err != nil {
			slog.Warn("GM map stream ended", "err", err)
		}
		m.mu.Unlock()
		time.Sleep(mapRetry)
	}
}

// once reads one stream until it breaks or nobody has looked for mapIdle.
func (m *LiveMap) once() error {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	stream, err := m.stream.Subscribe(ctx, &adminv1.SubscribeRequest{Area: verraArea, Detail: adminv1.StreamDetail_STREAM_DETAIL_ENTITIES})
	if err != nil {
		return err
	}
	var want uint64
	first := true
	for {
		frame, err := stream.Recv()
		if err != nil {
			return err
		}
		m.mu.Lock()
		if time.Since(m.asked) > mapIdle {
			m.mu.Unlock()
			return nil
		}
		if !first && frame.GetSeq() != want { // a gap: the deltas no longer add up, so start again
			m.mu.Unlock()
			return errGap
		}
		first = false
		want = frame.GetSeq() + 1
		m.apply(frame)
		m.mu.Unlock()
	}
}

type gapError struct{}

func (gapError) Error() string { return "the world stream skipped a frame" }

var errGap error = gapError{}

func (m *LiveMap) apply(frame *adminv1.SubscribeResponse) {
	put := func(e *adminv1.Entity) {
		m.entities[e.GetId()] = &mapEntity{ID: e.GetId(), Kind: int32(e.GetKind()), X: e.GetXCm(), Y: e.GetYCm(), Z: e.GetZCm(), Yaw: e.GetYaw(), State: e.GetState(),
			Level: e.GetLevel(), Health: e.GetHealthPct()}
		if e.GetName() != "" {
			m.names[e.GetId()] = e.GetName()
		}
	}
	switch body := frame.GetBody().(type) {
	case *adminv1.SubscribeResponse_Snapshot:
		m.entities, m.names = map[uint64]*mapEntity{}, map[uint64]string{}
		for _, e := range body.Snapshot.GetEntities() {
			put(e)
		}
	case *adminv1.SubscribeResponse_Delta:
		for _, e := range body.Delta.GetChanged() {
			put(e)
		}
		for _, id := range body.Delta.GetRemoved() {
			delete(m.entities, id)
			delete(m.names, id)
		}
	}
	m.seq, m.world, m.at, m.problem = frame.GetSeq(), frame.GetWorldTime(), time.Now(), ""
}
