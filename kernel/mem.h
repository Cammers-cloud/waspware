#ifndef KERNEL_MEM_H
#define KERNEL_MEM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096ULL

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} mem_region_t;

/*
 * Initialize the physical page-frame allocator from a Multiboot2
 * information structure. The kernel image and boot information region
 * are reserved before any frames are made available.
 *
 * Returns 0 on success, nonzero on malformed or unsupported input.
 */
int mem_init(uint32_t multiboot_magic, uint64_t multiboot_info_phys);

/* Allocate one 4 KiB physical page frame. Returns 0 on failure. */
uint64_t page_alloc(void);

/* Release a page-aligned physical frame previously returned by page_alloc. */
int page_free(uint64_t physical_address);

/*
 * Return an identity-mapped virtual address for a physical address.
 * This is valid only for addresses within the bootstrap's 1 GiB map.
 */
void *phys_to_virt(uint64_t physical_address);

/* Return the number of allocatable page frames currently free. */
uint64_t page_free_count(void);

#endif
