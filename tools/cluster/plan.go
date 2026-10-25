// Package cluster 解析节点清单、chunk 状态和 debugfs 统计，并生成 ctl 命令。
package cluster

import (
	"fmt"
	"strconv"
	"strings"

	"roswap/pkg/chunk"
)

// Node 是 nodes.txt 里的一行。
type Node struct {
	ID    string
	IP    string
	SSH   string
	Port  string
	Epoch string
}

// ExtentNode 是 debugfs extents 里一个节点的 mapped 计数。
type ExtentNode struct {
	ID     int
	Mapped int
}

// ParseNodes 解析 nodes.txt。空行和 # 开头的行被忽略。
// 缺省端口 18515，缺省 epoch 1。
func ParseNodes(text string) ([]Node, error) {
	var out []Node
	for i, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 3 {
			return nil, fmt.Errorf("第 %d 行至少需要 id、ip、ssh", i+1)
		}
		n := Node{ID: fields[0], IP: fields[1], SSH: fields[2], Port: "18515", Epoch: "1"}
		if len(fields) > 3 {
			n.Port = fields[3]
		}
		if len(fields) > 4 {
			n.Epoch = fields[4]
		}
		out = append(out, n)
	}
	return out, nil
}

// NodeAdd 生成一条 node_add 命令。
func NodeAdd(n Node) string {
	return fmt.Sprintf("node_add id=%s addr=%s:%s epoch=%s", n.ID, n.IP, n.Port, n.Epoch)
}

// DevCreate 生成 dev_create 命令。
func DevCreate(size string) string {
	return "dev_create size=" + size
}

// ChunkAdds 为每个 chunk 生成 chunk_add。
func ChunkAdds(nodeID, epoch string, st chunk.State) []string {
	lines := make([]string, 0, len(st.Chunks))
	for _, c := range st.Chunks {
		lines = append(lines, fmt.Sprintf(
			"chunk_add node=%s gid=%d raddr=%d rkey=%d len=%d epoch=%s",
			nodeID, c.GID, c.Addr, c.RKey, c.Len, epoch))
	}
	return lines
}

// FirstChunk 返回状态文件里的第一个 chunk。
func FirstChunk(st chunk.State) (chunk.Chunk, error) {
	if len(st.Chunks) == 0 {
		return chunk.Chunk{}, fmt.Errorf("chunks.json 里没有 chunk")
	}
	return st.Chunks[0], nil
}

// InsmodArgs 是 W1 自测的模块参数。
func InsmodArgs(ip string, c chunk.Chunk) []string {
	return []string{
		"selftest=1",
		"nr_conns=1",
		"size=0",
		"node_addr=" + ip,
		fmt.Sprintf("gid=%d", c.GID),
		fmt.Sprintf("raddr=%d", c.Addr),
		fmt.Sprintf("rkey=%d", c.RKey),
		fmt.Sprintf("len=%d", c.Len),
	}
}

// ParseExtents 解析 debugfs extents 文件。
func ParseExtents(text string) (nodes []ExtentNode, total int, hasTotal bool) {
	for _, line := range strings.Split(text, "\n") {
		fields := strings.Fields(line)
		if len(fields) == 0 || fields[0] == "node" {
			continue
		}
		if fields[0] == "TOTAL" && len(fields) >= 3 {
			n, err := strconv.Atoi(fields[2])
			if err == nil {
				total = n
				hasTotal = true
			}
			continue
		}
		if len(fields) < 4 {
			continue
		}
		id, err1 := strconv.Atoi(fields[0])
		mapped, err2 := strconv.Atoi(fields[3])
		if err1 != nil || err2 != nil {
			continue
		}
		nodes = append(nodes, ExtentNode{ID: id, Mapped: mapped})
	}
	return nodes, total, hasTotal
}

// CheckSpread 要求至少 want 个节点，且这些节点的 mapped 都大于 0。
func CheckSpread(nodes []ExtentNode, want int) error {
	if len(nodes) < want {
		return fmt.Errorf("已登记节点 %d，少于期望 %d", len(nodes), want)
	}
	positive := 0
	for _, n := range nodes {
		if n.Mapped > 0 {
			positive++
		}
	}
	if positive < want {
		return fmt.Errorf("mapped>0 的节点只有 %d，期望 %d", positive, want)
	}
	return nil
}

// CheckReclaimed 要求 discard 之后 TOTAL mapped 为 0。
func CheckReclaimed(total int, hasTotal bool) error {
	if !hasTotal || total != 0 {
		return fmt.Errorf("discard 后 mapped=%d，期望 0", total)
	}
	return nil
}
