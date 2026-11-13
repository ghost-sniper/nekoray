package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestSystemProxyControl(t *testing.T) {
	input, command := io.Pipe()
	defer command.Close()
	events, output := io.Pipe()
	defer events.Close()
	defer output.Close()

	control, ctx := newUpdateControl(context.Background(), input, json.NewEncoder(output), &sync.Mutex{})
	requests := json.NewDecoder(events)
	responses := json.NewEncoder(command)
	ctx, deadline := context.WithTimeout(ctx, 5*time.Second)
	defer deadline()

	for _, test := range []struct{ target, proxy string }{
		{"https://github.com/release", "http://localhost:3128"},
		{"https://release-assets.githubusercontent.com/file", ""},
	} {
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, test.target, nil)
		if err != nil {
			t.Fatal(err)
		}
		result := make(chan string, 1)
		go func() {
			proxy, err := control.proxy(req)
			if err != nil {
				result <- err.Error()
			} else if proxy == nil {
				result <- ""
			} else {
				result <- proxy.String()
			}
		}()
		var query struct {
			ID  int    `json:"request_id"`
			URL string `json:"url"`
		}
		if err := requests.Decode(&query); err != nil {
			t.Fatal(err)
		}
		if query.URL != test.target {
			t.Fatalf("proxy query for %q, want %q", query.URL, test.target)
		}
		if err := responses.Encode(proxyResponse{Action: "proxy", ID: query.ID, Proxy: test.proxy}); err != nil {
			t.Fatal(err)
		}
		if actual := <-result; actual != test.proxy {
			t.Fatalf("proxy = %q, want %q", actual, test.proxy)
		}
	}
	if err := responses.Encode(proxyResponse{Action: "cancel"}); err != nil {
		t.Fatal(err)
	}
	<-ctx.Done()
	if !errors.Is(ctx.Err(), context.Canceled) {
		t.Fatal(ctx.Err())
	}
}

func TestCancelComponentDownloadPreservesInstalledRelease(t *testing.T) {
	root := t.TempDir()
	parent := filepath.Join(root, "geoip")
	if err := os.Mkdir(parent, 0700); err != nil {
		t.Fatal(err)
	}
	old := installedComponent{Version: "old", Revision: strings.Repeat("a", 64)}
	if err := writeCurrent(parent, old); err != nil {
		t.Fatal(err)
	}
	flushed := make(chan struct{})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.WriteString(w, strings.Repeat("x", 16<<10))
		w.(http.Flusher).Flush()
		close(flushed)
		<-r.Context().Done()
	}))
	defer server.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	service := releaseService{client: server.Client()}
	candidate := componentRelease{
		Version: "new", Revision: strings.Repeat("b", 64),
		Files: []componentFile{{
			Name: "geoip.dat", Format: "raw",
			Asset: releaseAsset{Name: "geoip.dat", URL: server.URL, Size: 32 << 10},
		}},
	}
	event := componentEvent{ID: "geoip"}
	result := make(chan error, 1)
	go func() { result <- service.install(ctx, root, &event, candidate, func(componentEvent) {}) }()

	select {
	case <-flushed:
	case <-time.After(5 * time.Second):
		t.Fatal("download never started")
	}
	cancel()
	select {
	case err := <-result:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("install error = %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("download did not stop")
	}
	if installed := currentComponent(root, "geoip"); installed != old {
		t.Fatalf("installed release changed: %+v", installed)
	}
	entries, err := os.ReadDir(parent)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 || entries[0].Name() != "current.json" {
		t.Fatalf("cancelled download left staged files: %v", entries)
	}
}
