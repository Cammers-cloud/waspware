#ifndef LOADER_POLLEN_H
#define LOADER_POLLEN_H

#include <stddef.h>
#include <stdint.h>

/*
 * Format is little-endian and packed. The loader reads fields bytewise rather
 * than dereferencing packed integers, so the input buffer may be unaligned.
 */
#define POLLEN_VERSION 1U
#define POLLEN_ALIGNMENT 4096U
#define POLLEN_MAGIC_SIZE 6U

#define POLLEN_USER_MIN 0x0000000000010000ULL
#define POLLEN_USER_MAX 0x00007FFFFFFFF000ULL
#define POLLEN_USER_TOP 0x0000800000000000ULL

#define POLLEN_VM_READ  0x01U
#define POLLEN_VM_WRITE 0x02U
#define POLLEN_VM_EXEC  0x04U

#define POLLEN_USER_STACK_SIZE (64U * 1024U)

/*
 * Exact on-disk header layout, total size 66 bytes.
 * All integer fields are little-endian.
 */
typedef struct __attribute__((packed)) {
    uint8_t magic[6];
    uint16_t version;
    uint64_t virtual_base;
    uint64_t entry_point;
    uint64_t text_offset;
    uint64_t text_size;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t bss_size;
    uint32_t alignment;
} pollen_header_t;

typedef struct {
    uintptr_t instruction_pointer;
    uintptr_t stack_pointer;
    uintptr_t image_base;
    uintptr_t image_end;
} process_context_t;

/*
 * Kernel mapping APIs: return 0 on success and nonzero on failure.
 * Successful mappings are zero-filled and occupy the requested range exactly.
 */
extern int sys_vm_map(uintptr_t virtual_address, size_t size, uint32_t flags);
extern int sys_vm_protect(uintptr_t virtual_address, size_t size, uint32_t flags);

/* Must transfer to user mode and not return on success. */
extern void sys_user_context_switch(const process_context_t *context);

/*
 * Loads a version-1 Pollen image and transfers control to its entry point.
 * Returns 0 only if the context-switch API unexpectedly returns; errors are
 * negative values.
 */
int pollen_load_and_execute(void *file_buffer, size_t buffer_size);

#endif
