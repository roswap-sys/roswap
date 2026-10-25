package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"roswap/pkg/chunk"
	"roswap/tools/cluster"
	"roswap/tools/hostplan"
	"roswap/tools/ofedprobe"
)

func newFlag(name string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	return fs
}

func cmdProbe(args []string) int {
	fs := newFlag("probe")
	ofed := fs.String("ofed", "/usr/src/ofa_kernel/default", "OFED 目录")
	kdir := fs.String("kdir", "", "内核构建目录")
	out := fs.String("out", "kernel/roswap/include/rs_compat.h", "输出头文件")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *kdir == "" {
		rel, _ := exec.Command("uname", "-r").Output()
		*kdir = filepath.Join("/lib/modules", strings.TrimSpace(string(rel)), "build")
	}
	msg, err := ofedprobe.Probe(*ofed, *kdir, *out)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println(msg)
	return 0
}

func koPath(root, ko string) string {
	if ko != "" {
		return ko
	}
	return filepath.Join(root, "kernel/roswap/roswap.ko")
}

func cmdLoad(args []string) int {
	fs := newFlag("load")
	root := fs.String("root", ".", "仓库根目录")
	ko := fs.String("ko", "", "ko 路径")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	path := koPath(*root, *ko)
	if _, err := os.Stat(path); err != nil {
		fmt.Fprintf(os.Stderr, "未找到 %s，先在目标机执行 cmake --build build --target kmod\n", path)
		return 1
	}
	mods, _ := os.ReadFile("/proc/modules")
	if hostplan.ModuleListed(string(mods), "roswap") {
		fmt.Fprintln(os.Stderr, "roswap 已加载，先执行 roswap unload")
		return 1
	}
	if !requireRoot() {
		return 1
	}
	argv := append([]string{"insmod", path}, fs.Args()...)
	if err := run(argv); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println("已加载。dmesg 查看日志；控制文件 /sys/kernel/debug/roswap/ctl")
	return 0
}

func cmdUnload(args []string) int {
	if len(args) > 0 {
		fmt.Fprintln(os.Stderr, "用法: roswap unload")
		return 2
	}
	mods, err := os.ReadFile("/proc/modules")
	if err != nil || !hostplan.ModuleListed(string(mods), "roswap") {
		fmt.Println("roswap 未加载")
		return 0
	}
	if !requireRoot() {
		return 1
	}
	if err := run([]string{"rmmod", "roswap"}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println("已卸载 roswap")
	return 0
}

func cmdHugepages(args []string) int {
	fs := newFlag("hugepages")
	pages := fs.String("pages", "", "每个 NUMA 节点的 1GB 页数")
	mnt := fs.String("mount", "/mnt/rshuge", "挂载点")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *pages == "" {
		fmt.Fprintln(os.Stderr, "用法: roswap hugepages --pages N [--mount /mnt/rshuge]")
		return 2
	}
	files, err := hostplan.HugepageNRFiles("/sys/devices/system/node")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if !requireRoot() {
		return 1
	}
	for _, path := range files {
		if err := os.WriteFile(path, []byte(*pages+"\n"), 0644); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		got, _ := os.ReadFile(path)
		fmt.Printf("set %s = %s (实际 %s)\n", path, *pages, strings.TrimSpace(string(got)))
	}
	if err := os.MkdirAll(*mnt, 0755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if _, code := capture([]string{"findmnt", "-n", *mnt}); code != 0 {
		if err := run(hostplan.MountArgs(*mnt)); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
	}
	free, _ := os.ReadFile("/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages")
	if len(free) == 0 {
		free = []byte("?\n")
	}
	fmt.Printf("free_hugepages=%s\n", strings.TrimSpace(string(free)))
	if err := run([]string{"findmnt", *mnt}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func cmdW1(args []string) int {
	fs := newFlag("w1")
	root := fs.String("root", ".", "仓库根目录")
	chunks := fs.String("chunks", "/run/roswap-agent/chunks.json", "chunks.json")
	ko := fs.String("ko", "", "ko 路径")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if fs.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "用法: roswap w1 <从节点 RoCE IP> [--chunks FILE]")
		return 2
	}
	if _, err := os.Stat(*chunks); err != nil {
		fmt.Fprintf(os.Stderr, "找不到 %s。先在从节点启动 roswap-agent。\n", *chunks)
		return 2
	}
	if !requireRoot() {
		return 1
	}
	st, err := chunk.LoadState(*chunks)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	c, err := cluster.FirstChunk(st)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	path := koPath(*root, *ko)
	mods, _ := os.ReadFile("/proc/modules")
	if hostplan.ModuleListed(string(mods), "roswap") {
		if err := run([]string{"rmmod", "roswap"}); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
	}
	_ = run([]string{"dmesg", "-C"})
	argv := append([]string{"insmod", path}, cluster.InsmodArgs(fs.Arg(0), c)...)
	if err := run(argv); err != nil {
		fmt.Fprintln(os.Stderr, err)
	}
	dmesg, _ := exec.Command("dmesg").CombinedOutput()
	if strings.Contains(string(dmesg), "roswap: selftest OK") {
		fmt.Println("selftest OK")
		return 0
	}
	fmt.Fprintln(os.Stderr, "dmesg 里没有 selftest OK")
	lines := strings.Split(strings.TrimRight(string(dmesg), "\n"), "\n")
	if len(lines) > 40 {
		lines = lines[len(lines)-40:]
	}
	fmt.Fprintln(os.Stderr, strings.Join(lines, "\n"))
	return 1
}

func isBlock(path string) bool {
	st, err := os.Stat(path)
	return err == nil && st.Mode()&os.ModeDevice != 0
}

func cmdW2(args []string) int {
	fs := newFlag("w2")
	dev := fs.String("dev", "/dev/roswap0", "块设备")
	root := fs.String("root", ".", "仓库根目录")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if !isBlock(*dev) {
		fmt.Fprintf(os.Stderr, "%s 不存在。先加载模块并带上 size=，或向 debugfs ctl 写入 dev_create。\n", *dev)
		return 1
	}
	if !requireRoot() {
		return 1
	}
	show, _ := exec.Command("swapon", "--show=NAME", "--noheadings").Output()
	if hostplan.SwapActive(string(show), *dev) {
		fmt.Fprintf(os.Stderr, "%s 已经是 swap，先 swapoff\n", *dev)
		return 1
	}
	if _, err := exec.LookPath("fio"); err != nil {
		fmt.Fprintln(os.Stderr, "需要 fio")
		return 1
	}
	job := filepath.Join(*root, "tools/bench/fio/roswap-verify.fio")
	fmt.Println("fio verify ...")
	if err := runActions(hostplan.W2Actions(*dev, job)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println("w2 冒烟完成")
	return 0
}

func cmdW3(args []string) int {
	fs := newFlag("w3")
	want := fs.Int("nodes", 5, "期望节点数")
	dev := fs.String("dev", "/dev/roswap0", "块设备")
	root := fs.String("root", ".", "仓库根目录")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if !isBlock(*dev) {
		fmt.Fprintf(os.Stderr, "需要已加载的 roswap 和 %s\n", *dev)
		return 1
	}
	if _, err := os.Stat("/sys/kernel/debug/roswap/extents"); err != nil {
		fmt.Fprintln(os.Stderr, "需要已加载的 roswap 和 debugfs extents")
		return 1
	}
	if !requireRoot() {
		return 1
	}
	show, _ := exec.Command("swapon", "--show=NAME", "--noheadings").Output()
	if hostplan.SwapActive(string(show), *dev) {
		if err := run([]string{"swapoff", *dev}); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
	}
	fmt.Println("写入 512MB 以触发 extent 分配 ...")
	if err := run(hostplan.W3DD(*dev)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	text, err := os.ReadFile("/sys/kernel/debug/roswap/extents")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Print(string(text))
	nodes, _, _ := cluster.ParseExtents(string(text))
	if err := cluster.CheckSpread(nodes, *want); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if _, err := exec.LookPath("blkdiscard"); err != nil {
		fmt.Fprintln(os.Stderr, "需要 blkdiscard（util-linux）")
		return 1
	}
	fmt.Println("discard ...")
	if err := run(hostplan.W3Discard(*dev)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if err := os.WriteFile("/sys/kernel/debug/roswap/ctl", []byte("resv_flush\n"), 0200); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	text, err = os.ReadFile("/sys/kernel/debug/roswap/extents")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Print(string(text))
	_, total, has := cluster.ParseExtents(string(text))
	if err := cluster.CheckReclaimed(total, has); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if _, err := exec.LookPath("fio"); err != nil {
		fmt.Println("未安装 fio，跳过校验")
		return 0
	}
	job := filepath.Join(*root, "tools/bench/fio/roswap-verify.fio")
	if err := runActions([]hostplan.Action{hostplan.FioVerify(*dev, job)}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println("w3 验收完成")
	return 0
}

func writeCtl(path, line string) error {
	fmt.Println("ctl:", line)
	return os.WriteFile(path, []byte(line+"\n"), 0200)
}

func cmdBootstrap(args []string) int {
	fs := newFlag("bootstrap")
	nodesPath := fs.String("nodes", "", "nodes.txt")
	size := fs.String("size", "1T", "设备容量")
	ctl := fs.String("ctl", "/sys/kernel/debug/roswap/ctl", "debugfs ctl")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *nodesPath == "" {
		fmt.Fprintln(os.Stderr, "用法: roswap bootstrap --nodes nodes.txt [--size 1T]")
		return 2
	}
	body, err := os.ReadFile(*nodesPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if _, err := os.Stat(*ctl); err != nil {
		fmt.Fprintf(os.Stderr, "未找到 %s，先加载 roswap.ko（不要带 node_addr，由本命令登记节点）\n", *ctl)
		return 1
	}
	if !requireRoot() {
		return 1
	}
	nodes, err := cluster.ParseNodes(string(body))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	for _, n := range nodes {
		fmt.Println("拉取", n.SSH)
		out, err := exec.Command("ssh", "-o", "BatchMode=yes", n.SSH, "cat", "/run/roswap-agent/chunks.json").Output()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		if err := writeCtl(*ctl, cluster.NodeAdd(n)); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		var st chunk.State
		if err := json.Unmarshal(out, &st); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		for _, line := range cluster.ChunkAdds(n.ID, n.Epoch, st) {
			if err := writeCtl(*ctl, line); err != nil {
				fmt.Fprintln(os.Stderr, err)
				return 1
			}
		}
	}
	if err := writeCtl(*ctl, cluster.DevCreate(*size)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println("设备已创建，容量", *size)
	nodesBody, err := os.ReadFile("/sys/kernel/debug/roswap/nodes")
	if err == nil {
		fmt.Print(string(nodesBody))
	}
	return 0
}
