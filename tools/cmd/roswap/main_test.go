package main

import (
	"path/filepath"
	"testing"
)

func TestUsage(t *testing.T) {
	if Dispatch(nil) != 2 {
		t.Fatal("empty")
	}
	if Dispatch([]string{"nope"}) != 2 {
		t.Fatal("unknown")
	}
	if Dispatch([]string{"env"}) != 2 {
		t.Fatal("env was removed")
	}
}

func TestLoadMissingModule(t *testing.T) {
	if Dispatch([]string{"load", "--root", t.TempDir()}) != 1 {
		t.Fatal("missing ko")
	}
}

func TestW1Usage(t *testing.T) {
	if Dispatch([]string{"w1"}) != 2 {
		t.Fatal("w1 usage")
	}
	missing := filepath.Join(t.TempDir(), "no.json")
	if Dispatch([]string{"w1", "10.0.0.11", "--chunks", missing}) != 2 {
		t.Fatal("missing chunks")
	}
}
