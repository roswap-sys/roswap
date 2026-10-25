//go:build linux

package rdma

/*
#cgo CFLAGS: -I${SRCDIR}/../../uapi
#cgo LDFLAGS: -libverbs -lrdmacm
#include "cm_linux.h"
#include <stdlib.h>
*/
import "C"
import (
	"fmt"
	"unsafe"
)

type Agent struct {
	p *C.struct_rs_agent
}

func Open(ip string, port int, masterIP string, uuid []byte) (*Agent, error) {
	cip := C.CString(ip)
	defer C.free(unsafe.Pointer(cip))
	var cmaster *C.char
	if masterIP != "" {
		cmaster = C.CString(masterIP)
		defer C.free(unsafe.Pointer(cmaster))
	}
	var u *C.uint8_t
	check := C.int(0)
	if len(uuid) == 16 {
		u = (*C.uint8_t)(C.CBytes(uuid))
		defer C.free(unsafe.Pointer(u))
		check = 1
	}
	p := C.rs_agent_open(cip, C.int(port), cmaster, u, check)
	if p == nil {
		return nil, fmt.Errorf("rdma listen %s:%d 失败", ip, port)
	}
	return &Agent{p: p}, nil
}

func (a *Agent) Map(path string, length uint64, numa int) (uint64, uint32, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	r := C.rs_agent_map(a.p, cpath, C.size_t(length), C.int(numa))
	if r.err != 0 {
		return 0, 0, fmt.Errorf("映射 %s: %d", path, int(r.err))
	}
	return uint64(r.addr), uint32(r.rkey), nil
}

func (a *Agent) Serve() error {
	rc := C.rs_agent_serve(a.p)
	if rc != 0 {
		return fmt.Errorf("事件循环退出: %d", int(rc))
	}
	return nil
}

func (a *Agent) Stop() {
	if a != nil && a.p != nil {
		C.rs_agent_stop(a.p)
	}
}

func (a *Agent) Close() {
	if a != nil && a.p != nil {
		C.rs_agent_close(a.p)
		a.p = nil
	}
}
