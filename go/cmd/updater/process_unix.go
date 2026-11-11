//go:build !windows

package main

import (
	"errors"
	"os"
	"strconv"
	"strings"
	"syscall"
)

func processAlive(pid int) (bool, error) {
	err := syscall.Kill(pid, 0)
	if errors.Is(err, syscall.ESRCH) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	stat, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/stat")
	if err == nil {
		fields := string(stat)
		end := strings.LastIndexByte(fields, ')')
		if end >= 0 && strings.HasPrefix(fields[end+1:], " Z ") {
			return false, nil
		}
	}
	return true, nil
}
