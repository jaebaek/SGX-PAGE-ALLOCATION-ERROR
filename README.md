# SGX-PAGE-ALLOCATION-ERROR

Introduction
------------
This is a simple demo of the weird permission setup by SGX.
I met this when doing researches on security systems using SGX
([SGX-Shield](https://github.com/jaebaek/SGX-Shield)).

I allocated additional RWX pages and RW pages
to load my program into these pages,
conduct the relocation and run it.
To prevent attackers from injecting code dynamically, I applied
software-based Data Execution Prevention (DEP).

What I did:
------------
- git-cloned [Intel Linux SGX SDK](https://github.com/01org/linux-sgx)
and [Intel Linux SGX Device Driver](https://github.com/01org/linux-sgx-driver)
- modified **linux-sdk/sdk/sign_tool/SignTool/manage_metadata.cpp**
to allocate 32MB RWX pages and 32MB RW pages
(See line 401 ~ 421 of the file).
- added a few line in driver (see line 793 of **linux-sgx-driver/isgx_ioctl.c**)
and sdk (see line 158 of **linux-sgx/psw/urts/linux/enclave_creator_hw.cpp**)
to confirm setup page permissions
- dynamically loaded a program into RWX pages and ran it (see case1/).

What I did:
------------
- The program can be loaded, but cannot be executed.
- I confirm that the sdk and driver try to set page permissions as what I intended
(see **case1/out which** is printed by sdk and **case1/kern.log** which is kernel log).
- The result of case1 is shown in **case1/result**.
- In my personal opinion, it is not a software bug .. could anyone explain this?
