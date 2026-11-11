package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

type releaseTransport func(*http.Request) (*http.Response, error)

func (f releaseTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func testAsset(repo, tag, name, contents string) releaseAsset {
	digest := sha256.Sum256([]byte(contents))
	return releaseAsset{
		Name: name, URL: "https://github.com/" + repo + "/releases/download/" + tag + "/" + name,
		Digest: "sha256:" + hex.EncodeToString(digest[:]), Size: int64(len(contents)),
	}
}

func fakeReleaseService(responses map[string]string) *releaseService {
	return &releaseService{
		api: "https://api.github.com", cache: make(map[string]*releaseResponse),
		client: &http.Client{Transport: releaseTransport(func(r *http.Request) (*http.Response, error) {
			body, ok := responses[r.URL.String()]
			if !ok {
				return nil, errors.New("network unavailable")
			}
			return &http.Response{StatusCode: http.StatusOK, Body: io.NopCloser(strings.NewReader(body)), Header: make(http.Header)}, nil
		})},
	}
}

func encodeFixture(t *testing.T, value any) string {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func TestDiscoverNekoray(t *testing.T) {
	endpoint := "https://api.github.com/repos/" + forkRepo + "/releases?per_page=20"
	suffix := "-" + runtime.GOOS + "64.zip"
	stable := release{Tag: "v4.1.0", Assets: []releaseAsset{testAsset(forkRepo, "v4.1.0", "nekoray-4.1.0-2026-10-01"+suffix, "stable")}}
	pre := release{Tag: "v4.2.0-rc.1", Pre: true, Assets: []releaseAsset{testAsset(forkRepo, "v4.2.0-rc.1", "nekoray-4.2.0-rc.1"+suffix, "pre")}}
	fixtures := []struct {
		name, current, wantState, wantVersion string
		releases                              []release
		pre                                   bool
	}{
		{"no release", "4.0.1", "no-release", "", []release{}, false},
		{"stable only", "4.0.1", "available", "4.1.0-2026-10-01", []release{pre, stable}, false},
		{"already current", "4.1.0-2026-10-01", "current", "4.1.0-2026-10-01", []release{stable}, false},
		{"pre-release enabled", "4.1.0-2026-10-01", "available", "4.2.0-rc.1", []release{pre, stable}, true},
		{"stable replaces RC", "4.1.0-rc.2", "available", "4.1.0-2026-10-01", []release{stable}, false},
	}
	for _, fixture := range fixtures {
		t.Run(fixture.name, func(t *testing.T) {
			service := fakeReleaseService(map[string]string{endpoint: encodeFixture(t, fixture.releases)})
			event, _ := service.status(context.Background(), t.TempDir(), "nekoray", fixture.current, fixture.pre)
			if event.State != fixture.wantState || event.Available != fixture.wantVersion {
				t.Fatalf("state %q, version %q, error %q", event.State, event.Available, event.Error)
			}
		})
	}
}

func TestDiscoverOfficialCore(t *testing.T) {
	ctx := context.Background()
	for _, platform := range []string{"linux", "windows"} {
		repo, tag := "MetaCubeX/mihomo", "v1.19.34"
		name, format, member := coreAsset("mihomo", tag, platform)
		asset := testAsset(repo, tag, name, "official")
		endpoint := "https://api.github.com/repos/" + repo + "/releases/latest"
		service := fakeReleaseService(map[string]string{endpoint: encodeFixture(t, release{Tag: tag, Assets: []releaseAsset{asset}})})
		candidate, err := service.discover(ctx, "mihomo", platform, "amd64", false)
		if err != nil || len(candidate.Files) != 1 || candidate.Files[0].Format != format || candidate.Files[0].Member != member {
			t.Fatalf("%s: %+v, %v", platform, candidate, err)
		}
		if _, err := service.discover(ctx, "mihomo", platform, "arm64", false); !errors.Is(err, errNoAsset) {
			t.Fatalf("wrong architecture accepted: %v", err)
		}
		for _, bad := range []releaseAsset{
			{Name: asset.Name, URL: asset.URL, Size: asset.Size},
			{Name: asset.Name, URL: "https://example.org/" + asset.Name, Size: asset.Size, Digest: asset.Digest},
		} {
			badService := fakeReleaseService(map[string]string{endpoint: encodeFixture(t, release{Tag: tag, Assets: []releaseAsset{bad}})})
			if _, err := badService.discover(ctx, "mihomo", platform, "amd64", false); err == nil {
				t.Fatal("unverified asset accepted")
			}
		}
	}
}

func TestStageAndActivateGeo(t *testing.T) {
	ctx, root := context.Background(), t.TempDir()
	repo := "MetaCubeX/meta-rules-dat"
	makeRelease := func(contents string) componentRelease {
		dat := testAsset(repo, "latest", "geoip.dat", contents)
		db := testAsset(repo, "latest", "geoip.metadb", "database")
		candidate, err := identifyRelease(componentRelease{
			Version: "latest",
			Files:   []componentFile{{dat, repo, "geoip.dat", "raw", ""}, {db, repo, "geoip.metadb", "raw", ""}},
		})
		if err != nil {
			t.Fatal(err)
		}
		return candidate
	}
	first, second := makeRelease("first"), makeRelease("second")
	if first.Revision == second.Revision {
		t.Fatal("updates to a mutable tag must have distinct revisions")
	}
	emit := func(componentEvent) {}
	event := componentEvent{ID: "geoip"}
	firstDat, db := first.Files[0].Asset.URL, first.Files[1].Asset.URL
	service := fakeReleaseService(map[string]string{firstDat: "first", db: "database"})
	if err := service.install(ctx, root, &event, first, emit); err != nil {
		t.Fatal(err)
	}
	if event.State != "staged" || currentComponent(root, "geoip").Revision != "" {
		t.Fatal("staging changed the active revision")
	}
	if _, err := activateStaged(root, "geoip", first.Revision); err != nil {
		t.Fatal(err)
	}

	for name, fixture := range map[string]map[string]string{
		"offline":  {firstDat: "second"},
		"bad hash": {firstDat: "second", db: "corrupt!"},
	} {
		t.Run(name, func(t *testing.T) {
			failing := fakeReleaseService(fixture)
			if err := failing.install(ctx, root, &event, second, emit); err == nil {
				t.Fatal("partial download was accepted")
			}
			if got := currentComponent(root, "geoip").Revision; got != first.Revision {
				t.Fatalf("active revision changed to %s", got)
			}
			if _, err := readStaged(root, "geoip", second.Revision); err == nil {
				t.Fatal("partial release became available")
			}
		})
	}
}

func makePortableFixture(t *testing.T, platform string, extra map[string]string) string {
	t.Helper()
	files := map[string]string{
		executable("nekobox", platform):      "new app",
		executable("nekobox_core", platform): "new built-in core",
		portableHelper(platform):             "new updater",
		"config/groups/user.json":            "should never be installed",
	}
	for name, data := range extra {
		files[name] = data
	}
	var buf bytes.Buffer
	w := zip.NewWriter(&buf)
	for name, data := range files {
		member, err := w.Create("nekoray/" + name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := io.WriteString(member, data); err != nil {
			t.Fatal(err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	archive := filepath.Join(t.TempDir(), "app.zip")
	if err := os.WriteFile(archive, buf.Bytes(), 0600); err != nil {
		t.Fatal(err)
	}
	return archive
}

func TestPortableArchiveValidation(t *testing.T) {
	for _, platform := range []string{"linux", "windows"} {
		archive := makePortableFixture(t, platform, nil)
		if err := extractPortable(archive, t.TempDir(), platform); err != nil {
			t.Fatal(err)
		}
		if err := extractPortable(archive, t.TempDir(), map[string]string{"linux": "windows", "windows": "linux"}[platform]); err == nil {
			t.Fatal("wrong platform package accepted")
		}
	}
	archive := makePortableFixture(t, runtime.GOOS, map[string]string{"../escaped": "bad"})
	if err := extractPortable(archive, t.TempDir(), runtime.GOOS); err == nil {
		t.Fatal("unsafe path accepted")
	}
}

func TestPortableReplacementRollback(t *testing.T) {
	install := t.TempDir()
	app, core := executable("nekobox", runtime.GOOS), executable("nekobox_core", runtime.GOOS)
	if err := os.MkdirAll(filepath.Join(install, "config", "groups"), 0700); err != nil {
		t.Fatal(err)
	}
	write := func(name, data string) {
		t.Helper()
		if err := os.WriteFile(filepath.Join(install, name), []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
	}
	assert := func(name, want string) {
		t.Helper()
		got, err := os.ReadFile(filepath.Join(install, name))
		if err != nil || string(got) != want {
			t.Fatalf("%s: got %q, %v", name, got, err)
		}
	}
	write(app, "old app")
	write(core, "old core")
	userFile := filepath.Join("config", "groups", "user.json")
	write(userFile, "user data")

	archive := makePortableFixture(t, runtime.GOOS, nil)
	unpacked, backup := t.TempDir(), t.TempDir()
	if err := extractPortable(archive, unpacked, runtime.GOOS); err != nil {
		t.Fatal(err)
	}
	fail := errors.New("simulated file lock")
	err, rollbackErr := replacePortable(unpacked, backup, install, func(from, to string) error {
		if from == filepath.Join(unpacked, core) {
			return fail
		}
		return os.Rename(from, to)
	})
	if !errors.Is(err, fail) || rollbackErr != nil {
		t.Fatalf("replacement %v, rollback %v", err, rollbackErr)
	}
	assert(app, "old app")
	assert(core, "old core")
	assert(userFile, "user data")

	digest, err := fileSHA256(archive)
	if err != nil {
		t.Fatal(err)
	}
	revision := sha256.Sum256([]byte(fmt.Sprintf("app.zip:%s\n", digest)))
	if err := applyPortable(archive, hex.EncodeToString(revision[:]), install); err != nil {
		t.Fatal(err)
	}
	assert(app, "new app")
	assert(core, "new built-in core")
	assert(userFile, "user data")
}
