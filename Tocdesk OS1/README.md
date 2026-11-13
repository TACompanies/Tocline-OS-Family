# Tocdesk OS

Minimale x86-Betriebssystem-Grundstruktur mit GRUB-bootfähiger ISO.

## Projektstruktur

| Datei | Zweck |
|-------|-------|
| `kernel/kernel.c` | C-Kernel mit VGA-Ausgabe und Tastatur-Polling |
| `kernel/start.S` | Multiboot-Einstiegspunkt (setzt Stack, ruft kmain auf) |
| `include/kernel.h` | Kernel-Header |
| `iso/boot/grub/grub.cfg` | GRUB-Bootloader-Konfiguration |
| `build/linker.ld` | Linker-Skript für Kernel-Layout |
| `Makefile` | Build-System (compiliert, linked, erstellt ISO) |

## Ausführungsfluss

Hier wird genau erklärt, wie der Code vom Start bis zur Tastatur-Eingabe läuft:

### 1. **GRUB lädt die ISO**
```
Boot-Media (ISO) → GRUB-Bootloader → Liest /boot/tocdesk.elf
```
- GRUB parsiert `iso/boot/grub/grub.cfg`
- Lädt den Kernel (ELF-Format) ins Memory
- Sprung zu Multiboot-Einstiegspunkt

### 2. **Kernel-Start (kernel/start.S)**
```asm
_start:
  mov $0x90000, %esp       # Stack-Pointer auf 0x90000 setzen
  xor %ebp, %ebp          # Base-Pointer nullen
  call kmain              # Sprung zu C-Kernel
  cli                     # Interrupts deaktivieren
  hlt                     # CPU halte Loop
```
**Was passiert:**
- Assembler-Code richtet einen Stack ein
- Übergibt Kontrolle an `kmain()` in C

### 3. **Kernel-Main (kernel/kernel.c → kmain)**
```c
void kmain(void) {
    write_string("Tocdesk OS booted!\n", 0x0F);
    wait_for_keyboard();
}
```
**Ablauf:**
1. **Boot-Nachricht anzeigen**: Schreibt Text ins VGA-Videomemory (`0xB8000`)
2. **Tastatur-Loop starten**: Wartet auf Eingaben

### 4. **VGA-Textmodus Ausgabe**
```c
static volatile uint16_t *const video = (uint16_t *)0xB8000;

put_char(char c, uint8_t color):
  video[cursor_pos++] = (uint16_t)c | ((uint16_t)color << 8);
```
**Speicherformat:** Jedes Zeichen braucht 2 Bytes
- **Byte 1**: ASCII-Code (Charakter)
- **Byte 2**: Farbe (Vordergrund/Hintergrund)
- Screen hat 80×25 = 2000 Zeichen Platz
- Adresse `0xB8000` ist das VGA-Textmemory

### 5. **Tastatur-Input (PS/2-Port)**
```c
void wait_for_keyboard(void):
  status = inb(0x64)      # Lese Status-Register (Port 0x64)
  if (status & 0x01):     # Bit 0 = Daten verfügbar?
    scancode = inb(0x60)  # Lese Scancode (Port 0x60)
    if (scancode < 128):
      put_char(scancode_map[scancode], color)
```
**Hardwarekommunikation:**
- **Port 0x64**: PS/2-Controller Status
  - Bit 0 = 1: Daten im Input-Buffer
- **Port 0x60**: PS/2 Daten-Register
  - Lesen ergibt den Scancode
- **scancode_map[]**: Mapping von Scancode → ASCII (z.B. 0x10 = 'q')

## Speicherlayout

```
0x00000000 ┌─────────────────┐
           │  Real Mode IVT  │
           └─────────────────┘
...
0x00090000 ┌─────────────────┐
           │  Kernel Stack   │  ← esp (Stack-Pointer)
           └─────────────────┘
...
0x000B8000 ┌─────────────────┐
           │  VGA Textmemory │  ← Video Output (80×25)
           └─────────────────┘
...
0x01000000 ┌─────────────────┐
           │  Kernel Code/Data
           │  (Multiboot)    │
           └─────────────────┘
```

## Voraussetzungen & Installation

### Windows Setup

**Was du brauchst:**

1. **GCC & LD (i386 Cross-Compiler)**
   - Option A: [MinGW-w64](https://www.mingw-w64.org/) mit i686-Unterstützung
   - Option B: [WSL2](https://learn.microsoft.com/de-de/windows/wsl/) mit Linux (empfohlen)
   - Check: `gcc --version` sollte funktionieren

2. **NASM (Assembler)** - nur wenn Assembler-Code vorhanden ist
   - Download: [nasm.us](https://www.nasm.us/download.html)
   - Check: `nasm -version`

3. **Make**
   - Mit MinGW: `mingw-get install mingw32-make`
   - Mit WSL2: `sudo apt install build-essential`
   - Check: `make --version`

4. **GRUB mkrescue** (für ISO-Erstellung)
   - Mit MinGW: `mingw-get install grub2`
   - Mit WSL2: `sudo apt install grub-pc-bin xorriso mtools`
   - Check: `grub-mkrescue --version`

5. **QEMU** (optional, zum Testen)
   - Download: [qemu.org](https://www.qemu.org/download/)
   - Check: `qemu-system-i386 --version`

### Schnell-Check: Sind alle Tools da?

```bash
gcc --version
ld --version
make --version
grub-mkrescue --version
qemu-system-i386 --version  # optional
```

Alle sollten funktionieren. Falls nicht → siehe Fehler-Lösungen unten.

## Build & Run

### Schritt 0: In den Projektordner gehen

**Wichtig:** Der Ordner hat Leerzeichen! Eine dieser Optionen verwenden:

**Option A: Mit Anführungszeichen (empfohlen)**
```powershell
cd "Tocdesk OS"
```

**Option B: Mit vollständigen Pfad**
```powershell
cd "/mnt/c/Users/Tom/Documents/Zuhause/Coding/Tocline OS/Tocdesk OS"
```

**Option C: Mit cd / (aus Verzeichnis oben)**
```powershell
cd .\Tocdesk\ OS
```

Jetzt sollte das Prompt zeigen: `Tocdesk OS\`

### Schritt 1: Tools überprüfen

Prüfe, ob alle Werkzeuge vorhanden sind:
```powershell
gcc --version
make --version
grub-mkrescue --version
qemu-system-i386 --version
```

**Falls etwas fehlt:** Siehe Fehler-Lösungen unten und installiere die Tools.

### Schritt 2: Quellcode überprüfen
```powershell
ls kernel/kernel.c kernel/start.S iso/boot/grub/grub.cfg
```
Alle drei Dateien sollten vorhanden sein.

### Schritt 2: ISO bauen
```bash
make
```
**Erwartet:** 
- Dateien in `obj/` und `bin/` werden erstellt
- `bin/tocdesk.iso` entsteht
- ✅ Erfolg = "Wrote..." Nachricht oder ISO-Datei größer 0 Bytes

### Schritt 3: In QEMU testen
```bash
make run
```
**Erwartet:**
- QEMU öffnet sich
- Schwarzer Bildschirm → "Tocdesk OS booted!"
- Du kannst Tasten drücken und sie erscheinen

### Schritt 4: Aufräumen
```bash
make clean
```
Entfernt alle erstellten Dateien.

## Häufige Fehler & Lösungen

| Fehler | Lösung |
|--------|--------|
| `gcc: command not found` | GCC nicht installiert → MinGW/WSL Setup |
| `make: command not found` | Make nicht installiert → Build-Tools installieren |
| `grub-mkrescue: command not found` | GRUB nicht installiert → `apt install grub-pc-bin xorriso` |
| `ld: cannot find -lc` | i386-Libs fehlen → `apt install gcc-multilib` (WSL) |
| QEMU startet nicht | QEMU nicht installiert → Download von qemu.org |
| ISO größe = 0 Bytes | Build-Fehler → `make clean` dann `make` nochmal |

## Build-Prozess (was genau passiert)

```
make aufgerufen
    ↓
1. C-Dateien kompilieren: kernel/*.c → obj/kernel/*.o
2. Assembly kompilieren: kernel/*.S → obj/kernel/*.o
3. Alle .o-Dateien linken: ld → bin/kernel.elf
4. GRUB-Ordner vorbereiten: iso/boot/grub/
5. kernel.elf kopieren → iso/boot/tocdesk.elf
6. grub.cfg kopieren → iso/boot/grub/grub.cfg
7. ISO erstellen: grub-mkrescue → bin/tocdesk.iso
    ↓
✅ Fertig: bin/tocdesk.iso vorhanden
```

## Speicherlayout

```sh
make
```

Danach liegt das Boot-Image in `bin/tocdesk.img`.

Die Datei kann direkt mit `make run` gestartet werden, wenn `qemu-system-i386` installiert ist.

```sh
make run
```

### ISO erstellen (optional)

Wenn du eine ISO möchtest, installiere `grub-mkrescue` und führe aus:

```sh
make iso
```

Die ISO erscheint als `bin/tocdesk.iso`.

Um die ISO direkt in QEMU zu starten, verwende:

```sh
make runiso
```

## Test mit QEMU

Wenn `qemu-system-i386` installiert ist, kannst du das Image starten mit:

```sh
qemu-system-i386 -drive format=raw,file=bin/tocdesk.img
```

> Hinweis: Diese Anleitung zeigt nur eine sehr frühe Testphase eines Betriebssystems. Ein echtes OS braucht Bootloader, Speicherverwaltung, Interrupt-Handling, Dateisysteme und mehr.

## Nächste Schritte

- Einen echten Bootloader hinzufügen (z. B. GRUB-Konfiguration oder eigener Bootsektor).
- Speicherverwaltung mit Page Tables implementieren.
- Interrupt- und Fehlerbehandlung hinzufügen.
- Eingabe über Tastatur und einfache Shell implementieren.

## Dateibeschreibung

- `Makefile` kompiliert `.c`, `.S` und `.asm` Dateien in `obj/` und verlinkt sie.
- `include/kernel.h` stellt den Kernel-Einstieg als externe Funktion zur Verfügung.
- `kernel/kernel.c` nutzt VGA-Textmodus bei `0xB8000`, um Text auszugeben.
