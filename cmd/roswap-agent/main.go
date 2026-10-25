//go:build linux

package main

import (
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	"roswap/pkg/chunk"
	"roswap/pkg/rdma"
)

func main() {
	listen := flag.String("listen", "", "RoCE 地址，形如 10.0.0.11:18515")
	hugepath := flag.String("hugetlbfs", "/mnt/rshuge", "1GB hugetlbfs 挂载点")
	contribution := flag.String("contribution", "1G", "贡献容量，向下取整到 1GB")
	epoch := flag.Uint("epoch", 1, "chunk 纪元")
	gidBase := flag.Uint("gid-base", 1, "起始 chunk gid")
	masterIP := flag.String("master-ip", "", "只接受该主节点 IP 的 RDMA 连接")
	cluster := flag.String("cluster-uuid", "", "32 位十六进制集群 UUID，空则不校验")
	statePath := flag.String("state", "/run/roswap-agent/chunks.json", "chunk 状态文件")
	rdmaDev := flag.String("rdma-dev", "mlx5_0", "用于选择 NUMA 节点的 RDMA 设备")
	fresh := flag.Bool("fresh", false, "删除状态文件里记录的旧 chunk 后重新创建")
	reopen := flag.Bool("reopen", false, "重新映射已有 chunk 文件并注册新的 rkey")
	flag.Parse()

	if err := syscall.Mlockall(syscall.MCL_CURRENT | syscall.MCL_FUTURE); err != nil {
		log.Printf("mlockall: %v（建议以 root 运行）", err)
	}
	ip, port, err := splitHost(*listen)
	if err != nil {
		log.Fatal(err)
	}
	var uuid []byte
	if *cluster != "" {
		uuid, err = hex.DecodeString(*cluster)
		if err != nil || len(uuid) != 16 {
			log.Fatal("cluster-uuid 需要 32 位十六进制")
		}
	}
	agent, err := rdma.Open(ip, port, *masterIP, uuid)
	if err != nil {
		log.Fatal(err)
	}
	defer agent.Close()

	numa := nicNUMA(*rdmaDev)
	log.Printf("网卡 NUMA 节点: %d", numa)

	var st chunk.State
	switch {
	case *reopen:
		st, err = reopenChunks(agent, *statePath, numa)
	default:
		if *fresh {
			dropOld(*statePath)
		}
		st, err = createChunks(agent, *hugepath, *contribution, uint32(*gidBase), uint32(*epoch), numa)
	}
	if err != nil {
		log.Fatal(err)
	}
	if err := chunk.SaveState(*statePath, st); err != nil {
		log.Fatal(err)
	}
	for _, c := range st.Chunks {
		fmt.Printf("chunk gid=%d addr=%d rkey=%d len=%d numa=%d file=%s\n",
			c.GID, c.Addr, c.RKey, c.Len, c.NUMA, c.File)
	}
	log.Printf("状态已写入 %s", *statePath)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		log.Printf("正在停止")
		agent.Stop()
	}()
	if err := agent.Serve(); err != nil {
		log.Fatal(err)
	}
}

func splitHost(s string) (string, int, error) {
	if s == "" {
		return "", 0, fmt.Errorf("必须指定 --listen <roce-ip>:18515，不能绑定 0.0.0.0")
	}
	host, ps, err := net.SplitHostPort(s)
	if err != nil {
		return "", 0, fmt.Errorf("listen: %w", err)
	}
	if host == "" || host == "0.0.0.0" {
		return "", 0, fmt.Errorf("必须绑定具体 RoCE 地址，不能使用 0.0.0.0")
	}
	port, err := strconv.Atoi(ps)
	if err != nil || port <= 0 || port > 65535 {
		return "", 0, fmt.Errorf("端口无效")
	}
	return host, port, nil
}

func createChunks(agent *rdma.Agent, hugepath, contribution string, gidBase, epoch uint32, numa int) (chunk.State, error) {
	var st chunk.State
	total, err := chunk.ParseSize(contribution)
	if err != nil {
		return st, err
	}
	n := int(total / (1 << 30))
	if n < 1 {
		return st, fmt.Errorf("贡献容量至少 1GB")
	}
	if uint64(n)<<30 != total {
		log.Printf("贡献容量向下取整为 %dGB", n)
	}
	if err := chunk.StatHugetlbfs(hugepath); err != nil {
		return st, err
	}
	free, err := freeHugePages()
	if err == nil && free < n {
		return st, fmt.Errorf("1GB 大页空闲 %d，需要 %d。先运行 roswap hugepages", free, n)
	}
	for i := 0; i < n; i++ {
		gid := gidBase + uint32(i)
		path := filepath.Join(hugepath, fmt.Sprintf("chunk-%d-%d", gid, epoch))
		addr, rkey, err := agent.Map(path, 1<<30, numa)
		if err != nil {
			return st, err
		}
		st.Chunks = append(st.Chunks, chunk.Chunk{
			GID: gid, Addr: addr, Len: 1 << 30, RKey: rkey, NUMA: numa, File: path,
		})
	}
	return st, nil
}

func reopenChunks(agent *rdma.Agent, statePath string, numa int) (chunk.State, error) {
	st, err := chunk.LoadState(statePath)
	if err != nil {
		return st, err
	}
	for i := range st.Chunks {
		c := &st.Chunks[i]
		if err := chunk.StatHugetlbfs(filepath.Dir(c.File)); err != nil {
			return st, err
		}
		addr, rkey, err := agent.Map(c.File, c.Len, numa)
		if err != nil {
			return st, err
		}
		c.Addr = addr
		c.RKey = rkey
		c.NUMA = numa
	}
	return st, nil
}

func dropOld(statePath string) {
	st, err := chunk.LoadState(statePath)
	if err != nil {
		return
	}
	for _, c := range st.Chunks {
		if c.File != "" {
			_ = os.Remove(c.File)
		}
	}
	_ = os.Remove(statePath)
}

func freeHugePages() (int, error) {
	b, err := os.ReadFile("/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages")
	if err != nil {
		return 0, err
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(b)))
	return n, err
}

func nicNUMA(dev string) int {
	base := "/sys/class/infiniband/" + dev + "/device/net"
	entries, err := os.ReadDir(base)
	if err != nil || len(entries) == 0 {
		return -1
	}
	b, err := os.ReadFile("/sys/class/net/" + entries[0].Name() + "/device/numa_node")
	if err != nil {
		return -1
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil {
		return -1
	}
	return n
}
