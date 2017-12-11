#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

int main()
{
    int fd = open("/dev/kvm", O_RDWR);
    int ret = 0;
    ioctl(fd, KVM_CREATE_VM, KVM_VM_PPC_HV);
    ret = ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_PPC_HTM);
    close(fd);
    if (!ret) {
	    printf("check_htm_extension of HV, ret=%d\n", ret);
	    return -1;
    }

    fd = open("/dev/kvm", O_RDWR);
    ioctl(fd, KVM_CREATE_VM, KVM_VM_PPC_PR);
    ret = ioctl(fd, KVM_CHECK_EXTENSION, KVM_CAP_PPC_HTM);
    close(fd);
    if (!ret) {
	    printf("check_htm_extension of PR, ret=%d\n", ret);
	    return -1;
    }
    printf("test successfully\n");
    return 0;
}
