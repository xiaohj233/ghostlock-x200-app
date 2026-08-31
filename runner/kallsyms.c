/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * kallsyms.c — /proc/kallsyms 解析 + 符号查找 + capsym 确定性筛选。
 * 筛选规则 = offsets_auto.py (v1.3.5) live 提取同规则。
 */
#include "runner.h"
#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int symtab_add(SymTab *st, uint64_t addr, char type, const char *name) {
    if (st->count >= st->cap) {
        size_t nc = st->cap ? st->cap * 2 : 4096;
        Sym *ni = realloc(st->items, nc * sizeof(Sym));
        if (!ni) return -1;
        st->items = ni;
        st->cap = nc;
    }
    Sym *s = &st->items[st->count++];
    s->addr = addr;
    s->type = type;
    snprintf(s->name, sizeof(s->name), "%s", name);
    return 0;
}

static int symtab_parse(SymTab *st, FILE *f) {
    char line[512];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if (first && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) {
            p += 3;   /* 跳过 UTF-8 BOM */
        }
        first = 0;
        char type = 0;
        uint64_t addr = 0;
        char name[256] = {0};
        /* 格式: <addr> <type> <name> */
        if (sscanf(p, "%llx %c %255s", (unsigned long long *)&addr, &type, name) != 3)
            continue;
        if (symtab_add(st, addr, type, name) != 0) return -1;
    }
    return 0;
}

int symtab_load(SymTab *st, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(st, 0, sizeof(*st));
    int r = symtab_parse(st, f);
    fclose(f);
    return r;
}

int symtab_load_proc(SymTab *st) {
    FILE *f = fopen("/proc/kallsyms", "r");
    if (!f) return -1;
    memset(st, 0, sizeof(*st));
    int r = symtab_parse(st, f);
    fclose(f);
    return r;
}

void symtab_free(SymTab *st) {
    free(st->items);
    memset(st, 0, sizeof(*st));
}

uint64_t symtab_find(const SymTab *st, const char *name) {
    for (size_t i = 0; i < st->count; i++) {
        if (st->items[i].addr != 0 && strcmp(st->items[i].name, name) == 0)
            return st->items[i].addr;
    }
    return 0;
}

/* capsym 评分：cap 位 1(DAC_OVERRIDE)/12(FOWNER)/19(SYS_ADMIN)/21(SYS_PTRACE) 计数 */
static int capsym_score(uint64_t addr) {
    int sc = 0;
    if ((addr >> 1) & 1) sc++;          /* bit 1 */
    if ((addr >> 12) & 1) sc++;         /* bit 12 */
    if ((addr >> 19) & 1) sc++;         /* bit 19 */
    if ((addr >> 21) & 1) sc++;         /* bit 21 */
    return sc;
}

uint64_t symtab_capsym(const SymTab *st, uint64_t kbase) {
    uint64_t best = kbase;      /* 无候选回退 kbase */
    int best_pref = 0, best_score = -1;
    uint64_t best_addr = 0;

    for (size_t i = 0; i < st->count; i++) {
        const Sym *s = &st->items[i];
        if (!(s->type == 'b' || s->type == 'B')) continue;      /* .bss */
        const char *p = strstr(s->name, "__key");
        if (!p || p[5] != '\0') continue;                        /* 仅 .*__key 精确结尾 */
        if (s->addr < kbase || s->addr >= kbase + DEV_CAPSYM_WIN) continue;
        if (!((s->addr >> 16) & 1)) continue;                    /* caps_ok 位 */
        int pref = (strcmp(s->name, "init_completion.__key") == 0) ? 1 : 0;
        int sc = capsym_score(s->addr);
        if (pref > best_pref ||
            (pref == best_pref && (sc > best_score ||
             (sc == best_score && (best_addr == 0 || s->addr < best_addr))))) {
            best = s->addr;
            best_pref = pref;
            best_score = sc;
            best_addr = s->addr;
        }
    }
    return best;
}
