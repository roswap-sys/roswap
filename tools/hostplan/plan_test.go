package hostplan

import (
	"os"
	"path/filepath"
	"testing"
)

func TestModuleListedAndSwap(t *testing.T) {
	text := "Module Size Used by\nnvme 1 0\nroswap 4096 0\n"
	if !ModuleListed(text, "roswap") || ModuleListed(text, "ib_core") {
		t.Fatal("module parse")
	}
	if !SwapActive("/dev/roswap0\n/dev/sda1\n", "/dev/roswap0") {
		t.Fatal("swap active")
	}
	if SwapActive("/dev/sda1\n", "/dev/roswap0") {
		t.Fatal("swap inactive")
	}
}

func TestHugepages(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "node0", "hugepages", "hugepages-1048576kB")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "nr_hugepages"), []byte("2\n"), 0644); err != nil {
		t.Fatal(err)
	}
	got, err := HugepageNRFiles(root)
	if err != nil || len(got) != 1 {
		t.Fatal(err, got)
	}
	if _, err := HugepageNRFiles(t.TempDir()); err == nil {
		t.Fatal("empty node root")
	}
	mnt := MountArgs("/mnt/rshuge")
	if mnt[0] != "mount" || mnt[len(mnt)-1] != "/mnt/rshuge" {
		t.Fatal(mnt)
	}
}

func TestWorkloads(t *testing.T) {
	w2 := W2Actions("/dev/roswap0", "job.fio")
	want := []string{"fio", "mkswap", "swapon", "swapoff"}
	if len(w2) != len(want) {
		t.Fatal(len(w2))
	}
	for i, name := range want {
		if w2[i].Argv[0] != name {
			t.Fatalf("%d got %s", i, w2[i].Argv[0])
		}
	}
	if got := FormatAction(w2[0]); got != "+ ROSWAP_DEV=/dev/roswap0 fio job.fio" {
		t.Fatal(got)
	}
	if len(w2[1].Env) != 0 {
		t.Fatal(w2[1].Env)
	}
	dd := W3DD("/dev/roswap0")
	if dd[0] != "dd" || dd[5] != "oflag=direct" {
		t.Fatal(dd)
	}
	if W3Discard("/dev/roswap0")[0] != "blkdiscard" {
		t.Fatal("discard")
	}
}
