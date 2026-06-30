#pragma once

#include <Kernel/Types.h>

// Типы памяти, которые можно безопасно очистить
typedef enum {
    KPMC_TYPE_BOOTLOADER = 0,   // GRUB/Limine и др. (0x0 - 0x100000)
    KPMC_TYPE_VGA = 1,           // VGA memory (0xA0000 - 0xBFFFF)
    KPMC_TYPE_BIOS_SHADOW = 2,   // BIOS shadow (0xE0000 - 0xFFFFF)
    KPMC_TYPE_MULTIBOOT2 = 3,    // Multiboot2 структуры
    KPMC_TYPE_ACPI_RECLAIM = 4,  // ACPI Reclaimable (тип 3)
    KPMC_TYPE_UEFI = 5,          // UEFI временные структуры
    KPMC_TYPE_FRAMEBUFFER = 6,   // Фреймбуфер после инициализации
    KPMC_TYPE_ALL = 99           // Всё вышеперечисленное
} KpmcType;

// Инициализация и очистка
NOPTR KpmcInit(NOPTR);
NOPTR KpmcClean(KpmcType Type);
NOPTR KpmcCleanAll(NOPTR);

// Получить статистику (сколько освобождено)
UINT64 KPMC_GetFreedPages(NOPTR);
UINT64 KPMC_GetFreedBytes(NOPTR);