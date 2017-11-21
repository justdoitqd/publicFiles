/* test tbegin at privileged state:
 * It is for PR KVM test and need to run at PR KVM guest.
 * Here we verfiy the behavior that tbegin is always failed at PR KVM guest, and
 * return with expected SPR TM reg vals.
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <asm/reg.h>
#include <asm/tm.h>
#include <asm/asm-prototypes.h>

#define SPRN_TEXASR     0x82    /* Transaction Exception and Status Register */
#define SPRN_TFIAR      0x81    /* Transaction Failure Inst Addr    */
#define SPRN_TFHAR      0x80    /* Transaction Failure Handler Addr */

#if 0
#define TEXASR_FC_LG    (63 - 7)        /* Failure Code */
#define TEXASR_HV_LG    (63 - 34)       /* Hypervisor state*/
#define TEXASR_PR_LG    (63 - 35)       /* Privilege level */
#define TEXASR_FS_LG    (63 - 36)       /* failure summary */
#define TEXASR_EX_LG    (63 - 37)       /* TFIAR exact bit */
#ifdef __ASSEMBLY__
#define TEXASR_FC       (0xFF << TEXASR_FC_LG)
#else
#define TEXASR_FC       (0x7FUL << TEXASR_FC_LG)
#endif
#define TEXASR_HV       __MASK(TEXASR_HV_LG)
#define TEXASR_PR       __MASK(TEXASR_PR_LG)
#define TEXASR_FS       __MASK(TEXASR_FS_LG)
#define TEXASR_EX       __MASK(TEXASR_EX_LG)

#define TM_CAUSE_PERSISTENT     0x01
#define TM_CAUSE_EMULATE        0xd0
#endif

#define LOOP_CNT 100
MODULE_LICENSE("GPL");

struct task_struct *kthread_task;

static void print_tm_sprs(unsigned long texasr, unsigned long tfiar,
		unsigned long tfhar, unsigned long tbegin_pc,
		unsigned long msr) {
	printk(KERN_EMERG "texasr[%lx],tfiar[%lx],tfhar[%lx],tbg_pc[%lx],msr[%lx]",
			texasr, tfiar, tfhar, tbegin_pc, msr);
}

int test_tbegin(void) {
	unsigned long texasr = 0xdeadbeaf, tfiar = 0xdeadbeaf, tfhar = 0xdeadbeaf, msr= 0xdeadbeaf, tbegin_pc = 0xdeadbeaf;
	int result = 0;
	tm_enable();
	asm __volatile__(
			"mfmsr %[msr];"
			"mflr 6;"
			"bl 1f;"
			"1: ;"
			"mflr %[tbegin_pc];"
			"mtlr 6;"
			"2: ;"
			"tbegin.;"
			"beq 3f;"
			"tend.;"
			"li 0, 0;"
			"ori %[res], 0, 0;"
			"b 4f;"
			/* Transaction abort handler */
			"3: ;"
			"li 0, 1;"
			"ori %[res], 0, 0;"
			"mfspr %[texasr], %[sprn_texasr];"
			"mfspr %[tfiar], %[sprn_tfiar];"
			"mfspr %[tfhar], %[sprn_tfhar];"
			"4: ;"
			"addi %[tbegin_pc], %[tbegin_pc], 8;"
			: [res] "=r" (result), [texasr] "=&r" (texasr), [tfiar] "=r" (tfiar), 
			[tfhar] "=r" (tfhar), [msr] "=r" (msr), [tbegin_pc] "=r" (tbegin_pc)
			: [sprn_texasr] "i" (SPRN_TEXASR), [sprn_tfiar] "i" (SPRN_TFIAR), [sprn_tfhar] "i" (SPRN_TFHAR)
			: "memory", "r6");
	tm_disable();


	if (!result)
		return -1;

	/* check texasr value */
	if (!(texasr & TEXASR_EX)) {
		result = -2;
		goto out;
	}
		
	if (!(texasr & TEXASR_FS)) {
		result = -3;
		goto out;
	}

	if (!!(texasr & TEXASR_PR)  !=  !!(msr & MSR_PR)) {
		result = -4;
		goto out;
	}

	if (!!(texasr & TEXASR_HV)  != !!(msr & MSR_HV)) {
		result = -5;
		goto out;
	}

	if (((texasr & TEXASR_FC) >> TEXASR_FC_LG) != (TM_CAUSE_EMULATE | TM_CAUSE_PERSISTENT)) {
		result = -6;
		goto out;
	}

	/* check tfiar value */
	if (tfiar != tbegin_pc) {
		result = -7;
		goto out;
	}

	if (tfhar != tbegin_pc + 4) {
		result = -8;
		goto out;
	}

out:
	if (result < 0) {
		print_tm_sprs(texasr, tfiar, tfhar, tbegin_pc, msr);
		return result;
	}

	return 0;
}

int test_tbegin_thread_func(void *data)
{
	int i, res;

	for (i = 0; i < LOOP_CNT; i++) {
		if ((i % 50) == 0)
			schedule();
		if ((res = test_tbegin()) != 0)
			break;
	}

	if (i == LOOP_CNT)
		printk(KERN_EMERG "test_tbegin success!\n");
	else
		printk(KERN_EMERG "test_tbegin failed at %d attempt, res=%d!\n", i, res);
	return 0;
}

static int __init test_tbegin_init(void)
{
	int ret = 0;
	kthread_task = kthread_run(test_tbegin_thread_func, NULL, "test_tbegin");
	return ret;
}

module_init(test_tbegin_init);

static void __exit test_tbegin_exit(void)
{
	return;
}
module_exit(test_tbegin_exit);
