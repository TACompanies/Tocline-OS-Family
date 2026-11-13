// ata_pio.c
#include "ata_pio.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

static void ata_wait_bsy_clear(uint16_t ctrl_base) {
    // BSY (Status bit 7) soll 0 werden
    while (inb(ctrl_base + 0x07) & 0x80) {
        io_wait();
    }
}

static int ata_wait_drq_set(uint16_t ctrl_base, uint32_t spins) {
    // DRQ (Status bit 3) soll 1 werden
    while (spins--) {
        uint8_t st = inb(ctrl_base + 0x07);
        if (st & 0x08) return 0;
        if (st & 0x01) return -1; // ERR
        io_wait();
    }
    return -2; // timeout
}

static int ata_wait_err_free(uint16_t ctrl_base, uint32_t spins) {
    while (spins--) {
        uint8_t st = inb(ctrl_base + 0x07);
        if (st & 0x01) return -1; // ERR
        if (!(st & 0x80)) return 0; // !BSY
        io_wait();
    }
    return -2;
}

int ata_identify(uint16_t ctrl_base, uint16_t *out_serial_buf, size_t serial_buf_words) {
    // ctrl_base: Primary = 0x1F0, Secondary = 0x170
    // drive: master/slave gesetzt in DEV register

    // Hier nur Master als Beispiel:
    outb(ctrl_base + 0x06, 0xA0); // Drive/Head: 0xA0 master

    outb(ctrl_base + 0x07, 0x00); // Features = 0
    outb(ctrl_base + 0x02, 0x00); // Sector count = 0
    outb(ctrl_base + 0x03, 0x00); // LBA low
    outb(ctrl_base + 0x04, 0x00); // LBA mid
    outb(ctrl_base + 0x05, 0x00); // LBA high
    outb(ctrl_base + 0x01, 0xEC); // Command: IDENTIFY DEVICE (0xEC)

    ata_wait_bsy_clear(ctrl_base);
    if (ata_wait_drq_set(ctrl_base, 1000000) != 0) return -1;

    // IDENTIFY liefert 256 WORDs im Datenregister (0x1F0)
    uint16_t words[256];
    for (int i = 0; i < 256; i++) {
        words[i] = inw(ctrl_base + 0x00);
    }

    // Serial number steht in Worten 10..19 (20 bytes),
    // oft in 2-Byte little-endian gepaart. Wir kopieren/entpacken minimal:
    // word index 10..19 => 10 words => 20 chars
    int idx = 0;
    for (int w = 10; w <= 19 && idx < (int)serial_buf_words; w++) {
        out_serial_buf[idx++] = words[w];
    }
    return 0;
}

static int ata_set_lba_pio28(uint16_t ctrl_base, uint8_t drive, uint32_t lba) {
    // drive: 0 = master, 1 = slave
    // Drive/Head register: 0xA0 | ((drive) << 4) | ((lba >> 24) & 0x0F)
    uint8_t dh = 0xA0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F);
    outb(ctrl_base + 0x06, dh);

    outb(ctrl_base + 0x02, 1);            // Sector count (count) set by caller typically
    outb(ctrl_base + 0x03, (uint8_t)(lba & 0xFF));         // LBA low
    outb(ctrl_base + 0x04, (uint8_t)((lba >> 8) & 0xFF));  // LBA mid
    outb(ctrl_base + 0x05, (uint8_t)((lba >> 16) & 0xFF)); // LBA high
    return 0;
}

int read_sectors_pio28(uint16_t ctrl_base, uint8_t drive, uint32_t lba,
                        uint8_t count, void *buf) {
    if (!count) return -3;

    // Nur 28-bit LBA
    // BSY -> wait, set registers
    ata_wait_bsy_clear(ctrl_base);

    uint8_t dh = 0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F);
    outb(ctrl_base + 0x06, dh);

    outb(ctrl_base + 0x02, count);               // Sector count
    outb(ctrl_base + 0x03, (uint8_t)(lba & 0xFF));
    outb(ctrl_base + 0x04, (uint8_t)((lba >> 8) & 0xFF));
    outb(ctrl_base + 0x05, (uint8_t)((lba >> 16) & 0xFF));

    outb(ctrl_base + 0x07, 0x20); // Command: READ SECTORS (0x20)

    uint8_t *dst = (uint8_t*)buf;

    for (uint8_t s = 0; s < count; s++) {
        ata_wait_bsy_clear(ctrl_base);
        if (ata_wait_drq_set(ctrl_base, 1000000) != 0) return -1;

        // 512 bytes = 256 WORDs
        for (int i = 0; i < 256; i++) {
            uint16_t w = inw(ctrl_base + 0x00);
            dst[0] = (uint8_t)(w & 0xFF);
            dst[1] = (uint8_t)(w >> 8);
            dst += 2;
        }
    }
    return 0;
}

int write_sectors_pio28(uint16_t ctrl_base, uint8_t drive, uint32_t lba,
                         uint8_t count, const void *buf) {
    if (!count) return -3;

    ata_wait_bsy_clear(ctrl_base);

    uint8_t dh = 0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F);
    outb(ctrl_base + 0x06, dh);

    outb(ctrl_base + 0x02, count);
    outb(ctrl_base + 0x03, (uint8_t)(lba & 0xFF));
    outb(ctrl_base + 0x04, (uint8_t)((lba >> 8) & 0xFF));
    outb(ctrl_base + 0x05, (uint8_t)((lba >> 16) & 0xFF));

    outb(ctrl_base + 0x07, 0x30); // Command: WRITE SECTORS (0x30)

    const uint8_t *src = (const uint8_t*)buf;

    for (uint8_t s = 0; s < count; s++) {
        ata_wait_bsy_clear(ctrl_base);

        // Pro Write: warte bis DRQ gesetzt
        if (ata_wait_drq_set(ctrl_base, 1000000) != 0) return -1;

        for (int i = 0; i < 256; i++) {
            uint16_t w = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
            outw(ctrl_base + 0x00, w);
            src += 2;
        }

        // Nach dem Ausgeben: BSY werden + ggf warten bis fertig
        ata_wait_err_free(ctrl_base, 1000000);
    }

    return 0;
}
