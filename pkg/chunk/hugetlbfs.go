package chunk

import "fmt"

// HugetlbfsMagic 是 statfs 中 hugetlbfs 的 f_type。
const HugetlbfsMagic = 0x958458f6

// ChunkPageSize 是 chunk 依赖的大页尺寸。
const ChunkPageSize = 1 << 30

// CheckHugetlbfs 校验 statfs 结果：必须是 hugetlbfs 且 pagesize=1G。
func CheckHugetlbfs(path string, fsType, blockSize int64) error {
	if fsType != HugetlbfsMagic {
		return fmt.Errorf("%s 不是 hugetlbfs，先运行 roswap hugepages", path)
	}
	if blockSize != ChunkPageSize {
		return fmt.Errorf("%s 的 pagesize 是 %d，需要 1G", path, blockSize)
	}
	return nil
}
