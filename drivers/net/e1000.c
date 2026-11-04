#include "e1000.h"
#include "../net/stack.h"

#define PCI_VENDOR_DEVICE_OFFSET 0x00U
#define PCI_COMMAND_OFFSET       0x04U
#define PCI_BAR0_OFFSET          0x10U
#define PCI_COMMAND_MEMORY       0x00000002U
#define PCI_COMMAND_BUS_MASTER   0x00000004U

#define REG_CTRL     0x0000U
#define REG_STATUS   0x0008U
#define REG_EERD     0x0014U
#define REG_ICR      0x00C0U
#define REG_IMC      0x00D8U
#define REG_RCTL     0x0100U
#define REG_TCTL     0x0400U
#define REG_TIPG     0x0410U
#define REG_RDBAL    0x2800U
#define REG_RDBAH    0x2804U
#define REG_RDLEN    0x2808U
#define REG_RDH      0x2810U
#define REG_RDT      0x2818U
#define REG_TDBAL    0x3800U
#define REG_TDBAH    0x3804U
#define REG_TDLEN    0x3808U
#define REG_TDH      0x3810U
#define REG_TDT      0x3818U
#define REG_MTA      0x5200U
#define REG_RAL0     0x5400U
#define REG_RAH0     0x5404U

#define RCTL_EN       (1U << 1)
#define RCTL_SBP      (1U << 2)
#define RCTL_UPE      (1U << 3)
#define RCTL_MPE      (1U << 4)
#define RCTL_LPE      (1U << 5)
#define RCTL_BAM      (1U << 15)
#define RCTL_BSIZE_2048 0U
#define RCTL_SECRC     (1U << 26)

#define TCTL_EN       (1U << 1)
#define TCTL_PSP      (1U << 3)
#define TCTL_CT_SHIFT 4U
#define TCTL_COLD_SHIFT 12U
#define TCTL_DEFAULT  (TCTL_EN | TCTL_PSP | (0x10U << TCTL_CT_SHIFT) | \
                       (0x40U << TCTL_COLD_SHIFT))

#define TX_CMD_EOP  (1U << 0)
#define TX_CMD_IFCS (1U << 1)
#define TX_CMD_RS   (1U << 3)
#define TX_STATUS_DD (1U << 0)
#define RX_STATUS_DD (1U << 0)
#define RX_STATUS_EOP (1U << 1)

#define E1000_MMIO_SIZE 0x6000U
#define E1000_FRAME_MIN 14U
#define E1000_FRAME_MAX (E1000_BUFFER_SIZE - 4U)

_Static_assert(sizeof(e1000_rx_desc_t) == 16, "RX descriptor must be 16 bytes");
_Static_assert(sizeof(e1000_tx_desc_t) == 16, "TX descriptor must be 16 bytes");

typedef struct {
    uint8_t *virtual_address;
    uint64_t physical_address;
} dma_buffer_t;

static volatile uint8_t *mmio;
static e1000_rx_desc_t *rx_ring;
static uint64_t rx_ring_physical;
static e1000_tx_desc_t *tx_ring;
static uint64_t tx_ring_physical;
static dma_buffer_t rx_buffers[E1000_RX_DESC_COUNT];
static dma_buffer_t tx_buffers[E1000_TX_DESC_COUNT];
static uint16_t rx_next;
static uint16_t tx_next;
static int initialized;

static inline void dma_wmb(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void dma_rmb(void)
{
    __asm__ volatile("" ::: "memory");
}

static uint32_t mmio_read32(uint32_t offset)
{
    if (mmio == NULL || offset > E1000_MMIO_SIZE - sizeof(uint32_t)) {
        return 0xFFFFFFFFU;
    }

    return *(volatile uint32_t *)(void *)(mmio + offset);
}

static void mmio_write32(uint32_t offset, uint32_t value)
{
    if (mmio == NULL || offset > E1000_MMIO_SIZE - sizeof(uint32_t)) {
        return;
    }

    *(volatile uint32_t *)(void *)(mmio + offset) = value;
    __asm__ volatile("" ::: "memory");
}

static int alloc_dma_buffer(dma_buffer_t *buffer)
{
    uint64_t physical = 0;
    void *virtual_address =
        kernel_dma_alloc(E1000_BUFFER_SIZE, 128U, &physical);

    if (virtual_address == NULL || physical == 0 ||
        (physical & 127U) != 0 ||
        physical > UINT64_MAX - E1000_BUFFER_SIZE) {
        return -1;
    }

    buffer->virtual_address = (uint8_t *)virtual_address;
    buffer->physical_address = physical;
    return 0;
}

static int alloc_rings(void)
{
    uint64_t rx_physical = 0;
    uint64_t tx_physical = 0;

    void *rx_memory = kernel_dma_alloc(
        sizeof(e1000_rx_desc_t) * E1000_RX_DESC_COUNT,
        128U, &rx_physical);

    void *tx_memory = kernel_dma_alloc(
        sizeof(e1000_tx_desc_t) * E1000_TX_DESC_COUNT,
        128U, &tx_physical);

    if (rx_memory == NULL || tx_memory == NULL ||
        rx_physical == 0 || tx_physical == 0 ||
        (rx_physical & 127U) != 0 || (tx_physical & 127U) != 0 ||
        rx_physical > UINT64_MAX - sizeof(e1000_rx_desc_t) * E1000_RX_DESC_COUNT ||
        tx_physical > UINT64_MAX - sizeof(e1000_tx_desc_t) * E1000_TX_DESC_COUNT) {
        return -1;
    }

    rx_ring = (e1000_rx_desc_t *)rx_memory;
    tx_ring = (e1000_tx_desc_t *)tx_memory;
    rx_ring_physical = rx_physical;
    tx_ring_physical = tx_physical;

    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; ++i) {
        rx_ring[i].address = 0;
        rx_ring[i].length = 0;
        rx_ring[i].checksum = 0;
        rx_ring[i].status = 0;
        rx_ring[i].errors = 0;
        rx_ring[i].special = 0;

        if (alloc_dma_buffer(&rx_buffers[i]) != 0) {
            return -2;
        }
        rx_ring[i].address = rx_buffers[i].physical_address;
    }

    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; ++i) {
        tx_ring[i].address = 0;
        tx_ring[i].length = 0;
        tx_ring[i].cso = 0;
        tx_ring[i].command = 0;
        tx_ring[i].status = TX_STATUS_DD;
        tx_ring[i].css = 0;
        tx_ring[i].special = 0;

        if (alloc_dma_buffer(&tx_buffers[i]) != 0) {
            return -3;
        }
    }

    return 0;
}

static int read_mac_address(uint8_t mac[6])
{
    uint32_t low = mmio_read32(REG_RAL0);
    uint32_t high = mmio_read32(REG_RAH0);

    if (low == 0xFFFFFFFFU || high == 0xFFFFFFFFU ||
        (high & (1U << 31)) == 0) {
        return -1;
    }

    mac[0] = (uint8_t)(low);
    mac[1] = (uint8_t)(low >> 8);
    mac[2] = (uint8_t)(low >> 16);
    mac[3] = (uint8_t)(low >> 24);
    mac[4] = (uint8_t)(high);
    mac[5] = (uint8_t)(high >> 8);

    uint8_t any_nonzero = 0;
    uint8_t all_ff = 1;
    for (uint32_t i = 0; i < 6; ++i) {
        any_nonzero |= mac[i];
        all_ff &= (mac[i] == 0xFFU);
    }

    return any_nonzero != 0 && !all_ff ? 0 : -2;
}

int e1000_init(uint8_t bus, uint8_t slot, uint8_t func)
{
    initialized = 0;
    mmio = NULL;
    rx_ring = NULL;
    tx_ring = NULL;
    rx_next = 0;
    tx_next = 0;

    uint32_t identity = pci_read_config(bus, slot, func,
                                        PCI_VENDOR_DEVICE_OFFSET);
    if ((identity & 0xFFFFU) != E1000_VENDOR_ID ||
        (identity >> 16) != E1000_DEVICE_ID) {
        return -1;
    }

    uint32_t command = pci_read_config(bus, slot, func,
                                       PCI_COMMAND_OFFSET);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    /* PCI command is the low 16 bits of the dword at offset 0x04. */
    uint32_t command_status = pci_read_config(bus, slot, func,
                                               PCI_COMMAND_OFFSET);
    command_status = (command_status & 0xFFFF0000U) | (command & 0xFFFFU);
    extern void pci_write_config(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
    pci_write_config(bus, slot, func, PCI_COMMAND_OFFSET, command_status);

    uint32_t bar0 = pci_read_config(bus, slot, func, PCI_BAR0_OFFSET);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFU || (bar0 & 1U) != 0) {
        return -2;
    }

    uint64_t bar_address = (uint64_t)(bar0 & ~0x0FU);
    if (bar_address == 0 || bar_address > UINT64_MAX - E1000_MMIO_SIZE) {
        return -3;
    }

    mmio = (volatile uint8_t *)kernel_map_mmio(bar_address,
                                               E1000_MMIO_SIZE);
    if (mmio == NULL) {
        return -4;
    }

    uint32_t status = mmio_read32(REG_STATUS);
    if (status == 0xFFFFFFFFU) {
        mmio = NULL;
        return -5;
    }

    /* Reset device, wait boundedly for reset bit to clear. */
    uint32_t ctrl = mmio_read32(REG_CTRL);
    mmio_write32(REG_CTRL, ctrl | (1U << 26));

    uint32_t spin;
    for (spin = 0; spin < 1000000U; ++spin) {
        if ((mmio_read32(REG_CTRL) & (1U << 26)) == 0) {
            break;
        }
    }
    if (spin == 1000000U) {
        mmio = NULL;
        return -6;
    }

    mmio_write32(REG_IMC, 0xFFFFFFFFU);
    (void)mmio_read32(REG_ICR);

    if (alloc_rings() != 0) {
        mmio = NULL;
        return -7;
    }

    uint8_t mac[6];
    if (read_mac_address(mac) != 0) {
        mmio = NULL;
        return -8;
    }

    if (net_set_local_mac(mac) != 0) {
        mmio = NULL;
        return -9;
    }

    /* Disable receive/transmit while programming ring registers. */
    mmio_write32(REG_RCTL, 0);
    mmio_write32(REG_TCTL, 0);

    mmio_write32(REG_RDBAL, (uint32_t)rx_ring_physical);
    mmio_write32(REG_RDBAH, (uint32_t)(rx_ring_physical >> 32));
    mmio_write32(REG_RDLEN,
                 (uint32_t)(sizeof(e1000_rx_desc_t) * E1000_RX_DESC_COUNT));
    mmio_write32(REG_RDH, 0);
    mmio_write32(REG_RDT, E1000_RX_DESC_COUNT - 1U);

    mmio_write32(REG_TDBAL, (uint32_t)tx_ring_physical);
    mmio_write32(REG_TDBAH, (uint32_t)(tx_ring_physical >> 32));
    mmio_write32(REG_TDLEN,
                 (uint32_t)(sizeof(e1000_tx_desc_t) * E1000_TX_DESC_COUNT));
    mmio_write32(REG_TDH, 0);
    mmio_write32(REG_TDT, 0);

    for (uint32_t i = 0; i < 128U; ++i) {
        mmio_write32(REG_MTA + i * 4U, 0);
    }

    mmio_write32(REG_TIPG, 10U | (8U << 10) | (6U << 20));

    dma_wmb();

    /* Enable transmit and receive; accept broadcast and unicast traffic. */
    mmio_write32(REG_TCTL, TCTL_DEFAULT);
    mmio_write32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC |
                           RCTL_BSIZE_2048);

    initialized = 1;
    return 0;
}

int e1000_is_initialized(void)
{
    return initialized;
}

void ethernet_poll(void)
{
    if (!initialized || mmio == NULL || rx_ring == NULL) {
        return;
    }

    for (uint32_t processed = 0; processed < E1000_RX_DESC_COUNT; ++processed) {
        uint16_t index = rx_next;
        e1000_rx_desc_t *descriptor = &rx_ring[index];

        dma_rmb();
        if ((descriptor->status & RX_STATUS_DD) == 0) {
            break;
        }

        uint16_t length = descriptor->length;
        uint8_t status = descriptor->status;
        uint8_t errors = descriptor->errors;

        if ((status & RX_STATUS_EOP) != 0 &&
            errors == 0 &&
            length >= E1000_FRAME_MIN &&
            length <= E1000_FRAME_MAX) {
            /*
             * Synchronous parsing: the packet buffer is not retained.
             * net_parse_packet must finish reading it before returning.
             */
            net_parse_packet(rx_buffers[index].virtual_address, length);
        }

        /*
         * DD is written by hardware and is not cleared by software.
         * Re-arm by clearing descriptor fields and advancing RDT.
         */
        descriptor->length = 0;
        descriptor->checksum = 0;
        descriptor->errors = 0;
        descriptor->special = 0;
        descriptor->status = 0;

        dma_wmb();
        mmio_write32(REG_RDT, index);

        rx_next = (uint16_t)((index + 1U) % E1000_RX_DESC_COUNT);
    }
}

int ethernet_send(const void *frame, size_t length)
{
    if (!initialized || frame == NULL ||
        length < E1000_FRAME_MIN || length > E1000_FRAME_MAX) {
        return -1;
    }

    uint16_t index = tx_next;
    e1000_tx_desc_t *descriptor = &tx_ring[index];

    /* Do not overwrite a descriptor still owned by the device. */
    dma_rmb();
    if ((descriptor->status & TX_STATUS_DD) == 0) {
        return -2;
    }

    uint8_t *destination = tx_buffers[index].virtual_address;
    const uint8_t *source = (const uint8_t *)frame;

    for (size_t i = 0; i < length; ++i) {
        destination[i] = source[i];
    }

    descriptor->address = tx_buffers[index].physical_address;
    descriptor->length = (uint16_t)length;
    descriptor->cso = 0;
    descriptor->css = 0;
    descriptor->special = 0;
    descriptor->status = 0;
    descriptor->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;

    dma_wmb();

    uint16_t next = (uint16_t)((index + 1U) % E1000_TX_DESC_COUNT);
    mmio_write32(REG_TDT, next);
    tx_next = next;

    return 0;
}
