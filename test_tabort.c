/* test tabort at kernel:
 * copy paste some from http://neuling.org/devel/junkcode/aligntm.c
 */
#include <stdio.h>
#include <stdlib.h>

#define TBEGIN          asm(".long 0x7C00051D":::"memory")
#define TEND            asm(".long 0x7C00055D":::"memory")
#define TSUSPEND        asm(".long 0x7C0005DD":::"memory")
#define TRESUME         asm(".long 0x7C2005DD":::"memory")

#define SPRN_TEXASR     0x82    /* Transaction Exception and Status Register */
#define SPRN_TFIAR      0x81    /* Transaction Failure Inst Addr    */
#define SPRN_TFHAR      0x80    /* Transaction Failure Handler Addr */

static inline void *lfdp(void *i)
{

	asm("lfdp 4, 0(%0)": "+b" (i) :);
	return i;
}

char array[1024*1024];

int main()
{
	char *i, *j, *k;
	unsigned long result, texasr, tfiar, tfhar;

	j = &array[32];
	/* misalign over page boundary to make sure it fails */
	k = (char *)((long unsigned int)(j + 0x4000) & ~(0x3fffUL)) - 2;

	asm __volatile__(
			"1: ;"
			"tbegin.;"
			"beq 2f;"
			"lfdp 4, 0(%[k_addr]);"
			"tend.;"
			"li 0, 0;"
			"ori %[res], 0, 0;"
			"b 3f;"
			/* Transaction abort handler */
			"2: ;"
			"li 0, 1;"
			"ori %[res], 0, 0;"
			"mfspr %[texasr], %[sprn_texasr];"
			"mfspr %[tfiar], %[sprn_tfiar];"
			"mfspr %[tfhar], %[sprn_tfhar];"

			"3: ;"
			: [res] "=r" (result), 
			[texasr] "=r" (texasr),
			[tfiar] "=r" (tfiar),
			[tfhar] "=r" (tfhar)
				: [sprn_texasr] "i" (SPRN_TEXASR),
				[sprn_tfiar] "i" (SPRN_TFIAR),
				[sprn_tfhar] "i" (SPRN_TFHAR),
				[k_addr] "b" (k)
				   : "memory"
					   );

	if (result)
		printf("executed fail handler! texasr=%lx(d30000018c000001 in HV KVM guest), tfiar=%lx, tfhar=%lx\n", texasr, tfiar, tfhar);

	return 0;
}

