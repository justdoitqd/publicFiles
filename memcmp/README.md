# Background
To optimize ppc64 memcmp() with VMX instruction, we need to think about the VMX penalty brought with:
If kernel uses VMX instruction, it needs to save/restore current thread's VMX registers. There are 
32 x 128 bits VMX registers in PPC, which means 32 x 16 = 512 bytes for load and store.

The major concern regarding the memcmp() performance in kernel is KSM, who will use memcmp() to merge
identical pages. So it will make sense to take some measures/enhancement on KSM to see whether any improvement
can be done here.  Cyril Bur indicates that the memcmp() for KSM has a higher possibility to fail (unmatch) 
early in previous bytes in following mail. And I am taking a follow-up on this.
    https://patchwork.ozlabs.org/patch/817322/#1773629

# Measure KSM early unmatch byte#
To measure the possibility of memcmp() early failure on KSM, I modified ksm memcmp API with following patch, 
so that the compared bytes number for each KSM memcmp() can be recorded and dumped.
https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_cnt/0001-Add-memcmp-cnt-function-to-measure-KSM-cnt-bytes.patch

The test was performed at 2 host named "Ju" and "Jin", and the data is collect at:
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_cnt/ju
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_cnt/jin

In both machines, several guests are running while collecting the data: 
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
![Ju memcmp cnt](https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_cnt/ju/Ju_cnt1.png "Ju memcmp cnt")

Further calculation shows:
```
    - 76% cases will fail/unmatch before 16 bytes;
    - 83% cases will fail/unmatch before 32 bytes;
    - 84% cases will fail/unmatch before 64 bytes;
```

**As a result, I think it will be good for KSM to pre-check 32 bytes before going into VMX instructions.**

# KSM memcmp optimization and profiling
Following patch inserts a previous 32 bytes checking before use VMX instructions:
https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_profile/0001-powerpc-64-add-KSM-optimization-for-memcmp.patch

Then I use ftrace to measure the memcmp() average execution time. Looks the ftrace 
cannot profile the Assembly memcmp implementation, and I add a simple C wrapper so
that I can use ftrace:
https://github.com/justdoitqd/publicFiles/blob/master/memcmp/memcmp_profile/0001-wrap-memcmp-with-a-C-function-so-that-it-can-be-pg-a.patch

the data is collected at:

https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/patch
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/patch_2
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/no_patch
https://github.com/justdoitqd/publicFiles/tree/master/memcmp/memcmp_profile/no_patch_2

Following is the average execute time of memcmp():
```
- Without patch:
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk no_patch/trace
average us = 0.185893 us
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk no_patch_2/trace
average us = 0.211385 us
```

```
- With patch:
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk patch/trace
average us = 0.15108 us
root@ubuntu1710:/sdb/publicFiles/memcmp/memcmp_profile# ./memcmp_dur_average.awk patch_2/trace
average us = 0.141374 us
```

**As can be seen, the average memcmp() time can be improved ~+20% with the 32 bytes
pre-checking optmization.**
