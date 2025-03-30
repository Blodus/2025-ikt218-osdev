#pragma once
#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "libc/stddef.h"
#include "libc/stdint.h"

#define EI_NIDENT 16

typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

#define PT_NULL    0
#define PT_LOAD    1

/**
 * Loads an ELF binary from a file into a new address space.
 *
 * @param path         Path to the ELF binary.
 * @param page_directory Pointer to the process's page directory to update.
 * @param entry_point  Output parameter to receive the ELF entry point.
 * @return 0 on success, -1 on failure.
 */
int load_elf_binary(const char *path, uint32_t *page_directory, uint32_t *entry_point);

#endif // ELF_LOADER_H
