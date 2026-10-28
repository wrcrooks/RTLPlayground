# MokerLink POE-2G040210GSM (2M-PCB49-V1.1)

The PoE version of the MokerLink 2G040210GSM web-managed 4+2 switch. Work in progress: the machine
target `POE_2G040210GSM` exists, but nothing has been tested on this board yet.

> [!CAUTION]
> This switch has its power supply inside and runs from mains voltage, which can be a dangerous or
> potentially lethal hazard. High-voltage capacitors can stay charged after it is unplugged. Unplug
> it and let the capacitors discharge before opening it or probing the board.

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

`POE_2G040210GSM` starts as a copy of `KP_9000_6XHML_X2_V1_1`, as both are RTL8372 4+2 designs from
the same vendor. Its header layout is closer to the [2M-PCB23-V3.1](2M-PCB23-V3_1.md)'s, though, so
the port mapping, SFP pins, reset button and LEDs all still have to be confirmed on this board.

### PCB overview

**Board markings**
- Top silkscreen: 2M-PCB49-V1.1
- PoE daughterboard: StandardPOE8-V1.0

**Key components**
- RTL8372 (presumably) under the heatsink, with crystal `Y1` next to it
- `U6`: flash, Winbond W25Q16JVSIQ, 2 MiB
- `U5`: an unpopulated SOIC-8 footprint next to `T7`, like `U7` on the 2M-PCB23-V3.1 (an I²C
  EEPROM on the slave interface's bus)
- `U16`: a 2×10 connector for the PoE daughterboard
- `T14`: V48C01S magnetics for the four RJ45 ports
- `JA1`-`JA4`: the two SFP+ cages
- `J1`/`J2` (`V+`, `V-`): power from the internal supply board, whose label reads "72W REV:A"

The flash's 2 MiB means the standard layout applies (web updates need no `LAYOUT=compact`), and the
default dual-I/O flash reads should work (`flash_sio` stays 0). For a backup with a CH341A, flashrom
names the chip `W25Q16.V`.

Top side, with the PoE daughterboard and the supply board above it:

<img src="photos/2M-PCB49-V1.1/pcb-top.jpg" width="600" />

Headers `T3`, `T7`, `T9`-`T12` and `U5`:

<img src="photos/2M-PCB49-V1.1/headers.jpg" width="600" />

Power input `J1`/`J2` and the daughterboard connector `U16`:

<img src="photos/2M-PCB49-V1.1/power-input.jpg" width="600" />

Flash `U6`:

<img src="photos/2M-PCB49-V1.1/flash.jpg" width="600" />

Bottom:

<img src="photos/2M-PCB49-V1.1/pcb-bottom.jpg" width="600" />

### PoE

As on the other PoE boards (see [supported devices](../supported_devices.md)), RTLPlayground doesn't
configure PoE.

- **Controller:** an RTL8238B (`U1`) on the StandardPOE8-V1.0 daughterboard, an 8-channel design
  of which 4 are used. It plugs into `U16` on the main board, so whatever controls PoE runs
  through that connector. The daughterboard has a 4-pad row on its right edge, perhaps for
  programming or a UART, and an outlined spot `U2` near its top edge that may or may not hold a
  part. The [Hasivo S1100P](Hasivo-S1100P-8GT-1SX-S.md) also has an RTL8238B, managed there by a
  separate STC8H8K64U microcontroller. (The MokerLink POE-2G080110GS,
  [2M-PCB51-ML-V1.1](2M-PCB51-ML-V1_1.md), uses an RTL8238C.)
- **Protocol** (likely, from the relatives): RTL8238-based PoE is driven through a microcontroller
  over a UART, 115200 8N1, 12-byte frames (`cmd, seq, 9 data bytes, checksum`), Realtek's dialect
  of the Broadcom PoE protocol ([realtek-poe#25](https://github.com/Hurricos/realtek-poe/issues/25)).
- **Check before trusting the SFP pins:** if the stock firmware has PoE settings, the RTL8372 talks
  to the PoE side through `U16`, most likely on UART1 (GPIO50 TX, GPIO51 RX). The target copied from
  the 2M-PCB43-V1.1 uses GPIO50 as the left SFP's module detect, which would then be wrong.

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
looks like its 6-pin `T10` (1 GPIO49, 2 GPIO47, 3 3V3, 4 GPIO48, 5 GND, 6 GPIO46). The board also
has `T4`, `T5` and `T6`, not yet looked at.

The slave interface on `T7` should answer at I²C address `0x5c` with chip ID `0x83720000` (register
`0x0004`) if the `IF_SEL` straps select I²C, as on the other boards. `U5` next to it supports that.

### Still open

- Photos of the front and the label.
- A backup of the stock firmware (SPI clip, switch unplugged), before anything else is flashed.
- The RTL chip's marking, to confirm the RTL8372.
- What `U16` carries: continuity from its pins to the RTL8372 side, above all to GPIO50/51
  (UART1) and the I²C buses. Whether the stock web UI has PoE settings.
- The serial console on `T9`: baud rate and the stock firmware's boot log.
- SFP pins (module detect, LOS, I²C), the reset button and the LED wiring. These can be read from
  the stock firmware's pin-mux, GPIO and LED registers over `T7` while it runs.
- What `T3` is: a 2-pin header with pin 1 on GND, next to the crystal `Y1`, so perhaps a button or
  jumper input (the reset button?).

## Power supply

An internal supply board, labelled "72W REV:A", runs from mains and feeds the main board through
`J1`/`J2` (`V+`, `V-`). Its output voltage is still to be measured.
