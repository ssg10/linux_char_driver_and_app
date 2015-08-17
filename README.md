# Linux kernel character driver and user-space application for testing the driver
Linux kernel character driver and app program for prototype hardware. 
The idea is this is useful as as a basic framework and reusable code.

This is a complete systems software from kernel driver to user-space application program.
Developed as an systems software prototype memory storage type hardware for cloud servers, in-memory computing, 
and high-performance computing.

Taken as it is without prototype hardware available, it is using RAM space as scratch memory
and exercising data movement between Linux user-space to kernel space to memory hardware.
A full top-to-bottom software solution in Linux.

Some features from user space program to access the memory:
- read()
- write()
- mmap(): The sleak way to access physical memory from user space.
          User can map kernel physical memory into kernel virtual memory and then mapped directly into user-space virtual memory. User can then use its user-space virtual memory to do data manipulation and the changes are directly reflected in the physical memory.

Written in C.

Tested on Linux CentOS 7 distribution.
