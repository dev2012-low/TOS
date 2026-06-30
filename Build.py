#!/usr/bin/env python3
import os
import sys
from pathlib import Path
import subprocess
import time
import shutil
# Цвета
C_GREEN = "\033[0;32m"
C_YELLOW = "\033[1;33m"
C_RED = "\033[0;31m"
C_BLUE = "\033[0;34m"
C_NC = "\033[0m"

def log_status(msg): print(f"{C_GREEN}[+]{C_NC} {msg}")
def log_warning(msg): print(f"{C_YELLOW}[!]{C_NC} {msg}")
def log_error(msg): print(f"{C_RED}[ERROR]{C_NC} {msg}")
def log_info(msg): print(f"{C_BLUE}[i]{C_NC} {msg}")

SCRIPT_PATH = Path(__file__).resolve()

# -----------------------------------------------------------------------------
# ПУТИ (будут перезаписаны при настройке)
# -----------------------------------------------------------------------------
KERNEL_DIR = "/home/alex/Рабочий стол/TOS"
MAKEOS_DIR = "/home/alex/Рабочий стол/TOS/BuildTools/Make"
ISO_PATH = "/home/alex/Рабочий стол/TOS/TOS.iso"
BUILD_ELF = "/home/alex/Рабочий стол/TOS/Bin/TOS.SF"
BOOT_ELF = "/home/alex/Рабочий стол/TOS/BuildTools/Make/boot/TOS.SF"
BOOT_DIR = "/home/alex/Рабочий стол/TOS/BuildTools/Make/boot"
# -----------------------------------------------------------------------------

def ask_path(prompt, default):
    while True:
        resp = input(f"{prompt} [{default}]: ").strip()
        if not resp:
            resp = default
        p = Path(resp).resolve()
        if p.exists():
            return str(p)
        log_error(f"Path does not exist: {p}")

def save_config_in_self(new_kernel_dir, new_makeos_dir):
    """Перезаписывает этот скрипт, подставляя новые пути в секцию конфигурации."""
    try:
        content = SCRIPT_PATH.read_text(encoding="utf-8")
    except Exception as e:
        log_error(f"Failed to read own script: {e}")
        sys.exit(1)

    # Простая замена строк в блоке конфигурации (по шаблону)
    # Мы ищем строки вида KERNEL_DIR = "..." или KERNEL_DIR = './...' и меняем их
    def replace_line(content, var_name, new_value):
        import re
        # Поддерживаем одинарные и двойные кавычки
        pattern = rf'^{var_name}\s*=\s*(["\'])(.*?)\1\s*$'
        def repl(m):
            quote = m.group(1)
            return f'{var_name} = {quote}{new_value}{quote}'
        new_content, count = re.subn(pattern, repl, content, flags=re.MULTILINE)
        if count == 0:
            log_warning(f"Could not find '{var_name}' line to update. Check the script structure.")
        return new_content

    content = replace_line(content, "KERNEL_DIR", new_kernel_dir.replace("\\", "/"))
    content = replace_line(content, "MAKEOS_DIR", new_makeos_dir.replace("\\", "/"))

    # Пересчитаем остальные пути на основе новых KERNEL_DIR и MAKEOS_DIR
    kd = Path(new_kernel_dir)
    md = Path(new_makeos_dir)
    new_iso = str((kd / "TOS.iso").resolve())
    new_build = str((kd / "Bin" / "TOS.SF").resolve())
    new_boot = str((md / "boot" / "TOS.SF").resolve())
    new_boot_dir = str((md / "boot").resolve())

    content = replace_line(content, "ISO_PATH", new_iso.replace("\\", "/"))
    content = replace_line(content, "BUILD_ELF", new_build.replace("\\", "/"))
    content = replace_line(content, "BOOT_ELF", new_boot.replace("\\", "/"))
    content = replace_line(content, "BOOT_DIR", new_boot_dir.replace("\\", "/"))

    try:
        SCRIPT_PATH.write_text(content, encoding="utf-8")
        log_status("Configuration saved successfully in the script itself.")
    except Exception as e:
        log_error(f"Failed to write updated script: {e}")
        sys.exit(1)

def check_paths():
    checks = [
        (KERNEL_DIR, "Kernel directory"),
        (MAKEOS_DIR, "MakeOS directory"),
        (BOOT_DIR, "Boot directory"),
    ]
    ok = True
    for path, name in checks:
        p = Path(path)
        if not p.exists():
            log_error(f"{name} does not exist: {p}")
            ok = False
    if not ok:
        return False

    # Проверим, что Makefile есть
    if not (Path(KERNEL_DIR) / "Makefile").exists():
        log_error("Makefile not found in kernel directory.")
        return False
    # Проверим grub-mkrescue
    try:
        subprocess.run(["grub-mkrescue", "--version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        log_error("grub-mkrescue not found. Install grub2-tools (e.g., sudo apt install grub2-common grub-pc-bin).")
        return False

    return True

def run_build():
    log_status("Starting TOS Kernel Build and Run...")

    kernel_path = Path(KERNEL_DIR).resolve()
    log_info(f"Changing to kernel directory: {kernel_path}")
    os.chdir(kernel_path)

    start_time = time.time()
    log_status("Running 'make'...")
    ret = subprocess.run(["make", f"-j{os.cpu_count()}"])
    if ret.returncode != 0:
        log_error("Build failed!")
        sys.exit(1)
    duration = int(time.time() - start_time)
    log_status(f"Build completed successfully in {duration} seconds.")

    build_elf = Path(BUILD_ELF)
    if not build_elf.exists():
        log_error("Built kernel not found: BUILD_ELF")
        sys.exit(1)
    log_status("Kernel found.")

    iso_path = Path(ISO_PATH)
    if iso_path.exists():
        log_status("Removing old ISO...")
        iso_path.unlink()
        log_status("Old ISO removed.")
    else:
        log_warning("No old ISO found to remove.")

    boot_dir = Path(BOOT_DIR)
    boot_dir.mkdir(parents=True, exist_ok=True)
    log_info(f"Boot directory: {boot_dir}")

    boot_elf = Path(BOOT_ELF)
    if boot_elf.exists():
        log_status("Removing old kernel from boot...")
        boot_elf.unlink()
        log_status("Old kernel removed from boot.")

    log_status("Copying new kernel to boot...")
    shutil.copy(build_elf, boot_elf)
    if boot_elf.exists():
        size = boot_elf.stat().st_size
        kb = size / 1024
        log_status(f"Kernel copied to {boot_elf}")
        log_info(f"Size: {kb:.1f} KB")
    else:
        log_error("Failed to copy kernel.")
        sys.exit(1)

    makeos_path = Path(MAKEOS_DIR).resolve()
    log_info(f"Creating ISO with grub-mkrescue from: {makeos_path}")
    os.chdir(makeos_path)

    start_time = time.time()
    ret = subprocess.run(["grub-mkrescue", "-o", str(iso_path), "."])
    duration = int(time.time() - start_time)

    if ret.returncode == 0 and iso_path.exists():
        log_status(f"ISO created successfully in {duration} seconds.")
        log_status(f"Path: {iso_path}")
        log_status(f"Size: {iso_path.stat().st_size / 1024 / 1024:.1f} MB")

        # Валидация ISO
        try:
            file_out = subprocess.check_output(["file", str(iso_path)], text=True)
            if "ISO" in file_out:
                log_status("ISO file is valid.")
            else:
                log_warning("ISO file may be corrupted.")
        except Exception:
            log_warning("Could not verify ISO with 'file' command.")
    else:
        log_error("Failed to create ISO!")
        sys.exit(1)

    log_status("Script finished successfully.")

def main():
    # Если пути не выглядят как относительные к проекту (например, /mnt/c/...),
    # можно добавить дополнительную проверку, но тут мы просто проверяем существование.
    if check_paths():
        run_build()
        return

    log_warning("Configuration paths are invalid or directories missing.")
    log_info("Please enter correct paths. The script will update itself and exit.")
    print()

    new_kernel = ask_path("Enter path to kernel directory (with Makefile)", KERNEL_DIR)
    new_makeos = ask_path("Enter path to 'make' directory (with boot/ and grub configs)", MAKEOS_DIR)

    save_config_in_self(new_kernel, new_makeos)

    print()
    log_status("Configuration updated in the script.")
    log_warning("Please re-run the script now:")
    print(f"  {sys.executable} {SCRIPT_PATH}")
    sys.exit(0)

if __name__ == "__main__":
    main()