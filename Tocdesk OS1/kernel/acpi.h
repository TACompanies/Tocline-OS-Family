#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

// ACPI Signature
typedef struct {
    uint8_t signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) ACPI_HEADER;

// RSDP (Root System Description Pointer)
typedef struct {
    uint8_t signature[8];
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
} __attribute__((packed)) RSDP;

// FADT (Fixed ACPI Description Table)
typedef struct {
    ACPI_HEADER header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
} __attribute__((packed)) FADT;

// RSDT (Root System Description Table)
typedef struct {
    ACPI_HEADER header;
    uint32_t table_offsets[];
} __attribute__((packed)) RSDT;

RSDP* acpi_find_rsdp(void);
FADT* acpi_get_fadt(void);
void acpi_power_off(void);

#endif
