#include "mem.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289U
#define MULTIBOOT2_TAG_TYPE_END 0U
#define MULTIBOOT2_TAG_TYPE_MMAP 6U
#define MULTIBOOT2_MMAP_TYPE_AVAILABLE 1U

#define MAX_PHYSICAL_ADDRESS (1ULL << 30)
#define MAX_FRAME_COUNT (MAX_PHYSICAL_ADDRESS / PAGE_SIZE)
#define BITMAP_WORD_BITS 64ULL
#define BITMAP_WORD_COUNT ((MAX_FRAME_COUNT + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS)

#define KERNEL_PHYSICAL_START 0x00100000ULL
#define KERNEL_RESERVED_END   0x00200000ULL

typedef struct {
    uint32_t type;
    uint32_t size;
} multiboot_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} multiboot_tag_mmap_t;

typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} multiboot_mmap_entry_t;

static uint64_t frame_bitmap[BITMAP_WORD_COUNT];
static uint64_t available_frames;
static int allocator_ready;

static inline void compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static int bitmap_test(uint64_t frame)
{
    return (frame_bitmap[frame / BITMAP_WORD_BITS] &
            (1ULL << (frame % BITMAP_WORD_BITS))) != 0;
}

static void bitmap_set(uint64_t frame)
{
    frame_bitmap[frame / BITMAP_WORD_BITS] |=
        1ULL << (frame % BITMAP_WORD_BITS);
}

static void bitmap_clear(uint64_t frame)
{
    frame_bitmap[frame / BITMAP_WORD_BITS] &=
        ~(1ULL << (frame % BITMAP_WORD_BITS));
}

static void reserve_range(uint64_t base, uint64_t length)
{
    if (length == 0 || base >= MAX_PHYSICAL_ADDRESS) {
        return;
    }

    uint64_t end;
    if (length > MAX_PHYSICAL_ADDRESS - base) {
        end = MAX_PHYSICAL_ADDRESS;
    } else {
        end = base + length;
    }

    uint64_t first_frame = base / PAGE_SIZE;
    uint64_t last_frame = (end + PAGE_SIZE - 1) / PAGE_SIZE;

    if (last_frame > MAX_FRAME_COUNT) {
        last_frame = MAX_FRAME_COUNT;
    }

    for (uint64_t frame = first_frame; frame < last_frame; ++frame) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            if (available_frames != 0) {
                --available_frames;
            }
        }
    }
}

static void release_range(uint64_t base, uint64_t length)
{
    if (length == 0 || base >= MAX_PHYSICAL_ADDRESS) {
        return;
    }

    uint64_t end;
    if (length > MAX_PHYSICAL_ADDRESS - base) {
        end = MAX_PHYSICAL_ADDRESS;
    } else {
        end = base + length;
    }

    /* Only expose complete pages wholly contained in the available range. */
    uint64_t first_frame = (base + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t last_frame = end / PAGE_SIZE;

    if (last_frame > MAX_FRAME_COUNT) {
        last_frame = MAX_FRAME_COUNT;
    }

    for (uint64_t frame = first_frame; frame < last_frame; ++frame) {
        if (bitmap_test(frame)) {
            bitmap_clear(frame);
            ++available_frames;
        }
    }
}

static int address_range_valid(uint64_t address, uint64_t size,
                               uint64_t limit)
{
    return address <= limit && size <= limit - address;
}

int mem_init(uint32_t multiboot_magic, uint64_t multiboot_info_phys)
{
    allocator_ready = 0;
    available_frames = 0;

    if (multiboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC ||
        multiboot_info_phys == 0 ||
        multiboot_info_phys >= MAX_PHYSICAL_ADDRESS) {
        return -1;
    }

    for (uint64_t i = 0; i < BITMAP_WORD_COUNT; ++i) {
        frame_bitmap[i] = UINT64_MAX;
    }

    uint32_t total_size = *(volatile uint32_t *)(uintptr_t)multiboot_info_phys;
    if (total_size < 16 || total_size > MAX_PHYSICAL_ADDRESS - multiboot_info_phys) {
        return -2;
    }

    uint64_t info_end = multiboot_info_phys + total_size;
    uint64_t tag_address = multiboot_info_phys + 8;
    int mmap_found = 0;

    while (tag_address + sizeof(multiboot_tag_t) <= info_end) {
        const multiboot_tag_t *tag =
            (const multiboot_tag_t *)(uintptr_t)tag_address;

        if (tag->size < sizeof(multiboot_tag_t) ||
            !address_range_valid(tag_address, tag->size, info_end)) {
            return -3;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            if (tag->size < sizeof(multiboot_tag_mmap_t)) {
                return -4;
            }

            const multiboot_tag_mmap_t *mmap =
                (const multiboot_tag_mmap_t *)tag;

            if (mmap->entry_size < sizeof(multiboot_mmap_entry_t)) {
                return -5;
            }

            uint64_t entries_begin = tag_address + sizeof(*mmap);
            uint64_t entries_bytes = tag->size - sizeof(*mmap);

            if (entries_bytes % mmap->entry_size != 0) {
                return -6;
            }

            for (uint64_t offset = 0; offset < entries_bytes;
                 offset += mmap->entry_size) {
                const multiboot_mmap_entry_t *entry =
                    (const multiboot_mmap_entry_t *)(uintptr_t)
                    (entries_begin + offset);

                if (entry->type == MULTIBOOT2_MMAP_TYPE_AVAILABLE) {
                    release_range(entry->base_addr, entry->length);
                }
            }

            mmap_found = 1;
        }

        uint64_t next_tag = tag_address + tag->size;
        next_tag = (next_tag + 7ULL) & ~7ULL;

        if (next_tag <= tag_address || next_tag > info_end) {
            return -7;
        }
        tag_address = next_tag;
    }

    if (!mmap_found) {
        return -8;
    }

    /*
     * Keep low memory, the kernel's initial image/boot structures, and
     * the Multiboot2 information block unavailable to the allocator.
     * The linker/build must keep the kernel within this reserved span or
     * extend KERNEL_RESERVED_END to the actual kernel end symbol.
     */
    reserve_range(0, 0x100000);
    reserve_range(KERNEL_PHYSICAL_START,
                  KERNEL_RESERVED_END - KERNEL_PHYSICAL_START);
    reserve_range(multiboot_info_phys, total_size);

    compiler_barrier();
    allocator_ready = 1;
    return 0;
}

uint64_t page_alloc(void)
{
    if (!allocator_ready) {
        return 0;
    }

    for (uint64_t word = 0; word < BITMAP_WORD_COUNT; ++word) {
        if (frame_bitmap[word] == UINT64_MAX) {
            continue;
        }

        for (uint64_t bit = 0; bit < BITMAP_WORD_BITS; ++bit) {
            uint64_t frame = word * BITMAP_WORD_BITS + bit;
            if (frame >= MAX_FRAME_COUNT) {
                return 0;
            }

            if (!bitmap_test(frame)) {
                bitmap_set(frame);
                if (available_frames != 0) {
                    --available_frames;
                }
                return frame * PAGE_SIZE;
            }
        }
    }

    return 0;
}

int page_free(uint64_t physical_address)
{
    if (!allocator_ready ||
        (physical_address & (PAGE_SIZE - 1)) != 0 ||
        physical_address >= MAX_PHYSICAL_ADDRESS) {
        return -1;
    }

    uint64_t frame = physical_address / PAGE_SIZE;
    if (!bitmap_test(frame)) {
        return -2;
    }

    bitmap_clear(frame);
    ++available_frames;
    return 0;
}

void *phys_to_virt(uint64_t physical_address)
{
    if (physical_address >= MAX_PHYSICAL_ADDRESS) {
        return (void *)0;
    }

    return (void *)(uintptr_t)physical_address;
}

uint64_t page_free_count(void)
{
    return allocator_ready ? available_frames : 0;
}
