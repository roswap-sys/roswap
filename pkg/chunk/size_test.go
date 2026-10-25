package chunk

import "testing"

func TestParseSize(t *testing.T) {
	cases := []struct {
		in   string
		want uint64
	}{
		{"4096", 4096},
		{"1G", 1 << 30},
		{"8g", 8 << 30},
		{"1T", 1 << 40},
		{"2M", 2 << 20},
		{"0x1000", 0x1000},
	}
	for _, c := range cases {
		got, err := ParseSize(c.in)
		if err != nil || got != c.want {
			t.Fatalf("%s => %d %v, want %d", c.in, got, err, c.want)
		}
	}
}
