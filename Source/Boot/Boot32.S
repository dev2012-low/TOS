.code32

.section .text
.globl Start
.globl Gdt
.globl TssBuffer
.globl Stack64Top
.globl BuildIdentityMap
.extern LongModeEntry
.extern BootPtPhysStart

.set VGA_BUF, 0xB8000
.set WHITE_ON_BLACK, 0x0F

.set PageSize, 4096
.set PtFrameMask, 0xFFFFF000
.set Pde2MbFlags, 0x083
.set PtTableFlags, 0x007

.set MAP_PML4_ENTRIES, 2
.set PDPT_ENTRIES, 512
.set PD_ENTRIES, 512

.set BOOT_PT_POOL_PAGES, (MAP_PML4_ENTRIES * (1 + PDPT_ENTRIES))
.set BOOT_PT_POOL_SIZE, (BOOT_PT_POOL_PAGES * PageSize)

# Dedicated physical region for early page tables (Link.ld .bootpt).
# While paging is off, CPU linear address == physical address here.
.section .bootpt, "aw", @nobits
.align PageSize

.globl Pml4Table
.globl KernelPml4
Pml4Table:
    .skip PageSize
KernelPml4 = Pml4Table

.align PageSize
.globl BootPagePool
BootPagePool:
    .skip BOOT_PT_POOL_SIZE

BootPoolCursor:
    .skip 4

MapPhysLo:
    .skip 4
MapPhysHi:
    .skip 4

.section .text

# Zero 4 KiB page; %edi = CPU pointer to the page (physical before paging).
PtClear:
    push %eax
    push %ecx
    push %edi
    mov $1024, %ecx
    xor %eax, %eax
    rep stosl
    pop %edi
    pop %ecx
    pop %eax
    ret

# Convert table frame address in %eax to a pointer PTE in %edx:%eax.
BootMakeTablePte:
    andl $PtFrameMask, %eax
    orl $PtTableFlags, %eax
    xorl %edx, %edx
    ret

# Store 64-bit PTE at CPU pointer %edi.
PtWriteEntry:
    movl %eax, (%edi)
    movl %edx, 4(%edi)
    ret

# Allocate one 4 KiB table page; returns physical frame in %eax.
BootAllocPtPage:
    push %edi
    movl BootPoolCursor, %eax
    addl $PageSize, BootPoolCursor
    movl %eax, %edi
    call PtClear
    pop %edi
    ret

# Fill a page directory at CPU pointer %edi (2 MiB identity entries).
PtFillPd:
    push %eax
    push %ebx
    push %ecx
    push %edx
    push %edi

    xor %edx, %edx

PtFillLoop:
    cmpl $PD_ENTRIES, %edx
    jge PtFillDone

    movl MapPhysLo, %eax
    movl MapPhysHi, %ecx
    orl $Pde2MbFlags, %eax
    movl %eax, (%edi)
    movl %ecx, 4(%edi)

    movl MapPhysLo, %eax
    addl $0x00200000, %eax
    movl %eax, MapPhysLo
    jnc PtFillNoCarry
    incl MapPhysHi
PtFillNoCarry:
    addl $8, %edi
    incl %edx
    jmp PtFillLoop

PtFillDone:
    pop %edi
    pop %edx
    pop %ecx
    pop %ebx
    pop %eax
    ret

# Build 1 TiB identity map (2 MiB pages). Returns PML4 physical frame in %eax.
BuildIdentityMap:
    pusha

    movl $BootPagePool, %eax
    movl %eax, BootPoolCursor

    movl $BootPtPhysStart, %edi
    call PtClear

    xorl %ebp, %ebp

BuildPml4Loop:
    cmpl $MAP_PML4_ENTRIES, %ebp
    jge BuildIdentityMapDone

    movl $0, MapPhysLo
    movl %ebp, %eax
    shll $7, %eax
    movl %eax, MapPhysHi

    call BootAllocPtPage
    movl %eax, %ebx

    movl %ebx, %eax
    call BootMakeTablePte
    movl $BootPtPhysStart, %edi
    leal (%edi, %ebp, 8), %edi
    call PtWriteEntry

    xorl %esi, %esi

BuildPdptLoop:
    cmpl $PDPT_ENTRIES, %esi
    jge BuildPdptNext

    call BootAllocPtPage
    movl %eax, %edi

    push %edi
    movl %edi, %eax
    call BootMakeTablePte
    leal (%ebx, %esi, 8), %edi
    call PtWriteEntry
    pop %edi

    call PtFillPd

    incl %esi
    jmp BuildPdptLoop

BuildPdptNext:
    incl %ebp
    jmp BuildPml4Loop

BuildIdentityMapDone:
    movl $BootPtPhysStart, %eax
    movl %eax, 28(%esp)
    popa
    ret

Start:
    cli

    mov $Message, %esi
    mov $VGA_BUF, %edi
    mov $WHITE_ON_BLACK, %ah
    call WriteString

    lgdt GdtDesc

    movl %cr4, %eax
    btsl $5, %eax
    movl %eax, %cr4

    movl %cr0, %eax
    andl $0xFFFFFFFB, %eax
    orl $0x2, %eax
    movl %eax, %cr0

    movl %cr4, %eax
    orl $0x600, %eax
    movl %eax, %cr4

    fninit

    call BuildIdentityMap
    movl %eax, %cr3

    movl $0xC0000080, %ecx
    rdmsr
    btsl $8, %eax
    btsl $11, %eax
    wrmsr

    movl %cr0, %eax
    btsl $31, %eax
    movl %eax, %cr0

    ljmp $0x08, $LongModeEntry

WriteString:
    lodsb
    test %al, %al
    jz WriteStringDone
    stosw
    jmp WriteString
WriteStringDone:
    ret

.section .rodata
Message:
    .asciz "TOS booting..."

.section .note.GNU-stack