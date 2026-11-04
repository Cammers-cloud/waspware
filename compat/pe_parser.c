#include "pe_parser.h"

#define PE_DOS_SIGNATURE 0x5A4DU
#define PE_SIGNATURE     0x00004550U
#define PE_MACHINE_AMD64 0x8664U
#define PE_OPTIONAL_MAGIC_PE32_PLUS 0x020BU

#define PE_FILE_RELOCS_STRIPPED 0x0001U
#define PE_FILE_EXECUTABLE_IMAGE 0x0002U
#define PE_FILE_DLL 0x2000U

#define PE_SECTION_MEM_EXECUTE 0x20000000U
#define PE_SECTION_MEM_READ    0x40000000U
#define PE_SECTION_MEM_WRITE   0x80000000U

#define PE_DIRECTORY_BASERELOC 5U
#define PE_REL_BASED_ABSOLUTE   0U
#define PE_REL_BASED_DIR64      10U

#define PE_PAGE_SIZE 4096U
#define PE_MAX_SECTIONS 96U
#define PE_MAX_IMAGE_SIZE (512U * 1024U * 1024U)
#define PE_MAX_HEADERS_SIZE (16U * 1024U * 1024U)
#define PE_MAX_RELOC_BLOCKS 65536U

typedef struct {
    uint16_t magic;
    uint16_t bytes_on_last_page;
    uint16_t pages_in_file;
    uint16_t relocation_count;
    uint16_t header_paragraphs;
    uint16_t minimum_extra_paragraphs;
    uint16_t maximum_extra_paragraphs;
    uint16_t initial_ss;
    uint16_t initial_sp;
    uint16_t checksum;
    uint16_t initial_ip;
    uint16_t initial_cs;
    uint16_t relocation_table_offset;
    uint16_t overlay_number;
    uint16_t reserved1[4];
    uint16_t oem_id;
    uint16_t oem_info;
    uint16_t reserved2[10];
    uint32_t new_header_offset;
} pe_dos_header_t;

typedef struct {
    uint32_t signature;
    uint16_t machine;
    uint16_t number_of_sections;
    uint32_t time_date_stamp;
    uint32_t pointer_to_symbol_table;
    uint32_t number_of_symbols;
    uint16_t size_of_optional_header;
    uint16_t characteristics;
} pe_file_header_t;

typedef struct {
    uint32_t virtual_address;
    uint32_t size;
} pe_data_directory_t;

typedef struct {
    uint16_t magic;
    uint8_t major_linker_version;
    uint8_t minor_linker_version;
    uint32_t size_of_code;
    uint32_t size_of_initialized_data;
    uint32_t size_of_uninitialized_data;
    uint32_t address_of_entry_point;
    uint32_t base_of_code;
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_operating_system_version;
    uint16_t minor_operating_system_version;
    uint16_t major_image_version;
    uint16_t minor_image_version;
    uint16_t major_subsystem_version;
    uint16_t minor_subsystem_version;
    uint32_t win32_version_value;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t size_of_stack_reserve;
    uint64_t size_of_stack_commit;
    uint64_t size_of_heap_reserve;
    uint64_t size_of_heap_commit;
    uint32_t loader_flags;
    uint32_t number_of_rva_and_sizes;
    pe_data_directory_t directories[16];
} pe_optional_header64_t;

typedef struct {
    uint8_t name[8];
    union {
        uint32_t physical_address;
        uint32_t virtual_size;
    } misc;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_line_numbers;
    uint16_t number_of_relocations;
    uint16_t number_of_line_numbers;
    uint32_t characteristics;
} pe_section_header_t;

typedef struct {
    uint32_t page_rva;
    uint32_t block_size;
} pe_base_relocation_block_t;

_Static_assert(sizeof(pe_dos_header_t) == 64, "Invalid DOS header layout");
_Static_assert(sizeof(pe_file_header_t) == 20, "Invalid PE file header layout");
_Static_assert(sizeof(pe_section_header_t) == 40, "Invalid PE section layout");

static int range_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    if (a > UINT64_MAX - b) {
        return 1;
    }
    *result = a + b;
    return 0;
}

static int align_up_u64(uint64_t value, uint64_t alignment, uint64_t *result)
{
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0) {
        return 1;
    }

    uint64_t mask = alignment - 1U;
    if (value > UINT64_MAX - mask) {
        return 1;
    }

    *result = (value + mask) & ~mask;
    return 0;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) |
           ((uint64_t)read_u32(p + 4) << 32);
}

static void write_u64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        p[i] = (uint8_t)(value >> (i * 8U));
    }
}

static int has_unsupported_directories(const pe_optional_header64_t *optional)
{
    /*
     * This minimal loader does not resolve imports, TLS callbacks, load
     * configuration, delay imports, or other directory-based features.
     * Base relocation is supported only as a bounded DIR64 relocation pass.
     */
    for (uint32_t i = 0; i < optional->number_of_rva_and_sizes &&
                         i < 16U; ++i) {
        if (i == PE_DIRECTORY_BASERELOC) {
            continue;
        }
        if (optional->directories[i].virtual_address != 0 ||
            optional->directories[i].size != 0) {
            return 1;
        }
    }
    return 0;
}

static uint32_t section_vm_flags(uint32_t characteristics)
{
    uint32_t flags = 0;

    if (characteristics & PE_SECTION_MEM_READ) {
        flags |= VM_READ;
    }
    if (characteristics & PE_SECTION_MEM_WRITE) {
        flags |= VM_WRITE;
    }
    if (characteristics & PE_SECTION_MEM_EXECUTE) {
        flags |= VM_EXEC;
    }

    /* A mapped section must be readable for ordinary x86 instruction/data use. */
    if (flags == 0) {
        flags = VM_READ;
    }

    return flags;
}

static int map_sections(uint8_t *file,
                        size_t file_size,
                        uint8_t *image,
                        uint64_t image_size,
                        uint32_t section_alignment,
                        uint32_t file_alignment,
                        const pe_section_header_t *sections,
                        uint16_t section_count)
{
    for (uint16_t i = 0; i < section_count; ++i) {
        const pe_section_header_t *section = &sections[i];
        uint64_t virtual_size = section->misc.virtual_size;
        uint64_t mapped_size = virtual_size;

        if (mapped_size < section->size_of_raw_data) {
            mapped_size = section->size_of_raw_data;
        }

        if (mapped_size == 0) {
            continue;
        }

        if (section->virtual_address % section_alignment != 0 ||
            section->virtual_address < PE_PAGE_SIZE ||
            section->virtual_address >= image_size ||
            mapped_size > image_size - section->virtual_address) {
            return -20;
        }

        if (section->size_of_raw_data != 0) {
            if (section->pointer_to_raw_data % file_alignment != 0 ||
                !range_valid(section->pointer_to_raw_data,
                             section->size_of_raw_data, file_size)) {
                return -21;
            }

            for (uint64_t j = 0; j < section->size_of_raw_data; ++j) {
                image[section->virtual_address + j] =
                    file[section->pointer_to_raw_data + j];
            }
        }

        /*
         * vm_alloc_at is required to provide zero-filled memory. Any tail
         * between raw-data size and virtual size is therefore already BSS.
         */
    }

    return 0;
}

static int apply_relocations(uint8_t *image,
                             uint64_t image_size,
                             uint32_t relocation_rva,
                             uint32_t relocation_size,
                             int64_t delta)
{
    if (delta == 0) {
        return 0;
    }

    if (relocation_rva == 0 || relocation_size < sizeof(pe_base_relocation_block_t) ||
        relocation_rva >= image_size ||
        relocation_size > image_size - relocation_rva) {
        return -30;
    }

    uint64_t cursor = relocation_rva;
    uint64_t end = (uint64_t)relocation_rva + relocation_size;
    uint32_t blocks_seen = 0;

    while (cursor < end) {
        if (++blocks_seen > PE_MAX_RELOC_BLOCKS ||
            end - cursor < sizeof(pe_base_relocation_block_t)) {
            return -31;
        }

        const uint8_t *block_bytes = image + cursor;
        uint32_t page_rva = read_u32(block_bytes);
        uint32_t block_size = read_u32(block_bytes + 4);

        if (block_size < sizeof(pe_base_relocation_block_t) ||
            block_size > end - cursor ||
            ((block_size - sizeof(pe_base_relocation_block_t)) & 1U) != 0) {
            return -32;
        }

        uint32_t entry_count =
            (block_size - (uint32_t)sizeof(pe_base_relocation_block_t)) / 2U;

        for (uint32_t i = 0; i < entry_count; ++i) {
            uint16_t item = read_u16(block_bytes + 8U + (uint64_t)i * 2U);
            uint16_t type = item >> 12;
            uint16_t offset = item & 0x0FFFU;

            if (type == PE_REL_BASED_ABSOLUTE) {
                continue;
            }
            if (type != PE_REL_BASED_DIR64) {
                return -33;
            }

            uint64_t target_rva = (uint64_t)page_rva + offset;
            if (target_rva > image_size ||
                sizeof(uint64_t) > image_size - target_rva) {
                return -34;
            }

            uint64_t original = read_u64(image + target_rva);
            uint64_t adjusted;

            if (delta >= 0) {
                if (add_overflow_u64(original, (uint64_t)delta, &adjusted)) {
                    return -35;
                }
            } else {
                uint64_t magnitude = (uint64_t)(-(delta + 1)) + 1U;
                if (original < magnitude) {
                    return -36;
                }
                adjusted = original - magnitude;
            }

            write_u64(image + target_rva, adjusted);
        }

        cursor += block_size;
    }

    return cursor == end ? 0 : -37;
}

static int protect_sections(uintptr_t base,
                            uint64_t image_size,
                            const pe_section_header_t *sections,
                            uint16_t section_count)
{
    for (uint16_t i = 0; i < section_count; ++i) {
        const pe_section_header_t *section = &sections[i];
        uint64_t content_size = section->misc.virtual_size;

        if (content_size < section->size_of_raw_data) {
            content_size = section->size_of_raw_data;
        }
        if (content_size == 0) {
            continue;
        }

        uint64_t begin = section->virtual_address & ~(uint64_t)(PE_PAGE_SIZE - 1U);
        uint64_t end_unaligned;

        if (add_overflow_u64(section->virtual_address, content_size,
                             &end_unaligned) ||
            align_up_u64(end_unaligned, PE_PAGE_SIZE, &end_unaligned) ||
            end_unaligned > image_size ||
            begin >= end_unaligned ||
            base > UINTPTR_MAX - begin) {
            return -40;
        }

        uintptr_t address = base + (uintptr_t)begin;
        size_t size = (size_t)(end_unaligned - begin);

        if (vm_protect(address, size,
                       section_vm_flags(section->characteristics)) != 0) {
            return -41;
        }
    }

    return 0;
}

int pe_load_binary(void *pe_file_buffer, size_t file_size)
{
    if (pe_file_buffer == NULL ||
        file_size < sizeof(pe_dos_header_t) ||
        file_size > SIZE_MAX - 1U) {
        return -1;
    }

    uint8_t *file = (uint8_t *)pe_file_buffer;

    if (read_u16(file) != PE_DOS_SIGNATURE) {
        return -2;
    }

    uint32_t pe_offset = read_u32(file + 0x3CU);
    if (pe_offset > file_size ||
        !range_valid(pe_offset, sizeof(pe_file_header_t), file_size)) {
        return -3;
    }

    if (read_u32(file + pe_offset) != PE_SIGNATURE) {
        return -4;
    }

    const uint8_t *file_header = file + pe_offset + 4U;
    uint16_t machine = read_u16(file_header);
    uint16_t section_count = read_u16(file_header + 2U);
    uint16_t optional_size = read_u16(file_header + 16U);
    uint16_t characteristics = read_u16(file_header + 18U);

    if (machine != PE_MACHINE_AMD64 ||
        section_count == 0 ||
        section_count > PE_MAX_SECTIONS ||
        (characteristics & PE_FILE_EXECUTABLE_IMAGE) == 0 ||
        (characteristics & PE_FILE_DLL) != 0) {
        return -5;
    }

    size_t optional_offset = (size_t)pe_offset + 4U +
                             sizeof(pe_file_header_t);

    if (optional_size < sizeof(pe_optional_header64_t) ||
        !range_valid(optional_offset, optional_size, file_size)) {
        return -6;
    }

    const uint8_t *optional_bytes = file + optional_offset;
    if (read_u16(optional_bytes) != PE_OPTIONAL_MAGIC_PE32_PLUS) {
        return -7;
    }

    /*
     * Copy fields into a local aligned object rather than dereferencing a
     * potentially unaligned file-buffer pointer.
     */
    pe_optional_header64_t optional;
    const size_t copied_optional =
        optional_size < sizeof(optional) ? optional_size : sizeof(optional);

    for (size_t i = 0; i < sizeof(optional); ++i) {
        ((uint8_t *)&optional)[i] = 0;
    }
    for (size_t i = 0; i < copied_optional; ++i) {
        ((uint8_t *)&optional)[i] = optional_bytes[i];
    }

    if (optional.subsystem != 3U ||
        optional.image_base == 0 ||
        optional.size_of_image == 0 ||
        optional.size_of_image > PE_MAX_IMAGE_SIZE ||
        optional.size_of_headers == 0 ||
        optional.size_of_headers > PE_MAX_HEADERS_SIZE ||
        optional.size_of_headers > file_size ||
        optional.size_of_headers > optional.size_of_image ||
        optional.number_of_rva_and_sizes > 16U ||
        optional.address_of_entry_point == 0 ||
        optional.address_of_entry_point >= optional.size_of_image) {
        return -8;
    }

    uint32_t section_alignment = optional.section_alignment;
    uint32_t file_alignment = optional.file_alignment;

    if (section_alignment < PE_PAGE_SIZE ||
        (section_alignment & (section_alignment - 1U)) != 0 ||
        file_alignment == 0 ||
        (file_alignment & (file_alignment - 1U)) != 0 ||
        file_alignment > section_alignment) {
        return -9;
    }

    if (has_unsupported_directories(&optional)) {
        return -10;
    }

    uint64_t section_table_offset =
        (uint64_t)optional_offset + optional_size;
    uint64_t section_table_bytes =
        (uint64_t)section_count * sizeof(pe_section_header_t);

    if (section_table_offset > file_size ||
        section_table_bytes > file_size - section_table_offset ||
        section_table_offset + section_table_bytes >
            optional_offset + optional.size_of_headers) {
        return -11;
    }

    const pe_section_header_t *sections =
        (const pe_section_header_t *)(const void *)(file + section_table_offset);

    /*
     * Validate section ranges and reject overlapping virtual sections.
     * Section headers are read as bytes below to avoid unaligned access.
     */
    pe_section_header_t local_sections[PE_MAX_SECTIONS];

    for (uint16_t i = 0; i < section_count; ++i) {
        const uint8_t *source = file + section_table_offset +
                                (uint64_t)i * sizeof(pe_section_header_t);
        uint8_t *destination = (uint8_t *)&local_sections[i];

        for (size_t j = 0; j < sizeof(pe_section_header_t); ++j) {
            destination[j] = source[j];
        }

        if (local_sections[i].size_of_raw_data != 0 &&
            !range_valid(local_sections[i].pointer_to_raw_data,
                         local_sections[i].size_of_raw_data,
                         file_size)) {
            return -12;
        }

        uint64_t span = local_sections[i].misc.virtual_size;
        if (span < local_sections[i].size_of_raw_data) {
            span = local_sections[i].size_of_raw_data;
        }

        if (span != 0) {
            uint64_t end;
            if (local_sections[i].virtual_address < PE_PAGE_SIZE ||
                local_sections[i].virtual_address % section_alignment != 0 ||
                add_overflow_u64(local_sections[i].virtual_address, span, &end) ||
                end > optional.size_of_image) {
                return -13;
            }
        }

        for (uint16_t j = 0; j < i; ++j) {
            uint64_t a_size = local_sections[i].misc.virtual_size;
            if (a_size < local_sections[i].size_of_raw_data) {
                a_size = local_sections[i].size_of_raw_data;
            }
            uint64_t b_size = local_sections[j].misc.virtual_size;
            if (b_size < local_sections[j].size_of_raw_data) {
                b_size = local_sections[j].size_of_raw_data;
            }

            if (a_size == 0 || b_size == 0) {
                continue;
            }

            uint64_t a_begin = local_sections[i].virtual_address;
            uint64_t b_begin = local_sections[j].virtual_address;
            uint64_t a_end = a_begin + a_size;
            uint64_t b_end = b_begin + b_size;

            if (a_begin < b_end && b_begin < a_end) {
                return -14;
            }
        }
    }

    (void)sections;

    uint64_t image_allocation_size;
    if (align_up_u64(optional.size_of_image, PE_PAGE_SIZE,
                     &image_allocation_size) ||
        image_allocation_size > SIZE_MAX ||
        optional.image_base > UINTPTR_MAX ||
        optional.image_base > UINTPTR_MAX - image_allocation_size) {
        return -15;
    }

    uintptr_t allocated_base = (uintptr_t)optional.image_base;

    /*
     * No alternate-address allocator was supplied. If the preferred base
     * cannot be mapped, fail rather than guessing an address or overwriting
     * another mapping.
     */
    if (vm_alloc_at(allocated_base, (size_t)image_allocation_size,
                    VM_READ | VM_WRITE) != 0) {
        return -16;
    }

    uint8_t *image = (uint8_t *)allocated_base;

    /* Copy headers into the zero-filled allocation. */
    for (uint32_t i = 0; i < optional.size_of_headers; ++i) {
        image[i] = file[i];
    }

    int result = map_sections(file, file_size, image,
                              optional.size_of_image,
                              section_alignment, file_alignment,
                              local_sections, section_count);
    if (result != 0) {
        return result;
    }

    /*
     * Preferred-base allocation means delta is zero. If the caller's VM
     * implementation later adds alternate-base allocation, relocation
     * handling is available here, but this API set cannot request that base.
     */
    int64_t delta = (int64_t)((uint64_t)allocated_base - optional.image_base);

    if (delta != 0) {
        if (characteristics & PE_FILE_RELOCS_STRIPPED) {
            return -17;
        }

        pe_data_directory_t reloc = optional.directories[PE_DIRECTORY_BASERELOC];
        result = apply_relocations(image, optional.size_of_image,
                                   reloc.virtual_address, reloc.size, delta);
        if (result != 0) {
            return result;
        }
    }

    result = protect_sections(allocated_base, image_allocation_size,
                              local_sections, section_count);
    if (result != 0) {
        return result;
    }

    uint64_t entry;
    if (add_overflow_u64(allocated_base,
                         optional.address_of_entry_point, &entry) ||
        entry > UINTPTR_MAX) {
        return -18;
    }

    /*
     * Stack allocation is not among the supplied APIs. Passing zero delegates
     * stack setup to user_mode_enter, which must establish a valid user stack.
     */
    user_mode_enter((uintptr_t)entry, 0);

    /* A correct user_mode_enter must not return. */
    return -19;
}
