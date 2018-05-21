/* Test for KVM mmio emulation patches:
 * qemu parameter:
 *    -object memory-backend-file,id=mb1,size=1M,share,mem-path=/dev/shm/test_mmio_simon -device ivshmem-plain,memdev=mb1
 *
 * and plus following qemu patch (modification for IVMEM emulation):
 *      https://github.com/justdoitqd/publicFiles/blob/master/test_mmio.qemu.patch
 * the io_address is retrieved from /proc/iomem for ivshmem io map addr
 *
 * test_mmio    // run all test cases
 * test_mmio 888 // run all test cases until one failure case (888 is a lucky number at China).
 * test_mmio 10 // run only test case 10#
 *
 * Author: Simon Guo
 * 2018.4
 */
#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define DEVICE "/dev/mem"

typedef struct {
	char u[16]; 
} vec_128_t __attribute__ ((aligned(16)));

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define UNALIGN_OFFSET 15

#define VEC_PRINT(_str, _vec) \
	        printf("%s :%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", _str, \
			              ((char*)(&(_vec)))[0], \
			              ((char*)(&(_vec)))[1], \
			              ((char*)(&(_vec)))[2], \
			              ((char*)(&(_vec)))[3], \
			              ((char*)(&(_vec)))[4], \
			              ((char*)(&(_vec)))[5], \
			              ((char*)(&(_vec)))[6], \
			              ((char*)(&(_vec)))[7], \
			              ((char*)(&(_vec)))[8], \
			              ((char*)(&(_vec)))[9], \
			              ((char*)(&(_vec)))[10], \
			              ((char*)(&(_vec)))[11], \
			              ((char*)(&(_vec)))[12], \
			              ((char*)(&(_vec)))[13], \
			              ((char*)(&(_vec)))[14], \
			              ((char*)(&(_vec)))[15])


typedef enum _INSTR_RESULT {
	NOT_RUN = 0,
	SUCC,
	FAIL
} INSTR_RESULT;

struct instruct_desc {
	int (*_load)(vec_128_t* vec_to, volatile char* from);
	int (*_store)(vec_128_t vec_from, volatile char* to);
	void (*init)(struct instruct_desc*);
	char* ram_buf;
	volatile char* io_buf;
	vec_128_t vec_source;
	vec_128_t vec_load_ram, vec_load_io;
	char* name;
	int mem_chk_bytes;
	int reg_chk_bytes;
	INSTR_RESULT res;
	int is_update;  /* check update GPR with addr */
	int is_altivec; 
};

volatile char *io_aligned_ptr;
char *ram_aligned_ptr;

void float_init(struct instruct_desc* instr_desc_p) {
	int i;
	for (i = 0; i < 16; i++)
		instr_desc_p->vec_source.u[i] = random() % 16;
	*(float*)&instr_desc_p->vec_source = 1.2345;
	VEC_PRINT("In float_init: ", instr_desc_p->vec_source);
}

void double_init(struct instruct_desc* instr_desc_p) {
	int i;
	for (i = 0; i < 16; i++)
		instr_desc_p->vec_source.u[i] = 0;
	*(double*)&instr_desc_p->vec_source = -3.14159;

	VEC_PRINT("In double_init: ", instr_desc_p->vec_source);
}

/* the dummy_store need do something so that mem/iomem contains
 * the same content. Let's use a passed store stxvd2x: 
 */
int dummy_store(vec_128_t vec_from, volatile char* to) { 
	int i = 0;
	int offset = (unsigned long)to & 0xfUL;
	for (; i < sizeof(vec_from); i++)
		(to)[i] = (((volatile char*)&vec_from))[i];

	for (i = 1; i <= offset; i++)
		to[0 - i] = ((volatile char*)&vec_from)[16 - i];

	return 1;
}

int dummy_load(vec_128_t* vec_to, volatile char* from) {
	return 1;
}

#define GEN_STORE_XFORM_GENERIC(__store, __type, __clobber) \
int __store##_store(vec_128_t vec_from, volatile char* to) { \
	__type long_from = *(__type*)&vec_from; \
	asm volatile ( "" #__store " %0, 0, %1 ;" \
			: : "" #__clobber (long_from), "r" (to) \
			: "memory" ); \
	return 1; \
} 

#define GEN_LOAD_XFORM_GENERIC(__load, __type, __clobber) \
int __load##_load(vec_128_t* vec_to, volatile char* from) { \
	__type l_to;	\
	asm volatile ("" #__load " %0, 0, %1 ;" \
			: "=&" #__clobber (l_to) \
			: "r" (from) \
			: "memory"); \
	*(__type*)vec_to = l_to; \
	return 1; \
}

#define GEN_LOAD_STORE_XFORM_GENERIC(__load, __store, __type, __clobber) \
	GEN_STORE_XFORM_GENERIC(__store, __type, __clobber) \
	GEN_LOAD_XFORM_GENERIC(__load, __type, __clobber) 

#define GEN_LOAD_STORE_XFORM_GENERIC_V(__load, __store, __type) \
	GEN_LOAD_STORE_XFORM_GENERIC(__load, __store, __type, v)

#define GEN_LOAD_STORE_XFORM_GENERIC_F(__load, __store, __type) \
	GEN_LOAD_STORE_XFORM_GENERIC(__load, __store, __type, f)

#define GEN_LOAD_XFORM_GENERIC_R(__load, __type) \
	GEN_LOAD_XFORM_GENERIC(__load, __type, r)

#define GEN_STORE_XFORM_GENERIC_R(__store, __type) \
	GEN_STORE_XFORM_GENERIC(__store, __type, r)

#define GEN_LOAD_STORE_XFORM_GENERIC_R(__load, __store, __type) \
	GEN_LOAD_STORE_XFORM_GENERIC(__load, __store, __type, r)

#define GEN_LOAD_XFORM_16V(__load) \
	GEN_LOAD_XFORM_GENERIC(__load, vec_128_t, v)

#define GEN_STORE_XFORM_16V(__store) \
	GEN_STORE_XFORM_GENERIC(__store, vec_128_t, v)

#define GEN_LOAD_STORE_XFORM_16V(__load, __store) \
	GEN_LOAD_STORE_XFORM_GENERIC_V(__load, __store, vec_128_t)

#define GEN_LOAD_XFORM_8R(__load) \
	GEN_LOAD_XFORM_GENERIC_R(__load, unsigned long long)

#define GEN_STORE_XFORM_8R(__store) \
	GEN_STORE_XFORM_GENERIC_R(__store, unsigned long long)

#define GEN_LOAD_STORE_XFORM_8R(__load, __store) \
	GEN_LOAD_STORE_XFORM_GENERIC_R(__load, __store, unsigned long long)

#define GEN_LOAD_XFORM_GENERIC_F(__load, __type) \
	GEN_LOAD_XFORM_GENERIC(__load, __type, f)

#define GEN_STORE_XFORM_GENERIC_F(__store, __type) \
	GEN_STORE_XFORM_GENERIC(__store, __type, f)

#define GEN_LOAD_XFORM_8F(__load) \
	GEN_LOAD_XFORM_GENERIC_F(__load, unsigned long long)

#define GEN_STORE_XFORM_8F(__store) \
	GEN_STORE_XFORM_GENERIC_F(__store, unsigned long long)

#define GEN_LOAD_STORE_XFORM_8F(__load, __store) \
	GEN_LOAD_STORE_XFORM_GENERIC_F(__load, __store, unsigned long long)

#define GEN_LOAD_STORE_XFORM_4R(__load, __store) \
	GEN_LOAD_STORE_XFORM_GENERIC_R(__load, __store, unsigned int)

#define GEN_LOAD_STORE_XFORM_4F(__load, __store) \
	GEN_LOAD_STORE_XFORM_GENERIC_F(__load, __store, float)

#define GEN_LOAD_STORE_XFORM_2R(__load, __store) \
	GEN_LOAD_STORE_XFORM_GENERIC_R(__load, __store, unsigned short)

#define GEN_LOAD_STORE_XFORM_1R(__load, __store) \
	GEN_LOAD_STORE_XFORM_GENERIC_R(__load, __store, unsigned char)


/* D FORM */
#define GEN_STORE_DFORM_GENERIC(__store, __type, __clobber) \
int __store##_store(vec_128_t vec_from, volatile char* to) { \
	__type long_from = *(__type*)&vec_from; \
	asm volatile ( "" #__store " %0, 0(%1) ;" \
			: : "" #__clobber (long_from), "r" (to) \
			: "memory" ); \
	return 1; \
} 

#define GEN_LOAD_DFORM_GENERIC(__load, __type, __clobber) \
int __load##_load(vec_128_t* vec_to, volatile char* from) { \
	__type l_to;	\
	asm volatile ("" #__load " %0, 0(%1) ;" \
			: "=&" #__clobber (l_to) \
			: "r" (from) \
			: "memory"); \
	*(__type*)vec_to = l_to; \
	return 1; \
}

/* RA update checking, use R10 */
#define GEN_STORE_DFORM_GENERIC_UPD(__store, __type, __clobber) \
int __store##_store(vec_128_t vec_from, volatile char* to) { \
	__type long_from = *(__type*)&vec_from; \
	register unsigned long long addr asm ("r10"); \
	unsigned long long r10;   \
	addr = (unsigned long long)to - 8;   \
						\
	asm volatile (  "" #__store " %0, 8(%1) ;" \
			" std 10, 0(%2) ; " \
			: : "" #__clobber (long_from), "r" (addr), "r" (&r10) \
			: "memory"); \
	if ((unsigned long long)to != r10) \
		printf("r10[%llx] not equal with to[%p]\n", r10, to); \
	else \
		printf("Cool[2] r10[%llx] equal with addr[%llx]\n", r10, addr); \
	return ((unsigned long long)to == r10); \
} 


#define GEN_LOAD_DFORM_GENERIC_UPD(__load, __type, __clobber) \
int __load##_load(vec_128_t* vec_to, volatile char* from) { \
	__type l_to;	\
	register unsigned long long addr asm ("r10"); \
	unsigned long long r10;   \
	addr = (unsigned long long) from - 8; \
	asm volatile ( "" #__load " %0, 8(%1) ;" \
			" std 10, 0(%2) ; " \
			: "=&" #__clobber (l_to) \
			: "r" (addr), "r" (&r10) \
			: "memory"); \
	*(__type*)vec_to = l_to; \
	if ((unsigned long long)from != r10) \
		printf("r10[%llx] not equal with from[%llx]\n", r10, from); \
	else \
		printf("Cool[1] r10[%llx] equal with from[%llx]\n", r10, from); \
	return ((unsigned long long)from == r10); \
}

#define GEN_LOAD_STORE_DFORM_GENERIC(__load, __store, __type, __clobber) \
	GEN_LOAD_DFORM_GENERIC(__load, __type, __clobber) \
	GEN_STORE_DFORM_GENERIC(__store, __type, __clobber)

#define GEN_LOAD_STORE_DFORM_GENERIC_V(__load, __store, __type) \
	GEN_LOAD_STORE_DFORM_GENERIC(__load, __store, __type, v)

#define GEN_LOAD_DFORM_GENERIC_F(__load, __type) \
	GEN_LOAD_DFORM_GENERIC(__load, __type, f)

#define GEN_STORE_DFORM_GENERIC_F(__store, __type) \
	GEN_STORE_DFORM_GENERIC(__store, __type, f)

#define GEN_LOAD_STORE_DFORM_GENERIC_F(__load, __store, __type) \
	GEN_LOAD_STORE_DFORM_GENERIC(__load, __store, __type, f)

#define GEN_LOAD_DFORM_GENERIC_R(__load, __type) \
	GEN_LOAD_DFORM_GENERIC(__load, __type, r)

#define GEN_STORE_DFORM_GENERIC_R(__store, __type) \
	GEN_STORE_DFORM_GENERIC(__store, __type, r)

#define GEN_LOAD_STORE_DFORM_GENERIC_R(__load, __store, __type) \
	GEN_LOAD_STORE_DFORM_GENERIC(__load, __store, __type, r)

#define GEN_LOAD_DFORM_8F(__load) \
	GEN_LOAD_DFORM_GENERIC_F(__load, unsigned long long)

#define GEN_STORE_DFORM_8F(__store) \
	GEN_STORE_DFORM_GENERIC_F(__store, unsigned long long)

#define GEN_LOAD_DFORM_8R(__load) \
	GEN_LOAD_DFORM_GENERIC_R(__load, unsigned long long)

#define GEN_STORE_DFORM_8R(__store) \
	GEN_STORE_DFORM_GENERIC_R(__store, unsigned long long)

#define GEN_LOAD_STORE_DFORM_16V(__load, __store) \
	GEN_LOAD_STORE_DFORM_GENERIC_V(__load, __store, vec_128_t)

#define GEN_LOAD_STORE_DFORM_8R(__load, __store) \
	GEN_LOAD_STORE_DFORM_GENERIC_R(__load, __store, unsigned long long)

#define GEN_LOAD_STORE_DFORM_8F(__load, __store) \
	GEN_LOAD_STORE_DFORM_GENERIC_F(__load, __store, unsigned long long)

#define GEN_LOAD_STORE_DFORM_4R(__load, __store) \
	GEN_LOAD_STORE_DFORM_GENERIC_R(__load, __store, unsigned int)

#define GEN_LOAD_STORE_DFORM_4F(__load, __store) \
	GEN_LOAD_STORE_DFORM_GENERIC_F(__load, __store, float)

#define GEN_LOAD_STORE_DFORM_2R(__load, __store) \
	GEN_LOAD_STORE_DFORM_GENERIC_R(__load, __store, unsigned short)

#define GEN_LOAD_STORE_DFORM_1R(__load, __store) \
	GEN_LOAD_STORE_DFORM_GENERIC_R(__load, __store, unsigned char)

/* update version */
#define GEN_LOAD_DFORM_GENERIC_UPD_R(__load, __type) \
	GEN_LOAD_DFORM_GENERIC_UPD(__load, __type, r)

#define GEN_STORE_DFORM_GENERIC_UPD_R(__store, __type) \
	GEN_STORE_DFORM_GENERIC_UPD(__store, __type, r)

#define GEN_LOAD_DFORM_GENERIC_UPD_F(__load, __type) \
	GEN_LOAD_DFORM_GENERIC_UPD(__load, __type, f)

#define GEN_STORE_DFORM_GENERIC_UPD_F(__store, __type) \
	GEN_STORE_DFORM_GENERIC_UPD(__store, __type, f)

#define GEN_LOAD_DFORM_UPD_8R(__load) \
	GEN_LOAD_DFORM_GENERIC_UPD_R(__load, unsigned long long)

#define GEN_STORE_DFORM_UPD_8R(__store) \
	GEN_STORE_DFORM_GENERIC_UPD_R(__store, unsigned long long)

#define GEN_LOAD_DFORM_UPD_8F(__load) \
	GEN_LOAD_DFORM_GENERIC_UPD_F(__load, unsigned long long)

#define GEN_STORE_DFORM_UPD_8F(__store) \
	GEN_STORE_DFORM_GENERIC_UPD_F(__store, unsigned long long)

GEN_LOAD_STORE_XFORM_16V(lvx, stvx);
GEN_LOAD_STORE_XFORM_8R(ldbrx, stdbrx);
GEN_LOAD_STORE_XFORM_8F(lfdx, stfdx);
GEN_LOAD_STORE_XFORM_8F(lfsx, stfsx);
GEN_LOAD_STORE_XFORM_16V(lxvd2x, stxvd2x);
GEN_LOAD_STORE_XFORM_16V(lxsspx, stxsspx);
GEN_LOAD_STORE_XFORM_16V(lxsdx, stxsdx);
GEN_LOAD_XFORM_16V(lxsiwax);
GEN_LOAD_STORE_XFORM_16V(lxvw4x, stxvw4x);
GEN_LOAD_XFORM_16V(lxsiwzx);
GEN_STORE_XFORM_16V(stxsiwx);
GEN_LOAD_XFORM_16V(lxvwsx);
GEN_LOAD_XFORM_16V(lxvdsx);

GEN_LOAD_XFORM_8R(lwzx);
GEN_LOAD_XFORM_8R(lbzx);  
GEN_LOAD_STORE_XFORM_8R(ldx, stdx);
GEN_STORE_XFORM_8R(stwx);
GEN_STORE_XFORM_8R(sthx);
GEN_STORE_XFORM_8R(stbx);
GEN_LOAD_XFORM_8R(lhax);
GEN_LOAD_XFORM_8R(lhzx);
GEN_LOAD_STORE_XFORM_8R(lwbrx, stwbrx);
GEN_LOAD_STORE_XFORM_8R(lhbrx, sthbrx);
GEN_LOAD_XFORM_8R(lwax);

//GEN_LOAD_STORE_XFORM_8F(lfsux, stfsux);
//GEN_LOAD_STORE_XFORM_8F(lfdux, stfdux);

GEN_LOAD_XFORM_8F(lfiwax);
GEN_LOAD_XFORM_8F(lfiwzx);
GEN_STORE_XFORM_8F(stfiwx);

GEN_LOAD_DFORM_8R(lwz);
GEN_LOAD_DFORM_8R(ld);
GEN_LOAD_DFORM_8R(lbz);
GEN_LOAD_DFORM_8R(lhz);
GEN_LOAD_DFORM_8R(lha);

GEN_STORE_DFORM_8R(stw);
GEN_STORE_DFORM_8R(stb);
GEN_STORE_DFORM_8R(sth);
GEN_STORE_DFORM_8R(std);

GEN_LOAD_DFORM_8F(lfs);
GEN_LOAD_DFORM_8F(lfd);

GEN_STORE_DFORM_8F(stfs);
GEN_STORE_DFORM_8F(stfd);

GEN_LOAD_DFORM_UPD_8R(ldu);
GEN_STORE_DFORM_UPD_8R(stdu);
GEN_LOAD_DFORM_UPD_8R(lwzu);
GEN_LOAD_DFORM_UPD_8R(lbzu);
GEN_LOAD_DFORM_UPD_8R(lhzu);
GEN_LOAD_DFORM_UPD_8R(lhau);
GEN_STORE_DFORM_UPD_8R(stwu);
GEN_STORE_DFORM_UPD_8R(stbu);
GEN_STORE_DFORM_UPD_8R(sthu);
GEN_LOAD_DFORM_UPD_8F(lfsu);
GEN_LOAD_DFORM_UPD_8F(lfdu);
GEN_STORE_DFORM_UPD_8F(stfdu);


GEN_LOAD_XFORM_16V(lvebx);
GEN_LOAD_XFORM_16V(lvehx);
GEN_LOAD_XFORM_16V(lvewx);

GEN_STORE_XFORM_16V(stvebx);
GEN_STORE_XFORM_16V(stvehx);
GEN_STORE_XFORM_16V(stvewx);

//GEN_LOAD_XFORM_8F(lfiwax);

/* even AT11.0 cannot compile those instructions currently :(  */
//GEN_LOAD_XFORM_8R(lwzux);  /*update */
//GEN_LOAD_XFORM_8R(lbzux);   /* update */
//GEN_LOAD_STORE_XFORM_8R(ldux, stdux); /* update! */
//GEN_STORE_XFORM_8R(stwux);   /*update*/
//GEN_STORE_XFORM_8R(stbux);   /*update*/
//GEN_LOAD_XFORM_8R(lhaux);
//GEN_LOAD_XFORM_8R(lhzux);
//GEN_STORE_XFORM_8R(sthux);
//GEN_LOAD_XFORM_8R(lwaux);

/* only for P9 (no P9 machine :(    */
//GEN_LOAD_STORE_DFORM_16V(lxvx, stxvx);


#define GEN_INSTR_ENTRY(__load, __store, __mem_bytes, __reg_bytes, __init, __update, __is_altivec) \
	{				\
		._load = __load##_load, \
		._store = __store##_store, \
		.name = #__load "_" #__store, \
		.mem_chk_bytes = __mem_bytes, \
		.reg_chk_bytes = __reg_bytes, \
		.init = __init, \
		.is_update = __update, \
		.is_altivec = __is_altivec, \
	}

struct instruct_desc instrs[] = {
	GEN_INSTR_ENTRY(lvx, stvx, 16, 16, NULL, 0, 1),
	GEN_INSTR_ENTRY(ldbrx, stdbrx, 8, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(ldu, stdu, 8, 8, NULL, 1, 0),
	GEN_INSTR_ENTRY(lfdx, stfdx, 8, 8, double_init, 0, 0),
	GEN_INSTR_ENTRY(lfsx, stfsx, 4, 8, double_init, 0, 0),
	GEN_INSTR_ENTRY(lxvd2x, stxvd2x, 16, 16, NULL, 0, 0),
	GEN_INSTR_ENTRY(lxsspx, stxsspx, 4, 8, double_init, 0, 0),
	GEN_INSTR_ENTRY(lxsdx, stxsdx, 8, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lxsiwax, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lxvw4x, stxvw4x, 16, 16, NULL, 0, 0),
	GEN_INSTR_ENTRY(lxsiwzx, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stxsiwx, 4, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(lxvdsx, dummy, 16, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(lwzx, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lbzx, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(ldx, stdx, 8, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stwx, 4, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, sthx, 2, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stbx, 1, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(lhax, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lhzx, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lwbrx, stwbrx, 4, 4, NULL, 0, 0),
	GEN_INSTR_ENTRY(lhbrx, sthbrx, 2, 2, NULL, 0, 0),
	GEN_INSTR_ENTRY(lwax, dummy, 0, 8, NULL, 0, 0),
	//GEN_INSTR_ENTRY(lfiwax, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lfiwzx, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stfiwx, 4, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(lwz, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(ld, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lbz, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lhz, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lha, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stw, 4, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stb, 1, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, sth, 2, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, std, 8, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(lfs, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(lfd, dummy, 0, 8, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stfs, 4, 0, NULL, 0, 0),
	GEN_INSTR_ENTRY(dummy, stfd, 8, 0, NULL, 0, 0),

	GEN_INSTR_ENTRY(lwzu, dummy, 0, 8, NULL, 1, 0),
	GEN_INSTR_ENTRY(lbzu, dummy, 0, 8, NULL, 1, 0),
	GEN_INSTR_ENTRY(lhzu, dummy, 0, 8, NULL, 1, 0),
	GEN_INSTR_ENTRY(lhau, dummy, 0, 8, NULL, 1, 0),

	GEN_INSTR_ENTRY(dummy, stwu, 4, 0, NULL, 1, 0),
	GEN_INSTR_ENTRY(dummy, stbu, 1, 0, NULL, 1, 0),
	GEN_INSTR_ENTRY(dummy, sthu, 2, 0, NULL, 1, 0),
	GEN_INSTR_ENTRY(lfsu, dummy, 0, 8, NULL, 1, 0),
	GEN_INSTR_ENTRY(lfdu, dummy, 0, 8, NULL, 1, 0),
	GEN_INSTR_ENTRY(dummy, stfdu, 8, 0, NULL, 1, 0),

	GEN_INSTR_ENTRY(lvebx, dummy, 0, 1, NULL, 0, 1),
	GEN_INSTR_ENTRY(dummy, stvebx, 1, 0, NULL, 0, 1),
	GEN_INSTR_ENTRY(lvehx, dummy, 0, 2, NULL, 0, 1),
	GEN_INSTR_ENTRY(dummy, stvehx, 2, 0, NULL, 0, 1),
	GEN_INSTR_ENTRY(lvewx, dummy, 0, 4, NULL, 0, 1),
	GEN_INSTR_ENTRY(dummy, stvewx, 4, 0, NULL, 0, 1),

	GEN_INSTR_ENTRY(lfiwax, dummy, 0, 8, double_init, 0, 0),
	//GEN_INSTR_ENTRY(lwzux, dummy, 0, 8, NULL, 0, 0),
	//GEN_INSTR_ENTRY(lbzux, dummy, 0, 8, NULL, 0, 0),
	//GEN_INSTR_ENTRY(ldux, stdux, 8, 8, NULL, 0, 0),
	//GEN_INSTR_ENTRY(dummy, stwux, 4, 0, NULL, 0, 0),
	//GEN_INSTR_ENTRY(dummy, stbux, 1, 0, NULL, 0, 0),
	//GEN_INSTR_ENTRY(lhaux, dummy, 0, 8, NULL, 0, 0),
	//GEN_INSTR_ENTRY(lhzux, dummy, 0, 8, NULL, 0, 0),
	//GEN_INSTR_ENTRY(dummy, sthux, 2, 0, NULL, 0, 0),
	//GEN_INSTR_ENTRY(lwaux, dummy, 0, 8, NULL, 0, 0),
	//GEN_INSTR_ENTRY(lfsux, stfsux, 4, 8, double_init, 0, 0),
	//GEN_INSTR_ENTRY(lfdux, stfdux, 8, 8, double_init, 0, 0),
	/* lxvwsx only for P9*/
	//GEN_INSTR_ENTRY(lxvwsx, dummy, 16, 0, NULL, 0, 0),
	//GEN_INSTR_ENTRY(lxvx, stxvx, 16, NULL, 0, 0),
};

void gen_vec_src(struct instruct_desc* instr_desc_p) {
	int i;
	for (i = 0; i < 16; i++)
		instr_desc_p->vec_source.u[i] = random() % 16;
	printf("gen_vec_src: \n");
	VEC_PRINT("	vec_src:", instr_desc_p->vec_source);
}

int scramble_load_vec(struct instruct_desc* p) {
	int i;
	for (i = 0; i < 16; i++) {
		p->vec_load_io.u[i] = random() % 16;
		p->vec_load_ram.u[i] = random() % 16;
	}
	printf("scramble_load_vec: \n");
	VEC_PRINT("	load_ram:", p->vec_load_ram);
	VEC_PRINT("	load_io: ", p->vec_load_io);
}

int scramble_buf(struct instruct_desc* p) {
	int i;
	for (i = 0; i < 32; i++)
		p->ram_buf[i] = random() % 16;
}

int generic_init(struct instruct_desc* instr_desc_p) {
	gen_vec_src(instr_desc_p);
	instr_desc_p->ram_buf = ram_aligned_ptr;
	instr_desc_p->io_buf = io_aligned_ptr;

	scramble_buf(instr_desc_p);

	if (instr_desc_p->is_altivec) {
		instr_desc_p->ram_buf += UNALIGN_OFFSET;
		instr_desc_p->io_buf += UNALIGN_OFFSET;
	}

	if (instr_desc_p->init)
		instr_desc_p->init(instr_desc_p);
}

int generic_store(struct instruct_desc* p, int is_io) {
	volatile char* buf = is_io ? p->io_buf : (volatile char*)p->ram_buf;
	return p->_store(p->vec_source, buf);
}

int generic_load(struct instruct_desc* p, int is_io) {
	if (is_io)
		return p->_load(&p->vec_load_io, p->io_buf);
	else
		return p->_load(&p->vec_load_ram, (volatile char*)p->ram_buf);
}

int generic_test(struct instruct_desc* instr_desc_p) {
	vec_128_t vec_ram, vec_buf;
	vec_128_t vec_tmp_iobuf_cp;
	int ret = -1;
	char i;

	for (i = 0; i < 16; i++)
		vec_tmp_iobuf_cp.u[i]= random() % 0xf;

	generic_init(instr_desc_p);

	generic_store(instr_desc_p, 0); 
	if (!generic_store(instr_desc_p, 1)) {
		printf("[%s]: store update gpr FAIL: \n", instr_desc_p->name);
		return -1;
	}
	if (instr_desc_p->_store != dummy_store) {
		/* check store result */
		int offset = 0;
		if (instr_desc_p->is_altivec) {
			offset = (UNALIGN_OFFSET % instr_desc_p->mem_chk_bytes);
		}
		vec_tmp_iobuf_cp = *(vec_128_t*)(instr_desc_p->io_buf - offset);

		//Note the below will actually "load" from the buffer
		ret = memcmp(instr_desc_p->ram_buf - offset, &vec_tmp_iobuf_cp,
				instr_desc_p->mem_chk_bytes);
		if (ret) {
			if (instr_desc_p->is_altivec) {
				printf("[%s]: [VMX] FAIL: store test and find different result:[ram_buf:%p][io_buf:%p][offset:%d][chk:%d]\n",
						instr_desc_p->name,
						instr_desc_p->ram_buf,
						instr_desc_p->io_buf,
						offset,
						instr_desc_p->mem_chk_bytes);
				VEC_PRINT("	ram_buf_offset:", (*(instr_desc_p->ram_buf - offset)));
				VEC_PRINT("	io_buf_offset: ", vec_tmp_iobuf_cp);
			} else {
				printf("[%s]: FAIL: store test and find different result\n",
						instr_desc_p->name);
				VEC_PRINT("	ram_buf:", (*instr_desc_p->ram_buf));
				VEC_PRINT("	io_buf: ", vec_tmp_iobuf_cp);
			}
			return -1;
		} else {
			if (instr_desc_p->is_altivec) {
				printf("[%s]: [VMX] store test SUCC: [ram_buf:%p][io_buf:%p][offset:%d][chk:%d] \n", instr_desc_p->name,
						instr_desc_p->ram_buf,
						instr_desc_p->io_buf,
						offset,
						instr_desc_p->mem_chk_bytes);
				VEC_PRINT("	ram_buf_offset:", (*(instr_desc_p->ram_buf - offset)));
				VEC_PRINT("	io_buf_offset: ", vec_tmp_iobuf_cp);
			} else {
				printf("[%s]: store test SUCC: \n", instr_desc_p->name);
				VEC_PRINT("	ram_buf:", (*instr_desc_p->ram_buf));
				VEC_PRINT("	io_buf: ", vec_tmp_iobuf_cp);
			}
		}
	}

	if (instr_desc_p->_load != dummy_load) {
		int offset = 0; /* offset within reg */
		scramble_load_vec(instr_desc_p);

		/* print mem */
		generic_load(instr_desc_p, 0); 
		if (!generic_load(instr_desc_p, 1)) {
			printf("[%s]: load update gpr FAIL: \n", instr_desc_p->name);
			return -1;
		}

		if (instr_desc_p->is_altivec) {
			offset = (UNALIGN_OFFSET & ~(instr_desc_p->reg_chk_bytes - 1));
		}

		ret = memcmp((char*)&instr_desc_p->vec_load_ram + offset, (char*)&instr_desc_p->vec_load_io + offset,
				instr_desc_p->reg_chk_bytes);
		if (ret) {
			if (instr_desc_p->is_altivec) {
				vec_128_t vec_tmp_iobuf_cp;
				printf("[%s]: [VMX] FAIL: load test and find different result:[offset:%d][chk:%d]\n",
						instr_desc_p->name,
						offset,
						instr_desc_p->reg_chk_bytes);
				vec_tmp_iobuf_cp = *(vec_128_t*)io_aligned_ptr;
				/* dump src: */
				VEC_PRINT("	ram_buf_aligned:", (*ram_aligned_ptr));
				VEC_PRINT("	io_buf_aligned: ", vec_tmp_iobuf_cp);
				VEC_PRINT("	load_ram_offset:", instr_desc_p->vec_load_ram); 
				VEC_PRINT("	load_buf_offset:", instr_desc_p->vec_load_io);
			} else {
				printf("[%s] FAIL: load test and find different result\n",
						instr_desc_p->name);
				VEC_PRINT("	load_ram:", instr_desc_p->vec_load_ram);
				VEC_PRINT("	load_io: ", instr_desc_p->vec_load_io);
			}
			return -2;
		} else {
			if (instr_desc_p->is_altivec) {
				vec_128_t vec_tmp_iobuf_cp;
				printf("[%s]: [VMX] load test SUCC :[offset:%d][chk:%d]\n",
						instr_desc_p->name,
						offset,
						instr_desc_p->reg_chk_bytes);
				vec_tmp_iobuf_cp = *(vec_128_t*)io_aligned_ptr;
				/* dump src: */
				VEC_PRINT("	ram_buf_aligned:", (*ram_aligned_ptr));
				VEC_PRINT("	io_buf_aligned: ", vec_tmp_iobuf_cp);
				VEC_PRINT("	load_ram_offset:", instr_desc_p->vec_load_ram); 
				VEC_PRINT("	load_buf_offset:", instr_desc_p->vec_load_io); 
			} else {
				printf("[%s] load test SUCC: \n", instr_desc_p->name);
				VEC_PRINT("	load_ram:", instr_desc_p->vec_load_ram);
				VEC_PRINT("	load_io: ", instr_desc_p->vec_load_io);
			}
		}
	}
	return 0;
}

int main(int argc, void* argv[])
{
	int fd, i, succ, fail;
	int test_entry = -1;

	ram_aligned_ptr = malloc(16 * 3);
	if (ram_aligned_ptr == NULL)
		return -1;
	ram_aligned_ptr = (char*)((unsigned long long)ram_aligned_ptr & ~0xFULL);

	if (argc > 1) {
		test_entry = atoi((char*)argv[1]);
		if ((test_entry != 888) && ((test_entry < 0) || (test_entry >= ARRAY_SIZE(instrs))))
				test_entry = -1;
	}

	srandom(time(0));
	fd = open(DEVICE, O_RDWR);
	if (fd < 0)
		return -2;

	io_aligned_ptr = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0x200080100000);
	if (io_aligned_ptr == (volatile char*)-1)  {
		perror("mmap failed!\n");
		close(fd);
		return -3;
	}

	io_aligned_ptr = (char*)((unsigned long long)io_aligned_ptr & ~0xFULL);

	printf("io_aligned_ptr=%p, ram_aligned_ptr=%p, unaligned_offset=%d \n",
			io_aligned_ptr, ram_aligned_ptr, UNALIGN_OFFSET);
	/* perform a store and load, check whether the memory
	 * contents are the same.
	 */ 
	succ = 0;
	fail = 0;

	if ((test_entry != 888) && (test_entry != -1)) { /* only run specified test_entry */
		if (generic_test(&instrs[test_entry]) < 0) {
			fail++;
		} else {
			succ++;
		}
	} else {
		for (i = 0; i < ARRAY_SIZE(instrs); i++) {
			printf("No# %d tests %s-----------------\n", 
					i, instrs[i].is_update ? "[UPD]": "");
			if (generic_test(&instrs[i]) < 0) {
				if (test_entry == 888)  /* fail immediately */
					return 1;
				fail++;
				instrs[i].res = FAIL;
			} else {
				succ++;
				instrs[i].res = SUCC;
			}
		}
	}

	printf("------------------------------\n");
	printf("Total %lu tests: + success=%d; - failed=%d\n",
			ARRAY_SIZE(instrs), succ, fail);
	if (fail != 0) {
		printf("  FAILED test case: \n");
		for (i = 0; i < ARRAY_SIZE(instrs); i++) {
			if (instrs[i].res == FAIL)
				printf("         [%d] %s\n", i, instrs[i].name);
		}
	}
	return fail;
}
