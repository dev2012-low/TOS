#!/usr/bin/env python3
import os
import sys
import hashlib
import subprocess
import struct

def extract_section(elf_file, section):
    """Извлекает секцию из ELF в бинарный файл"""
    try:
        # Проверяем, существует ли секция
        result = subprocess.run(
            ["objdump", "-h", elf_file],
            capture_output=True,
            text=True
        )
        if section not in result.stdout:
            print(f"[WARN] Section {section} not found in {elf_file}")
            return None
        
        # Извлекаем секцию
        subprocess.run(
            ["objcopy", "-O", "binary", "-j", section, elf_file, f"{section}.bin"],
            check=True,
            capture_output=True
        )
        with open(f"{section}.bin", "rb") as f:
            data = f.read()
        os.remove(f"{section}.bin")
        return data
    except Exception as e:
        print(f"[ERROR] Failed to extract {section}: {e}")
        return None

def get_section_info(elf_file, section):
    """Получает размер и смещение секции"""
    try:
        result = subprocess.run(
            ["objdump", "-h", elf_file],
            capture_output=True,
            text=True,
            check=True
        )
        for line in result.stdout.split('\n'):
            if section in line:
                parts = line.split()
                if len(parts) >= 7:
                    # Формат: idx name size vma lma fileoff algn
                    size = int(parts[2], 16)
                    offset = int(parts[5], 16)
                    return offset, size
    except Exception as e:
        print(f"[ERROR] Failed to get section info: {e}")
    return None, None

def main():
    # Путь к ELF-файлу
    elf_file = "Bin/TOS.SF"
    
    # Проверяем, существует ли файл
    if not os.path.exists(elf_file):
        print(f"[ERROR] File not found: {elf_file}")
        print("[INFO] Make sure you built the kernel first!")
        sys.exit(1)
    
    print(f"[INFO] Processing {elf_file}")
    
    # 1. Показываем все секции (для отладки)
    result = subprocess.run(
        ["objdump", "-h", elf_file],
        capture_output=True,
        text=True
    )
    print("[INFO] Available sections:")
    for line in result.stdout.split('\n'):
        if ' .text' in line or ' .rodata' in line or ' .data' in line:
            print(f"  {line.strip()}")
    
    # 2. Извлекаем .text и .rodata
    text = extract_section(elf_file, ".text")
    rodata = extract_section(elf_file, ".rodata")
    
    if text is None and rodata is None:
        print("[ERROR] No sections found! Check the ELF file.")
        sys.exit(1)
    
    # 3. Вычисляем хеш
    sha = hashlib.sha256()
    if text:
        print(f"[INFO] .text size: {len(text)} bytes")
        sha.update(text)
    if rodata:
        print(f"[INFO] .rodata size: {len(rodata)} bytes")
        sha.update(rodata)
    hash_bytes = sha.digest()
    
    print(f"[OK] Hash: {hash_bytes.hex()}")
    
    # 4. Проверяем секцию .checksum
    offset, size = get_section_info(elf_file, ".checksum")
    if offset is None:
        print("[WARN] .checksum section not found, creating it...")
        # Создаём .checksum секцию через objcopy
        with open("checksum.bin", "wb") as f:
            f.write(b'\x00' * 4096)  # 4KB выравнивание
        subprocess.run(
            ["objcopy", "--add-section", ".checksum=checksum.bin", 
             "--set-section-flags", ".checksum=alloc,load,readonly", 
             elf_file, elf_file],
            check=True
        )
        os.remove("checksum.bin")
        offset, size = get_section_info(elf_file, ".checksum")
        if offset is None:
            print("[ERROR] Failed to create .checksum section")
            sys.exit(1)
    
    if size < 32:
        print(f"[ERROR] .checksum section too small: {size} bytes (need 32)")
        sys.exit(1)
    
    # 5. Патчим ELF
    print(f"[INFO] Patching .checksum at offset 0x{offset:x}, size {size} bytes")
    with open(elf_file, "r+b") as f:
        f.seek(offset)
        f.write(hash_bytes)
        if size > 32:
            f.write(b'\x00' * (size - 32))
    
    print("[OK] Kernel checksum patched successfully!")

if __name__ == "__main__":
    main()