#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>

#define VFIO_TYPE1_IOMMU                1
#define VFIO_TYPE1v2_IOMMU              3
#define VFIO_TYPE1v3_SPLIT_IOMMU        9
#define VFIO_TYPE1_IOMMU_MAX        	10

const char* vfio_type1_name[VFIO_TYPE1_IOMMU_MAX] = {
	"", 		         // 0
	"VFIO_TYPE1_IOMMU",  	// 1
	"",			// 2
	"VFIO_TYPE1v2_IOMMU", 	// 3
	"",			// 4
	"",			// 5
	"",			// 6
	"",			// 7
	"",			// 8
	"VFIO_TYPE1v3_SPLIT_IOMMU", // 9
};


#define PAGE_SIZE_BITS 12
/* vfio_test :
 * usage:
 * vfio_test grpid iova page_number
 */
int vfio_init(int groupid, int type, int *group_fd) {
	int container = -1, group;
	int version;
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(iommu_info) };
	char group_path[30];
	int ret = 0;

	printf("get groupid=%d\n", groupid);

	/* Create a new container */
	container = open("/dev/vfio/vfio", O_RDWR);
	printf("container fd=%d\n", container);

	version = ioctl(container, VFIO_GET_API_VERSION);
	printf("ioctl VFIO_GET_API_VERSION = %d\n", version);

	if (ioctl(container, VFIO_CHECK_EXTENSION, type))
		printf("ioctl %s support\n", vfio_type1_name[type]);
	else {
		printf("ioctl %s not support\n", vfio_type1_name[type]);
		return -1;
	}

out:
	printf("ok, vfio_init continue!\n");

	/* Open the group */
	sprintf(group_path, "/dev/vfio/%d", groupid);
	group = open(group_path, O_RDWR);
	printf("open %s fd=%d\n", group_path, group);

	/* Add the group to the container */
	if (ret = ioctl(group, VFIO_GROUP_SET_CONTAINER, &container)) {
		printf("fail to VFIO_GROUP_SET_CONTAINER! ret=%d\n", ret);
		return -5;
	}

	/* Enable the IOMMU model we want */
	if (ret = ioctl(container, VFIO_SET_IOMMU, type)) {
		printf("fail to set IOMMU %s! ret=%d\n", vfio_type1_name[type], ret);
		return -6;
	}

	/* Get addition IOMMU info */
	ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info);

	*group_fd = group;

	return container;
}

void* vfio_test_map(int container, unsigned long long iova, int size, void* hint_addr)
{
	struct vfio_iommu_type1_dma_map dma_map = { .argsz = sizeof(dma_map) };
	int ret = 0;

	/* Allocate some space and setup a DMA mapping */
	if (!hint_addr)
		dma_map.vaddr = (unsigned long)mmap(hint_addr, size * 4 * 1024, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	else
		dma_map.vaddr = (unsigned long long) hint_addr;
	dma_map.size = size * 4 * 1024;
	dma_map.iova = iova;
	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	printf("dma_map: iova=%lx~%lx, vaddr=%lx~%lx, size=%d, hint_addr=%p\n",
			dma_map.iova, dma_map.iova + dma_map.size,
			dma_map.vaddr, dma_map.vaddr + dma_map.size,
			dma_map.size, hint_addr);
	if (ret = ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map)) {
		printf("fail to set VFIO_IOMMU_MAP_DMA! ret=%d\n", ret);
		return NULL;
	}
#if 0
	madvise(dma_map.vaddr, dma_map.size, QEMU_MADV_WILLNEED);
#endif
	return (void *)dma_map.vaddr;
}

/* retval:
 *    < 0: error
 *    >=0: unmapped size
 */
int vfio_test_unmap(int container, unsigned long long iova, int size)
{
	struct vfio_iommu_type1_dma_unmap dma_unmap = { .argsz = sizeof(dma_unmap) };
	int ret = 0;

	dma_unmap.size = size * 4 * 1024;
	dma_unmap.iova = iova; /* 1MB starting at 0x0 from device view */

	printf("dma_unmap: iova=%lx~%lx, size=%d\n",
			dma_unmap.iova, dma_unmap.iova + dma_unmap.size,
			dma_unmap.size);
	if (ret = ioctl(container, VFIO_IOMMU_UNMAP_DMA, &dma_unmap)) {
		printf("fail to set VFIO_IOMMU_UNMAP_DMA! ret=%d, dma_unmap size=%d\n", 
				ret, dma_unmap.size);
		return -1;
	}
	printf("dma_unmap returned size=%d\n", dma_unmap.size >> 12);

#if 0
	/* now perform madvise, need to know the unmapped vaddr.
	 * within qemu, it records vaddr/iova relationship via memory_region.
	 */
	madvise(ptr, len, QEMU_MADV_DONTNEED);
#endif
	return dma_unmap.size;
}

#define VFIO_UNMAP_RET_EXPECT(_val, _target_pfn ) \
	if (_val != (_target_pfn << PAGE_SIZE_BITS) ) {						\
		printf("line:%d: expected unmap %d pages, now size=%x\n", 			\
				__LINE__, _target_pfn, _val); 					\
		return -4;									\
	}											\

int vfio_test_v3(int container) {
	void *vaddr1, *vaddr2;
	int size;

	/* test vma split */
	if (!(vaddr1 = vfio_test_map(container, 0x40000, 32, NULL)))
		return -1;
	/* 0x40000 ~ 0x60000, vaddr1 ~ vaddr1 + x20000 */

	if (!(vaddr2 = vfio_test_map(container, 0x80000, 32, NULL)))
		return -2;
	/* 0x80000 ~ 0xa0000, vaddr2 ~ vaddr2 + x20000*/

	/* tail & tail within range */
	if ((size = vfio_test_unmap(container, 0x42000, 16)) < 0)
		return -3;
	//[0x40000~-x42000] [0x52000,0x6000] [0x80000,0xa0000]
	//vaddr1            vaddr1+0x2000    vaddr2
	VFIO_UNMAP_RET_EXPECT(size, 16);

	/* only tail within range */
	if ((size = vfio_test_unmap(container, 0x7f000, 16)) < 0)
		return -4;
	//[0x40000~-x42000] [0x52000,0x6000] [0x8f000,0xa0000]
	//vaddr1            vaddr1+0x2000    vaddr2 + 0xf000 
	VFIO_UNMAP_RET_EXPECT(size, 15);

	/* only head within range */
	if ((size = vfio_test_unmap(container, 0x5a000, 16)) < 0)
		return -1;
	//[0x40000~-x42000] [0x52000,0x5a000] [0x8f000,0xa0000]
	//vaddr1            vaddr1+0x2000     vaddr2+0xf000
	VFIO_UNMAP_RET_EXPECT(size, 6);

	/* doesn't relate */
	if ((size = vfio_test_unmap(container, 0x70000, 16)) < 0)
		return -1;
	//[0x40000~-x42000] [0x52000,0x5a000] [0x8f000,0xa0000]
	//vaddr1			vaddr1+0x2000     vaddr2+0xf000
	VFIO_UNMAP_RET_EXPECT(size, 0);

	/* just fit */
	if ((size = vfio_test_unmap(container, 0x40000, 2)) < 0)
		return -1;
	//[0x52000,0x5a000] [0x8f000,0xa0000]
	VFIO_UNMAP_RET_EXPECT(size, 2);

	/* just fit */
	if ((size = vfio_test_unmap(container, 0x52000, 8)) < 0)
		return -1;
	//[0x8f000,0xa0000]
	VFIO_UNMAP_RET_EXPECT(size, 8);

	/* just fit */
	if ((size = vfio_test_unmap(container, 0x8f000, 17)) < 0)
		return -1;
	//zero
	VFIO_UNMAP_RET_EXPECT(size, 17);


	/* start again, test vma merge */

	if (!(vaddr1 = vfio_test_map(container, 0x40000, 32, NULL)))
		return -1;
	/* 0x40000 ~ 0x60000, vaddr1 ~ vaddr1 + x20000 */

	if (!(vaddr2 = vfio_test_map(container, 0x80000, 32, NULL)))
		return -1;
	/* 0x80000 ~ 0xa0000, vaddr2 ~ vaddr2 + x20000*/

	printf("mapped vaddr1=%p, vaddr2=%p\n",vaddr1, vaddr2);

	if ((size = vfio_test_unmap(container, 0x42000, 16)) < 0)
		return -1;
	//[0x40000~-x42000] [0x52000,0x6000] [0x80000,0xa0000]
	//vaddr1            vaddr1+0x12000    vaddr2
	VFIO_UNMAP_RET_EXPECT(size, 16);

	/* merge with only head */
	if (!vfio_test_map(container, 0x50000, 2, vaddr1+0x10000))
		return -1;
	//[0x40000~-x42000] [0x50000,0x6000] [0x80000,0xa0000]
	//vaddr1            vaddr1+0x10000    vaddr2

	/* merge with only tail */
	if (!vfio_test_map(container, 0x42000, 2, vaddr1+0x2000))
		return -1;
	//[0x40000~-x44000] [0x50000,0x60000] [0x80000,0xa0000]
	//vaddr1            vaddr1+0x10000    vaddr2

	if (!vfio_test_map(container, 0x44000, 12, vaddr1+0x4000))
		return -1;
	//[0x40000~0x60000] [0x80000,0xa0000]
	/* merge with both head & tail */

	printf("please dump sys dma mapping if you can: the dma should be [0x40000~0x60000] [0x80000,0xa0000] \n");
}

int vfio_test_v2(int container) {
	void *vaddr1, *vaddr2, *vaddr3;
	int size;

	/* test vma split */
	if (!(vaddr1 = vfio_test_map(container, 0x40000, 32, NULL)))
		return -1;
	/* 0x40000 ~ 0x60000, vaddr1 ~ vaddr1 + x20000 */

	if (!(vaddr2 = vfio_test_map(container, 0x80000, 32, NULL)))
		return -2;
	/* 0x80000 ~ 0xa0000, vaddr2 ~ vaddr2 + x20000*/

	if (!(vaddr3 = vfio_test_map(container, 0xb0000, 32, NULL)))
		return -2;
	//[0x40000~,0x6000] [0x80000,0xa0000] [0xb0000,0xd0000]
	//vaddr1            vaddr2            vaddr3

	/* tail & tail within range, expect fail */
	size = vfio_test_unmap(container, 0x42000, 16);
	if (size >= 0) {
		printf("v2 test:suppose fail, but size returns %d\n", size);
		return -3;
	}
	//[0x40000,0x6000] [0x80000,0xa0000] [0xb0000,0xd0000]
	//vaddr1            vaddr2            vaddr3

	/* only tail within range */
	if ((size = vfio_test_unmap(container, 0x7f000, 16)) >= 0)
		return -4;
	//[0x40000,0x6000] [0x80000,0xa0000] [0xb0000,0xd0000]
	//vaddr1            vaddr2            vaddr3

	/* only head within range */
	if ((size = vfio_test_unmap(container, 0x5a000, 16)) >= 0)
		return -1;
	//[0x40000,0x6000] [0x80000,0xa0000] [0xb0000,0xd0000]
	//vaddr1            vaddr2            vaddr3

	/* doesn't relate, expect return 0 */
	if ((size = vfio_test_unmap(container, 0x60000, 16)) != 0)
		return -1;
	//[0x40000,0x6000] [0x80000,0xa0000] [0xb0000,0xd0000]
	//vaddr1            vaddr2            vaddr3

	/* just fit */
	if ((size = vfio_test_unmap(container, 0x40000, 32)) <= 0)
		return -1;
	//[0x80000,0xa0000] [0xb0000,0xd0000]
	//vaddr2            vaddr3
	VFIO_UNMAP_RET_EXPECT(size, 32);

	/* fit 2 */
	if ((size = vfio_test_unmap(container, 0x80000, 80)) <= 0)
		return -1;
	//zero
	VFIO_UNMAP_RET_EXPECT(size, 64);

	printf("please dump sys dma mapping if you can: the dma should be [ ] \n");
	return 0;
}

/* vfio_dma:  continuous at iova and vaddr
 * internally: it may not be continuous at physical addr
 */
int main(int argc, void* argv[]) {
	int groupid = 0, container = 0, groupfd=-1;
	int is_map = 0;
	unsigned long long iova = 0;
	int size = 0;
	int ret = 0;

	if (argc != 2) {
		printf("%s groupid\n", argv[0]);
		return -2;
	}

	if (!sscanf(argv[1], "%d", &groupid))
			return -1;

	printf("get groupid = %d\n", groupid);

	/* test v3 */
	if ((container = vfio_init(groupid, VFIO_TYPE1v3_SPLIT_IOMMU, &groupfd)) < 0 ) {
		printf("vfio_init failure: %d\n", container);
		return -2;
	}
	ret = vfio_test_v3(container);
	if (!ret)
		goto close_fd;
	//getchar();

	close(groupfd);
	close(container);

	/* test v2 */
	if ((container = vfio_init(groupid, VFIO_TYPE1v2_IOMMU, &groupfd)) < 0 ) {
		printf("vfio_init failure: %d\n", container);
		return -2;
	}
	ret = vfio_test_v2(container);
	if (ret)
		goto close_fd;
	//getchar();

close_fd:
	close(groupfd);
	close(container);
	return ret;
}
