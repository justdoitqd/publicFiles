#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>

#define VFIO_TYPE1v3_IOMMU              9

#if 0
#include <rb_tree.h>
typedef unsigned long long dma_addr_t;

struct vfio_dma {
	struct rb_node          node;
	dma_addr_t              iova;           /* Device address */
	unsigned long           vaddr;          /* Process virtual addr */
	size_t                  size;           /* Map size (bytes) */
	int                     prot;           /* IOMMU_READ/WRITE */
};

static struct rb_node* dma_list;

static void vfio_link_dma(struct vfio_dma *new)
{
	struct rb_node **link = &dma_list, *parent = NULL;
	struct vfio_dma *dma;

	while (*link) {
		parent = *link;
		dma = rb_entry(parent, struct vfio_dma, node);

		if (new->iova + new->size <= dma->iova)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, dma_list);
}

static void vfio_unlink_dma(struct vfio_iommu *iommu, struct vfio_dma *old)
{
	rb_erase(&old->node, dma_list);
}
#endif

/* vfio_test :
 * usage:
 * vfio_test grpid iova page_number
 */
int vfio_init(int groupid) {
	int container = -1, group;
	int version;
	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(iommu_info) };
	int ret = 0;

	printf("get groupid=%d\n", groupid);

	/* Create a new container */
	container = open("/dev/vfio/vfio", O_RDWR);
	printf("container fd=%d\n", container);

	version = ioctl(container, VFIO_GET_API_VERSION);
	printf("ioctl VFIO_GET_API_VERSION = %d\n", version);

	if (ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU))
		printf("ioctl VFIO_TYPE1_IOMMU support\n");
	if (ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU))
		printf("ioctl VFIO_TYPE1v2_IOMMU support\n");
	if (ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v3_IOMMU))
		printf("ioctl VFIO_TYPE1v3_IOMMU support\n");

out:
	printf("ok, let's continue!\n");

	/* Open the group */
	group = open("/dev/vfio/14", O_RDWR);
	printf("group fd=%d\n", group);

	/* Add the group to the container */
	if (ret = ioctl(group, VFIO_GROUP_SET_CONTAINER, &container)) {
		printf("fail to VFIO_GROUP_SET_CONTAINER! ret=%d\n", ret);
		return -5;
	}

	/* Enable the IOMMU model we want */
	if (ret = ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1v3_IOMMU)) {
		printf("fail to set IOMMU VFIO_TYPE1v3_IOMMU! ret=%d\n", ret);
		return -6;
	}

	/* Get addition IOMMU info */
	ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info);

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
		dma_map.vaddr = hint_addr;
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
	return dma_map.vaddr;
}

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
	return 0;
}


/* vfio_dma:  continuous at iova and vaddr
 * internally: it may not be continuous at physical addr
 */
int main(int argc, void* argv[]) {
	int groupid = 0, container = 0;
	int is_map = 0;
	unsigned long long iova = 0;
	int size = 0;
	void *vaddr1, *vaddr2;

	if (argc != 2) {
		printf("%s groupid\n", argv[0]);
		return -2;
	}

	if (!sscanf(argv[1], "%d", &groupid))
			return -1;

	printf("get groupid = %d\n", groupid);

	if ((container = vfio_init(groupid)) < 0 ) {
		printf("vfio_init failure: %d\n", container);
		return -2;
	}

#if 0
	/* 0x40000 ~ 0x60000, vaddr1 ~ vaddr1 + x20000 */
	if (!(vaddr1 = vfio_test_map(container, 0x40000, 32, NULL)))
		return -1;

	/* 0x80000 ~ 0xa0000, vaddr2 ~ vaddr2 + x20000*/
	if (!(vaddr2 = vfio_test_map(container, 0x80000, 32, NULL)))
		return -1;

	//[0x40000~-x42000] [0x52000,0x6000] [0x80000,0xa0000]
	//vaddr1            vaddr1+0x2000    vaddr2
	/* tail & tail within range */
	if (vfio_test_unmap(container, 0x42000, 16))
		return -1;

	//[0x40000~-x42000] [0x52000,0x6000] [0x8f000,0xa0000]
	//vaddr1            vaddr1+0x2000    vaddr2 + 0xf000 
	/* only tail within range */
	if (vfio_test_unmap(container, 0x7f000, 16))
		return -1;

	//[0x40000~-x42000] [0x52000,0x5a000] [0x8f000,0xa0000]
	//vaddr1            vaddr1+0x2000     vaddr2+0xf000
	/* only head within range */
	if (vfio_test_unmap(container, 0x5a000, 16))
		return -1;

	//[0x40000~-x42000] [0x52000,0x5a000] [0x8f000,0xa0000]
	//vaddr1			vaddr1+0x2000     vaddr2+0xf000
	/* doesn't relate */
	if (vfio_test_unmap(container, 0x70000, 16))
		return -1;

	//[0x52000,0x5a000] [0x8f000,0xa0000]
	/* just fit */
	if (vfio_test_unmap(container, 0x40000, 2))
		return -1;

	//[0x8f000,0xa0000]
	/* just fit */
	if (vfio_test_unmap(container, 0x52000, 8))
		return -1;

	//zero
	/* just fit */
	if (vfio_test_unmap(container, 0x8f000, 17))
		return -1;

#endif
	/* start again, test vma merge */

	/* 0x40000 ~ 0x60000, vaddr1 ~ vaddr1 + x20000 */
	if (!(vaddr1 = vfio_test_map(container, 0x40000, 32, NULL)))
		return -1;

	/* 0x80000 ~ 0xa0000, vaddr2 ~ vaddr2 + x20000*/
	if (!(vaddr2 = vfio_test_map(container, 0x80000, 32, NULL)))
		return -1;

	printf("mapped vaddr1=%p, vaddr2=%p\n",vaddr1, vaddr2);

	if (vfio_test_unmap(container, 0x42000, 16))
		return -1;
	//[0x40000~-x42000] [0x52000,0x6000] [0x80000,0xa0000]
	//vaddr1            vaddr1+0x12000    vaddr2

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

	printf("please dump sysinfo: the dma should be [0x40000~0x60000] [0x80000,0xa0000] \n");

	while (1) {
		printf("input iova:");
		if (!scanf("%lx", &iova))
			return -3;

		printf("\ninput page number:");
		if (!scanf("%d", &size))
			return -4;

		printf("\ndo_map(0: no; 1:yes) :");
		if (!scanf("%d", &is_map))
			return -5;

		if (is_map) 
			vfio_test_map(container, iova, size, NULL);
		else
			vfio_test_unmap(container, iova, size);
	}

close_fd:
	close(container);
	return 0;
}
