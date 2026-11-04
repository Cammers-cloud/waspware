#ifndef DRIVERS_NET_E1000_H
#define DRIVERS_NET_E1000_H

#include <stddef.h>
#include <stdint.h>

#define E1000_VENDOR_ID 0x8086U
#define E1000_DEVICE_ID 0x100EU

#define E1000_RX_DESC_COUNT 128U
#define E1000_TX_DESC_COUNT 8U
#define E1000_BUFFER_SIZE 2048U

typedef struct {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct {
    uint64_t address;
    uint16_t length;
    uint8_t cso;
    uint8_t command;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} e1000_tx_desc_t;

/*
 * Kernel-provided platform interfaces.
 *
 * pci_read_config returns the requested 32-bit PCI config-space value.
 * kernel_map_mmio maps the physical BAR range as device memory and returns
 * its kernel virtual address, or NULL on failure.
 * kernel_dma_alloc returns suitably aligned, physically contiguous DMA
 * memory and writes its physical address to *physical_address, or NULL.
 */
extern uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset);
extern void *kernel_map_mmio(uint64_t physical_address, size_t length);
extern void *kernel_dma_alloc(size_t size, size_t alignment,
                              uint64_t *physical_address);

int e1000_init(uint8_t bus, uint8_t slot, uint8_t func);
void ethernet_poll(void);
int ethernet_send(const void *frame, size_t length);
int e1000_is_initialized(void);

#endif
