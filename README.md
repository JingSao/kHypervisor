# kHypervisor
kHypervisor is an Open Source light-weighted Nested-Virtual Machine Monitor in Windows x64 platform. Temporarily not supported multi-core yet, and which is using a VT framework from Hyper-platform :-)

#Environment
- Visual Studio 2015 update 3 
- Windows SDK 10
- Windowr Driver Kit 10

#Description

The kHypervisor is not yet completed, and it will be rapidly update on progress: 

#Progress
2016-10-19 :  First commit, Supporting nested itself only, and nested software breakpoint exception from Level 2. And the nested-Vmm is able to dispatch this exception to L1 and help L1 to resume to L2.


2016-10-21 : Fixed Ring-3 vm-exit emulation error. 
