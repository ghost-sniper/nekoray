package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const maxDownload = 512 << 20

type stagedComponent struct {
	installedComponent
	Files map[string]string `json:"files"`
}

func readStaged(root, id, revision string) (stagedComponent, error) {
	var record stagedComponent
	dir := filepath.Join(root, id, revision)
	data, err := os.ReadFile(filepath.Join(dir, ".release.json"))
	if err != nil {
		return record, err
	}
	if err := json.Unmarshal(data, &record); err != nil {
		return record, err
	}
	if record.Revision != revision || record.Version == "" || len(record.Files) == 0 {
		return record, errors.New("invalid staged release record")
	}
	for name, digest := range record.Files {
		if name != filepath.Base(name) || name == "." || name == ".." || !revisionPattern.MatchString(digest) {
			return record, errors.New("invalid staged file record")
		}
		info, err := os.Stat(filepath.Join(dir, name))
		if err != nil || !info.Mode().IsRegular() || info.Size() == 0 {
			return record, fmt.Errorf("missing staged file %s", name)
		}
	}
	return record, nil
}

func activateStaged(root, id, revision string) (string, error) {
	if id == "nekoray" {
		return "", errors.New("restart Nekoray to apply the staged app update")
	}
	record, err := readStaged(root, id, revision)
	if err != nil {
		return "", err
	}
	for name, digest := range record.Files {
		actual, err := fileSHA256(filepath.Join(root, id, revision, name))
		if err != nil {
			return "", err
		}
		if actual != digest {
			return "", fmt.Errorf("staged %s no longer matches its verified digest", name)
		}
	}
	err = writeCurrent(filepath.Join(root, id), record.installedComponent)
	return record.Version, err
}

type downloadProgress struct {
	written, size int64
	last          int
	report        func(int)
}

func (p *downloadProgress) Write(data []byte) (int, error) {
	p.written += int64(len(data))
	percent := int(p.written * 100 / p.size)
	if percent >= p.last+2 && percent <= 100 {
		p.last = percent
		p.report(percent)
	}
	return len(data), nil
}

func (s *releaseService) download(ctx context.Context, asset releaseAsset, target string, report func(int)) error {
	if asset.Size <= 0 || asset.Size > maxDownload {
		return fmt.Errorf("invalid download size for %s", asset.Name)
	}
	response, err := s.request(ctx, asset.URL)
	if err != nil {
		return err
	}
	defer response.Body.Close()

	file, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	sum := sha256.New()
	progress := &downloadProgress{size: asset.Size, report: report}
	n, copyErr := io.Copy(io.MultiWriter(file, sum, progress), io.LimitReader(response.Body, asset.Size+1))
	err = errors.Join(copyErr, file.Sync(), file.Close())
	if err != nil {
		return err
	}
	if n != asset.Size || "sha256:"+hex.EncodeToString(sum.Sum(nil)) != asset.Digest {
		return fmt.Errorf("SHA-256 or size mismatch for %s", asset.Name)
	}
	return nil
}

func (s *releaseService) install(ctx context.Context, root string, event *componentEvent, candidate componentRelease, emit func(componentEvent)) error {
	parent := filepath.Join(root, event.ID)
	if err := os.MkdirAll(parent, 0700); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(parent, ".download-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)

	for index, file := range candidate.Files {
		event.State, event.Progress = "downloading", index*100/len(candidate.Files)
		emit(*event)
		payload := filepath.Join(stage, file.Name)
		if file.Format != "raw" {
			payload = filepath.Join(stage, "archive")
		}
		err := s.download(ctx, file.Asset, payload, func(percent int) {
			event.Progress = (index*100 + percent) / len(candidate.Files)
			emit(*event)
		})
		if err != nil {
			return err
		}
		if file.Format != "raw" {
			if err := extractExecutable(payload, filepath.Join(stage, file.Name), file); err != nil {
				return err
			}
			if event.ID == "sing-box" {
				library := "libcronet.so"
				if runtime.GOOS == "windows" {
					library = "libcronet.dll"
				}
				companion := file
				companion.Member = path.Join(path.Dir(file.Member), library)
				err := extractExecutable(payload, filepath.Join(stage, library), companion)
				if err != nil && !errors.Is(err, errArchiveMemberMissing) {
					return err
				}
			}
			if event.ID == "xray" {
				for _, asset := range []string{"geoip.dat", "geosite.dat"} {
					companion := file
					companion.Member = asset
					err := extractExecutable(payload, filepath.Join(stage, asset), companion)
					if err != nil && !errors.Is(err, errArchiveMemberMissing) {
						return err
					}
				}
			}
			if err := os.Remove(payload); err != nil {
				return err
			}
		}
	}
	if event.ID == "nekoray" {
		if err := extractPortable(filepath.Join(stage, "app.zip"), filepath.Join(stage, "validate"), runtime.GOOS); err != nil {
			return err
		}
		if err := os.RemoveAll(filepath.Join(stage, "validate")); err != nil {
			return err
		}
	}

	record := stagedComponent{installedComponent{candidate.Version, candidate.Revision}, make(map[string]string)}
	entries, err := os.ReadDir(stage)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		digest, err := fileSHA256(filepath.Join(stage, entry.Name()))
		if err != nil {
			return err
		}
		record.Files[entry.Name()] = digest
	}
	data, err := json.Marshal(record)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(stage, ".release.json"), data, 0600); err != nil {
		return err
	}

	destination := filepath.Join(parent, candidate.Revision)
	if err := os.Rename(stage, destination); err != nil {
		// A previous attempt may have installed this immutable revision already.
		// Compare every output before selecting it.
		for name, digest := range record.Files {
			current, readErr := fileSHA256(filepath.Join(destination, name))
			if readErr != nil || digest != current {
				return fmt.Errorf("cannot reuse staged %s: %w", name, err)
			}
		}
	}
	event.State, event.Stage = "staged", candidate.Revision
	return nil
}

func writeCurrent(parent string, current installedComponent) error {
	file, err := os.CreateTemp(parent, ".current-")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if err := errors.Join(json.NewEncoder(file).Encode(current), file.Sync(), file.Close()); err != nil {
		return err
	}
	return os.Rename(file.Name(), filepath.Join(parent, "current.json"))
}

func fileSHA256(name string) (string, error) {
	file, err := os.Open(name)
	if err != nil {
		return "", err
	}
	defer file.Close()
	sum := sha256.New()
	if _, err := io.Copy(sum, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(sum.Sum(nil)), nil
}

func copyArchiveMember(reader io.Reader, dest string, limit int64, mode os.FileMode) error {
	file, err := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE|os.O_EXCL, mode)
	if err != nil {
		return err
	}
	n, copyErr := io.Copy(file, io.LimitReader(reader, limit+1))
	if err := errors.Join(copyErr, file.Close()); err != nil {
		return err
	}
	if (n == 0 && limit != 0) || n > limit {
		return fmt.Errorf("invalid archive member size for %s", filepath.Base(dest))
	}
	return nil
}

var errArchiveMemberMissing = errors.New("archive member missing")

func extractExecutable(archive, dest string, spec componentFile) error {
	if spec.Format == "zip" {
		reader, err := zip.OpenReader(archive)
		if err != nil {
			return err
		}
		defer reader.Close()
		var member *zip.File
		for _, file := range reader.File {
			if file.Name == spec.Member {
				if member != nil || !file.Mode().IsRegular() {
					return errors.New("invalid executable archive member")
				}
				member = file
			}
		}
		if member == nil {
			return fmt.Errorf("%w: %s", errArchiveMemberMissing, spec.Member)
		}
		input, err := member.Open()
		if err != nil {
			return err
		}
		defer input.Close()
		return copyArchiveMember(input, dest, maxDownload, 0755)
	}

	file, err := os.Open(archive)
	if err != nil {
		return err
	}
	defer file.Close()
	reader, err := gzip.NewReader(file)
	if err != nil {
		return err
	}
	defer reader.Close()
	if spec.Format == "gz" {
		return copyArchiveMember(reader, dest, maxDownload, 0755)
	}
	tarReader := tar.NewReader(io.LimitReader(reader, maxDownload))
	for {
		entry, err := tarReader.Next()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return fmt.Errorf("%w: %s", errArchiveMemberMissing, spec.Member)
			}
			return err
		}
		if entry.Name == spec.Member && entry.Typeflag == tar.TypeReg {
			return copyArchiveMember(tarReader, dest, maxDownload, 0755)
		}
	}
}

func validArchivePath(name, platform string) bool {
	if !strings.HasPrefix(name, "nekoray/") || path.Clean(name) != name {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if part == "." || part == ".." || strings.ContainsAny(part, "\\:\x00") ||
			strings.TrimRight(part, ". ") != part {
			return false
		}
		if platform == "windows" {
			device, _, _ := strings.Cut(strings.ToUpper(part), ".")
			switch device {
			case "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$":
				return false
			}
			if len(device) == 4 && (strings.HasPrefix(device, "COM") || strings.HasPrefix(device, "LPT")) &&
				device[3] >= '0' && device[3] <= '9' {
				return false
			}
		}
	}
	return true
}

func portableEntryAllowed(name string, directory bool) bool {
	name = strings.ToLower(name)
	if directory {
		switch name {
		case "usr", "bearer", "generic", "iconengines", "imageformats", "networkinformation",
			"platforms", "platformthemes", "styles", "tls", "translations", "plugins":
			return true
		}
		return false
	}
	switch name {
	case "nekobox", "nekobox.exe", "nekobox_core", "nekobox_core.exe", "launcher", "updater",
		"updater.exe", "nekobox.png", "geoip.dat", "geosite.dat", "geoip.db", "geosite.db",
		"geoip.metadb", "license", "license.txt", "readme.md", "qt.conf", "qtbase_zh_cn.qm":
		return true
	}
	return strings.HasSuffix(name, ".dll")
}

func extractPortable(archive, destination, platform string) error {
	reader, err := zip.OpenReader(archive)
	if err != nil {
		return err
	}
	defer reader.Close()
	if err := os.MkdirAll(destination, 0700); err != nil {
		return err
	}
	seen := make(map[string]bool)
	var total uint64
	for _, member := range reader.File {
		name := strings.TrimSuffix(member.Name, "/")
		if name == "nekoray" && member.FileInfo().IsDir() {
			continue
		}
		if !validArchivePath(name, platform) || seen[strings.ToLower(name)] {
			return fmt.Errorf("invalid portable archive path %q", member.Name)
		}
		seen[strings.ToLower(name)] = true
		relative := strings.TrimPrefix(name, "nekoray/")
		top, _, nested := strings.Cut(relative, "/")
		if !portableEntryAllowed(top, nested || member.FileInfo().IsDir()) {
			continue
		}
		dest := filepath.Join(destination, filepath.FromSlash(relative))
		if member.FileInfo().IsDir() {
			if err := os.MkdirAll(dest, 0755); err != nil {
				return err
			}
			continue
		}
		if !member.Mode().IsRegular() || member.UncompressedSize64 > maxDownload {
			return fmt.Errorf("invalid portable archive member %q", member.Name)
		}
		total += member.UncompressedSize64
		if total > 1<<30 {
			return errors.New("portable archive is too large")
		}
		if err := os.MkdirAll(filepath.Dir(dest), 0755); err != nil {
			return err
		}
		input, err := member.Open()
		if err != nil {
			return err
		}
		mode := member.Mode().Perm() | 0600
		switch top {
		case "nekobox", "nekobox_core", "launcher", "updater":
			mode |= 0111
		}
		err = errors.Join(copyArchiveMember(input, dest, int64(member.UncompressedSize64), mode), input.Close())
		if err != nil {
			return err
		}
	}
	for _, name := range []string{executable("nekobox", platform), executable("nekobox_core", platform), portableHelper(platform)} {
		info, err := os.Stat(filepath.Join(destination, name))
		if err != nil || !info.Mode().IsRegular() || info.Size() == 0 {
			return fmt.Errorf("portable archive does not contain %s", name)
		}
	}
	return nil
}

func waitForParent(pid int) error {
	deadline := time.Now().Add(30 * time.Second)
	for {
		alive, err := processAlive(pid)
		if err != nil || !alive {
			return err
		}
		if time.Now().After(deadline) {
			return errors.New("Nekoray did not exit before the update deadline")
		}
		time.Sleep(100 * time.Millisecond)
	}
}
