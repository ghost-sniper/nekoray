package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
)

func portableHelper(platform string) string {
	if platform == "windows" {
		return "updater.exe"
	}
	return "launcher"
}

func applyAndRestart(args []string) error {
	if len(args) < 5 || !filepath.IsAbs(args[0]) || !revisionPattern.MatchString(args[1]) || !filepath.IsAbs(args[2]) {
		return errors.New("expected apply ROOT REVISION INSTALL_DIR PID RESTART_MODE [ARGS ...]")
	}
	root, revision, installDir := args[0], args[1], args[2]
	pid, err := strconv.Atoi(args[3])
	if err != nil || pid <= 1 || pid == os.Getpid() {
		return errors.New("invalid Nekoray process ID")
	}
	if err := waitForParent(pid); err != nil {
		return err
	}
	program := executable("nekobox", runtime.GOOS)
	restartArgs := args[5:]
	if args[4] == "launcher" && runtime.GOOS == "linux" {
		program = "launcher"
		restartArgs = append([]string{"--"}, restartArgs...)
	}
	archive := filepath.Join(root, "nekoray", revision, "app.zip")
	if worker, err := os.Executable(); err == nil && runtime.GOOS != "windows" &&
		filepath.Dir(worker) == os.TempDir() && strings.HasPrefix(filepath.Base(worker), "nekoray-updater-") {
		defer os.Remove(worker)
	}
	applyErr := applyPortable(archive, revision, installDir)
	errorFile := filepath.Join(root, "nekoray", "last-error")
	if applyErr != nil {
		_ = os.WriteFile(errorFile, []byte(applyErr.Error()), 0600)
	} else {
		_ = os.Remove(errorFile)
	}
	cmd := exec.Command(filepath.Join(installDir, program), restartArgs...)
	cmd.Dir = installDir
	return errors.Join(applyErr, cmd.Start())
}

func applyPortable(archive, revision, installDir string) error {
	digest, err := fileSHA256(archive)
	if err != nil {
		return err
	}
	sum := sha256.Sum256([]byte("app.zip:" + digest + "\n"))
	if hex.EncodeToString(sum[:]) != revision {
		return errors.New("staged Nekoray archive does not match its verified release")
	}
	lock := filepath.Join(installDir, ".nekoray-update.lock")
	if err := os.Mkdir(lock, 0700); err != nil {
		return fmt.Errorf("cannot lock the Nekoray installation: %w", err)
	}
	defer os.Remove(lock)
	stage, err := os.MkdirTemp(installDir, ".nekoray-update-")
	if err != nil {
		return err
	}
	keepBackup := false
	defer func() {
		if !keepBackup {
			os.RemoveAll(stage)
		}
	}()
	unpacked, backup := filepath.Join(stage, "new"), filepath.Join(stage, "backup")
	if err := extractPortable(archive, unpacked, runtime.GOOS); err != nil {
		return err
	}
	if err := os.Mkdir(backup, 0700); err != nil {
		return err
	}
	err, rollbackErr := replacePortable(unpacked, backup, installDir, os.Rename)
	if rollbackErr != nil {
		keepBackup = true
		return errors.Join(err, fmt.Errorf("rollback failed, backup saved at %s: %w", backup, rollbackErr))
	}
	return err
}

func replacePortable(unpacked, backup, installDir string, rename func(string, string) error) (err, rollbackErr error) {
	entries, err := os.ReadDir(unpacked)
	if err != nil {
		return err, nil
	}
	type replacement struct {
		name        string
		backedUp    bool
		replacement bool
	}
	var changes []replacement
	defer func() {
		if err == nil {
			return
		}
		for i := len(changes) - 1; i >= 0; i-- {
			change := changes[i]
			destination := filepath.Join(installDir, change.name)
			if change.replacement {
				if moveErr := rename(destination, filepath.Join(unpacked, change.name)); moveErr != nil {
					rollbackErr = errors.Join(rollbackErr, moveErr)
					continue
				}
			}
			if change.backedUp {
				rollbackErr = errors.Join(rollbackErr, rename(filepath.Join(backup, change.name), destination))
			}
		}
	}()
	for _, entry := range entries {
		name := entry.Name()
		destination := filepath.Join(installDir, name)
		change := replacement{name: name}
		if _, err := os.Lstat(destination); err == nil {
			if err := rename(destination, filepath.Join(backup, name)); err != nil {
				return err, nil
			}
			change.backedUp = true
		} else if !errors.Is(err, os.ErrNotExist) {
			return err, nil
		}
		changes = append(changes, change)
		if err := rename(filepath.Join(unpacked, name), destination); err != nil {
			return err, nil
		}
		changes[len(changes)-1].replacement = true
	}
	return nil, nil
}
