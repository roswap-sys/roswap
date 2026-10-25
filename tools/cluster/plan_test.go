package cluster

import (
	"strings"
	"testing"

	"roswap/pkg/chunk"
)

func TestParseNodesDefaults(t *testing.T) {
	text := "# comment\n\n1 10.0.0.11 root@a\n2 10.0.0.12 root@b 18516 4\n"
	nodes, err := ParseNodes(text)
	if err != nil {
		t.Fatal(err)
	}
	if len(nodes) != 2 {
		t.Fatalf("len %d", len(nodes))
	}
	if nodes[0].Port != "18515" || nodes[0].Epoch != "1" || nodes[0].SSH != "root@a" {
		t.Fatalf("%+v", nodes[0])
	}
	if nodes[1].Port != "18516" || nodes[1].Epoch != "4" {
		t.Fatalf("%+v", nodes[1])
	}
	if got := NodeAdd(nodes[0]); got != "node_add id=1 addr=10.0.0.11:18515 epoch=1" {
		t.Fatal(got)
	}
}

func TestParseNodesRejectsShortLine(t *testing.T) {
	if _, err := ParseNodes("1 10.0.0.11\n"); err == nil {
		t.Fatal("expected error")
	}
}

func TestChunkAddsAndInsmod(t *testing.T) {
	st := chunk.State{Chunks: []chunk.Chunk{{
		GID: 2, Addr: 9, Len: 5, RKey: 7,
	}}}
	lines := ChunkAdds("1", "3", st)
	if len(lines) != 1 || lines[0] != "chunk_add node=1 gid=2 raddr=9 rkey=7 len=5 epoch=3" {
		t.Fatal(lines)
	}
	c, err := FirstChunk(st)
	if err != nil {
		t.Fatal(err)
	}
	args := InsmodArgs("10.0.0.11", c)
	joined := strings.Join(args, " ")
	for _, want := range []string{"selftest=1", "nr_conns=1", "size=0", "node_addr=10.0.0.11", "gid=2", "raddr=9", "rkey=7", "len=5"} {
		if !strings.Contains(joined, want) {
			t.Fatalf("missing %s in %s", want, joined)
		}
	}
	if _, err := FirstChunk(chunk.State{}); err == nil {
		t.Fatal("empty state")
	}
}

func TestExtents(t *testing.T) {
	text := "node chunks free mapped\n1 2 10 4\n2 2 10 0\nTOTAL mapped 4 cached 1\n"
	nodes, total, has := ParseExtents(text)
	if !has || total != 4 || len(nodes) != 2 || nodes[0].Mapped != 4 || nodes[1].Mapped != 0 {
		t.Fatalf("%+v %d %v", nodes, total, has)
	}
	if err := CheckSpread(nodes, 2); err == nil {
		t.Fatal("node 2 is empty")
	}
	nodes[1].Mapped = 3
	if err := CheckSpread(nodes, 2); err != nil {
		t.Fatal(err)
	}
	if err := CheckReclaimed(0, true); err != nil {
		t.Fatal(err)
	}
	if err := CheckReclaimed(1, true); err == nil {
		t.Fatal("mapped should be zero")
	}
	if err := CheckReclaimed(0, false); err == nil {
		t.Fatal("missing total")
	}
}

func TestDevCreate(t *testing.T) {
	if DevCreate("1T") != "dev_create size=1T" {
		t.Fatal(DevCreate("1T"))
	}
}
