// Package hostplan 描述主机侧操作的命令序列，不直接执行。
package hostplan

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Action 是一条可打印、可执行的步骤。
type Action struct {
	Argv        []string
	Env         []string
	IgnoreError bool
}

// FormatAction 打印将要执行的步骤。
func FormatAction(a Action) string {
	if len(a.Argv) == 0 {
		return ""
	}
	return "+ " + strings.Join(append(append([]string{}, a.Env...), a.Argv...), " ")
}

// FioVerify 运行校验作业；作业文件通过 ROSWAP_DEV 取设备，设备至少 1GB。
func FioVerify(dev, fioJob string) Action {
	return Action{Argv: []string{"fio", fioJob}, Env: []string{"ROSWAP_DEV=" + dev}}
}

// ModuleListed 判断 lsmod / /proc/modules 文本里是否有该模块。
func ModuleListed(text, name string) bool {
	for _, line := range strings.Split(text, "\n") {
		fields := strings.Fields(line)
		if len(fields) > 0 && fields[0] == name {
			return true
		}
	}
	return false
}

// SwapActive 判断 swapon --show 的输出里是否包含该设备。
func SwapActive(show, dev string) bool {
	for _, line := range strings.Split(show, "\n") {
		if strings.TrimSpace(line) == dev {
			return true
		}
	}
	return false
}

// HugepageNRFiles 列出每个 NUMA 节点的 1GB nr_hugepages 文件。
func HugepageNRFiles(nodeRoot string) ([]string, error) {
	ents, err := os.ReadDir(nodeRoot)
	if err != nil {
		return nil, fmt.Errorf("内核没有 1GB hugetlb。检查 CONFIG_HUGETLBFS 和启动参数 hugepagesz=1G")
	}
	var out []string
	for _, ent := range ents {
		if !strings.HasPrefix(ent.Name(), "node") {
			continue
		}
		path := filepath.Join(nodeRoot, ent.Name(), "hugepages", "hugepages-1048576kB", "nr_hugepages")
		st, err := os.Stat(path)
		if err == nil && !st.IsDir() {
			out = append(out, path)
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("内核没有 1GB hugetlb。检查 CONFIG_HUGETLBFS 和启动参数 hugepagesz=1G")
	}
	return out, nil
}

// MountArgs 挂载 1GB hugetlbfs。
func MountArgs(mnt string) []string {
	return []string{"mount", "-t", "hugetlbfs", "-o", "pagesize=1G,mode=0770", "none", mnt}
}

// W2Actions 是块设备验收：fio 校验，然后 mkswap / swapon / swapoff。
func W2Actions(dev, fioJob string) []Action {
	return []Action{
		FioVerify(dev, fioJob),
		{Argv: []string{"mkswap", dev}},
		{Argv: []string{"swapon", "-p", "32767", "--discard=pages", dev}},
		{Argv: []string{"swapoff", dev}},
	}
}

// W3DD 向设备写入 512MB 以触发 extent 分配。
func W3DD(dev string) []string {
	return []string{"dd", "if=/dev/urandom", "of=" + dev, "bs=1M", "count=512", "oflag=direct", "status=progress"}
}

// W3Discard 丢弃整盘。
func W3Discard(dev string) []string {
	return []string{"blkdiscard", "-f", dev}
}
