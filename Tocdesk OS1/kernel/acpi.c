#include "acpi.h"

RSDP* acpi_find_rsdp(void) {
    uint8_t *ptr = (uint8_t *)0xE0000;
    uint8_t *end = (uint8_t *)0x100000;

    while (ptr < end) {
        if (ptr[0] == 'R' && ptr[1] == 'S' && ptr[2] == 'D' && ptr[3] == ' ' &&
            ptr[4] == 'P' && ptr[5] == 'T' && ptr[6] == 'R' && ptr[7] == ' ') {
            return (RSDP *)ptr;
        }
        ptr += 16;
    }
    return 0;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

// Durchsucht die RSDT nach der FADT-Tabelle (Signatur "FACP").
FADT* acpi_get_fadt(void) {
    RSDP *rsdp = acpi_find_rsdp();
    if (!rsdp) return 0;

    RSDT *rsdt = (RSDT *)(uintptr_t)rsdp->rsdt_address;
    if (!rsdt) return 0;

    uint32_t entry_count = (rsdt->header.length - sizeof(ACPI_HEADER)) / 4;

    for (uint32_t i = 0; i < entry_count; i++) {
        ACPI_HEADER *table = (ACPI_HEADER *)(uintptr_t)rsdt->table_offsets[i];
        if (table->signature[0] == 'F' && table->signature[1] == 'A' &&
            table->signature[2] == 'C' && table->signature[3] == 'P') {
            return (FADT *)table;
        }
    }
    return 0;
}

static inline uint16_t acpi_build_sleep_value(uint8_t sleep_type) {
    // SLP_EN = Bit 13, SLP_TYP = Bits 10..12
    return (uint16_t)((1u << 13) | ((uint16_t)(sleep_type & 0x7u) << 10));
}

// Sucht das Paket "\_S5" im DSDT und liest SLP_TYPa/SLP_TYPb aus den
// rohen AML-Bytes (kein vollstaendiger AML-Interpreter, sondern die
// klassische minimalinvasive Suche, siehe OSDev-Wiki "Shutdown").
// Gibt 1 bei Erfolg zurueck, 0 wenn "_S5_" nicht gefunden wurde.
static int acpi_find_s5(FADT *fadt, uint8_t *out_typ_a, uint8_t *out_typ_b) {
    if (!fadt || !fadt->dsdt) return 0;

    uint8_t *dsdt = (uint8_t *)(uintptr_t)fadt->dsdt;
    uint32_t dsdt_length = *(uint32_t *)(dsdt + 4);  // ACPI_HEADER.length

    uint8_t *s5 = dsdt + sizeof(ACPI_HEADER);
    uint8_t *end = dsdt + dsdt_length;

    while (s5 < end - 4) {
        if (s5[0] == '_' && s5[1] == 'S' && s5[2] == '5' && s5[3] == '_') {
            break;
        }
        s5++;
    }
    if (s5 >= end - 4) return 0;  // "_S5_" nicht gefunden

    // Nach "_S5_" folgt: optionales NameOp(0x08), dann PackageOp(0x12),
    // PkgLength, NumElements, dann die beiden SLP_TYP-Werte.
    s5 += 4;
    if (*s5 == 0x08) s5++;
    if (*s5 != 0x12) return 0;
    s5++;

    s5 += ((*s5 & 0xC0) >> 6) + 1;  // PkgLength-Bytes ueberspringen
    s5++;                            // NumElements-Byte ueberspringen

    if (*s5 == 0x0A) s5++;  // BytePrefix
    *out_typ_a = *s5;
    s5++;

    if (*s5 == 0x0A) s5++;  // BytePrefix
    *out_typ_b = *s5;

    return 1;
}

void acpi_power_off(void) {
    __asm__("cli");

    FADT *fadt = acpi_get_fadt();
    // Fallback, falls kein DSDT-Parsing moeglich ist: Typ 5 ist auf
    // vielen QEMU/Bochs-Systemen der uebliche S5-Wert.
    uint8_t typ_a = 5, typ_b = 5;

    if (fadt) {
        acpi_find_s5(fadt, &typ_a, &typ_b);

        if (fadt->pm1a_cnt_blk) {
            outw((uint16_t)fadt->pm1a_cnt_blk, acpi_build_sleep_value(typ_a));
        }
        if (fadt->pm1b_cnt_blk) {
            outw((uint16_t)fadt->pm1b_cnt_blk, acpi_build_sleep_value(typ_b));
        }
    }

    // Wenn ACPI nicht greift (kein FADT gefunden, Emulator ignoriert den
    // Port-Write, o.ae.): CPU anhalten als sicherer Fallback.
    for (;;) __asm__("hlt");
}
