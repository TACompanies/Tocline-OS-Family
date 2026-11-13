// ata_pio.h
#pragma once
#include <stdint.h>
#include <stddef.h>

int ata_identify(uint16_t ctrl_base, uint16_t *out_serial_buf, size_t serial_buf_words);
int read_sectors_pio28(uint16_t ctrl_base, uint8_t drive, uint32_t lba,
                        uint8_t count, void *buf);
int write_sectors_pio28(uint16_t ctrl_base, uint8_t drive, uint32_t lba,
                         uint8_t count, const void *buf);
