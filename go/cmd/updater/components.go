package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

const forkRepo = "ghost-sniper/nekoray"

var componentIDs = []string{"sing-box", "xray", "mihomo", "geoip", "geosite", "nekoray"}
var revisionPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)
var versionPattern = regexp.MustCompile(`^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$`)
var buildDatePattern = regexp.MustCompile(`-[0-9]{4}-[0-9]{2}-[0-9]{2}$`)

type releaseAsset struct {
	Name   string `json:"name"`
	URL    string `json:"browser_download_url"`
	Digest string `json:"digest"`
	Size   int64  `json:"size"`
}

type release struct {
	Tag       string         `json:"tag_name"`
	Published string         `json:"published_at"`
	Pre       bool           `json:"prerelease"`
	Draft     bool           `json:"draft"`
	Assets    []releaseAsset `json:"assets"`
}

type componentFile struct {
	Asset  releaseAsset
	Repo   string
	Name   string
	Format string
	Member string
}

type componentRelease struct {
	Version  string
	Revision string
	Files    []componentFile
}

type installedComponent struct {
	Version  string `json:"version"`
	Revision string `json:"revision"`
}

type componentEvent struct {
	ID        string `json:"id"`
	State     string `json:"state"`
	Installed string `json:"installed,omitempty"`
	Available string `json:"available,omitempty"`
	Revision  string `json:"revision,omitempty"`
	Progress  int    `json:"progress,omitempty"`
	Error     string `json:"error,omitempty"`
	Stage     string `json:"stage,omitempty"`
}

type releaseService struct {
	client *http.Client
	api    string
	mu     sync.Mutex
	cache  map[string]*releaseResponse
}

type releaseResponse struct {
	ready    chan struct{}
	releases []release
	err      error
}

var errNoRelease = errors.New("no release")
var errNoAsset = errors.New("no compatible release asset")

func (s *releaseService) request(ctx context.Context, address string) (*http.Response, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, address, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "Nekoray updater")
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		if resp.StatusCode == http.StatusTooManyRequests ||
			(resp.StatusCode == http.StatusForbidden && resp.Header.Get("X-RateLimit-Remaining") == "0") {
			return nil, errors.New("GitHub rate limit reached. Try again later.")
		}
		if resp.StatusCode == http.StatusNotFound && strings.HasPrefix(address, s.api+"/repos/") {
			return nil, errNoRelease
		}
		return nil, fmt.Errorf("GitHub HTTP %d", resp.StatusCode)
	}
	return resp, nil
}

func (s *releaseService) readReleases(ctx context.Context, repo, endpoint string) ([]release, error) {
	key := repo + endpoint
	s.mu.Lock()
	if entry, found := s.cache[key]; found {
		s.mu.Unlock()
		select {
		case <-entry.ready:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
		return entry.releases, entry.err
	}
	result := &releaseResponse{ready: make(chan struct{})}
	s.cache[key] = result
	s.mu.Unlock()
	defer close(result.ready)
	ctx, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()
	resp, err := s.request(ctx, s.api+"/repos/"+key)
	if err == nil {
		defer resp.Body.Close()
		decoder := json.NewDecoder(io.LimitReader(resp.Body, 8<<20))
		if endpoint == "/releases?per_page=20" {
			err = decoder.Decode(&result.releases)
		} else {
			var one release
			err = decoder.Decode(&one)
			result.releases = []release{one}
		}
	}
	result.err = err
	return result.releases, result.err
}

func executable(name, platform string) string {
	if platform == "windows" {
		return name + ".exe"
	}
	return name
}

func coreAsset(id, tag, platform string) (name, format, member string) {
	version := strings.TrimPrefix(tag, "v")
	switch id {
	case "sing-box":
		folder := "sing-box-" + version + "-" + platform + "-amd64"
		if platform == "windows" {
			return folder + ".zip", "zip", folder + "/sing-box.exe"
		}
		return folder + ".tar.gz", "tar.gz", folder + "/sing-box"
	case "xray":
		return "Xray-" + platform + "-64.zip", "zip", executable("xray", platform)
	case "mihomo":
		base := "mihomo-" + platform + "-amd64-compatible"
		if platform == "windows" {
			return base + "-v" + version + ".zip", "zip", base + ".exe"
		}
		return base + "-v" + version + ".gz", "gz", ""
	}
	return "", "", ""
}

func assetByName(r release, name string) (releaseAsset, error) {
	for _, asset := range r.Assets {
		if asset.Name == name {
			return asset, nil
		}
	}
	return releaseAsset{}, errNoAsset
}

func (s *releaseService) discover(ctx context.Context, id, platform, arch string, prerelease bool) (componentRelease, error) {
	var candidate componentRelease
	if id == "geoip" || id == "geosite" {
		var sources [][3]string
		if id == "geoip" {
			sources = [][3]string{
				{"Loyalsoldier/v2ray-rules-dat", "geoip.dat", "geoip.dat"},
				{"SagerNet/sing-geoip", "geoip.db", "geoip.db"},
				{"MetaCubeX/meta-rules-dat", "geoip.dat", "mihomo-geoip.dat"},
				{"MetaCubeX/meta-rules-dat", "geoip.metadb", "geoip.metadb"},
			}
		} else {
			sources = [][3]string{
				{"v2fly/domain-list-community", "dlc.dat", "geosite.dat"},
				{"SagerNet/sing-geosite", "geosite.db", "geosite.db"},
				{"MetaCubeX/meta-rules-dat", "geosite.dat", "mihomo-geosite.dat"},
			}
		}
		var versions []string
		seenRepos := make(map[string]bool)
		for _, source := range sources {
			repo, assetName, destination := source[0], source[1], source[2]
			endpoint := "/releases/latest"
			if repo == "MetaCubeX/meta-rules-dat" {
				endpoint = "/releases/tags/latest"
			}
			releases, err := s.readReleases(ctx, repo, endpoint)
			if err != nil {
				return candidate, fmt.Errorf("%s: %w", repo, err)
			}
			r := releases[0]
			asset, err := assetByName(r, assetName)
			if err != nil {
				return candidate, fmt.Errorf("%s %s: %w", repo, assetName, err)
			}
			if !seenRepos[repo] {
				version := r.Tag
				if version == "latest" && len(r.Published) >= 10 {
					version = r.Published[:10]
				}
				versions = append(versions, version)
				seenRepos[repo] = true
			}
			candidate.Files = append(candidate.Files, componentFile{asset, repo, destination, "raw", ""})
		}
		candidate.Version = strings.Join(versions, " / ")
		return identifyRelease(candidate)
	}
	if arch != "amd64" || (platform != "windows" && platform != "linux") {
		return candidate, errNoAsset
	}

	repo := map[string]string{"nekoray": forkRepo, "sing-box": "SagerNet/sing-box", "xray": "XTLS/Xray-core", "mihomo": "MetaCubeX/mihomo"}[id]
	if repo == "" {
		return candidate, fmt.Errorf("unknown component %q", id)
	}
	endpoint := "/releases/latest"
	if id == "nekoray" {
		endpoint = "/releases?per_page=20"
	}
	releases, err := s.readReleases(ctx, repo, endpoint)
	if err != nil {
		return candidate, err
	}
	bestTag := ""
	for _, r := range releases {
		if r.Draft || (r.Pre && !(id == "nekoray" && prerelease)) || !versionPattern.MatchString(r.Tag) {
			continue
		}
		if id == "nekoray" {
			suffix := "-" + platform + "64.zip"
			version := strings.TrimPrefix(r.Tag, "v")
			for _, asset := range r.Assets {
				stem := strings.TrimSuffix(asset.Name, suffix)
				packageVersion := strings.TrimPrefix(stem, "nekoray-")
				if !strings.HasSuffix(asset.Name, suffix) || stem == packageVersion ||
					(packageVersion != version && buildDatePattern.ReplaceAllString(packageVersion, "") != version) {
					continue
				}
				if bestTag == "" || compareVersions(r.Tag, bestTag) > 0 ||
					(r.Tag == bestTag && compareAppVersions(packageVersion, candidate.Version) > 0) {
					bestTag = r.Tag
					candidate = componentRelease{Version: packageVersion, Files: []componentFile{{asset, repo, "app.zip", "raw", ""}}}
				}
			}
			continue
		}
		name, format, member := coreAsset(id, r.Tag, platform)
		asset, err := assetByName(r, name)
		if err != nil && id == "mihomo" {
			name = strings.Replace(name, "-compatible-", "-v1-", 1)
			member = strings.Replace(member, "-compatible", "-v1", 1)
			asset, err = assetByName(r, name)
		}
		if err != nil {
			return candidate, err
		}
		candidate = componentRelease{Version: r.Tag, Files: []componentFile{{asset, repo, executable(id, platform), format, member}}}
		break
	}
	if len(candidate.Files) == 0 {
		if len(releases) == 0 {
			return candidate, errNoRelease
		}
		return candidate, errNoAsset
	}
	return identifyRelease(candidate)
}

func identifyRelease(candidate componentRelease) (componentRelease, error) {
	sum := sha256.New()
	for _, file := range candidate.Files {
		digest := strings.TrimPrefix(file.Asset.Digest, "sha256:")
		if digest == file.Asset.Digest || !revisionPattern.MatchString(digest) {
			return candidate, fmt.Errorf("missing SHA-256 for %s", file.Asset.Name)
		}
		address, err := url.Parse(file.Asset.URL)
		if err != nil || address.Scheme != "https" || address.Host != "github.com" ||
			!strings.HasPrefix(strings.ToLower(address.Path), "/"+strings.ToLower(file.Repo)+"/releases/download/") ||
			filepath.Base(address.Path) != file.Asset.Name {
			return candidate, fmt.Errorf("unexpected GitHub asset URL for %s", file.Asset.Name)
		}
		fmt.Fprintf(sum, "%s:%s\n", file.Name, digest)
	}
	candidate.Revision = hex.EncodeToString(sum.Sum(nil))
	return candidate, nil
}

func compareVersions(a, b string) int {
	left, right := versionPattern.FindStringSubmatch(a), versionPattern.FindStringSubmatch(b)
	if left == nil || right == nil {
		return strings.Compare(a, b)
	}
	for i := 1; i <= 3; i++ {
		x, _ := strconv.Atoi(left[i])
		y, _ := strconv.Atoi(right[i])
		if x != y {
			if x < y {
				return -1
			}
			return 1
		}
	}
	if left[4] == right[4] {
		return 0
	}
	if left[4] == "" {
		return 1
	}
	if right[4] == "" {
		return -1
	}
	for i, x := range strings.Split(left[4], ".") {
		parts := strings.Split(right[4], ".")
		if i >= len(parts) {
			return 1
		}
		y := parts[i]
		if x == y {
			continue
		}
		n, errX := strconv.Atoi(x)
		m, errY := strconv.Atoi(y)
		switch {
		case errX == nil && errY == nil:
			if n < m {
				return -1
			}
			return 1
		case errX == nil:
			return -1
		case errY == nil:
			return 1
		default:
			return strings.Compare(x, y)
		}
	}
	return -1
}

func compareAppVersions(a, b string) int {
	left, right := buildDatePattern.ReplaceAllString(a, ""), buildDatePattern.ReplaceAllString(b, "")
	if comparison := compareVersions(left, right); comparison != 0 {
		return comparison
	}
	leftDate, rightDate := buildDatePattern.FindString(a), buildDatePattern.FindString(b)
	if leftDate != "" && rightDate != "" {
		return strings.Compare(leftDate, rightDate)
	}
	return 0
}

func currentComponent(root, id string) installedComponent {
	var installed installedComponent
	data, err := os.ReadFile(filepath.Join(root, id, "current.json"))
	if err != nil || json.Unmarshal(data, &installed) != nil || !revisionPattern.MatchString(installed.Revision) {
		return installedComponent{}
	}
	return installed
}

func (s *releaseService) status(ctx context.Context, root, id, appVersion string, pre bool) (componentEvent, componentRelease) {
	event := componentEvent{ID: id, State: "available"}
	installed := currentComponent(root, id)
	if id == "nekoray" {
		installed.Version = appVersion
	} else if installed.Revision != "" {
		if _, err := readStaged(root, id, installed.Revision); err != nil {
			installed = installedComponent{}
		}
	}
	event.Installed = installed.Version
	candidate, err := s.discover(ctx, id, runtime.GOOS, runtime.GOARCH, pre)
	if err != nil {
		switch {
		case errors.Is(err, errNoRelease) && id == "nekoray":
			event.State = "no-release"
		case errors.Is(err, errNoAsset):
			event.State = "unsupported"
		default:
			event.State, event.Error = "error", err.Error()
		}
		return event, candidate
	}
	event.Available, event.Revision = candidate.Version, candidate.Revision
	complete := installed.Revision == candidate.Revision
	for _, file := range candidate.Files {
		info, err := os.Stat(filepath.Join(root, id, installed.Revision, file.Name))
		complete = complete && err == nil && info.Mode().IsRegular() && info.Size() > 0
	}
	if complete || (id == "nekoray" && compareAppVersions(candidate.Version, installed.Version) <= 0) {
		event.State = "current"
	} else if staged, err := readStaged(root, id, candidate.Revision); err == nil && staged.Version == candidate.Version {
		event.State, event.Stage = "staged", candidate.Revision
	}
	return event, candidate
}

func runComponents(args []string) error {
	if len(args) > 0 && args[0] == "apply" {
		return applyAndRestart(args[1:])
	}
	if len(args) < 4 || (args[0] != "check" && args[0] != "install" && args[0] != "activate") || !filepath.IsAbs(args[1]) {
		return errors.New("expected check|install|activate ROOT APP_VERSION PRERELEASE [ID=REVISION ...]")
	}
	mode, root, appVersion := args[0], args[1], args[2]
	pre := args[3] == "true"
	selections := map[string]string{}
	for _, item := range args[4:] {
		id, revision, ok := strings.Cut(item, "=")
		known := false
		for _, component := range componentIDs {
			if component == id {
				known = true
				break
			}
		}
		if !ok || !known || !revisionPattern.MatchString(revision) {
			return fmt.Errorf("invalid component selection %q", item)
		}
		selections[id] = revision
	}
	encoder := json.NewEncoder(os.Stdout)
	var output sync.Mutex
	emit := func(event componentEvent) {
		output.Lock()
		defer output.Unlock()
		_ = encoder.Encode(event)
	}
	ctx := context.Background()
	var transport *http.Transport
	if os.Getenv("NEKORAY_UPDATE_CONTROL") == "1" && mode != "activate" {
		control, controlledContext := newUpdateControl(ctx, os.Stdin, encoder, &output)
		ctx = controlledContext
		transport = http.DefaultTransport.(*http.Transport).Clone()
		transport.Proxy = control.proxy
		defer transport.CloseIdleConnections()
	}
	client := &http.Client{Timeout: 10 * time.Minute, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		host := req.URL.Hostname()
		github := host == "github.com" || host == "api.github.com" ||
			host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com" ||
			host == "github-releases.githubusercontent.com"
		if len(via) >= 8 || req.URL.Scheme != "https" || !github {
			return errors.New("invalid download redirect")
		}
		return nil
	}}
	if transport != nil {
		client.Transport = transport
	}
	service := releaseService{
		client: client,
		api:    "https://api.github.com", cache: make(map[string]*releaseResponse),
	}
	if mode == "check" {
		results := make(chan componentEvent, len(componentIDs))
		for _, id := range componentIDs {
			go func(id string) {
				event, _ := service.status(ctx, root, id, appVersion, pre)
				results <- event
			}(id)
		}
		for range componentIDs {
			emit(<-results)
		}
		return nil
	}
	for _, id := range componentIDs {
		if ctx.Err() != nil {
			break
		}
		if selections[id] == "" {
			continue
		}
		if mode == "activate" {
			event := componentEvent{ID: id, State: "installed", Revision: selections[id]}
			version, err := activateStaged(root, id, selections[id])
			event.Installed, event.Available = version, version
			if err != nil {
				event.State, event.Error = "error", err.Error()
			}
			emit(event)
			continue
		}
		event, candidate := service.status(ctx, root, id, appVersion, pre)
		if ctx.Err() != nil {
			break
		}
		if event.State != "available" {
			emit(event)
			continue
		}
		if candidate.Revision != selections[id] {
			event.State, event.Error = "error", "Release changed. Check for updates again."
			emit(event)
			continue
		}
		err := service.install(ctx, root, &event, candidate, emit)
		if errors.Is(err, context.Canceled) || (err != nil && ctx.Err() != nil) {
			break
		}
		if err != nil {
			event.State, event.Error = "error", err.Error()
		}
		emit(event)
	}
	return nil
}
