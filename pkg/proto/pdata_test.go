package proto

import "testing"

func TestPdataLayout(t *testing.T) {
	p := Pdata{
		Magic:        ConnMagic,
		Version:      ConnVersion,
		Purpose:      QPData,
		CPU:          3,
		NodeID:       7,
		Epoch:        9,
		MasterBootID: 0x1122334455667788,
	}
	p.ClusterUUID[0] = 0xab
	p.ClusterUUID[15] = 0xcd
	b := p.Marshal()
	if len(b) != 48 {
		t.Fatalf("len %d", len(b))
	}
	if b[0] != 'R' || b[1] != 'S' || b[2] != 'W' || b[3] != 'P' {
		t.Fatalf("magic %x", b[:4])
	}
	if b[4] != 0 || b[5] != 1 || b[6] != 1 || b[7] != 0 {
		t.Fatalf("version/purpose/rsvd %x", b[4:8])
	}
	if b[8] != 0 || b[9] != 3 || b[10] != 0 || b[11] != 7 {
		t.Fatalf("cpu/node %x", b[8:12])
	}
	got, ok := Unmarshal(b)
	if !ok || got.CPU != 3 || got.NodeID != 7 || got.Epoch != 9 || got.MasterBootID != p.MasterBootID {
		t.Fatalf("roundtrip %+v", got)
	}
	if got.ClusterUUID != p.ClusterUUID {
		t.Fatal("uuid")
	}
}
