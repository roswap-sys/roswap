//go:build !linux

package main

import (
	"fmt"
	"os"
)

func main() {
	fmt.Fprintln(os.Stderr, "roswap-agent 只能在 Linux 上构建（需要 libibverbs 和 librdmacm）")
	os.Exit(1)
}
