package chunk

import (
	"encoding/json"
	"os"
	"path/filepath"
)

type Chunk struct {
	GID  uint32 `json:"gid"`
	Addr uint64 `json:"addr"`
	Len  uint64 `json:"len"`
	RKey uint32 `json:"rkey"`
	NUMA int    `json:"numa"`
	File string `json:"file"`
}

type State struct {
	Chunks []Chunk `json:"chunks"`
}

func LoadState(path string) (State, error) {
	var st State
	b, err := os.ReadFile(path)
	if err != nil {
		return st, err
	}
	err = json.Unmarshal(b, &st)
	return st, err
}

func SaveState(path string, st State) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	b, err := json.MarshalIndent(st, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, b, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
