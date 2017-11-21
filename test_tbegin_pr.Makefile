ifeq (,$(KERNEL_SRC_TREE))
	KERNEL_SRC_TREE=/lib/modules/`uname -r`/build
endif

CFLAGS_test_tbegin_pr.o := -DDEBUG

ifeq (,$(KERNELRELEASE))
#Building standalone
default:
	@echo Building standalone!
	$(MAKE) -C $(KERNEL_SRC_TREE) M=$(PWD) modules

else
#Building from kernel source tree
#@echo Building from kernel source tree!
obj-m := test_tbegin_pr.o

endif

clean:
	rm -rf *.o *.ko
