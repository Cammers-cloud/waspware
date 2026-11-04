#include "pollen.h"

#define POLLEN_HEADER_SIZE 66U
#define POLLEN_MAX_IMAGE_SIZE (512ULL * 1024ULL * 1024ULL)

static const uint8_t pollen_magic[POLLEN_MAGIC_SIZE] = {
    'P', 'O', 'L', 'L', 'E', 'N'
};

static uint16_t load_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] |
                      ((uint16_t)p[1] << 8));
}

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t load_le64(const uint8_t *p)
{
    return (uint64_t)load_le32(p) |
           ((uint64_t)load_le32(p + 4) << 32);
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b) {
        return 1;
    }
    *out = a + b;
    return 0;
}

static int align_up_u64(uint64_t value, uint64_t alignment, uint64_t *out)
{
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0) {
        return 1;
    }

    uint64_t mask = alignment - 1U;
    if (value > UINT64_MAX - mask) {
        return 1;
    }

    *out = (value + mask) & ~mask;
    return 0;
}

static int range_within(uint64_t offset, uint64_t length, uint64_t limit)
{
    return offset <= limit && length <= limit - offset;
}

static int user_range_valid(uint64_t base, uint64_t size)
{
    uint64_t end;

    if (size == 0 || base < POLLEN_USER_MIN ||
        base >= POLLEN_USER_MAX ||
        add_u64(base, size, &end)) {
        return 0;
    }

    return end <= POLLEN_USER_TOP;
}

static void copy_bytes(uint8_t *destination,
                       const uint8_t *source,
                       uint64_t length)
{
    for (uint64_t i = 0; i < length; ++i) {
        destination[i] = source[i];
    }
}

static void zero_bytes(uint8_t *destination, uint64_t length)
{
    for (uint64_t i = 0; i < length; ++i) {
        destination[i] = 0;
    }
}

int pollen_load_and_execute(void *file_buffer, size_t buffer_size)
{
    if (file_buffer == NULL || buffer_size < POLLEN_HEADER_SIZE) {
        return -1;
    }

    const uint8_t *file = (const uint8_t *)file_buffer;

    for (size_t i = 0; i < POLLEN_MAGIC_SIZE; ++i) {
        if (file[i] != pollen_magic[i]) {
            return -2;
        }
    }

    uint16_t version = load_le16(file + 6);
    uint64_t virtual_base = load_le64(file + 8);
    uint64_t entry_point = load_le64(file + 16);
    uint64_t text_offset = load_le64(file + 24);
    uint64_t text_size = load_le64(file + 32);
    uint64_t data_offset = load_le64(file + 40);
    uint64_t data_size = load_le64(file + 48);
    uint64_t bss_size = load_le64(file + 56);
    uint32_t alignment = load_le32(file + 64);

    if (version != POLLEN_VERSION || alignment != POLLEN_ALIGNMENT) {
        return -3;
    }

    if (text_size == 0 || text_offset < POLLEN_HEADER_SIZE ||
        !range_within(text_offset, text_size, (uint64_t)buffer_size) ||
        !range_within(data_offset, data_size, (uint64_t)buffer_size)) {
        return -4;
    }

    uint64_t text_file_end;
    uint64_t data_file_end;

    if (add_u64(text_offset, text_size, &text_file_end) ||
        add_u64(data_offset, data_size, &data_file_end)) {
        return -5;
    }

    /*
     * Require payload ranges to be disjoint from the header and each other.
     * Empty data payloads may use any in-bounds offset, including zero.
     */
    if (text_offset < POLLEN_HEADER_SIZE ||
        (data_size != 0 && data_offset < POLLEN_HEADER_SIZE) ||
        (data_size != 0 &&
         text_offset < data_file_end &&
         data_offset < text_file_end)) {
        return -6;
    }

    uint64_t text_mapped_size;
    uint64_t data_offset_in_image;
    uint64_t data_mapped_size;
    uint64_t bss_offset_in_image;
    uint64_t bss_mapped_size;
    uint64_t image_size;

    if (align_up_u64(text_size, POLLEN_ALIGNMENT, &text_mapped_size) ||
        align_up_u64(text_mapped_size, POLLEN_ALIGNMENT,
                     &data_offset_in_image) ||
        align_up_u64(data_size, POLLEN_ALIGNMENT, &data_mapped_size) ||
        add_u64(data_offset_in_image, data_mapped_size,
                &bss_offset_in_image) ||
        align_up_u64(bss_size, POLLEN_ALIGNMENT, &bss_mapped_size) ||
        add_u64(bss_offset_in_image, bss_mapped_size, &image_size) ||
        image_size == 0 ||
        image_size > POLLEN_MAX_IMAGE_SIZE) {
        return -7;
    }

    if (!user_range_valid(virtual_base, image_size)) {
        return -8;
    }

    uint64_t entry_rva;
    if (entry_point >= virtual_base) {
        entry_rva = entry_point - virtual_base;
    } else {
        entry_rva = entry_point;
    }

    if (entry_rva >= text_size) {
        return -9;
    }

    uint64_t entry_absolute;
    if (add_u64(virtual_base, entry_rva, &entry_absolute) ||
        entry_absolute > UINTPTR_MAX ||
        virtual_base > UINTPTR_MAX ||
        image_size > UINTPTR_MAX - (uintptr_t)virtual_base) {
        return -10;
    }

    uintptr_t text_address = (uintptr_t)virtual_base;
    uintptr_t data_address =
        (uintptr_t)(virtual_base + data_offset_in_image);
    uintptr_t bss_address =
        (uintptr_t)(virtual_base + bss_offset_in_image);

    if (text_mapped_size > SIZE_MAX ||
        data_mapped_size > SIZE_MAX ||
        bss_mapped_size > SIZE_MAX) {
        return -11;
    }

    if (sys_vm_map(text_address, (size_t)text_mapped_size,
                   POLLEN_VM_READ | POLLEN_VM_WRITE) != 0) {
        return -12;
    }

    if (data_mapped_size != 0 &&
        sys_vm_map(data_address, (size_t)data_mapped_size,
                   POLLEN_VM_READ | POLLEN_VM_WRITE) != 0) {
        return -13;
    }

    if (bss_mapped_size != 0 &&
        sys_vm_map(bss_address, (size_t)bss_mapped_size,
                   POLLEN_VM_READ | POLLEN_VM_WRITE) != 0) {
        return -14;
    }

    /*
     * The mapping contract says memory is zero-filled. Explicitly clear
     * padding and BSS as well, so behavior does not depend on payload data.
     */
    uint8_t *text_memory = (uint8_t *)text_address;
    copy_bytes(text_memory, file + text_offset, text_size);
    if (text_mapped_size > text_size) {
        zero_bytes(text_memory + text_size, text_mapped_size - text_size);
    }

    if (data_size != 0) {
        uint8_t *data_memory = (uint8_t *)data_address;
        copy_bytes(data_memory, file + data_offset, data_size);
        if (data_mapped_size > data_size) {
            zero_bytes(data_memory + data_size,
                       data_mapped_size - data_size);
        }
    }

    if (bss_mapped_size != 0) {
        zero_bytes((uint8_t *)bss_address, bss_mapped_size);
    }

    if (sys_vm_protect(text_address, (size_t)text_mapped_size,
                       POLLEN_VM_READ | POLLEN_VM_EXEC) != 0) {
        return -15;
    }

    if (data_mapped_size != 0 &&
        sys_vm_protect(data_address, (size_t)data_mapped_size,
                       POLLEN_VM_READ | POLLEN_VM_WRITE) != 0) {
        return -16;
    }

    if (bss_mapped_size != 0 &&
        sys_vm_protect(bss_address, (size_t)bss_mapped_size,
                       POLLEN_VM_READ | POLLEN_VM_WRITE) != 0) {
        return -17;
    }

    uint64_t stack_size = POLLEN_USER_STACK_SIZE;
    uint64_t stack_top;

    if (POLLEN_USER_TOP < stack_size) {
        return -18;
    }
    stack_top = POLLEN_USER_TOP;
    uint64_t stack_base = stack_top - stack_size;

    if (stack_base < POLLEN_USER_MIN ||
        stack_base < virtual_base + image_size) {
        return -19;
    }

    if (sys_vm_map((uintptr_t)stack_base, (size_t)stack_size,
                   POLLEN_VM_READ | POLLEN_VM_WRITE) != 0) {
        return -20;
    }

    uintptr_t user_stack = (uintptr_t)(stack_top & ~0xFULL);
    if (sys_vm_protect((uintptr_t)stack_base, (size_t)stack_size,
                       POLLEN_VM_READ | POLLEN_VM_WRITE) != 0) {
        return -21;
    }

    process_context_t context;
    context.instruction_pointer = (uintptr_t)entry_absolute;
    context.stack_pointer = user_stack;
    context.image_base = (uintptr_t)virtual_base;
    context.image_end = (uintptr_t)(virtual_base + image_size);

    sys_user_context_switch(&context);

    /* Successful context switching must not return. */
    return -22;
}
