#ifndef COMPAT_PE_PARSER_H
#define COMPAT_PE_PARSER_H

#include <stddef.h>
#include <stdint.h>

/* vm_alloc_at / vm_protect flags expected by this loader. */
#define VM_READ  0x01U
#define VM_WRITE 0x02U
#define VM_EXEC  0x04U

/*
 * Kernel APIs:
 * vm_alloc_at returns 0 on success and nonzero on failure.
 * Allocated memory must be zero-filled.
 * vm_protect returns 0 on success and nonzero on failure.
 */
extern int vm_alloc_at(uintptr_t virt_addr, size_t size, uint32_t flags);
extern int vm_protect(uintptr_t virt_addr, size_t size, uint32_t flags);
extern void user_mode_enter(uintptr_t entry_point, uintptr_t user_stack);

/*
 * Validate and map a bounded x86-64 PE32+ console executable.
 *
 * Returns 0 if control is transferred by user_mode_enter.
 * Returns a negative error code if validation or loading fails.
 *
 * This routine does not return after a successful user_mode_enter call.
 */
int pe_load_binary(void *pe_file_buffer, size_t file_size);

#endif
