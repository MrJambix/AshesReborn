// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// auth serves the launcher's login API (HTTP) and token validation (gRPC).
package main

import (
	"context"
	"crypto/tls"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	adminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/account"
	"aocrop/services/internal/authgrpc"
	"aocrop/services/internal/authhttp"
	"aocrop/services/internal/character"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/gmhttp"
	"aocrop/services/internal/svcauth"
)

func main() {
	if err := run(); err != nil {
		slog.Error("auth stopped", "err", err)
		os.Exit(1)
	}
}

// [C | O | A + C Y B]

func run() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg := config.Load()
	token, err := svcauth.Load(cfg.ServiceTokenFile)
	if err != nil {
		return err
	}

	// Ctrl+C while the database is being reached is a stop, not a failure.
	pool, err := db.Open(ctx, cfg.DatabaseURL)
	if err != nil {
		return startupErr(ctx, err)
	}
	defer pool.Close()
	if err := db.Migrate(ctx, pool); err != nil {
		return startupErr(ctx, err)
	}
	store := account.NewStore(pool, cfg.TokenTTL)
	worldConn, err := grpc.NewClient(cfg.WorldAdminAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), token.DialOption())
	if err != nil {
		return err
	}
	defer worldConn.Close()
	api := http.NewServeMux()
	api.Handle("/api/v1/gm", gmhttp.Handler(store, character.NewStore(pool), adminv1.NewWorldAdminServiceClient(worldConn)))
	api.Handle("/", authhttp.Handler(store))

	// Bind both ports before saying "listening", so a taken port fails here.
	httpLis, err := net.Listen("tcp", cfg.AuthHTTPAddr)
	if err != nil {
		return err
	}
	grpcLis, err := net.Listen("tcp", cfg.AuthGRPCAddr)
	if err != nil {
		httpLis.Close()
		return err
	}
	httpSrv := &http.Server{
		Handler:           api,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}
	grpcSrv := newGRPCServer(token, store)

	errc := make(chan error, 3)
	// The public listener never answers in clear text: without its certificate the service does not start.
	var publicSrv *http.Server
	if cfg.AuthPublicAddr != "" {
		publicLis, err := net.Listen("tcp", cfg.AuthPublicAddr)
		if err != nil {
			httpLis.Close()
			grpcLis.Close()
			return err
		}
		cert, err := tls.LoadX509KeyPair(cfg.AuthTLSCert, cfg.AuthTLSKey)
		if err != nil {
			publicLis.Close()
			httpLis.Close()
			grpcLis.Close()
			return err
		}
		publicSrv = &http.Server{
			Handler:           authhttp.Limit(api, authhttp.DefaultLimits()),
			TLSConfig:         &tls.Config{Certificates: []tls.Certificate{cert}, MinVersion: tls.VersionTLS12},
			ReadHeaderTimeout: 5 * time.Second,
			ReadTimeout:       10 * time.Second,
			WriteTimeout:      15 * time.Second,
			IdleTimeout:       60 * time.Second,
		}
		go func() { errc <- publicSrv.ServeTLS(publicLis, "", "") }()
		slog.Info("auth listening over TLS", "public", publicLis.Addr().String())
	}
	go func() { errc <- httpSrv.Serve(httpLis) }()
	go func() { errc <- grpcSrv.Serve(grpcLis) }()
	go purgeTokens(ctx, store)
	slog.Info("auth listening", "http", httpLis.Addr().String(), "grpc", grpcLis.Addr().String())

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop() // from here a second Ctrl+C ends the process at once
	// Drain both servers within one budget, then cut what is left. Closing
	// cancels the stuck handlers' contexts, so their database queries end
	// and the deferred pool.Close() does not wait on them.
	force := time.AfterFunc(shutdownGrace, func() {
		grpcSrv.Stop()
		httpSrv.Close()
		if publicSrv != nil {
			publicSrv.Close()
		}
	})
	drained := make(chan struct{})
	go func() { grpcSrv.GracefulStop(); close(drained) }()
	if publicSrv != nil {
		_ = publicSrv.Shutdown(context.Background())
	}
	_ = httpSrv.Shutdown(context.Background())
	<-drained
	force.Stop()
	return err
}

// newGRPCServer is the token-validation server the gateway calls. It requires
// the service token.
func newGRPCServer(token svcauth.Token, tokens authgrpc.Tokens) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	authv1.RegisterAuthServiceServer(s, authgrpc.New(tokens))
	return s
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}

const shutdownGrace = 10 * time.Second

// purgeTokens deletes expired login tokens now and then hourly until ctx ends.
func purgeTokens(ctx context.Context, store *account.Store) {
	tick := time.NewTicker(time.Hour)
	defer tick.Stop()
	for {
		n, err := store.PurgeExpiredTokens(ctx)
		switch {
		case err != nil && ctx.Err() == nil:
			slog.Warn("token purge failed", "err", err)
		case n > 0:
			slog.Info("purged expired tokens", "count", n)
		}
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
	}
}
