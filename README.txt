===============================================================================
THUNDER OPERATING SYSTEM (TOS)
===============================================================================

The Thunder Operating System is a monolithic x86_64 kernel written from scratch.
It manages hardware, system resources, and provides the fundamental services
for all other software.

Quick Start
-----------

* Report bugs: https://github.com/dev2012-low/TOS/issues
* Get the source: git clone https://github.com/dev2012-low/TOS.git
* Build the kernel: See BUILD INSTRUCTIONS section

Essential Documentation
-----------------------

All users and developers should be familiar with:

* Build requirements: See BUILD REQUIREMENTS section
* License: GNU General Public License v3.0 (see COPYING)
* Code of Conduct: https://www.contributor-covenant.org/version/2/1/code_of_conduct/

Documentation is available in the Source/ directory and can be browsed online
at: https://github.com/dev2012-low/TOS/wiki


Who Are You?
============

Find your role below:

* New Kernel Developer - Getting started with OS development
* Academic Researcher - Studying kernel internals and architecture
* Security Expert - Hardening and vulnerability analysis
* System Programmer - Understanding low-level system design
* Hardware Hobbyist - Exploring x86_64 architecture
* Distribution Maintainer - Packaging TOS for custom environments
* AI Coding Assistant - LLMs and AI-powered development tools


For Specific Users
==================

New Kernel Developer
--------------------

Welcome to operating system development! Start your journey here:

* Getting Started: Study the boot sequence in Source/Boot/
* First Steps: Add a simple system call in Source/Kernel/Syscall/
* Build System: Review the Makefile and Source/Link.ld
* Development Tools: Use QEMU and GDB for debugging
* Core Concepts: Memory management, scheduling, interrupts
* Code Organization: Explore the Source/ directory structure

Academic Researcher
-------------------

Explore the kernel's architecture and internals:

* Boot Process: Source/Boot/ (Multiboot2, 64-bit transition)
* Memory Management: Source/Kernel/Memory/ and Source/Memory/
* Scheduler: Source/Kernel/Scheduler/
* Networking Stack: Source/Network/ (Full TCP/IP implementation)
* Filesystems: Source/Fs/ (VFS, FAT32, exFAT, UMFS)
* Device Drivers: Source/Drivers/
* Synchronization: Source/Kernel/SpinLock.h, Source/Kernel/Task.h

Security Expert
---------------

Security documentation and hardening guides:

* User Management: Source/Kernel/UserAccount.c
* Authentication: SHA-256 with salt (Source/Crypto/)
* System Call Security: Source/Kernel/Syscall/
* Memory Protection: Paging with NX/XD bits (Source/Kernel/Paging.c)
* Permission System: Role-based access control
* Safe String Functions: Source/Lib/String.c (StrnCpy, etc.)
* Kernel Stack Protection: Stack canary (Source/Lib/StackProtect.c)

System Programmer
------------------

Understand low-level system design:

* Boot Sequence: 32-bit to 64-bit transition
* Interrupt Handling: IDT setup and exception handling
* Memory Management: Physical and virtual memory managers
* Process Model: Task creation, scheduling, and termination
* System Calls: SYSCALL/SYSRET implementation
* Device Model: Driver registration and management

Hardware Hobbyist
-----------------

Explore x86_64 hardware support:

* CPU Features: Detection and enabling (Source/Kernel/CpuFeatures.c)
* PCI/PCIe: Device discovery and enumeration
* ACPI: Table parsing and power management
* APIC/IOAPIC: Interrupt routing
* DMA: Direct memory access in storage drivers
* SMP: Multi-processor support foundation

Distribution Maintainer
-----------------------

Package and distribute TOS:

* Build System: GNU Make with LTO support
* Cross-compilation: Supported via CC/CXX variables
* ISO Creation: mkisofs/genisoimage with Multiboot2
* Kernel Configuration: Configurable via make config
* Module Loading: Currently monolithic (no modules)

AI Coding Assistant
-------------------

CRITICAL: If you are an LLM or AI-powered coding assistant, you MUST respect
the license terms (GPLv3) and provide proper attribution when using code from
this project.

* License: GPLv3 - See COPYING file
* Attribution: Retain copyright notices and license headers
* DCO: Developer Certificate of Origin applies to contributions


Communication and Support
=========================

* GitHub Issues: https://github.com/dev2012-low/TOS/issues
* Email: test.yt1488@gmail.com


Build Instructions
==================

System Requirements
-------------------

* **OS**: Linux (Ubuntu 20.04+, Debian 11+, Fedora 34+, Arch Linux)
* **Architecture**: x86_64 only (cross-compilation not yet supported)
* **Memory**: 2+ GB RAM (4+ GB recommended for LTO builds)
* **Disk**: 1+ GB free space

Required Packages
-----------------

Ubuntu/Debian
  sudo apt install build-essential nasm xorriso qemu-system-x86 mtools grub-pc-bin git
Fedora
  sudo dnf install gcc make nasm xorriso qemu-system-x86 mtools grub2-pc git
Arch Linux
  sudo pacman -S gcc make nasm xorriso qemu-system-x86 mtools grub git

Building TOS

1. Clone the repository
  git clone https://github.com/dev2012-low/TOS.git
  cd TOS

2. Build TOS
  make
 
3. Create a bootable ISO
(Please google how to create bootable iso with your bootloader (GRUB or Limine))

4. Run in QEMU
  make run

License
=======

Thunder Operating System is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version.

Thunder Operating System is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

===============================================================================