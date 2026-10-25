package chunk

import (
	"fmt"
	"strconv"
	"strings"
)

func ParseSize(s string) (uint64, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0, fmt.Errorf("空的容量")
	}
	raw := s
	mul := uint64(1)
	last := s[len(s)-1]
	switch last {
	case 'K', 'k':
		mul = 1 << 10
		raw = s[:len(s)-1]
	case 'M', 'm':
		mul = 1 << 20
		raw = s[:len(s)-1]
	case 'G', 'g':
		mul = 1 << 30
		raw = s[:len(s)-1]
	case 'T', 't':
		mul = 1 << 40
		raw = s[:len(s)-1]
	}
	v, err := strconv.ParseUint(raw, 0, 64)
	if err != nil {
		return 0, fmt.Errorf("容量 %q: %w", s, err)
	}
	if mul != 1 && v > ^uint64(0)/mul {
		return 0, fmt.Errorf("容量溢出")
	}
	return v * mul, nil
}
