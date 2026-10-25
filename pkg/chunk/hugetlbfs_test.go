package chunk

import "testing"

func TestCheckHugetlbfs(t *testing.T) {
	cases := []struct {
		fsType, bsize int64
		ok            bool
	}{
		{HugetlbfsMagic, ChunkPageSize, true},
		{HugetlbfsMagic, 2 << 20, false},
		{0xef53, ChunkPageSize, false},
		{0x01021994, 4096, false},
	}
	for _, c := range cases {
		err := CheckHugetlbfs("/mnt/rshuge", c.fsType, c.bsize)
		if (err == nil) != c.ok {
			t.Fatalf("type %#x bsize %d: %v", c.fsType, c.bsize, err)
		}
	}
}
