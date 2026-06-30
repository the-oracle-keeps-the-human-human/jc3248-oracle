# Hardware Reference — Guition JC3248W535 (ESP32-S3)

> Read directly off the connected board via `esptool`/`espefuse` on 2026-05-29
> (read-only). Unit MAC `98:a3:16:e3:ec:c8`, on `/dev/cu.usbmodem13301`.
> Reproduce with `make chipid` (see repo root `Makefile`).

This is the target board for **all** firmware in this repo: `beacon-display`,
`jc3248w535` (brewing dashboard), `heartbeat`, and the WIP `buddy-port`.

## Silicon identity

| Field | Value | Decoded |
|---|---|---|
| Chip | **ESP32-S3** (QFN56) | `PKG_VERSION 0` = 56-pin QFN, no in-package flash |
| Revision | **v0.2** | wafer major 0, minor 2 |
| MAC (base) | `98:a3:16:e3:ec:c8` | OUI `98:a3:16` = Espressif |
| Crystal | 40 MHz | |

## Cores & compute

- **Dual-core Xtensa LX7 @ up to 240 MHz** + separate **LP / ULP RISC-V**
  coprocessor (esptool: "Dual Core + LP Core").
- **128-bit vector/SIMD instructions** — the S3's signature feature; accelerates
  ML/DSP (wake-word, image inference).
- 512 KB internal SRAM · 384 KB ROM · 16 KB RTC SRAM.

## Memory — this is an **N16R8** module

| | Value | Decoded |
|---|---|---|
| Flash | **16 MB** QSPI NOR | mfr `0x68`, device `0x4018` → `0x18` = 2²⁴ = 16 MiB (Boya/BoHong) |
| PSRAM | **8 MB embedded, octal** | `PSRAM_CAP=8M`, `PSRAM_VENDOR=AP_3v3` (AP Memory, 3.3 V), 85 °C rated |
| Pin power | `PIN_POWER_SELECTION = VDD_SPI` | GPIO33–37 powered from the octal flash/PSRAM rail → **not free GPIO** |

PSRAM headroom matters: `buddy-port`'s GIF feature needs a ~300 KB
`Arduino_Canvas` framebuffer, and LVGL double-buffering on the 320×480 panel
benefits from the 8 MB.

## Connectivity & I/O

- **Wi-Fi** 802.11 b/g/n (2.4 GHz) + **Bluetooth 5 (LE)**.
- **Native USB**: USB-OTG 1.1 **and** USB-Serial-JTAG → enumerates as
  `/dev/cu.usbmodem*` (no CP210x/CH340 bridge); esptool connects with no
  boot-button dance.
- LCD_CAM peripheral (QSPI/parallel LCD + DVP camera), 4× SPI, I²S, RMT,
  TWAI/CAN, 2× SAR ADC (8×12-bit), 14-ch touch, LEDC PWM, ~45 GPIO.

## Display & touch (board-level)

| Subsystem | Detail |
|---|---|
| Panel | AXS15231B **320×480 IPS**, **QSPI**, RGB, 40 MHz, no color invert |
| QSPI pins | `clk=47`, `data=[21,48,40,39]`, `cs=45` |
| Backlight | GPIO **1**, LEDC PWM (5 kHz) |
| Touch | AXS15231B capacitive, **I²C 0x3B**, `SDA=4 SCL=8 INT=3`, 5-point |

Touch protocol command (raw): `{0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x08}`,
8-byte response with 12-bit coordinates.

## Security / eFuse state — **open dev chip**

| Fuse | State |
|---|---|
| Secure Boot (`SECURE_BOOT_EN`) | ❌ disabled |
| Flash Encryption (`SPI_BOOT_CRYPT_CNT`) | ❌ disabled (`0b000`) |
| JTAG (`DIS_PAD_JTAG`/`DIS_USB_JTAG`) | enabled |
| Download mode | enabled |
| USB-Serial-JTAG | enabled |

Fully flashable, no locks. Factory **ADC calibration** present (ATTEN0–3 for
both SAR ADCs) and **temp-sensor cal** (`-12.3`), so ADC/temperature reads can
be accurate without manual calibration.

> ⚠️ Burning Secure Boot v2 + Flash Encryption is a **one-way door** — don't, on
> a dev board.

## Quick commands

```bash
make chipid                 # re-read chip (read-only)
make ports                  # list serial ports
make flash APP=beacon-display   # compile + upload + logs
```
