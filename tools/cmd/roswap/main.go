// Command roswap 加载内核模块、准备从节点大页，并跑通 RoCEv2 数据面验收。
package main

import (
	"fmt"
	"os"
	"os/exec"

	"roswap/tools/hostplan"
)

func main() {
	os.Exit(Dispatch(os.Args[1:]))
}

func Dispatch(args []string) int {
	if len(args) == 0 || args[0] == "-h" || args[0] == "--help" || args[0] == "help" {
		fmt.Fprint(os.Stderr, usage)
		return 2
	}
	switch args[0] {
	case "probe":
		return cmdProbe(args[1:])
	case "load":
		return cmdLoad(args[1:])
	case "unload":
		return cmdUnload(args[1:])
	case "hugepages":
		return cmdHugepages(args[1:])
	case "w1":
		return cmdW1(args[1:])
	case "w2":
		return cmdW2(args[1:])
	case "w3":
		return cmdW3(args[1:])
	case "bootstrap":
		return cmdBootstrap(args[1:])
	default:
		fmt.Fprintf(os.Stderr, "未知命令 %q\n%s", args[0], usage)
		return 2
	}
}

const usage = `用法: roswap <命令> [参数]

  probe [--ofed DIR] [--kdir DIR] [--out FILE]
  load [--root DIR] [--ko FILE] -- <模块参数>
  unload
  hugepages --pages N [--mount /mnt/rshuge]
  w1 <从节点 IP> [--chunks FILE] [--root DIR]
  w2 [--dev /dev/roswap0] [--root DIR]
  w3 [--nodes 5] [--dev /dev/roswap0] [--root DIR]
  bootstrap --nodes FILE [--size 1T]

`

func requireRoot() bool {
	if os.Geteuid() == 0 {
		return true
	}
	fmt.Fprintln(os.Stderr, "需要 root")
	return false
}

func run(argv []string) error {
	return runEnv(argv, nil)
}

func runEnv(argv, env []string) error {
	cmd := exec.Command(argv[0], argv[1:]...)
	if len(env) > 0 {
		cmd.Env = append(os.Environ(), env...)
	}
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func capture(argv []string) (string, int) {
	cmd := exec.Command(argv[0], argv[1:]...)
	out, err := cmd.CombinedOutput()
	if err == nil {
		return string(out), 0
	}
	if ee, ok := err.(*exec.ExitError); ok {
		return string(out), ee.ExitCode()
	}
	return string(out) + err.Error(), 127
}

func applyAction(a hostplan.Action) error {
	if len(a.Argv) == 0 {
		return nil
	}
	return runEnv(a.Argv, a.Env)
}

func runActions(acts []hostplan.Action) error {
	for _, a := range acts {
		if msg := hostplan.FormatAction(a); msg != "" {
			fmt.Println(msg)
		}
		if err := applyAction(a); err != nil && !a.IgnoreError {
			return err
		}
	}
	return nil
}
