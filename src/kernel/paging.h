#pragma once

#include <aarch64/mmu.h>
#include <kernel/proc.h>

#define ST_FILE 1
#define ST_SWAP (1 << 1)
#define ST_RO (1 << 2)
#define ST_HEAP (1 << 3)
#define ST_STACK (1 << 4)
#define ST_TEXT (ST_FILE | ST_RO)
#define ST_DATA ST_FILE
#define ST_BSS ST_FILE

#define STACK_TOP 0x800000

struct section {
    u64 flags;
    u64 begin;
    u64 end;
    ListNode stnode;
    // These are for file-backed sections
    struct file *fp; // pointer to file struct
    u64 offset;      // the offset in file
    u64 length;      // the length of mapped content in file
};

struct sections_info {
    u64 text_begin;
    u64 text_end;
    u64 data_begin;
    u64 data_end;
    u64 bss_begin;
    u64 bss_end;
    u64 heap_begin;
    u64 heap_end;
    u64 stack_begin;
    u64 stack_end;
};

int pgfault_handler(u64 iss);
void init_sections(ListNode *section_head);
void copy_sections(struct pgdir *dst, struct pgdir *src);
void set_sections(struct pgdir *dst, struct sections_info secs);
void get_sections(struct pgdir *src, struct sections_info *secs);
void free_sections(struct pgdir *pd);
struct section *get_section_by_va(u64 va);
u64 sbrk(i64 size);