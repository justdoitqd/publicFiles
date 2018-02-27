/*
 * One TM loop function to test qemu migration for HTM
 *
 * it copies some code from linux kernel git tree:
 *   linux/tools/testing/selftests/powerpc/ptrace/ptrace-tm-gpr.c
 *
 * It can be compiled under the same directory of the above file.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include "ptrace.h"
#include "ptrace-gpr.h"
#include "tm.h"

unsigned long vsx_load[2] = {123, 456};
unsigned long vsx_load_chkpt[2] = {789,987};

int simon_tm_gpr(void)
{
	int result = 0;

	asm __volatile__(
		"li 14, 1;"
		"li 15, 2;"
		"li 16, 1;"

		"LXVD2X 0, 0, %[vsx_load_chkpt]; "
		"LXVD2X 1, 0, %[vsx_load]; "
		"LXVD2X 2, 0, %[vsx_load_chkpt]; "

		".Ltrans_: ;"
		"tbegin.;"
		"beq .Ltrans_; "
		"1: ;"
		"li 16, 2;"
		"LXVD2X 2, 0, %[vsx_load]; "

		"2: ;"
		"cmpld cr0, 15, 16; "
		"bne cr0, 3f; "
		"vcmpequd. 3,2,1; "
		"bf 24, 3f; "
		"b 2b;"  /* retry loop */

		"3: ;"  /* find mismatch */
		"tend.;"

		"li 0, 1;"
		"ori %[res], 0, 0;"
		: [res] "=r" (result)
		: [vsx_load] "r" (&vsx_load[0]), [vsx_load_chkpt] "r" (&vsx_load_chkpt[0])
		: "memory", "r0", "r14", "r15", "r16"
		);

	if (result) {
		printf("meet error\n");
			exit(1);
	}
	
	exit(0);
}



int main(int argc, char *argv[])
{
	return simon_tm_gpr();
}
