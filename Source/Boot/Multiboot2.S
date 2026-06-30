# Multiboot2.S - Multiboot2 header for 32-bit boot
.section .multiboot, "ax"
.code32
.align 8

# Multiboot2 header
.align 8
Mb2HeaderStart:
    .long 0xE85250D6
    .long 0x00000000
    .long (Mb2HeaderEnd - Mb2HeaderStart)
    .long -(0xE85250D6 + 0x00000000 + (Mb2HeaderEnd - Mb2HeaderStart))

.align 8
InfoRequestTag:
    .word 1
    .word 0
    .long (InfoRequestTagEnd - InfoRequestTag)
    .long 6
InfoRequestTagEnd:

# Framebuffer tag
.align 8
FbTagStart:
    .word 5
    .word 0
    .word (FbTagEnd - FbTagStart)
    .long 0
    .long 0
    .long 0
FbTagEnd:

# End tag
.align 8
EndTag:
    .word 0
    .word 0
    .long 8
Mb2HeaderEnd:

.section .note.GNU-stack