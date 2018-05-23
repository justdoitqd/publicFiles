# 32 bytes pre-checking on memcmp()

## Background
To optimize ppc64 memcmp() with VMX instruction, we need to think about the VMX penalty brought with:
If kernel uses VMX instruction, it needs to save/restore current thread's VMX registers. There are 
32 x 128 bits VMX registers in PPC, which means 32 x 16 = 512 bytes for load and store.

The major concern regarding the memcmp() performance in kernel is KSM, who will use memcmp() to merge
identical pages. So it will make sense to take some measures/enhancement on KSM to see whether any improvement
can be done here.  Cyril Bur indicates that the memcmp() for KSM has a higher possibility to fail (unmatch) 
early in previous bytes in following mail. And I am taking a follow-up on this.
    https://patchwork.ozlabs.org/patch/817322/#1773629
    
The work will expand to commons cases other than KSM later.

## 1. KSM
### Measure KSM early unmatch byte#
To measure the possibility of memcmp() early failure on KSM, I modified ksm memcmp API with following patch, 
so that the compared bytes number for each KSM memcmp() can be recorded and dumped.
https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_cnt/0001-Add-memcmp-cnt-function-to-measure-KSM-cnt-bytes.patch

The test was performed at 2 host named "Ju" and "Jin", and the data is collect at:
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_cnt/ju/ksm

https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_cnt/jin


In both machines, several guests are running while collecting the data, and turning on KSM: 
echo 1 > /sys/kernel/mm/ksm/run

```
root@Ju:~# virsh list
 Id    Name                           State
----------------------------------------------------
 2     centos72_tt                    running
 3     rhel72                         running
 4     rhel73                         running
 5     ubuntu1710                     running
 
 [simon@jin ~]$ virsh list
 Id    名称                         状态
----------------------------------------------------
 1     ubuntu_small                   running
 2     xenial2                        running
 3     centos72_le_small              running
```

All data reveals a high possibility of early fail/unmatch of memcmp() on KSM.
Following is a graph based on one data from Ju:
```
X: the unmatch byte# which memcmp() stops; 
Y: counter of that unmatch byte# which memcmp() stops
```  
![Ju ksm memcmp cnt](https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_cnt/ju/ksm/Ju_cnt1.png "Ju ksm memcmp cnt")

Further calculation shows:
```
    - 76% cases will fail/unmatch before 16 bytes;
    - 83% cases will fail/unmatch before 32 bytes;
    - 84% cases will fail/unmatch before 64 bytes;
```

**As a result, I think it will be good for KSM to pre-check 32 bytes before going into VMX instructions.**

### memcmp optimization and profiling
Following patch inserts a previous 32 bytes checking before use VMX instructions:
https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_profile/ksm/0001-powerpc-64-add-KSM-optimization-for-memcmp.patch

Then I use ftrace to measure the memcmp() average execution time. Looks the ftrace 
cannot profile the Assembly memcmp implementation, and I add a simple C wrapper so
that I can use ftrace:
https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_profile/ksm/0001-wrap-memcmp-with-a-C-function-so-that-it-can-be-pg-a.patch

the data is collected at:

https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/ksm/patch
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/ksm/patch_2
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/ksm/no_patch
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/ksm/no_patch_2

Following is the average execute time of memcmp():
```
- Without patch:
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk ksm/no_patch/trace
average us = 0.185893 us
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk ksm/no_patch_2/trace
average us = 0.211385 us
```

```
- With patch:
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk ksm/patch/trace
average us = 0.15108 us
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk ksm/patch_2/trace
average us = 0.141374 us
```

**As can be seen, the average memcmp() time can be improved ~+20% with the 32 bytes
pre-checking optmization.**

## 2. Common case:
This section tests whether the change will improve common cases without KSM: 
/sys/kernel/mm/ksm/run = 0


I didn't launch any special call load other than VMs. 


- The memcmp() fail/unmatch bytes statistics also demonstrates an early failure pattern:
~ +73% for the early 32 or even 16 bytes. 

Data locates at:
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_cnt/ju/noksm


- For the memcmp() average execute time, it also improves:
```
// with https://patchwork.ozlabs.org/patch/914265/  and https://patchwork.ozlabs.org/patch/914271/ , which has VMX optimization and no 32B prechk optimization
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile/noksm# ../memcmp_dur_average.awk patch_32Bn_VMXy_noksm/trace
average us = 0.0689134 us

// with https://patchwork.ozlabs.org/patch/914265/  and https://patchwork.ozlabs.org/patch/914271/ and https://patchwork.ozlabs.org/patch/914273/ , which enables 32B prechk optimization
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile/noksm# ../memcmp_dur_average.awk patch_32By_VMXy_noksm/trace
average us = 0.0552645 us
```
Data locates at: https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/noksm

**As can be seen, with no specific call load, the average memcmp() time can also be improved ~+20% with the 32 bytes
pre-checking optmization. **
