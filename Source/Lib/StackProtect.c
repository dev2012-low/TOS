#include <Kernel/Types.h>
#include <Asm/Cpu.h>
#include <Kernel/SysStop.h>

UINTPTR __stack_chk_guard = 0xBAAAD00Du;

// Добавь эти атрибуты, чтобы LTO не трогала функцию
static NOPTR ATTRIBUTE(noreturn) ATTRIBUTE(used) ATTRIBUTE(noinline) 
KStackPanic(NOPTR) {
    SysStop("KSTACK_PANIC");
    __builtin_unreachable();
}

NOPTR ATTRIBUTE(noreturn) ATTRIBUTE(used) __stack_chk_fail(NOPTR) {
    KStackPanic();
}

NOPTR ATTRIBUTE(noreturn) ATTRIBUTE(used) __stack_chk_fail_local(NOPTR) {
    KStackPanic();
}
