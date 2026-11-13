#include <stdint.h>
#include "kernel.h"
#include "acpi.h"
#include "ata_pio.h"

static uint8_t shiftdown = 0;

static int kernel_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b)
    {
        a++;
        b++;
    }
    return (unsigned char)(*a) - (unsigned char)(*b);
}

void demo_read_write()
{
    uint8_t sector[512];

    // Lese LBA 0 (MBR) vom Primary Master:
    read_sectors_pio28(0x1F0, 0, 0, 1, sector);

    // (Optional) schreib es zurück:
    write_sectors_pio28(0x1F0, 0, 0, 1, sector);
}

// ============================================================
// Minimales Dateisystem fuer Notes/Explorer, direkt auf dem
// vorhandenen ATA-PIO-Treiber aufgebaut. Kein echtes Dateisystem
// (kein FAT/ext), sondern ein fest zugeordnetes Slot-Schema:
//
//   LBA 100          : Verzeichnis-Sektor (Namen + Laengen)
//   LBA 101..101+31  : Datei-Slot 0 (4 Sektoren = 2048 Bytes)
//   LBA 105..105+31  : Datei-Slot 1
//   ...
//
// Das ist bewusst einfach gehalten und geht davon aus, dass diese
// LBAs frei sind (kein Partitionsschema vorhanden). Fuer ein
// hobby-OS in diesem Entwicklungsstand ausreichend.
// ============================================================

#define FS_MAX_FILES 10
#define FS_DIR_LBA 100
#define FS_DATA_START_LBA 101
#define FS_SECTORS_PER_FILE 4 // 4 * 512 = 2048 Bytes/Datei
#define FS_MAX_FILE_SIZE (FS_SECTORS_PER_FILE * 512)
#define FS_NAME_LEN 15
#define CONFIG_LBA 99

typedef struct
{
    uint8_t used;
    char name[FS_NAME_LEN];
    uint32_t length;
} __attribute__((packed)) fs_entry_t;

static void fs_read_dir(fs_entry_t entries[FS_MAX_FILES])
{
    uint8_t sector[512];
    read_sectors_pio28(0x1F0, 0, FS_DIR_LBA, 1, sector);
    for (int i = 0; i < FS_MAX_FILES; i++)
    {
        uint8_t *src = sector + (uint32_t)i * sizeof(fs_entry_t);
        uint8_t *dst = (uint8_t *)&entries[i];
        for (uint32_t b = 0; b < sizeof(fs_entry_t); b++)
            dst[b] = src[b];
    }
}

static void fs_write_dir(fs_entry_t entries[FS_MAX_FILES])
{
    uint8_t sector[512];
    for (uint32_t i = 0; i < 512; i++)
        sector[i] = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
    {
        uint8_t *dst = sector + (uint32_t)i * sizeof(fs_entry_t);
        uint8_t *src = (uint8_t *)&entries[i];
        for (uint32_t b = 0; b < sizeof(fs_entry_t); b++)
            dst[b] = src[b];
    }
    write_sectors_pio28(0x1F0, 0, FS_DIR_LBA, 1, sector);
}

static void fs_save_file(int idx, const char *name, const char *data, uint32_t length)
{
    if (idx < 0 || idx >= FS_MAX_FILES)
        return;
    if (length > FS_MAX_FILE_SIZE)
        length = FS_MAX_FILE_SIZE;

    fs_entry_t entries[FS_MAX_FILES];
    fs_read_dir(entries);

    entries[idx].used = 1;
    uint32_t i = 0;
    for (; i < FS_NAME_LEN - 1 && name[i]; i++)
    {
        entries[idx].name[i] = name[i];
    }
    for (; i < FS_NAME_LEN; i++)
    {
        entries[idx].name[i] = '\0';
    }
    entries[idx].length = length;
    fs_write_dir(entries);

    uint32_t base_lba = FS_DATA_START_LBA + (uint32_t)idx * FS_SECTORS_PER_FILE;
    uint8_t sector[512];
    uint32_t written = 0;
    for (uint32_t s = 0; s < FS_SECTORS_PER_FILE; s++)
    {
        for (uint32_t b = 0; b < 512; b++)
        {
            sector[b] = (written < length) ? (uint8_t)data[written] : 0;
            if (written < length)
                written++;
        }
        write_sectors_pio28(0x1F0, 0, base_lba + s, 1, sector);
    }
}

static uint32_t fs_load_file(int idx, char *buf, uint32_t maxlen)
{
    if (idx < 0 || idx >= FS_MAX_FILES)
        return 0;

    fs_entry_t entries[FS_MAX_FILES];
    fs_read_dir(entries);
    if (!entries[idx].used)
        return 0;

    uint32_t length = entries[idx].length;
    if (maxlen > 0 && length > maxlen - 1)
        length = maxlen - 1;

    uint32_t base_lba = FS_DATA_START_LBA + (uint32_t)idx * FS_SECTORS_PER_FILE;
    uint8_t sector[512];
    uint32_t read_total = 0;
    for (uint32_t s = 0; s < FS_SECTORS_PER_FILE && read_total < length; s++)
    {
        read_sectors_pio28(0x1F0, 0, base_lba + s, 1, sector);
        for (uint32_t b = 0; b < 512 && read_total < length; b++)
        {
            buf[read_total++] = (char)sector[b];
        }
    }
    if (maxlen > 0)
        buf[read_total] = '\0';
    return read_total;
}

static void fs_delete_file(int idx)
{
    if (idx < 0 || idx >= FS_MAX_FILES)
        return;
    fs_entry_t entries[FS_MAX_FILES];
    fs_read_dir(entries);
    entries[idx].used = 0;
    entries[idx].name[0] = '\0';
    entries[idx].length = 0;
    fs_write_dir(entries);
}

static char username[16] = "admin";
static char password[16] = "1234";

static int network_connected = 0;
static int security_enabled = 1;
static int installed_app_count = 0;
static char installed_apps[8][16];
static char blocked_domains[4][16];
static int blocked_domain_count = 0;
static int firewall_mode = 1;

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
    {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

typedef struct
{
    char username[16];
    char password[16];
} credentials_config_t;

static void copy_string(char *dst, uint32_t dst_size, const char *src)
{
    uint32_t i = 0;
    if (dst_size == 0)
        return;

    while (i + 1 < dst_size && src[i] != '\0')
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void kernel_memcpy(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        dst[i] = src[i];
    }
}

static uint16_t kernel_checksum16(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i += 2)
    {
        uint16_t word = (uint16_t)data[i] << 8;
        if (i + 1 < len)
            word |= data[i + 1];
        sum += word;
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

static uint32_t ipv4_from_string(const char *s)
{
    uint32_t octets[4] = {0, 0, 0, 0};
    uint32_t idx = 0;
    uint32_t value = 0;
    while (*s && idx < 4)
    {
        if (*s == '.')
        {
            octets[idx++] = value;
            value = 0;
        }
        else if (*s >= '0' && *s <= '9')
        {
            value = value * 10 + (uint32_t)(*s - '0');
        }
        else
        {
            break;
        }
        s++;
    }
    octets[idx] = value;
    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

static uint32_t ipv4_from_name(const char *name)
{
    if (kernel_strcmp(name, "google") == 0 || kernel_strcmp(name, "google.com") == 0)
    {
        return 0x08080808u;
    }
    if (kernel_strcmp(name, "example") == 0 || kernel_strcmp(name, "example.com") == 0)
    {
        return 0x5DB8D822u;
    }
    return ipv4_from_string(name);
}

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_BAR0 0x10

#define E1000_REG_CTRL 0x00000
#define E1000_REG_STATUS 0x00008
#define E1000_REG_TDBAL 0x03800
#define E1000_REG_TDBAH 0x03804
#define E1000_REG_TDLEN 0x03808
#define E1000_REG_TDT 0x03810
#define E1000_REG_TCTL 0x00400
#define E1000_REG_TIPG 0x00410
#define E1000_REG_RDBAL 0x02800
#define E1000_REG_RDBAH 0x02804
#define E1000_REG_RDLEN 0x02808
#define E1000_REG_RDH 0x02810
#define E1000_REG_RDT 0x02818
#define E1000_REG_RCTL 0x02400
#define E1000_REG_RA 0x05400
#define E1000_REG_IMS 0x000D0
#define E1000_REG_ICR 0x000C0
#define E1000_REG_RAL 0x05400
#define E1000_REG_RAH 0x05404

#define E1000_TCTL_EN 0x00000002
#define E1000_TCTL_PSP 0x00000008
#define E1000_TCTL_CT 0x00000C00
#define E1000_TCTL_COLD 0x00030000
#define E1000_TCTL_SWXOFF 0x00400000
#define E1000_TCTL_RTLC 0x01000000

#define E1000_RCTL_EN 0x00000002
#define E1000_RCTL_SBP 0x00000004
#define E1000_RCTL_UPE 0x00000008
#define E1000_RCTL_MPE 0x00000010
#define E1000_RCTL_LPE 0x00000020
#define E1000_RCTL_BAM 0x00008000
#define E1000_RCTL_SECRC 0x04000000

#define E1000_TXD_CMD_EOP 0x01
#define E1000_TXD_CMD_IFCS 0x02
#define E1000_TXD_CMD_RS 0x08
#define E1000_TXD_STAT_DD 0x01

#define E1000_RXD_STAT_DD 0x01
#define E1000_RXD_STAT_EOP 0x02

#define E1000_TX_RING_SIZE 8
#define E1000_RX_RING_SIZE 8

typedef struct
{
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct
{
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct
{
    uint8_t dest[6];
    uint8_t src[6];
    uint16_t ethertype;
} __attribute__((packed)) ethernet_header_t;

typedef struct
{
    uint16_t hwtype;
    uint16_t protocol;
    uint8_t hwlen;
    uint8_t protolen;
    uint16_t opcode;
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} __attribute__((packed)) arp_packet_t;

typedef struct
{
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ipv4_header_t;

typedef struct
{
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed)) icmp_echo_t;

typedef struct
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_offset_flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

typedef struct
{
    uint8_t *data;
    uint32_t length;
    uint32_t offset;
} http_response_t;

static volatile uint32_t *e1000_mmio = 0;
static uint8_t e1000_mac[6] = {0};
static uint8_t e1000_gateway_mac[6] = {0};
static uint32_t e1000_local_ip = 0x0A00020Fu;
static uint32_t e1000_gateway_ip = 0x0A000202u;
static uint8_t e1000_tx_buffers[E1000_TX_RING_SIZE][2048];
static uint8_t e1000_rx_buffers[E1000_RX_RING_SIZE][2048];
static e1000_tx_desc_t e1000_tx_descs[E1000_TX_RING_SIZE];
static e1000_rx_desc_t e1000_rx_descs[E1000_RX_RING_SIZE];
static uint32_t e1000_tx_index = 0;
static uint32_t e1000_rx_index = 0;
static int e1000_ready = 0;

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t address = (uint32_t)(0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)device << 11) | ((uint32_t)function << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value)
{
    uint32_t address = (uint32_t)(0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)device << 11) | ((uint32_t)function << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static int e1000_probe(void)
{
    // bus war vorher uint8_t (Maximalwert 255) - "bus < 256" war dadurch
    // IMMER wahr, die Schleife lief also endlos weiter (bus lief nach 255
    // wieder auf 0 zurueck, statt die Schleife zu verlassen). Das war der
    // Grund, warum "net connect" den ganzen Kernel eingefroren hat.
    for (uint16_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t device = 0; device < 32; device++)
        {
            uint16_t vendor = (uint16_t)(pci_config_read32((uint8_t)bus, device, 0, PCI_VENDOR_ID) & 0xFFFF);
            if (vendor == 0xFFFF)
                continue;
            uint16_t device_id = (uint16_t)((pci_config_read32((uint8_t)bus, device, 0, PCI_DEVICE_ID) >> 16) & 0xFFFF);
            if (((vendor == 0x8086) && (device_id == 0x100E || device_id == 0x1000)) || (vendor == 0x8086 && device_id == 0x100F))
            {
                uint32_t bar0 = pci_config_read32((uint8_t)bus, device, 0, PCI_BAR0);
                uint32_t cmd = pci_config_read32((uint8_t)bus, device, 0, PCI_COMMAND);
                pci_config_write32((uint8_t)bus, device, 0, PCI_COMMAND, cmd | 0x00000007u);
                if ((bar0 & 0x1u) == 0)
                {
                    e1000_mmio = (volatile uint32_t *)(uintptr_t)(bar0 & 0xFFFFFFF0u);
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void e1000_write_reg(uint32_t reg, uint32_t value)
{
    if (!e1000_mmio)
        return;
    e1000_mmio[reg >> 2] = value;
}

static uint32_t e1000_read_reg(uint32_t reg)
{
    if (!e1000_mmio)
        return 0;
    return e1000_mmio[reg >> 2];
}

static void e1000_setup_tx_rx(void)
{
    for (uint32_t i = 0; i < E1000_TX_RING_SIZE; i++)
    {
        e1000_tx_descs[i].buffer_addr = (uint64_t)(uintptr_t)e1000_tx_buffers[i];
        e1000_tx_descs[i].length = 0;
        e1000_tx_descs[i].cmd = 0;
        e1000_tx_descs[i].status = 0;
    }
    for (uint32_t i = 0; i < E1000_RX_RING_SIZE; i++)
    {
        e1000_rx_descs[i].buffer_addr = (uint64_t)(uintptr_t)e1000_rx_buffers[i];
        e1000_rx_descs[i].length = 0;
        e1000_rx_descs[i].status = 0;
    }

    e1000_write_reg(E1000_REG_TDBAL, (uint32_t)(uintptr_t)e1000_tx_descs);
    e1000_write_reg(E1000_REG_TDBAH, (uint32_t)((uintptr_t)e1000_tx_descs >> 32));
    e1000_write_reg(E1000_REG_TDLEN, E1000_TX_RING_SIZE * sizeof(e1000_tx_desc_t));
    e1000_write_reg(E1000_REG_TDT, 0);
    e1000_write_reg(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);
    e1000_write_reg(E1000_REG_TIPG, 0x0A0A0A0Au);

    e1000_write_reg(E1000_REG_RDBAL, (uint32_t)(uintptr_t)e1000_rx_descs);
    e1000_write_reg(E1000_REG_RDBAH, (uint32_t)((uintptr_t)e1000_rx_descs >> 32));
    e1000_write_reg(E1000_REG_RDLEN, E1000_RX_RING_SIZE * sizeof(e1000_rx_desc_t));
    e1000_write_reg(E1000_REG_RDH, 0);
    e1000_write_reg(E1000_REG_RDT, E1000_RX_RING_SIZE - 1);
    e1000_write_reg(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
}

static void e1000_read_mac(void)
{
    uint32_t low = e1000_read_reg(E1000_REG_RAL);
    uint32_t high = e1000_read_reg(E1000_REG_RAH);
    e1000_mac[0] = (uint8_t)(low & 0xFF);
    e1000_mac[1] = (uint8_t)((low >> 8) & 0xFF);
    e1000_mac[2] = (uint8_t)((low >> 16) & 0xFF);
    e1000_mac[3] = (uint8_t)((low >> 24) & 0xFF);
    e1000_mac[4] = (uint8_t)(high & 0xFF);
    e1000_mac[5] = (uint8_t)((high >> 8) & 0xFF);
}

static void e1000_send_frame(uint8_t *frame, uint32_t len)
{
    if (!e1000_ready || !e1000_mmio)
        return;
    uint32_t idx = e1000_tx_index;
    kernel_memcpy(e1000_tx_buffers[idx], frame, len);
    e1000_tx_descs[idx].length = (uint16_t)len;
    e1000_tx_descs[idx].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    e1000_tx_descs[idx].status = 0;
    e1000_write_reg(E1000_REG_TDT, idx + 1);
    e1000_tx_index = (e1000_tx_index + 1) % E1000_TX_RING_SIZE;
}

static void e1000_send_arp(uint32_t target_ip, uint16_t op)
{
    uint8_t frame[256];
    ethernet_header_t *eth = (ethernet_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + sizeof(ethernet_header_t));

    for (int i = 0; i < 6; i++)
    {
        eth->dest[i] = 0xFF;
        eth->src[i] = e1000_mac[i];
    }
    eth->ethertype = 0x0608;

    arp->hwtype = 0x0100;
    arp->protocol = 0x0008;
    arp->hwlen = 6;
    arp->protolen = 4;
    arp->opcode = op;
    for (int i = 0; i < 6; i++)
    {
        arp->sender_mac[i] = e1000_mac[i];
        arp->target_mac[i] = 0;
    }
    arp->sender_ip = e1000_local_ip;
    arp->target_ip = target_ip;
    e1000_send_frame(frame, sizeof(ethernet_header_t) + sizeof(arp_packet_t));
}

static void e1000_send_ping(uint32_t target_ip)
{
    uint8_t frame[256];
    ethernet_header_t *eth = (ethernet_header_t *)frame;
    ipv4_header_t *ip = (ipv4_header_t *)(frame + sizeof(ethernet_header_t));
    icmp_echo_t *icmp = (icmp_echo_t *)(frame + sizeof(ethernet_header_t) + sizeof(ipv4_header_t));

    for (int i = 0; i < 6; i++)
    {
        eth->dest[i] = e1000_gateway_mac[i];
        eth->src[i] = e1000_mac[i];
    }
    eth->ethertype = 0x0008;

    ip->version_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_length = 0x0000;
    ip->identification = 0x0000;
    ip->flags_fragment = 0x0000;
    ip->ttl = 64;
    ip->protocol = 1;
    ip->checksum = 0;
    ip->src_ip = e1000_local_ip;
    ip->dst_ip = target_ip;
    ip->total_length = 0x0008 + sizeof(icmp_echo_t);
    ip->checksum = kernel_checksum16((const uint8_t *)ip, sizeof(ipv4_header_t));

    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = 0x1234;
    icmp->sequence = 1;
    icmp->checksum = kernel_checksum16((const uint8_t *)icmp, sizeof(icmp_echo_t));

    e1000_send_frame(frame, sizeof(ethernet_header_t) + sizeof(ipv4_header_t) + sizeof(icmp_echo_t));
}

static void e1000_send_http_get(uint32_t target_ip, const char *host, const char *path)
{
    uint8_t frame[1536];
    ethernet_header_t *eth = (ethernet_header_t *)frame;
    ipv4_header_t *ip = (ipv4_header_t *)(frame + sizeof(ethernet_header_t));
    tcp_header_t *tcp = (tcp_header_t *)(frame + sizeof(ethernet_header_t) + sizeof(ipv4_header_t));
    char *payload = (char *)(frame + sizeof(ethernet_header_t) + sizeof(ipv4_header_t) + sizeof(tcp_header_t));

    for (int i = 0; i < 6; i++)
    {
        eth->dest[i] = e1000_gateway_mac[i];
        eth->src[i] = e1000_mac[i];
    }
    eth->ethertype = 0x0008;

    ip->version_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_length = 0x0000;
    ip->identification = 0x0001;
    ip->flags_fragment = 0x0000;
    ip->ttl = 64;
    ip->protocol = 6;
    ip->checksum = 0;
    ip->src_ip = e1000_local_ip;
    ip->dst_ip = target_ip;

    tcp->src_port = 0x1234;
    tcp->dst_port = 0x5000;
    tcp->seq_num = 1;
    tcp->ack_num = 0;
    tcp->data_offset_flags = 0x5002;
    tcp->window_size = 0x0200;
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    uint32_t payload_len = 0;
    payload[payload_len++] = 'G';
    payload[payload_len++] = 'E';
    payload[payload_len++] = 'T';
    payload[payload_len++] = ' ';
    for (uint32_t i = 0; path[i] && payload_len < 512; i++)
        payload[payload_len++] = path[i];
    payload[payload_len++] = ' ';
    payload[payload_len++] = 'H';
    payload[payload_len++] = 'T';
    payload[payload_len++] = 'T';
    payload[payload_len++] = 'P';
    payload[payload_len++] = '/';
    payload[payload_len++] = '1';
    payload[payload_len++] = '.';
    payload[payload_len++] = '1';
    payload[payload_len++] = '\r';
    payload[payload_len++] = '\n';
    payload[payload_len++] = 'H';
    payload[payload_len++] = 'o';
    payload[payload_len++] = 's';
    payload[payload_len++] = 't';
    payload[payload_len++] = ':';
    payload[payload_len++] = ' ';
    for (uint32_t i = 0; host[i] && payload_len < 512; i++)
        payload[payload_len++] = host[i];
    payload[payload_len++] = '\r';
    payload[payload_len++] = '\n';
    payload[payload_len++] = '\r';
    payload[payload_len++] = '\n';

    ip->total_length = (uint16_t)(sizeof(ipv4_header_t) + sizeof(tcp_header_t) + payload_len);
    ip->checksum = kernel_checksum16((const uint8_t *)ip, sizeof(ipv4_header_t));
    e1000_send_frame(frame, sizeof(ethernet_header_t) + sizeof(ipv4_header_t) + sizeof(tcp_header_t) + payload_len);
}

static void e1000_poll(void)
{
    if (!e1000_ready || !e1000_mmio)
        return;
    for (uint32_t i = 0; i < E1000_RX_RING_SIZE; i++)
    {
        e1000_rx_desc_t *desc = &e1000_rx_descs[i];
        if (desc->status & E1000_RXD_STAT_DD)
        {
            if (desc->status & E1000_RXD_STAT_EOP)
            {
                uint8_t *pkt = (uint8_t *)(uintptr_t)desc->buffer_addr;
                uint32_t len = desc->length;
                ethernet_header_t *eth = (ethernet_header_t *)pkt;
                if (eth->ethertype == 0x0608)
                {
                    arp_packet_t *arp = (arp_packet_t *)(pkt + sizeof(ethernet_header_t));
                    if (arp->opcode == 0x0200 && arp->target_ip == e1000_local_ip)
                    {
                        for (int j = 0; j < 6; j++)
                            e1000_gateway_mac[j] = arp->sender_mac[j];
                    }
                }
                desc->status = 0;
                desc->length = 0;
            }
        }
    }
}

void delay_cycles(uint32_t cycles);

static int e1000_network_init(void)
{
    if (!e1000_probe())
        return 0;
    e1000_setup_tx_rx();
    e1000_read_mac();
    e1000_ready = 1;
    e1000_send_arp(e1000_gateway_ip, 0x0100);
    for (int i = 0; i < 200; i++)
    {
        e1000_poll();
        if (e1000_gateway_mac[0] || e1000_gateway_mac[1] || e1000_gateway_mac[2] || e1000_gateway_mac[3] || e1000_gateway_mac[4] || e1000_gateway_mac[5])
            break;
        delay_cycles(10000);
    }
    return 1;
}

static void credentials_load(void)
{
    uint8_t sector[512];
    credentials_config_t *cfg = (credentials_config_t *)sector;

    read_sectors_pio28(0x1F0, 0, CONFIG_LBA, 1, sector);

    if (cfg->username[0] != '\0')
    {
        copy_string(username, sizeof(username), cfg->username);
    }
    if (cfg->password[0] != '\0')
    {
        copy_string(password, sizeof(password), cfg->password);
    }
}

#define SNAKE_HIGHSCORE_LBA 98
#define SNAKE_HIGHSCORE_MAGIC 0x53434F52u // "SCOR" - erkennt, ob der Sektor schon mal beschrieben wurde

typedef struct
{
    uint32_t magic;
    uint32_t highscore;
} snake_highscore_t;

static uint32_t snake_load_highscore(void)
{
    uint8_t sector[512];
    read_sectors_pio28(0x1F0, 0, SNAKE_HIGHSCORE_LBA, 1, sector);
    snake_highscore_t *hs = (snake_highscore_t *)sector;
    if (hs->magic == SNAKE_HIGHSCORE_MAGIC)
    {
        return hs->highscore;
    }
    return 0; // Sektor noch nie beschrieben (oder Datenmuell) -> Highscore 0
}

static void snake_save_highscore(uint32_t value)
{
    uint8_t sector[512];
    for (uint32_t i = 0; i < 512; i++)
        sector[i] = 0;
    snake_highscore_t *hs = (snake_highscore_t *)sector;
    hs->magic = SNAKE_HIGHSCORE_MAGIC;
    hs->highscore = value;
    write_sectors_pio28(0x1F0, 0, SNAKE_HIGHSCORE_LBA, 1, sector);
}

static void credentials_save(void)
{
    uint8_t sector[512];
    credentials_config_t *cfg = (credentials_config_t *)sector;

    for (uint32_t i = 0; i < sizeof(sector); i++)
    {
        sector[i] = 0;
    }

    copy_string(cfg->username, sizeof(cfg->username), username);
    copy_string(cfg->password, sizeof(cfg->password), password);
    write_sectors_pio28(0x1F0, 0, CONFIG_LBA, 1, sector);
}

static inline void cpu_relax(void)
{
    __asm__ volatile("pause");
}

void delay_cycles(uint32_t cycles)
{
    for (uint32_t i = 0; i < cycles; i++)
    {
        cpu_relax();
    }
}

void sleep(uint32_t seconds)
{
    // grob: muss an deine CPU-Taktung angepasst werden
    // Beispielwert nur als Startpunkt
    uint32_t loops = seconds * 100000000;
    delay_cycles(loops);
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// Einfacher linearer Kongruenz-Zufallsgenerator.
// Der Seed wird bei jedem Spielstart weitergedreht (static), damit
// nicht jedes Spiel exakt gleich abläuft, auch ohne echten Timer.
static uint32_t rng_state = 0x2545F491u;

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

static uint32_t rng_range(uint32_t n)
{
    return rng_next() % n;
}

static void mouse_feed_byte(uint8_t byte); // Vorwaerts-Deklaration, Definition weiter unten beim Maustreiber

// Non-blocking: liefert 0 wenn keine Taste wartet, sonst den Scancode.
// Extended-Prefix (0xE0) wird bewusst ignoriert/verworfen, genau wie
// im bestehenden settings_menu_keyboard() -> der zweite Byte (0x48/0x50/...)
// reicht zur Erkennung der Pfeiltasten.
//
// WICHTIG: Tastatur und Maus teilen sich denselben Ausgabepuffer an Port
// 0x60 - Bit 5 im Statusregister (0x64) sagt, von welchem Geraet ein
// wartendes Byte stammt. Diese Funktion wird JETZT UEBERALL im Kernel zum
// Pollen benutzt (Shell, Spiele, Menues, ...), damit die Maus - sobald sie
// einmal aktiv ist - nirgends mehr Tastatureingaben verfaelscht.
static uint8_t poll_scancode(void)
{
    uint8_t status = inb(0x64);
    if (!(status & 0x01))
    {
        return 0;
    }
    uint8_t byte = inb(0x60);
    if (status & 0x20)
    {
        mouse_feed_byte(byte);
        return 0;
    }
    return byte;
}

// Grobe, unkalibrierte Frame-Pause. cycles ggf. anpassen, falls das
// Spiel auf deiner Zielhardware/QEMU-Version zu schnell/langsam läuft.
static void game_frame_delay(uint32_t cycles)
{
    delay_cycles(cycles);
}

static volatile uint16_t *const video = (uint16_t *)0xB8000;
static uint32_t cursor_pos = 0;
static uint8_t keyboard_layout = 0; // 0=QWERTY, 1=DVORAK
static int mouse_ready = 0;
static int mouse_x = 40;
static int mouse_y = 12;
static int mouse_buttons = 0;
static int mouse_last_buttons = 0;
static int mouse_visible = 1;
// static uint32_t keyboard_repeat_delay = 100;  // ms delay between repeats

static const char scancode_map_qwerty[128] = {
    0,
    27,
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    '0',
    '-',
    '=',
    '\b',
    '\t',
    'q',
    'w',
    'e',
    'r',
    't',
    'y',
    'u',
    'i',
    'o',
    'p',
    '[',
    ']',
    '\n',
    0,
    'a',
    's',
    'd',
    'f',
    'g',
    'h',
    'j',
    'k',
    'l',
    ';',
    '\'',
    '`',
    0,
    '\\',
    'z',
    'x',
    'c',
    'v',
    'b',
    'n',
    'm',
    ',',
    '.',
    '/',
    0,
    '*',
    0,
    ' ',
    0,
    0,
};

// QWERTZ Layout (German)
static const char scancode_map_qwertz[128] = {
    0,
    27,
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    '0',
    '-',
    '=',
    '\b',
    '\t',
    'q',
    'w',
    'e',
    'r',
    't',
    'z',
    'u',
    'i',
    'o',
    'p',
    '[',
    ']',
    '\n',
    0,
    'a',
    's',
    'd',
    'f',
    'g',
    'h',
    'j',
    'k',
    'l',
    ';',
    '\'',
    '`',
    0,
    '\\',
    'y',
    'x',
    'c',
    'v',
    'b',
    'n',
    'm',
    ',',
    '.',
    '/',
    0,
    '*',
    0,
    ' ',
    0,
    0,
};

// DVORAK Layout
static const char scancode_map_dvorak[128] = {
    0,
    27,
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    '0',
    '[',
    ']',
    '\b',
    '\t',
    '\'',
    ',',
    '.',
    'p',
    'y',
    'f',
    'g',
    'c',
    'r',
    'l',
    '/',
    '=',
    '\n',
    0,
    'a',
    'o',
    'e',
    'u',
    'i',
    'd',
    'h',
    't',
    'n',
    's',
    '-',
    '`',
    0,
    '\\',
    ';',
    'q',
    'j',
    'k',
    'x',
    'b',
    'm',
    'w',
    'v',
    'z',
    0,
    '*',
    0,
    ' ',
    0,
    0,
};

static const char *current_scancode_map = scancode_map_qwerty;

// Schiebt den kompletten Bildschirm um eine Zeile nach oben und leert
// die letzte Zeile. Wird aufgerufen, statt einfach an Position 0
// zurueckzuspringen, wenn der Bildschirm voll ist.
#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25

// Der Bereich, in den Text gerade geschrieben wird. Standardmaessig der
// komplette Bildschirm (kein Fenster aktiv) - macht bestehenden Code
// automatisch abwaertskompatibel, da 0,0,80,25 genau dem alten Verhalten
// von put_char()/scroll_screen() entspricht. Windowed Apps (CMD, Settings)
// setzen diese Werte auf ihren Innenbereich, solange sie geoeffnet sind.
static uint8_t win_x = 0, win_y = 0, win_w = SCREEN_WIDTH, win_h = SCREEN_HEIGHT;

// Einfacher Ein-Ebenen-Stack, um Fenstergrenzen zu sichern/wiederherzustellen -
// z.B. wenn man aus einem CMD-Fenster heraus ein Vollbild-Spiel startet und
// danach wieder ins Fenster zurueckkehrt.
static uint8_t win_saved_x, win_saved_y, win_saved_w, win_saved_h;

static void win_push_fullscreen(void)
{
    win_saved_x = win_x;
    win_saved_y = win_y;
    win_saved_w = win_w;
    win_saved_h = win_h;
    win_x = 0;
    win_y = 0;
    win_w = SCREEN_WIDTH;
    win_h = SCREEN_HEIGHT;
}

static void win_pop(void)
{
    win_x = win_saved_x;
    win_y = win_saved_y;
    win_w = win_saved_w;
    win_h = win_saved_h;
}

static void scroll_screen(void)
{
    // Schiebt nur den Inhalt INNERHALB des aktuellen Fensters eine Zeile
    // nach oben - bei Vollbild (Standard) ist das exakt der ganze Bildschirm.
    for (uint8_t row = win_y; row < win_y + win_h - 1; row++)
    {
        for (uint8_t col = win_x; col < win_x + win_w; col++)
        {
            video[(uint32_t)row * 80 + col] = video[(uint32_t)(row + 1) * 80 + col];
        }
    }
    for (uint8_t col = win_x; col < win_x + win_w; col++)
    {
        video[(uint32_t)(win_y + win_h - 1) * 80 + col] = 0;
    }
    cursor_pos -= 80; // Cursor bleibt in der (jetzt letzten) Zeile stehen
}

static void put_char(char c, uint8_t color)
{
    uint8_t row = (uint8_t)(cursor_pos / 80);
    uint8_t col = (uint8_t)(cursor_pos % 80);

    if (c == '\r')
    {
        cursor_pos = (uint32_t)row * 80 + win_x;
        return;
    }

    if (c == '\b')
    {
        // Cursor einen Platz zurueck, statt das rohe 0x08-Zeichen
        // (im VGA-Zeichensatz ein sichtbares Symbol) zu drucken.
        // Nicht ueber den linken Fensterrand hinaus zurueckgehen.
        if (col > win_x)
            cursor_pos--;
        return;
    }

    if (c == '\n')
    {
        row++;
        col = win_x;
    }
    else
    {
        video[cursor_pos] = (uint16_t)c | ((uint16_t)color << 8);
        col++;
        if (col >= win_x + win_w)
        {
            row++;
            col = win_x;
        }
    }

    cursor_pos = (uint32_t)row * 80 + col;

    if (row >= win_y + win_h)
    {
        scroll_screen();
    }
}

static void write_string(const char *str, uint8_t color)
{
    for (uint32_t i = 0; str[i]; ++i)
    {
        put_char(str[i], color);
    }
}

static void print_uint32(uint32_t value, uint8_t color)
{
    char numbuf[12];
    int digits = 0;
    if (value == 0)
    {
        put_char('0', color);
        return;
    }
    while (value > 0 && digits < (int)sizeof(numbuf))
    {
        numbuf[digits++] = '0' + (char)(value % 10);
        value /= 10;
    }
    for (int i = digits - 1; i >= 0; i--)
    {
        put_char(numbuf[i], color);
    }
}

static int mouse_cursor_prev_pos = -1;
static uint16_t mouse_cursor_saved_cell = 0;

static void clear_screen(void)
{
    for (uint32_t i = 0; i < 80 * 25; i++)
    {
        video[i] = 0;
    }
    cursor_pos = 2;
    // Alte Mauszeiger-Position ist jetzt ungueltig (Inhalt wurde geloescht) -
    // sonst wuerde draw_mouse_cursor() spaeter versuchen, laengst geloeschten
    // Inhalt "wiederherzustellen" und dabei frisch gezeichnete Sachen ueberschreiben.
    mouse_cursor_prev_pos = -1;
}

// Wie clear_screen(), aber loescht NUR den Innenbereich des aktuell aktiven
// Fensters (win_x/win_y/win_w/win_h) - fuer Windowed Apps (CMD, Settings),
// damit der Desktop drumherum stehen bleibt statt komplett geloescht zu werden.
static void win_clear(void)
{
    for (uint8_t row = win_y; row < win_y + win_h; row++)
    {
        for (uint8_t col = win_x; col < win_x + win_w; col++)
        {
            video[(uint32_t)row * 80 + col] = 0;
        }
    }
    cursor_pos = (uint32_t)win_y * 80 + win_x;
    mouse_cursor_prev_pos = -1;
}

static void set_cursor_xy(uint8_t x, uint8_t y)
{
    cursor_pos = (uint32_t)y * 80 + x;
}

static void draw_text_at(uint8_t x, uint8_t y, const char *text, uint8_t color)
{
    uint32_t old = cursor_pos;
    set_cursor_xy(x, y);
    write_string(text, color);
    cursor_pos = old;
}

static void draw_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const char *title, uint8_t border_color, uint8_t fill_color)
{
    for (uint8_t yy = y; yy < y + h; yy++)
    {
        for (uint8_t xx = x; xx < x + w; xx++)
        {
            video[(uint32_t)yy * 80 + xx] = (uint16_t)' ' | ((uint16_t)fill_color << 8);
        }
    }

    for (uint8_t xx = x; xx < x + w; xx++)
    {
        video[(uint32_t)y * 80 + xx] = (uint16_t)0xC4 | ((uint16_t)border_color << 8);
        video[(uint32_t)(y + h - 1) * 80 + xx] = (uint16_t)0xC4 | ((uint16_t)border_color << 8);
    }
    for (uint8_t yy = y; yy < y + h; yy++)
    {
        video[(uint32_t)yy * 80 + x] = (uint16_t)0xB3 | ((uint16_t)border_color << 8);
        video[(uint32_t)yy * 80 + x + w - 1] = (uint16_t)0xB3 | ((uint16_t)border_color << 8);
    }
    video[(uint32_t)y * 80 + x] = (uint16_t)0xDA | ((uint16_t)border_color << 8);
    video[(uint32_t)y * 80 + x + w - 1] = (uint16_t)0xBF | ((uint16_t)border_color << 8);
    video[(uint32_t)(y + h - 1) * 80 + x] = (uint16_t)0xC0 | ((uint16_t)border_color << 8);
    video[(uint32_t)(y + h - 1) * 80 + x + w - 1] = (uint16_t)0xD9 | ((uint16_t)border_color << 8);
    if (title)
    {
        draw_text_at(x + 2, y, title, border_color);
    }
}

// Zeichnet ein Windows-95-artiges Fenster: grauer Rahmen, blaue Titelleiste
// mit App-Namen, schwarzer Inhaltsbereich darunter (klassischer "DOS-Box in
// einem Fensterrahmen"-Look). Ruft KEIN clear_screen() auf - der Desktop
// bleibt dahinter sichtbar. Setzt win_x/y/w/h auf den Innenbereich, damit
// nachfolgende write_string()/read_line()-Aufrufe automatisch innerhalb des
// Fensters bleiben (siehe put_char()).
static void draw_window_frame(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const char *title)
{
    draw_box(x, y, w, h, 0, 0x07, 0x00); // grauer Rahmen, schwarzer Inhalt

    for (uint8_t xx = x + 1; xx < x + w - 1; xx++)
    {
        video[(uint32_t)(y + 1) * 80 + xx] = (uint16_t)' ' | ((uint16_t)0x1F << 8);
    }
    if (title)
    {
        draw_text_at(x + 2, y + 1, title, 0x1F);
    }
    draw_text_at(x + w - 6, y + 1, "[ESC]", 0x1C);

    win_x = (uint8_t)(x + 1);
    win_y = (uint8_t)(y + 2);
    win_w = (uint8_t)(w - 2);
    win_h = (uint8_t)(h - 3);
}

static void draw_mouse_cursor(void)
{
    // Vorherige Position wiederherstellen (falls noch gueltig - clear_screen()
    // setzt mouse_cursor_prev_pos zurueck, wenn der Bildschirm inzwischen
    // sowieso neu gezeichnet wurde).
    if (mouse_cursor_prev_pos >= 0)
    {
        video[mouse_cursor_prev_pos] = mouse_cursor_saved_cell;
        mouse_cursor_prev_pos = -1;
    }

    if (!mouse_visible)
        return;
    if (mouse_x >= 0 && mouse_x < 80 && mouse_y >= 0 && mouse_y < 25)
    {
        int pos = mouse_y * 80 + mouse_x;
        mouse_cursor_saved_cell = video[pos];
        video[pos] = (uint16_t)'+' | ((uint16_t)0x0E << 8);
        mouse_cursor_prev_pos = pos;
    }
}

static void network_http_get_cmd(const char *url);
static void handle_command(const char *cmd);
void delay_cycles(uint32_t cycles);

// Wartet, bis der Eingabepuffer des Controllers leer ist (sicher zu schreiben).
static void kbc_wait_input_clear(void)
{
    while (inb(0x64) & 0x02)
    {
        cpu_relax();
    }
}

// Wartet, bis ein Byte im Ausgabepuffer bereitsteht (sicher zu lesen).
static void kbc_wait_output_full(void)
{
    while (!(inb(0x64) & 0x01))
    {
        cpu_relax();
    }
}

static void mouse_write(uint8_t value)
{
    kbc_wait_input_clear();
    outb(0x64, 0xD4); // naechstes Byte geht an das Aux-Geraet (Maus)
    kbc_wait_input_clear();
    outb(0x60, value);
}

static void ps2_mouse_init(void)
{
    if (mouse_ready)
        return;

    kbc_wait_input_clear();
    outb(0x64, 0xA8); // Aux-Geraet (Maus) aktivieren

    kbc_wait_input_clear();
    outb(0x64, 0x20); // Command: Controller-Konfigurationsbyte lesen
    kbc_wait_output_full();
    uint8_t status = inb(0x60);
    status &= ~0x02; // Bit 1 (IRQ12) NICHT aktivieren - wir pollen nur,
                     // es gibt keine IDT/PIC-Einrichtung in diesem Kernel.
                     // Ein unbehandelter Interrupt waere hier riskant.

    kbc_wait_input_clear();
    outb(0x64, 0x60); // Command: Controller-Konfigurationsbyte schreiben
    kbc_wait_input_clear();
    outb(0x60, status);

    mouse_write(0xF4); // Data Reporting aktivieren
    mouse_ready = 1;
}

// Schaltet die Datenuebertragung der Maus wieder ab. Wichtig beim Verlassen
// einer GUI-App: sonst spuckt die Maus weiter Pakete an Port 0x60 aus, und
// jeder normale Tastatur-Poll anderswo im Kernel (Shell, Spiele, ...) liest
// diese Bytes versehentlich als Scancodes ein.
__attribute__((unused)) static void mouse_disable_reporting(void)
{
    if (!mouse_ready)
        return;
    mouse_write(0xF5);
    mouse_ready = 0;
}

// Verarbeitet EIN Byte, von dem bereits feststeht, dass es von der Maus
// stammt (siehe gui_poll_input()). Reine Paket-Zustandsmaschine, kein
// eigener Port-Zugriff mehr.
static void mouse_feed_byte(uint8_t byte)
{
    static uint8_t mouse_cycle = 0;
    static uint8_t mouse_packet[3];

    switch (mouse_cycle)
    {
    case 0:
        if (byte & 0x08)
        { // Sync-Bit: muss im ersten Byte immer 1 sein
            mouse_packet[0] = byte;
            mouse_cycle = 1;
        }
        break;
    case 1:
        mouse_packet[1] = byte;
        mouse_cycle = 2;
        break;
    case 2:
        mouse_packet[2] = byte;
        mouse_cycle = 0;
        mouse_buttons = mouse_packet[0] & 0x07;
        {
            int8_t dx = (int8_t)mouse_packet[1];
            int8_t dy = (int8_t)mouse_packet[2];
            mouse_x += dx / 2;
            mouse_y -= dy / 2;
            if (mouse_x < 0)
                mouse_x = 0;
            if (mouse_y < 0)
                mouse_y = 0;
            if (mouse_x >= 80)
                mouse_x = 79;
            if (mouse_y >= 25)
                mouse_y = 24;
        }
        break;
    }
}

// Alias von poll_scancode() - historisch mit eigenem Namen fuer GUI-Loops,
// macht inzwischen exakt dasselbe (siehe poll_scancode() weiter oben).
static uint8_t gui_poll_input(void)
{
    return poll_scancode();
}

static int app_is_installed(const char *name)
{
    for (int i = 0; i < installed_app_count; i++)
    {
        if (kernel_strcmp(installed_apps[i], name) == 0)
            return 1;
    }
    return 0;
}

static void add_installed_app(const char *name)
{
    if (app_is_installed(name))
        return;
    if (installed_app_count < 8)
    {
        copy_string(installed_apps[installed_app_count], sizeof(installed_apps[0]), name);
        installed_app_count++;
    }
}

static void list_installed_apps(void)
{
    write_string("Installed apps:\n", 0x0A);
    if (installed_app_count == 0)
    {
        write_string("  (none)\n", 0x0F);
        return;
    }
    for (int i = 0; i < installed_app_count; i++)
    {
        write_string("  - ", 0x0F);
        write_string(installed_apps[i], 0x0F);
        write_string("\n", 0x0F);
    }
}

static int is_blocked_domain(const char *domain)
{
    if (!security_enabled)
        return 0;
    for (int i = 0; i < blocked_domain_count; i++)
    {
        if (kernel_strcmp(blocked_domains[i], domain) == 0)
            return 1;
    }
    return 0;
}

static void firewall_allow(const char *domain)
{
    if (is_blocked_domain(domain))
        return;
    if (blocked_domain_count < 4)
    {
        copy_string(blocked_domains[blocked_domain_count], sizeof(blocked_domains[0]), domain);
        blocked_domain_count++;
    }
}

static int firewall_check(const char *domain)
{
    if (!security_enabled)
        return 1;
    if (domain == 0 || domain[0] == '\0')
        return 0;
    if (kernel_strcmp(domain, "packages.tocdesk.local") == 0)
        return 1;
    if (kernel_strcmp(domain, "repo.tocdesk.local") == 0)
        return 1;
    if (kernel_strcmp(domain, "download.tocdesk.local") == 0)
        return 1;
    if (is_blocked_domain(domain))
        return 0;
    return 1;
}

static void draw_gui_cursor(uint8_t x, uint8_t y)
{
    if (x < 80 && y < 25)
    {
        video[y * 80 + x] = (uint16_t)'>' | ((uint16_t)0x0E << 8);
    }
}

static void launch_firefox_app(void)
{
    mouse_last_buttons = 0;
    int loaded = 0;

    while (1)
    {
        clear_screen();
        write_string("=== Mozilla Firefox ===\n\n", 0x0A);
        if (loaded)
        {
            write_string("Loading mozilla.org...\n", 0x0F);
            if (network_connected)
            {
                network_http_get_cmd("www.mozilla.org");
            }
            else
            {
                write_string("Network offline. Browser stub running.\n", 0x0C);
            }
            write_string("\n", 0x0F);
        }
        else
        {
            write_string("Browser UI ready.\n", 0x0F);
            write_string("Use the mouse to move the cursor.\n", 0x0F);
            write_string("Left click loads the home page.\n", 0x0F);
        }
        write_string("Press ESC to exit.\n", 0x0F);
        draw_mouse_cursor();

        uint8_t scancode = gui_poll_input();

        if (mouse_buttons & 0x01 && !(mouse_last_buttons & 0x01))
        {
            loaded = 1;
        }
        mouse_last_buttons = mouse_buttons;

        if (scancode == 0x01)
        { // ESC
            break;
        }

        if (scancode == 0)
        {
            delay_cycles(10000);
        }
    }

    clear_screen();
}

static void launch_gui_app(const char *name)
{
    if (!app_is_installed(name))
    {
        write_string("App is not installed. Use 'install <app>' first.\n", 0x0C);
        return;
    }

    ps2_mouse_init();

    if (kernel_strcmp(name, "firefox") == 0 || kernel_strcmp(name, "browser") == 0)
    {
        launch_firefox_app();
        return;
    }

    clear_screen();
    write_string("=== ", 0x0A);
    write_string(name, 0x0A);
    write_string(" ===\n\n", 0x0A);
    write_string("GUI stub: use arrow keys to move the cursor.\n", 0x0F);
    write_string("Press ESC to exit.\n", 0x0F);

    uint8_t x = 10;
    uint8_t y = 6;
    draw_gui_cursor(x, y);
    draw_mouse_cursor();

    while (1)
    {
        uint8_t scancode = gui_poll_input();

        if (scancode == 0)
        {
            delay_cycles(10000);
            continue;
        }

        if (scancode == 0x01)
        {
            break;
        }
        else if (scancode == 0x4B)
        {
            if (x > 0)
                x--;
        }
        else if (scancode == 0x4D)
        {
            if (x < 78)
                x++;
        }
        else if (scancode == 0x48)
        {
            if (y > 0)
                y--;
        }
        else if (scancode == 0x50)
        {
            if (y < 24)
                y++;
        }

        clear_screen();
        write_string("=== ", 0x0A);
        write_string(name, 0x0A);
        write_string(" ===\n\n", 0x0A);
        write_string("GUI stub: use arrow keys to move the cursor.\n", 0x0F);
        write_string("Press ESC to exit.\n", 0x0F);
        draw_gui_cursor(x, y);
        draw_mouse_cursor();
    }

    clear_screen();
}

static void install_cmd(const char *arg)
{
    char app_name[16];
    char repo[24];
    copy_string(app_name, sizeof(app_name), arg);

    if (app_name[0] == '\0')
    {
        write_string("Usage: install <app>\n", 0x0E);
        return;
    }

    if (!network_connected)
    {
        write_string("No network connection. Use 'net connect' first.\n", 0x0C);
        return;
    }

    if (security_enabled)
    {
        write_string("Security firewall active. Checking repository access...\n", 0x0A);
    }

    if (kernel_strcmp(app_name, "firefox") == 0 || kernel_strcmp(app_name, "browser") == 0)
    {
        copy_string(repo, sizeof(repo), "repo.tocdesk.local");
        if (!firewall_check(repo))
        {
            write_string("Blocked by firewall: repo.tocdesk.local\n", 0x0C);
            return;
        }
        add_installed_app("firefox");
        write_string("Downloading Mozilla Firefox package metadata...\n", 0x0A);
        write_string("Installed: Mozilla Firefox (browser UI)\n", 0x0A);
    }
    else if (kernel_strcmp(app_name, "notes") == 0)
    {
        copy_string(repo, sizeof(repo), "packages.tocdesk.local");
        if (!firewall_check(repo))
        {
            write_string("Blocked by firewall: packages.tocdesk.local\n", 0x0C);
            return;
        }
        add_installed_app("notes");
        write_string("Downloading package from packages.tocdesk.local...\n", 0x0A);
        write_string("Installed: Notes app\n", 0x0A);
    }
    else if (kernel_strcmp(app_name, "explorer") == 0)
    {
        copy_string(repo, sizeof(repo), "download.tocdesk.local");
        if (!firewall_check(repo))
        {
            write_string("Blocked by firewall: download.tocdesk.local\n", 0x0C);
            return;
        }
        add_installed_app("explorer");
        write_string("Downloading package from download.tocdesk.local...\n", 0x0A);
        write_string("Installed: Explorer app\n", 0x0A);
    }
    else if (kernel_strcmp(app_name, "games") == 0)
    {
        copy_string(repo, sizeof(repo), "repo.tocdesk.local");
        if (!firewall_check(repo))
        {
            write_string("Blocked by firewall: repo.tocdesk.local\n", 0x0C);
            return;
        }
        add_installed_app("games");
        write_string("Downloading package from repo.tocdesk.local...\n", 0x0A);
        write_string("Installed: Games app\n", 0x0A);
    }
    else
    {
        write_string("Unknown package. Supported: firefox, notes, explorer, games\n", 0x0C);
    }
}

static void network_connect_cmd(void)
{
    if (e1000_network_init())
    {
        network_connected = 1;
        write_string("Network connected. NIC ready.\n", 0x0A);
    }
    else
    {
        network_connected = 0;
        write_string("Network connection failed. No supported NIC found.\n", 0x0C);
    }
}

static void network_status_cmd(void)
{
    if (network_connected)
    {
        write_string("Network: connected\n", 0x0A);
    }
    else
    {
        write_string("Network: disconnected\n", 0x0C);
    }
}

static void network_ping_cmd(const char *target)
{
    if (!network_connected)
    {
        write_string("No network connection. Use 'net connect' first.\n", 0x0C);
        return;
    }
    uint32_t ip = ipv4_from_name(target);
    if (ip == 0)
    {
        write_string("Unable to resolve target.\n", 0x0C);
        return;
    }
    write_string("Pinging target...\n", 0x0A);
    e1000_send_ping(ip);
    write_string("Ping request sent.\n", 0x0A);
}

static void network_http_get_cmd(const char *url)
{
    if (!network_connected)
    {
        write_string("No network connection. Use 'net connect' first.\n", 0x0C);
        return;
    }

    char host[64];
    char path[64];
    uint32_t i = 0;
    const char *p = url;
    while (*p && *p != '/' && i < sizeof(host) - 1)
    {
        host[i++] = *p++;
    }
    host[i] = '\0';

    if (*p == '/')
    {
        p++;
    }
    i = 0;
    while (*p && i < sizeof(path) - 1)
    {
        path[i++] = *p++;
    }
    path[i] = '\0';

    if (path[0] == '\0')
    {
        copy_string(path, sizeof(path), "/");
    }

    uint32_t ip = ipv4_from_name(host);
    if (ip == 0)
    {
        write_string("Unable to resolve host.\n", 0x0C);
        return;
    }

    write_string("Sending HTTP GET request...\n", 0x0A);
    e1000_send_http_get(ip, host, path);
    write_string("HTTP request sent.\n", 0x0A);
}

static void security_status_cmd(void)
{
    if (security_enabled)
    {
        write_string("Security: enabled (firewall active)\n", 0x0A);
    }
    else
    {
        write_string("Security: disabled\n", 0x0C);
    }
}

static void security_toggle_cmd(const char *arg)
{
    if (kernel_strcmp(arg, "on") == 0 || kernel_strcmp(arg, "enable") == 0)
    {
        security_enabled = 1;
        write_string("Security enabled. Firewall active.\n", 0x0A);
    }
    else if (kernel_strcmp(arg, "off") == 0 || kernel_strcmp(arg, "disable") == 0)
    {
        security_enabled = 0;
        write_string("Security disabled.\n", 0x0C);
    }
    else
    {
        write_string("Usage: security on|off\n", 0x0E);
    }
}

static void firewall_cmd(const char *arg)
{
    if (kernel_strcmp(arg, "allow") == 0)
    {
        firewall_allow("repo.tocdesk.local");
        write_string("Firewall allow rule added for repo.tocdesk.local\n", 0x0A);
    }
    else if (kernel_strcmp(arg, "block") == 0)
    {
        write_string("Firewall block mode active.\n", 0x0C);
        firewall_mode = 0;
    }
    else
    {
        write_string("Usage: firewall allow|block\n", 0x0E);
    }
}

static void read_line_internal(char *buf, uint32_t len, int masked)
{
    uint32_t pos = 0;
    while (pos < len - 1)
    {
        uint8_t scancode = poll_scancode();
        if (scancode == 0)
        {
            // Kein Tastatur-Byte gerade - trotzdem den Mauszeiger aktuell
            // halten, damit die Maus auch waehrend Texteingaben (CMD, Notes,
            // Settings, Login, ...) sichtbar und benutzbar bleibt.
            draw_mouse_cursor();
            continue;
        }

        if (scancode == 0x2A || scancode == 0x36)
        {
            shiftdown = 1;
            continue;
        }
        if (scancode == 0xAA || scancode == 0xB6)
        {
            shiftdown = 0;
            continue;
        }

        if (scancode < sizeof(scancode_map_qwerty))
        {
            char c = current_scancode_map[scancode];

            if (c >= 'a' && c <= 'z' && shiftdown)
            {
                c = (char)(c - 'a' + 'A');
            }

            if (c == '\n')
            {
                buf[pos] = '\0';
                put_char('\n', 0x0F);
                return;
            }

            if (c == '\b' && pos > 0)
            {
                pos--;
                put_char('\b', 0x0F);
                put_char(' ', 0x0F);
                put_char('\b', 0x0F);
                continue;
            }

            if (c > 0 && c != '\t')
            {
                buf[pos++] = c;
                put_char(masked ? '*' : c, 0x0F);
            }
        }
    }
    buf[pos] = '\0';
}

static void read_line(char *buf, uint32_t len)
{
    read_line_internal(buf, len, 0);
}

// Wie read_line(), aber zeigt '*' statt der echten Zeichen an -> fuer Passwoerter.
static void read_line_masked(char *buf, uint32_t len)
{
    read_line_internal(buf, len, 1);
}

// Vorwaerts-Deklaration: die eigentliche Definition steht weiter unten
// bei der Login-Logik, wird aber schon von notes_app/explorer_app gebraucht.
static void wait_any_key(void);

static void reboot(void)
{
    write_string("System rebooting...\n", 0x0C);

    // Keyboard Controller Reset
    uint8_t result;
    do
    {
        result = inb(0x64); // Keyboard status
    } while (result & 0x02); // Warte bis Input-Buffer leer

    outb(0x64, 0xFE); // Reboot-Befehl

    // Fallback: CPU halten
    __asm__("cli");
    while (1)
        __asm__("hlt");
}

static void shutdown_cmd(void)
{
    write_string("Shutting down...\n", 0x0C);
    acpi_power_off();

    // Sollte nie erreicht werden - acpi_power_off() haelt die CPU am Ende
    // in jedem Fall an (auch wenn ACPI selbst nicht greift).
    write_string("ACPI shutdown fehlgeschlagen, CPU angehalten.\n", 0x0C);
    __asm__("cli");
    while (1)
        __asm__("hlt");
}

static void settings_menu_username(void)
{
    char new_username[sizeof(username)];

    win_clear();
    write_string("=== CHANGE USERNAME ===\n\n", 0x0A);
    write_string("Enter new username:\n", 0x0F);
    read_line(new_username, sizeof(new_username));

    if (new_username[0] != '\0')
    {
        copy_string(username, sizeof(username), new_username);
        credentials_save();
        win_clear();
        write_string("Username changed.\n\n", 0x0A);
    }
    else
    {
        win_clear();
        write_string("Username was not changed.\n\n", 0x0C);
    }

    sleep(1);
    write_string("Press any key to return...\n", 0x0F);
    wait_any_key();
}

static void settings_menu_password(void)
{
    char new_password[sizeof(password)];

    win_clear();
    write_string("=== CHANGE PASSWORD ===\n\n", 0x0A);
    write_string("Enter new password:\n", 0x0F);
    read_line_masked(new_password, sizeof(new_password));

    if (new_password[0] != '\0')
    {
        copy_string(password, sizeof(password), new_password);
        credentials_save();
        win_clear();
        write_string("Password changed.\n\n", 0x0A);
    }
    else
    {
        win_clear();
        write_string("Password was not changed.\n\n", 0x0C);
    }

    sleep(1);
    write_string("Press any key to return...\n", 0x0F);
    wait_any_key();
}

static void settings_menu_keyboard(void)
{
    uint8_t selected = 0; // Current selection
    char layout_names[3][8] = {"QWERTY", "QWERTZ", "DVORAK"};

    while (1)
    {
        win_clear();
        write_string("=== KEYBOARD SETTINGS ===\n\n", 0x0A);
        write_string("Select Layout:\n\n", 0x0F);

        for (uint8_t i = 0; i < 3; i++)
        {
            write_string("  ", 0x0F);
            if (i == selected)
            {
                write_string("> ", 0x0A);
                write_string(layout_names[i], 0x0A);
            }
            else
            {
                write_string("  ", 0x0F);
                write_string(layout_names[i], 0x0F);
            }
            write_string("\n", 0x0F);
        }

        write_string("\nUse UP/DOWN (or 1-3) to select\n", 0x0F);
        write_string("Press ENTER to confirm\n", 0x0F);
        write_string("Press ESC to cancel\n", 0x0F);

        // Wait for key
        uint8_t key = 0;
        while (!key)
        {
            uint8_t scancode = poll_scancode();
            if (scancode == 0)
            {
                delay_cycles(10000);
                continue;
            }
            if (scancode == 0x48)
            { // UP arrow
                key = 'U';
            }
            else if (scancode == 0x50)
            { // DOWN arrow
                key = 'D';
            }
            else if (scancode == 0x1C)
            { // ENTER
                key = 'E';
            }
            else if (scancode == 0x01)
            { // ESC
                key = 'X';
            }
            else if (scancode >= 0x02 && scancode <= 0x04)
            { // 1-3
                selected = scancode - 0x02;
                key = 'E';
            }
        }

        if (key == 'U')
        {
            selected = (selected > 0) ? selected - 1 : 2;
        }
        else if (key == 'D')
        {
            selected = (selected < 2) ? selected + 1 : 0;
        }
        else if (key == 'E')
        {
            // Apply selection
            switch (selected)
            {
            case 0:
                current_scancode_map = scancode_map_qwerty;
                break;
            case 1:
                current_scancode_map = scancode_map_qwertz;
                break;
            case 2:
                current_scancode_map = scancode_map_dvorak;
                break;
            }
            keyboard_layout = selected;
            win_clear();
            write_string("Keyboard layout set to: ", 0x0A);
            write_string(layout_names[selected], 0x0A);
            write_string("\n\n", 0x0F);
            return;
        }
        else if (key == 'X')
        {
            win_clear();
            return;
        }
    }
}

static void settings_menu(void)
{
    char option[32];
    while (1)
    {
        win_clear();
        write_string("=== SETTINGS MENU ===\n\n", 0x0A);
        write_string("1. Change Username\n", 0x0F);
        write_string("2. Change Password\n", 0x0F);
        write_string("3. Change Keyboard Layout\n", 0x0F);
        write_string("4. Return to Shell\n", 0x0F);
        write_string("\nSelect an option (1-4): ", 0x0F);

        read_line(option, sizeof(option));

        if (kernel_strcmp(option, "1") == 0)
        {
            settings_menu_username();
        }
        else if (kernel_strcmp(option, "2") == 0)
        {
            settings_menu_password();
        }
        else if (kernel_strcmp(option, "3") == 0)
        {
            settings_menu_keyboard();
        }
        else if (kernel_strcmp(option, "4") == 0)
        {
            break;
        }
        else
        {
            write_string("Invalid option. Press any key to try again...\n", 0x0C);
            while (1)
            {
                uint8_t status = inb(0x64);
                if (status & 0x01)
                {
                    inb(0x60); // Read scancode to clear buffer
                    break;
                }
            }
        }
    }
}

static void notes_app(void)
{
    fs_entry_t entries[FS_MAX_FILES];
    fs_read_dir(entries);

    int slot = -1;
    for (int i = 0; i < FS_MAX_FILES; i++)
    {
        if (!entries[i].used)
        {
            slot = i;
            break;
        }
    }

    win_clear();
    write_string("=== NOTES ===\n\n", 0x0A);

    if (slot == -1)
    {
        write_string("Kein freier Speicherplatz mehr (max. 10 Notizen).\n", 0x0C);
        write_string("Loesche zuerst eine Notiz im Explorer ('explorer').\n\n", 0x0F);
        write_string("Press any key to return...\n", 0x0F);
        wait_any_key();
        return;
    }

    write_string("Name der Notiz (max 14 Zeichen): ", 0x0F);
    char name[FS_NAME_LEN];
    read_line(name, sizeof(name));

    write_string("\nText eingeben (eine Zeile, ENTER = speichern):\n", 0x0F);
    write_string("------------------------------------------\n", 0x08);
    char content[FS_MAX_FILE_SIZE];
    read_line(content, sizeof(content));

    uint32_t len = 0;
    while (content[len])
        len++;

    fs_save_file(slot, name, content, len);

    win_clear();
    write_string("Notiz gespeichert: ", 0x0A);
    write_string(name, 0x0A);
    write_string("\n\nMit 'explorer' kannst du sie dir spaeter wieder ansehen.\n", 0x0F);
    write_string("\nPress any key to return...\n", 0x0F);
    wait_any_key();
}

static void explorer_app(void)
{
    char option[8];

    while (1)
    {
        fs_entry_t entries[FS_MAX_FILES];
        fs_read_dir(entries);

        win_clear();
        write_string("=== EXPLORER ===\n\n", 0x0A);
        for (int i = 0; i < FS_MAX_FILES; i++)
        {
            write_string("  ", 0x0F);
            put_char('0' + i, 0x0B);
            write_string(") ", 0x0F);
            if (entries[i].used)
            {
                write_string(entries[i].name, 0x0A);
            }
            else
            {
                write_string("(leer)", 0x08);
            }
            write_string("\n", 0x0F);
        }
        write_string("\nNummer = oeffnen, 'd'+Nummer = loeschen (z.B. d2)\n", 0x0F);
        write_string("'q' = Zurueck zur Shell\n> ", 0x0F);

        read_line(option, sizeof(option));

        if (option[0] == 'q' || option[0] == '\0')
        {
            break;
        }

        int delete_mode = (option[0] == 'd') ? 1 : 0;
        int digit_pos = delete_mode ? 1 : 0;

        if (option[digit_pos] >= '0' && option[digit_pos] <= '9')
        {
            int idx = option[digit_pos] - '0';
            if (idx >= 0 && idx < FS_MAX_FILES)
            {
                if (delete_mode)
                {
                    fs_delete_file(idx);
                }
                else if (entries[idx].used)
                {
                    char content[FS_MAX_FILE_SIZE];
                    fs_load_file(idx, content, sizeof(content));
                    win_clear();
                    write_string("=== ", 0x0A);
                    write_string(entries[idx].name, 0x0A);
                    write_string(" ===\n\n", 0x0A);
                    write_string(content, 0x0F);
                    sleep(1);
                    write_string("\n\nPress any key to return...\n", 0x08);
                    wait_any_key();
                }
            }
        }
    }
}

#define SNAKE_MAX_LEN 256
#define SNAKE_MIN_X 1
#define SNAKE_MAX_X 78 // exklusiv, also gueltige X: 1..77
#define SNAKE_MIN_Y 2
#define SNAKE_MAX_Y 23 // exklusiv, also gueltige Y: 2..22

typedef struct
{
    int8_t x, y;
} snake_cell_t;

static void snake_draw_borders(void)
{
    for (uint32_t x = 0; x < 80; x++)
    {
        video[1 * 80 + x] = (uint16_t)0xB2 | (0x08 << 8);  // Zeile 1 = obere Wand
        video[23 * 80 + x] = (uint16_t)0xB2 | (0x08 << 8); // Zeile 23 = untere Wand
    }
    for (uint32_t y = 1; y <= 23; y++)
    {
        video[y * 80 + 0] = (uint16_t)0xB2 | (0x08 << 8);
        video[y * 80 + 79] = (uint16_t)0xB2 | (0x08 << 8);
    }
}

static void snake_put(int x, int y, char c, uint8_t color)
{
    video[y * 80 + x] = (uint16_t)c | ((uint16_t)color << 8);
}

static void snakes(void)
{
    snake_cell_t body[SNAKE_MAX_LEN];
    int length = 3;
    int dx = 1, dy = 0;
    int score = 0;
    int game_over = 0;
    int food_x, food_y;
    uint32_t highscore = snake_load_highscore();

    // Startposition: Mitte des Feldes, Kopf zuerst in body[0]
    body[0].x = 20;
    body[0].y = 11;
    body[1].x = 19;
    body[1].y = 11;
    body[2].x = 18;
    body[2].y = 11;

    // Erstes Futter platzieren
    food_x = SNAKE_MIN_X + (int)rng_range(SNAKE_MAX_X - SNAKE_MIN_X);
    food_y = SNAKE_MIN_Y + (int)rng_range(SNAKE_MAX_Y - SNAKE_MIN_Y);

    clear_screen();

    while (!game_over)
    {
        // --- Eingabe abfragen, mehrfach waehrend der Frame-Pause ---
        for (int i = 0; i < 40; i++)
        {
            uint8_t sc = poll_scancode();
            if (sc == 0x01)
            { // ESC
                return;
            }
            else if (sc == 0x48 && dy == 0)
            { // hoch
                dx = 0;
                dy = -1;
            }
            else if (sc == 0x50 && dy == 0)
            { // runter
                dx = 0;
                dy = 1;
            }
            else if (sc == 0x4B && dx == 0)
            { // links
                dx = -1;
                dy = 0;
            }
            else if (sc == 0x4D && dx == 0)
            { // rechts
                dx = 1;
                dy = 0;
            }
            game_frame_delay(200000);
        }

        // --- Bewegen: Koerper folgt dem Vorgaenger, Kopf zuletzt ---
        for (int i = length - 1; i > 0; i--)
        {
            body[i] = body[i - 1];
        }
        body[0].x += dx;
        body[0].y += dy;

        // --- Kollisionen ---
        if (body[0].x < SNAKE_MIN_X || body[0].x >= SNAKE_MAX_X ||
            body[0].y < SNAKE_MIN_Y || body[0].y >= SNAKE_MAX_Y)
        {
            game_over = 1;
            break;
        }
        for (int i = 1; i < length; i++)
        {
            if (body[i].x == body[0].x && body[i].y == body[0].y)
            {
                game_over = 1;
                break;
            }
        }
        if (game_over)
            break;

        // --- Futter gefressen? ---
        if (body[0].x == food_x && body[0].y == food_y)
        {
            if (length < SNAKE_MAX_LEN)
            {
                body[length] = body[length - 1];
                length++;
            }
            score++;
            if ((uint32_t)score > highscore)
            {
                highscore = (uint32_t)score;
                snake_save_highscore(highscore);
            }
            // Neues Futter, das nicht auf der Schlange liegt
            int valid;
            do
            {
                valid = 1;
                food_x = SNAKE_MIN_X + (int)rng_range(SNAKE_MAX_X - SNAKE_MIN_X);
                food_y = SNAKE_MIN_Y + (int)rng_range(SNAKE_MAX_Y - SNAKE_MIN_Y);
                for (int i = 0; i < length; i++)
                {
                    if (body[i].x == food_x && body[i].y == food_y)
                    {
                        valid = 0;
                        break;
                    }
                }
            } while (!valid);
        }

        // --- Zeichnen ---
        clear_screen();
        cursor_pos = 0;
        write_string("=== Snake ===  Score: ", 0x0A);
        print_uint32((uint32_t)score, 0x0A);
        write_string("  Highscore: ", 0x0A);
        print_uint32(highscore, 0x0A);
        write_string("   ESC = Beenden\n", 0x0F);

        snake_draw_borders();
        snake_put(food_x, food_y, 0x04, 0x0C);       // Futter: rotes Herz-Symbol
        snake_put(body[0].x, body[0].y, 0x02, 0x0E); // Kopf: gelbes Smiley
        for (int i = 1; i < length; i++)
        {
            snake_put(body[i].x, body[i].y, 0xDB, 0x0A); // Koerper: gruener Block
        }
    }

    clear_screen();
    write_string("=== Game Over ===\n\n", 0x0C);
    write_string("Score: ", 0x0F);
    print_uint32((uint32_t)score, 0x0F);
    write_string("\nHighscore: ", 0x0F);
    print_uint32(highscore, 0x0F);
    sleep(1);
    write_string("\n\nPress any key to return...\n", 0x0F);
    while (1)
    {
        if (inb(0x64) & 0x01)
        {
            inb(0x60);
            break;
        }
    }
}

#define PAC_W 31
#define PAC_H 15
#define PAC_OFFSET_X 24
#define PAC_OFFSET_Y 3
#define PAC_NUM_GHOSTS 2

// '#' = Wand, '.' = Punkt (Score), ' ' = leerer, bereits besuchter Weg
static const char *pac_template[PAC_H] = {
    "###############################",
    "#...............#........#....#",
    "#.........#.....#.............#",
    "#...#######.....#....#####....#",
    "#...#...........#....#........#",
    "#...#................#........#",
    "#...#..#..############..#######",
    "#......#......................#",
    "#......#......#...............#",
    "#....#####....#......#####....#",
    "#........#....#......#........#",
    "#........#...#####...#........#",
    "#...####......................#",
    "#....................#........#",
    "###############################",
};

static int pac_is_wall(char maze[PAC_H][PAC_W], int x, int y)
{
    if (x < 0 || x >= PAC_W || y < 0 || y >= PAC_H)
        return 1;
    return maze[y][x] == '#';
}

static void pacman(void)
{
    char maze[PAC_H][PAC_W];
    int px, py;
    int gx[PAC_NUM_GHOSTS], gy[PAC_NUM_GHOSTS];
    int dots_left = 0;
    int score = 0;
    int win = 0;

    for (int y = 0; y < PAC_H; y++)
    {
        for (int x = 0; x < PAC_W; x++)
        {
            maze[y][x] = pac_template[y][x];
            if (maze[y][x] == '.')
                dots_left++;
        }
    }

    px = 15;
    py = 7;
    gx[0] = 5;
    gy[0] = 5;
    gx[1] = 25;
    gy[1] = 9;

    // cur_dx/cur_dy: die Richtung, in die sich Pacman JEDEN Tick automatisch
    // weiterbewegt (wie im echten Spiel). queued_dx/dy: die zuletzt gewuenschte
    // Richtung - wird jeden Tick erneut versucht, bis sie frei ist. So kann
    // man kurz VOR einer Kreuzung abbiegen und muss nicht exakt den Frame treffen.
    int cur_dx = 1, cur_dy = 0;
    int queued_dx = 1, queued_dy = 0;

    clear_screen();

    while (1)
    {
        // --- Eingabe abfragen, mehrfach waehrend der Frame-Pause ---
        for (int i = 0; i < 40; i++)
        {
            uint8_t sc = poll_scancode();
            if (sc == 0x01)
            { // ESC
                return;
            }
            else if (sc == 0x48)
            {
                queued_dx = 0;
                queued_dy = -1;
            }
            else if (sc == 0x50)
            {
                queued_dx = 0;
                queued_dy = 1;
            }
            else if (sc == 0x4B)
            {
                queued_dx = -1;
                queued_dy = 0;
            }
            else if (sc == 0x4D)
            {
                queued_dx = 1;
                queued_dy = 0;
            }
            game_frame_delay(200000);
        }

        // --- Abbiegen: gewuenschte Richtung uebernehmen, sobald sie frei ist ---
        if (!pac_is_wall(maze, px + queued_dx, py + queued_dy))
        {
            cur_dx = queued_dx;
            cur_dy = queued_dy;
        }

        // --- Auto-Move: Pacman laeuft jeden Tick weiter, bis eine Wand kommt ---
        if (!pac_is_wall(maze, px + cur_dx, py + cur_dy))
        {
            px += cur_dx;
            py += cur_dy;
        }

        // --- Punkt einsammeln ---
        if (maze[py][px] == '.')
        {
            maze[py][px] = ' ';
            score++;
            dots_left--;
            if (dots_left <= 0)
            {
                win = 1;
                break;
            }
        }

        // --- Geister: 70% greedy Richtung Pacman, 30% zufaellig ---
        for (int g = 0; g < PAC_NUM_GHOSTS; g++)
        {
            int dirs_x[4] = {0, 0, -1, 1};
            int dirs_y[4] = {-1, 1, 0, 0};
            int use_random = (rng_next() % 100) < 30;

            if (use_random)
            {
                int valid[4];
                int valid_count = 0;
                for (int d = 0; d < 4; d++)
                {
                    valid[d] = !pac_is_wall(maze, gx[g] + dirs_x[d], gy[g] + dirs_y[d]);
                    if (valid[d])
                        valid_count++;
                }
                if (valid_count > 0)
                {
                    int pick = (int)rng_range((uint32_t)valid_count);
                    int seen = 0;
                    for (int d = 0; d < 4; d++)
                    {
                        if (valid[d])
                        {
                            if (seen == pick)
                            {
                                gx[g] += dirs_x[d];
                                gy[g] += dirs_y[d];
                                break;
                            }
                            seen++;
                        }
                    }
                }
            }
            else
            {
                int best_d = -1;
                int best_dist = 0x7fffffff;
                for (int d = 0; d < 4; d++)
                {
                    int nx = gx[g] + dirs_x[d];
                    int ny = gy[g] + dirs_y[d];
                    if (pac_is_wall(maze, nx, ny))
                        continue;
                    int ddx = nx - px;
                    int ddy = ny - py;
                    if (ddx < 0)
                        ddx = -ddx;
                    if (ddy < 0)
                        ddy = -ddy;
                    int dist = ddx + ddy; // Manhattan-Distanz
                    if (dist < best_dist)
                    {
                        best_dist = dist;
                        best_d = d;
                    }
                }
                if (best_d != -1)
                {
                    gx[g] += dirs_x[best_d];
                    gy[g] += dirs_y[best_d];
                }
            }
        }

        // --- Kollision mit Geist? ---
        for (int g = 0; g < PAC_NUM_GHOSTS; g++)
        {
            if (gx[g] == px && gy[g] == py)
            {
                win = 0;
                goto pac_game_over;
            }
        }

        // --- Zeichnen ---
        clear_screen();
        cursor_pos = 0;
        write_string("=== Pacman ===  Score: ", 0x0A);
        print_uint32(score, 0x0A);
        write_string("   ESC = Beenden\n", 0x0F);

        for (int y = 0; y < PAC_H; y++)
        {
            for (int x = 0; x < PAC_W; x++)
            {
                int sx = PAC_OFFSET_X + x;
                int sy = PAC_OFFSET_Y + y;
                char c = maze[y][x];
                uint8_t color = 0x09; // Waende: blau
                char draw = c;
                if (c == '.')
                {
                    color = 0x0E;
                    draw = 0xF9;
                } // Punkt: gelb
                else if (c == ' ')
                {
                    color = 0x00;
                    draw = ' ';
                }
                video[sy * 80 + sx] = (uint16_t)draw | ((uint16_t)color << 8);
            }
        }
        // Spieler und Geister ueber das Maze zeichnen
        video[(PAC_OFFSET_Y + py) * 80 + (PAC_OFFSET_X + px)] =
            (uint16_t)0x02 | ((uint16_t)0x0E << 8); // Pacman: gelbes Smiley
        for (int g = 0; g < PAC_NUM_GHOSTS; g++)
        {
            video[(PAC_OFFSET_Y + gy[g]) * 80 + (PAC_OFFSET_X + gx[g])] =
                (uint16_t)0x01 | ((uint16_t)0x0C << 8); // Geist: rotes Symbol
        }
    }

pac_game_over:
    clear_screen();
    if (win)
    {
        write_string("=== Gewonnen! ===\n\n", 0x0A);
    }
    else
    {
        write_string("=== Game Over ===\n\n", 0x0C);
    }
    write_string("Score: ", 0x0F);
    print_uint32(score, 0x0F);
    sleep(1);
    write_string("\n\nPress any key to return...\n", 0x0F);
    while (1)
    {
        if (inb(0x64) & 0x01)
        {
            inb(0x60);
            break;
        }
    }
}

static void game_launcher(void)
{
    char option[32];
    win_push_fullscreen();
    while ("1")
    {
        clear_screen();
        write_string("=== GAME LAUNCHER ===\n\n", 0x0A);
        write_string("1. Snakes\n", 0x0F);
        write_string("2. Pacman\n", 0x0F);
        write_string("3. Return to Shell\n", 0x0F);
        write_string("Select an option (1-3):\n", 0x0F);

        read_line(option, sizeof(option));

        if (kernel_strcmp(option, "1") == 0)
        {
            snakes();
        }
        else if (kernel_strcmp(option, "2") == 0)
        {
            pacman();
        }
        else if (kernel_strcmp(option, "3") == 0)
        {
            break;
        }
        else
        {
            write_string("Invalid option. Press any key to try again...\n", 0x0C);
            while (1)
            {
                uint8_t status = inb(0x64);
                if (status & 0x01)
                {
                    inb(0x60); // Read scancode to clear buffer
                    break;
                }
            }
        }
    }
    win_pop();
}

static void settings_cmd(const char *arg)
{
    (void)arg; // Unused
    settings_menu();
}

static void open_cmd_window(void)
{
    char cmd[128];

    win_push_fullscreen(); // sichert die Grenzen von VOR dem Fenster
    draw_window_frame(4, 1, 72, 22, " Tocdesk CMD ");
    win_clear();
    write_string("Tocdesk CMD\n", 0x0A);
    write_string("Type 'help' for commands, 'exit' to return to the desktop\n\n", 0x0F);

    while (1)
    {
        write_string("C:\\> ", 0x0B);
        read_line(cmd, sizeof(cmd));

        if (cmd[0] == '\0')
        {
            continue;
        }
        if (kernel_strcmp(cmd, "exit") == 0)
        {
            break;
        }

        // Kein Cursor-Reset mehr auf eine feste Position vor handle_command():
        // das schrieb die Kommando-Ausgabe (z.B. bei "help") bisher direkt
        // ueber den Fensterrahmen/Titel drueber. Jetzt laeuft die Ausgabe
        // einfach normal weiter (scrollt bei Bedarf INNERHALB des Fensters).
        handle_command(cmd);
        write_string("\nPress any key to continue (ESC = close window)...\n", 0x08);
        uint8_t key = 0;
        while (!key)
        {
            key = poll_scancode();
            if (key == 0)
            {
                delay_cycles(10000);
            }
        }
        if (key == 0x01)
        {
            win_pop();
            return;
        }
        write_string("\n", 0x0F);
    }

    win_pop();
}

// Die GUI-Settings waren bisher reine Attrappen (nur Labels, keine
// Funktion) - jetzt ein echtes Fenster, das dieselbe funktionierende Logik
// benutzt wie 'settings' in der CMD (Username/Passwort/Tastatur), plus
// echte Netzwerk-Verbindung/-Status.
static void open_settings_window(void)
{
    char option[32];

    win_push_fullscreen(); // sichert die Grenzen von VOR dem Fenster
    draw_window_frame(10, 2, 60, 18, " Settings ");
    win_clear();

    while (1)
    {
        win_clear();
        write_string("=== SETTINGS ===\n\n", 0x0A);
        write_string("1. Change Username\n", 0x0F);
        write_string("2. Change Password\n", 0x0F);
        write_string("3. Keyboard Layout\n", 0x0F);
        write_string("4. Network Connect\n", 0x0F);
        write_string("5. Network Status\n", 0x0F);
        write_string("6. Close\n\n", 0x0F);
        write_string("Select (1-6): ", 0x0B);

        read_line(option, sizeof(option));

        if (kernel_strcmp(option, "1") == 0)
        {
            settings_menu_username();
        }
        else if (kernel_strcmp(option, "2") == 0)
        {
            settings_menu_password();
        }
        else if (kernel_strcmp(option, "3") == 0)
        {
            settings_menu_keyboard();
        }
        else if (kernel_strcmp(option, "4") == 0)
        {
            network_connect_cmd();
            write_string("\nPress any key to continue...\n", 0x08);
            wait_any_key();
        }
        else if (kernel_strcmp(option, "5") == 0)
        {
            network_status_cmd();
            write_string("\nPress any key to continue...\n", 0x08);
            wait_any_key();
        }
        else if (kernel_strcmp(option, "6") == 0 || option[0] == '\0')
        {
            break;
        }
    }

    win_pop();
}

// Fuellt ein Rechteck mit einem Zeichen/einer Farbe - fuer schlichte,
// randlose Flaechen (Wallpaper, Icon-Kacheln), im Gegensatz zu draw_box()
// das immer einen Rahmen zeichnet.
static void fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, char c, uint8_t color)
{
    for (uint8_t yy = y; yy < y + h; yy++)
    {
        for (uint8_t xx = x; xx < x + w; xx++)
        {
            video[(uint32_t)yy * 80 + xx] = (uint16_t)c | ((uint16_t)color << 8);
        }
    }
}

// Prueft, ob der Punkt (mx,my) innerhalb der gegebenen Box liegt - fuer Mausklicks auf Icons.
static int point_in_box(int mx, int my, int x, int y, int w, int h)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void desktop_draw(void)
{
    clear_screen();

    // "Wallpaper": im VGA-Textmodus gibt es keinen Pixel-Framebuffer, nur
    // 80x25 Zeichenzellen mit je 16 Vordergrund-/16 Hintergrundfarben -
    // echte Bild-Hintergruende (JPG/PNG) sind damit technisch nicht
    // moeglich. Das ginge nur mit einem Wechsel in einen VGA-Grafikmodus
    // (z.B. Mode 13h, 320x200x256 Farben) - eine groessere separate
    // Umbau-Aktion, da darauf so ziemlich der ganze Kernel (Spiele, GUI,
    // Textausgabe) aufbaut. Bis dahin: ein einfarbiger Windows-10-blauer
    // Flaechenhintergrund als Ersatz.
    fill_rect(0, 0, 80, 22, ' ', 0x10); // Hintergrundfarbe 0x1 = Blau

    draw_text_at(2, 1, "Tocdesk OS", 0x1F);
    draw_text_at(2, 3, "Klick mit der Maus auf ein Icon, oder ENTER = CMD, TAB = Start", 0x1F);

    // CMD-Icon
    fill_rect(3, 6, 12, 6, ' ', 0x10);
    draw_text_at(7, 8, "[>_]", 0x1E);
    draw_text_at(6, 10, "CMD", 0x1F);

    // Firefox-Icon
    fill_rect(18, 6, 14, 6, ' ', 0x10);
    draw_text_at(23, 8, "(*)", 0x1C);
    draw_text_at(20, 10, "Firefox", 0x1F);

    // Settings-Icon
    fill_rect(34, 6, 14, 6, ' ', 0x10);
    draw_text_at(39, 8, "{*}", 0x17);
    draw_text_at(36, 10, "Settings", 0x1F);

    // Taskleiste (dunkler, wie bei Windows 10)
    fill_rect(0, 22, 80, 3, ' ', 0x08);
    draw_text_at(2, 23, "[Start]", 0x1F);
    draw_text_at(12, 23, "CMD", 0x0F);
    draw_text_at(20, 23, "Firefox", 0x0F);
    draw_text_at(34, 23, "Settings", 0x0F);
    draw_text_at(68, 23, "10:00", 0x0F);
}

// Wartet auf einen Tastendruck und verwirft ihn (fuer "Press any key..."-Screens).
static void wait_any_key(void)
{
    while (1)
    {
        if (inb(0x64) & 0x01)
        {
            inb(0x60);
            return;
        }
    }
}

// Fragt Username + Passwort ab und prueft sie WIRKLICH gegen die
// gespeicherten Werte. Gibt 1 zurueck bei Erfolg, 0 bei Fehlschlag.
static int login_attempt(void)
{
    char user_buf[128];
    char pass_buf[128];

    clear_screen();
    draw_box(14, 4, 52, 14, " Login ", 0x0B, 0x00);
    draw_text_at(18, 6, "Tocdesk OS", 0x0E);
    draw_text_at(18, 8, "Username:", 0x0F);
    set_cursor_xy(30, 8);
    read_line(user_buf, sizeof(user_buf));

    draw_text_at(18, 10, "Password:", 0x0F);
    set_cursor_xy(30, 10);
    read_line_masked(pass_buf, sizeof(pass_buf));

    if (kernel_strcmp(user_buf, username) == 0 && kernel_strcmp(pass_buf, password) == 0)
    {
        draw_text_at(18, 12, "Access granted", 0x0A);
        return 1;
    }

    draw_text_at(18, 12, "Access denied", 0x0C);
    sleep(1);
    draw_text_at(18, 14, "Press any key to try again", 0x0F);
    wait_any_key();
    return 0;
}

int lock_screen(void)
{
    login_attempt();
}

static void open_start_menu(void)
{
    ps2_mouse_init();
    mouse_last_buttons = 0;
    int last_mouse_x = -1, last_mouse_y = -1;

    // Kein clear_screen() hier: das Startmenue soll als kleines Panel UEBER
    // dem Desktop erscheinen (wie bei Windows), nicht den ganzen Bildschirm
    // durch eine eigene Vollbild-Anzeige ersetzen. Der Desktop im Hintergrund
    // bleibt sichtbar - draw_box() zeichnet einfach oben drauf.
    draw_box(0, 5, 30, 17, " Start ", 0x0B, 0x00);
    draw_text_at(2, 7, "Installed Apps", 0x0E);
    draw_text_at(2, 9, "- CMD", 0x0F);
    draw_text_at(2, 11, "- Firefox", 0x0F);
    draw_text_at(2, 13, "- Settings", 0x0F);
    draw_text_at(2, 15, "Power", 0x0E);
    draw_text_at(2, 17, "- Restart", 0x0F);
    draw_text_at(2, 19, "- Shutdown", 0x0F);
    draw_text_at(2, 20, "- Lock", 0x0F);
    draw_mouse_cursor();

    while (1)
    {
        uint8_t sc = gui_poll_input();
        if (sc == 0)
        {
            delay_cycles(10000);
            continue;
        }
        if (sc == 0x1C)
        {
            open_cmd_window();
            break;
        }
        else if (sc == 0x02)
        {
            if (app_is_installed("firefox"))
                launch_gui_app("firefox");
            break;
        }
        else if (sc == 0x03)
        {
            open_settings_window();
            break;
        }
        else if (sc == 0x04)
        {
            reboot();
            break;
        }
        else if (sc == 0x05)
        {
            shutdown_cmd();
            break;
        }
        else if (sc == 0x06)
        {
            clear_screen();
            lock_screen();
            break;
        }
        else if (sc == 0x01)
        {
            break;
        }
    }
}

static void desktop_loop(void)
{
    ps2_mouse_init();
    mouse_last_buttons = 0;
    int last_mouse_x = -1, last_mouse_y = -1;

    desktop_draw();
    draw_mouse_cursor();

    while (1)
    {
        uint8_t sc = gui_poll_input();

        // Tastatur-Kurzbefehle
        if (sc == 0x1C || sc == 0x3B)
        { // Enter oder F1
            open_cmd_window();
            desktop_draw();
            draw_mouse_cursor();
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
            continue;
        }
        else if (sc == 0x0F)
        { // Tab
            open_start_menu();
            desktop_draw();
            draw_mouse_cursor();
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
            continue;
        }
        else if (sc == 0x01)
        { // Esc
            break;
        }

        // Mausklick auf eines der Icons (Desktop-Box oder Taskleiste)
        int clicked = (mouse_buttons & 0x01) && !(mouse_last_buttons & 0x01);
        mouse_last_buttons = mouse_buttons;

        if (clicked)
        {
            int action = 0;
            if (point_in_box(mouse_x, mouse_y, 0, 22, 10, 3))
            {
                action = 4; // Start-Button -> Startmenue
            }
            else if (point_in_box(mouse_x, mouse_y, 3, 6, 12, 6) || point_in_box(mouse_x, mouse_y, 10, 22, 10, 3))
            {
                action = 1; // CMD
            }
            else if (point_in_box(mouse_x, mouse_y, 18, 6, 14, 6) || point_in_box(mouse_x, mouse_y, 20, 22, 14, 3))
            {
                action = 2; // Firefox
            }
            else if (point_in_box(mouse_x, mouse_y, 34, 6, 14, 6) || point_in_box(mouse_x, mouse_y, 34, 22, 14, 3))
            {
                action = 3; // Settings
            }

            if (action == 1)
            {
                open_cmd_window();
            }
            else if (action == 2)
            {
                if (app_is_installed("firefox"))
                    launch_gui_app("firefox");
            }
            else if (action == 3)
            {
                open_settings_window();
            }
            else if (action == 4)
            {
                open_start_menu();
            }
            desktop_draw();
            draw_mouse_cursor();
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
            continue;
        }

        // Nur neu zeichnen, wenn sich die Maus tatsaechlich in eine neue
        // Zelle bewegt hat - das war der Grund fuer das Geflacker: vorher
        // wurde JEDE Schleifeniteration (also tausende Male pro Sekunde,
        // ganz ohne Pause) der komplette Bildschirm neu gezeichnet.
        if (mouse_x != last_mouse_x || mouse_y != last_mouse_y)
        {
            desktop_draw();
            draw_mouse_cursor();
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
        }

        if (sc == 0)
        {
            delay_cycles(10000);
        }
    }
}

static void handle_command(const char *cmd)
{
    if (kernel_strcmp(cmd, "help") == 0)
    {
        write_string("Available commands:\n", 0x0A);
        write_string("  help         - Show this help\n", 0x0F);
        write_string("  clear        - Clear screen\n", 0x0F);
        write_string("  echo <text>  - Echo text\n", 0x0F);
        write_string("  info         - System info\n", 0x0F);
        write_string("  reboot       - Reboot system\n", 0x0F);
        write_string("  shutdown     - Shutdown system\n", 0x0F);
        write_string("  lock         - brings you to the Login Screen\n", 0x0F);
        write_string("  settings     - Show/change settings\n", 0x0F);
        write_string("  install <app>- Install a supported app\n", 0x0F);
        write_string("  apps         - List installed apps\n", 0x0F);
        write_string("  net connect      - Connect to the network\n", 0x0F);
        write_string("  net status       - Show network status\n", 0x0F);
        write_string("  ping <host>      - Send an ICMP ping request\n", 0x0F);
        write_string("  http get <url>   - Send a simple HTTP GET request\n", 0x0F);
        write_string("  security on|off  - Enable/disable firewall\n", 0x0F);
        write_string("  firewall allow   - Allow trusted repository traffic\n", 0x0F);
        write_string("  firewall block   - Block untrusted traffic\n", 0x0F);
        write_string("  firefox      - Open Firefox GUI stub\n", 0x0F);
        write_string("  notes        - Show notes\n", 0x0F);
        write_string("  explorer     - Browse/open/delete saved notes\n", 0x0F);
        write_string("  games        - Opens the game launcher\n", 0x0F);
    }
    else if (kernel_strcmp(cmd, "clear") == 0)
    {
        clear_screen();
    }
    else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == ' ')
    {
        write_string(&cmd[5], 0x0F);
        write_string("\n", 0x0F);
    }
    else if (kernel_strcmp(cmd, "info") == 0)
    {
        write_string("Tocdesk OS v8.8\n", 0x0A);
        write_string("i386 Protected Mode with ACPI\n", 0x0F);
    }
    else if (kernel_strcmp(cmd, "reboot") == 0)
    {
        reboot();
    }
    else if (kernel_strcmp(cmd, "shutdown") == 0)
    {
        shutdown_cmd();
    }
    else if (cmd[0] == 's' && cmd[1] == 'e' && cmd[2] == 't' && cmd[3] == 't' && cmd[4] == 'i' && cmd[5] == 'n' && cmd[6] == 'g' && cmd[7] == 's')
    {
        settings_cmd(&cmd[8]);
    }
    else if (starts_with(cmd, "install"))
    {
        install_cmd(skip_spaces(&cmd[7]));
    }
    else if (kernel_strcmp(cmd, "apps") == 0)
    {
        list_installed_apps();
    }
    else if (kernel_strcmp(cmd, "net") == 0 || starts_with(cmd, "net "))
    {
        if (starts_with(cmd, "net connect"))
        {
            network_connect_cmd();
        }
        else if (starts_with(cmd, "net status"))
        {
            network_status_cmd();
        }
        else
        {
            write_string("Usage: net connect | net status\n", 0x0E);
        }
    }
    else if (starts_with(cmd, "ping "))
    {
        network_ping_cmd(&cmd[5]);
    }
    else if (starts_with(cmd, "http get "))
    {
        network_http_get_cmd(&cmd[9]);
    }
    else if (starts_with(cmd, "security"))
    {
        if (starts_with(cmd, "security on") || starts_with(cmd, "security enable"))
        {
            security_toggle_cmd("on");
        }
        else if (starts_with(cmd, "security off") || starts_with(cmd, "security disable"))
        {
            security_toggle_cmd("off");
        }
        else
        {
            security_status_cmd();
        }
    }
    else if (starts_with(cmd, "firewall"))
    {
        firewall_cmd(skip_spaces(&cmd[8]));
    }
    else if (kernel_strcmp(cmd, "notes") == 0)
    {
        notes_app();
    }
    else if (kernel_strcmp(cmd, "explorer") == 0)
    {
        explorer_app();
    }
    else if (kernel_strcmp(cmd, "games") == 0)
    {
        game_launcher();
    }
    else if (kernel_strcmp(cmd, "firefox") == 0)
    {
        launch_gui_app("firefox");
    }
    else if (kernel_strcmp(cmd, "lock") == 0)
    {
        lock_screen();
    }
    else if (cmd[0] != '\0')
    {
        write_string("Unknown command: ", 0x0C);
        write_string(cmd, 0x0C);
        write_string("\n", 0x0C);
    }
}

static void shell(void)
{
    while (1)
    {
        if (!login_attempt())
        {
            continue;
        }
        desktop_loop();
        break;
    }
}

void kmain(void)
{
    clear_screen();
    credentials_load();
    ps2_mouse_init();
    shell();
}