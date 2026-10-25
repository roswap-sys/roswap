//go:build linux

package chunk

import (
	"fmt"
	"syscall"
)

// StatHugetlbfs 确认 path 是 1GB hugetlbfs 挂载点。
func StatHugetlbfs(path string) error {
	var st syscall.Statfs_t
	if err := syscall.Statfs(path, &st); err != nil {
		return fmt.Errorf("statfs %s: %w", path, err)
	}
	return CheckHugetlbfs(path, int64(st.Type), int64(st.Bsize))
}
