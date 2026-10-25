package proto

import "encoding/binary"

const (
	ConnMagic   = 0x52535750
	ConnVersion = 1
	QPData      = 1
	AgentPort   = 18515
	PdataLen    = 48
)

type Pdata struct {
	Magic        uint32
	Version      uint16
	Purpose      uint8
	CPU          uint16
	NodeID       uint16
	Epoch        uint32
	ClusterUUID  [16]byte
	MasterBootID uint64
}

func (p Pdata) Marshal() []byte {
	b := make([]byte, PdataLen)
	binary.BigEndian.PutUint32(b[0:], p.Magic)
	binary.BigEndian.PutUint16(b[4:], p.Version)
	b[6] = p.Purpose
	binary.BigEndian.PutUint16(b[8:], p.CPU)
	binary.BigEndian.PutUint16(b[10:], p.NodeID)
	binary.BigEndian.PutUint32(b[12:], p.Epoch)
	copy(b[16:32], p.ClusterUUID[:])
	binary.BigEndian.PutUint64(b[32:], p.MasterBootID)
	return b
}

func Unmarshal(b []byte) (Pdata, bool) {
	var p Pdata
	if len(b) < PdataLen {
		return p, false
	}
	p.Magic = binary.BigEndian.Uint32(b[0:])
	p.Version = binary.BigEndian.Uint16(b[4:])
	p.Purpose = b[6]
	p.CPU = binary.BigEndian.Uint16(b[8:])
	p.NodeID = binary.BigEndian.Uint16(b[10:])
	p.Epoch = binary.BigEndian.Uint32(b[12:])
	copy(p.ClusterUUID[:], b[16:32])
	p.MasterBootID = binary.BigEndian.Uint64(b[32:])
	return p, true
}
