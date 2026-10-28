# MokerLink POE-2G040210GSM (2M-PCB49-V1.1)

The PoE version of the MokerLink 2G040210GSM web-managed 4+2 switch. Work in progress: the machine
target `POE_2G040210GSM` exists, but nothing has been tested on this board yet.

### Label specifications

- **Name**: 4-port 2.5G PoE web-managed switch
- **Model**: POE-2G040210GSM
- **Ports**:
  - 4 × RJ45: 10/100/1000/2500 Mbps, PoE
  - 2 × SFP+: 1000 / 2500 / 10000 Mbps

### Machine target

`make MACHINE=POE_2G040210GSM`

| | 2G040210GSM | POE-2G040210GSM |
| --- | --- | --- |
| PCB | `2M-PCB43-V1.1` | `2M-PCB49-V1.1` |
| Target | `KP_9000_6XHML_X2_V1_1` | `POE_2G040210GSM` |
| Status | supported ([doc](2M-PCB43-V1.1.md)) | not tested |

`POE_2G040210GSM` starts as a copy of `KP_9000_6XHML_X2_V1_1`, as both are RTL8372 4+2 designs from the
same vendor. Its header layout is closer to the [2M-PCB23-V3.1](2M-PCB23-V3_1.md)'s, though, so the
port mapping, SFP pins, reset button and LEDs all still have to be confirmed on this board.

Flash: a Winbond W25Q16JVSIQ, 2 MiB, so the standard layout applies (web updates need no
`LAYOUT=compact`), and the default dual-I/O flash reads should work (`flash_sio` stays 0). For a
backup with a CH341A, flashrom names the chip `W25Q16.V`.

### PoE

As on the other PoE boards (see [supported devices](../supported_devices.md)), RTLPlayground doesn't
configure PoE. Not yet checked on this board, but likely from its relatives:

- **Controller:** the MokerLink POE-2G080110GS ([2M-PCB51-ML-V1.1](2M-PCB51-ML-V1_1.md)) uses an
  RTL8238C, and the [Hasivo S1100P](Hasivo-S1100P-8GT-1SX-S.md) an RTL8238B managed by a separate
  STC8H8K64U microcontroller.
- **Protocol:** RTL8238-based PoE is driven through such a microcontroller over a UART, 115200 8N1,
  12-byte frames (`cmd, seq, 9 data bytes, checksum`), Realtek's dialect of the Broadcom PoE
  protocol ([realtek-poe#25](https://github.com/Hurricos/realtek-poe/issues/25)).
- **Check before trusting the SFP pins:** if the stock firmware has PoE settings, the RTL8372 talks
  to that microcontroller, most likely on UART1 (GPIO50 TX, GPIO51 RX). The target copied from the
  2M-PCB43-V1.1 uses GPIO50 as the left SFP's module detect, which would then be wrong.

### Headers

Pin 1 is the square pad. "Measured" is a continuity check on this board; everything else is a guess
from the 2M-PCB23-V3.1's matching headers and still has to be checked.

| Header | Pin | Signal | Source |
| --- | --- | --- | --- |
| `T3`, 2 pins | 1 | GND | measured |
| | 2 | ? | |
| `T7`, slave interface, 6 pins | 1 | SCL / SCK / MDC | guess (2M-PCB23-V3.1 `T7`) |
| | 2 | GND | measured |
| | 3 | SDA / SDI / MDIO | guess |
| | 4 | 3V3 | measured |
| | 5, 6 | GPIO? | guess |
| `T9`, serial console, 4 pins | 1 | U0TXD, GPIO31 (output) | guess (2M-PCB23-V3.1 `T8`) |
| | 2 | GND | measured |
| | 3 | U0RXD, GPIO32 (input) | guess |
| | 4 | 3V3 | measured |
| `T10`, SMI (port 6?), 3 pins | 2 | GND | measured |
| `T11`, GPIO?, 6 pins | 3 | 3V3 | measured |
| | 5 | GND | measured |
| `T12`, SMI (port 5?), 3 pins | 2 | GND | measured |

`T10` and `T12` look like the 2M-PCB23-V3.1's 3-pin SMI header `T9` (1 MDO, 2 GND, 3 MDC). `T11`
looks like its 6-pin `T10` (1 GPIO49, 2 GPIO47, 3 3V3, 4 GPIO48, 5 GND, 6 GPIO46).

The slave interface on `T7` should answer at I²C address `0x5c` with chip ID `0x83720000` (register
`0x0004`) if the `IF_SEL` straps select I²C, as on the other boards.

### Still open

- Photos of the front, label and both sides of the PCB.
- A backup of the stock firmware (SPI clip, switch unpowered), before anything else is flashed.
- The RTL chip's marking, to confirm the RTL8372.
- The PoE controller and its microcontroller, and whether that one is wired to the RTL8372's UART1
  (GPIO50/51) or an I²C bus. Whether the stock web UI has PoE settings.
- The serial console on `T9`: baud rate and the stock firmware's boot log.
- SFP pins (module detect, LOS, I²C), the reset button and the LED wiring. These can be read from
  the stock firmware's pin-mux, GPIO and LED registers over `T7` while it runs.
- What `T3` is: a 2-pin header with pin 1 on GND, so perhaps a button or jumper input (the reset button?).

## Power supply

To be documented: the supply's voltage and current, and the connector. The 2G040210GSM runs from
12 V, 1 A; PoE needs more than that. The POE-2G080110GS has its supply inside and runs from mains:
if this one does too, the caution in its [doc](2M-PCB51-ML-V1_1.md) applies, and the board must be
unplugged, with its capacitors discharged, before probing.
