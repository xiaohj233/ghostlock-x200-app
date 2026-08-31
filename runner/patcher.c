/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * patcher.c — 内核模块符号地址迷你 patcher（patch_ko_all.py 的 C 移植）。
 * 对 ELF64 relocatable (ET_REL) .ko：
 *   1. 解析 section 头表 + 符号表（SHT_SYMTAB）。
 *   2. 遍历符号：对每个 UNDEF 符号，若 SymTab 有解析值 →
 *      st_value=符号绝对地址 + st_shndx=SHN_ABS(0xFFF1)。
 *      SHN_ABS 后 insmod 时内核不再查导出符号，直接用绝对地址。
 *   3. 清空 __versions 段 sh_size=0（本地 CRC 表与真机不匹配会被拒）。
 * 只需处理 ELF64 + aarch64 内核模块（小端）。
 */
#include "runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <elf.h>

typedef struct {
    uint8_t *data;
    size_t size;
} Buf;

/* section 名称解析（读 shstrtab，需在内存镜像内） */
static const char *sec_name_at(const Buf *b, const Elf64_Ehdr *eh, int idx) {
    if (idx < 0 || idx >= eh->e_shnum) return "";
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(b->data + eh->e_shoff);
    if (eh->e_shstrndx >= eh->e_shnum) return "";
    const Elf64_Shdr *sst = &sh[eh->e_shstrndx];
    size_t off = sh[idx].sh_name;
    if (sst->sh_offset + off >= b->size) return "";
    return (const char *)(b->data + sst->sh_offset + off);
}

/* 1) 符号重定位（对齐 patch_ko_all.py）：
 *    UNDEF 符号在 SymTab 有值 → st_value=addr + st_shndx=SHN_ABS(0xFFF1)。
 *    返回写入个数。 */
static size_t resolve_symbols(Buf *b, const Elf64_Ehdr *eh, const SymTab *st) {
    size_t written = 0;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(b->data + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        int strndx = sh[i].sh_link;
        if (strndx < 0 || strndx >= eh->e_shnum) continue;
        const Elf64_Shdr *strsh = &sh[strndx];
        if (strsh->sh_offset >= b->size) continue;
        const char *strtab = (const char *)(b->data + strsh->sh_offset);
        size_t strsz = strsh->sh_size;

        size_t entsz = sh[i].sh_entsize ? sh[i].sh_entsize : sizeof(Elf64_Sym);
        size_t cnt = sh[i].sh_size / entsz;
        for (size_t j = 0; j < cnt; j++) {
            Elf64_Sym *sym = (Elf64_Sym *)(b->data + sh[i].sh_offset + j * entsz);
            if (sym->st_shndx != SHN_UNDEF) continue;   /* 只处理未定义符号 */
            if (sym->st_name == 0 || sym->st_name >= strsz) continue;
            const char *name = strtab + sym->st_name;
            uint64_t addr = symtab_find(st, name);
            if (addr == 0) continue;    /* 样例 kallsyms 可能缺符号 → 跳过 */
            sym->st_value = addr;
            sym->st_shndx = 0xFFF1;     /* SHN_ABS */
            written++;
        }
    }
    return written;
}

/* 2) 清空 __versions 段 sh_size=0。返回 section 索引或 -1。 */
static int strip_versions(Buf *b, const Elf64_Ehdr *eh) {
    for (int i = 0; i < eh->e_shnum; i++) {
        if (strcmp(sec_name_at(b, eh, i), "__versions") == 0) {
            Elf64_Shdr *t = (Elf64_Shdr *)(b->data + eh->e_shoff + i * eh->e_shentsize);
            t->sh_size = 0;
            return i;
        }
    }
    return -1;
}

int patch_ko(const char *src, const char *dst, const SymTab *st) {
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < (long)sizeof(Elf64_Ehdr)) { fclose(f); return -1; }

    Buf b = { malloc((size_t)sz), (size_t)sz };
    if (!b.data) { fclose(f); return -1; }
    if (fread(b.data, 1, (size_t)sz, f) != (size_t)sz) {
        free(b.data); fclose(f); return -1;
    }
    fclose(f);

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b.data;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_type != ET_REL) {
        free(b.data);
        return -1;
    }

    size_t written = resolve_symbols(&b, eh, st);
    int vsec = strip_versions(&b, eh);

    FILE *o = fopen(dst, "wb");
    if (!o) { free(b.data); return -1; }
    int ok = (fwrite(b.data, 1, b.size, o) == b.size);
    fclose(o);
    free(b.data);
    if (!ok) { remove(dst); return -1; }
    LOG("patch_ko %s -> %s: %zu syms, __versions %s", src, dst, written,
        vsec >= 0 ? "stripped" : "not found");
    return (int)written;   /* 返回写入符号个数，>0 即成功 */
}
