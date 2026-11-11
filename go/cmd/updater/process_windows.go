package main

import (
	"errors"
	"syscall"
)

func processAlive(pid int) (bool, error) {
	process, err := syscall.OpenProcess(syscall.SYNCHRONIZE, false, uint32(pid))
	if errors.Is(err, syscall.EINVAL) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	defer syscall.CloseHandle(process)
	status, err := syscall.WaitForSingleObject(process, 0)
	return status == syscall.WAIT_TIMEOUT, err
}
