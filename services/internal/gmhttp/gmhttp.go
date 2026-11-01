// Package gmhttp bridges a player's HTTPS login to worldd's account-checked GM tools.
package gmhttp

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/account"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protojson"
)

type Tokens interface {
	ValidateToken(context.Context, string) (account.Account, error)
}
type Grants interface {
	GMLevel(context.Context, int64) (int32, error)
}
type World interface {
	AccountCommand(context.Context, *adminv1.AccountCommandRequest, ...grpc.CallOption) (*adminv1.AccountCommandResponse, error)
}

func Handler(tokens Tokens, grants Grants, world World) http.Handler {
	var mu sync.Mutex
	var next time.Time // only one GM account exists; cap commands/polls to 5 per second
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Type", "application/json")
		errorReply := func(code int, message string) {
			w.WriteHeader(code)
			_ = json.NewEncoder(w).Encode(map[string]string{"message": message})
		}
		if r.URL.Path != "/api/v1/gm" {
			errorReply(404, "Unknown GM endpoint.")
			return
		}
		if r.Method != http.MethodGet && r.Method != http.MethodPost {
			w.Header().Set("Allow", "GET, POST")
			errorReply(405, "Use GET or POST.")
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		headers := r.Header.Values("Authorization")
		if len(headers) != 1 || !strings.HasPrefix(headers[0], "Bearer ") || len(headers[0]) > 4103 {
			errorReply(401, "Sign in first.")
			return
		}
		acc, err := tokens.ValidateToken(ctx, strings.TrimPrefix(headers[0], "Bearer "))
		if err != nil {
			errorReply(401, "Your sign-in expired. Sign in again.")
			return
		}
		level, err := grants.GMLevel(ctx, acc.ID)
		if err != nil {
			errorReply(503, "Account permissions are unavailable.")
			return
		}
		if level != 3 {
			errorReply(403, "This account is not a GM.")
			return
		}
		mu.Lock()
		allowed := !time.Now().Before(next)
		if allowed {
			next = time.Now().Add(200 * time.Millisecond)
		}
		mu.Unlock()
		if !allowed {
			errorReply(429, "Please wait before the next GM request.")
			return
		}
		command := ""
		if r.Method == http.MethodPost {
			r.Body = http.MaxBytesReader(w, r.Body, 4096)
			dec := json.NewDecoder(r.Body)
			dec.DisallowUnknownFields()
			var body struct {
				Command string `json:"command"`
			}
			if err := dec.Decode(&body); err != nil {
				errorReply(400, "Invalid GM request.")
				return
			}
			var trailing any
			if dec.Decode(&trailing) != io.EOF || len(body.Command) > 1024 || strings.TrimSpace(body.Command) == "" {
				errorReply(400, "Supply one GM command.")
				return
			}
			command = body.Command
		}
		result, err := world.AccountCommand(ctx, &adminv1.AccountCommandRequest{AccountId: acc.ID, Command: command})
		if err != nil {
			code, message := 503, "The world server is unavailable."
			switch status.Code(err) {
			case codes.PermissionDenied:
				code, message = 403, "GM mode is off or this account is not a GM. Use .GM On."
			case codes.InvalidArgument:
				code, message = 400, status.Convert(err).Message()
			case codes.FailedPrecondition:
				code, message = 409, status.Convert(err).Message()
			case codes.NotFound:
				code, message = 404, "Enter the world with your GM account first."
			}
			errorReply(code, message)
			return
		}
		data, err := (protojson.MarshalOptions{UseProtoNames: true, EmitUnpopulated: true}).Marshal(result)
		if err != nil {
			errorReply(500, "Invalid world response.")
			return
		}
		_, _ = w.Write(data)
	})
}
