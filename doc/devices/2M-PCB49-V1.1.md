# MokerLink POE-2G040210GSM (2M-PCB49-V1.1)

The PoE version of the MokerLink 2G040210GSM web-managed 4+2 switch. Work in progress: RTLPlayground boots
on it with the machine target `POE_2G040210GSM`, but most of the board is still to be checked.

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
| Status | supported ([doc](2M-PCB43-V1.1.md)) | boots, partly tested |

`POE_2G040210GSM` started as a copy of `KP_9000_6XHML_X2_V1_1`, as both are RTL8372 4+2 designs
from the same vendor. Its header layout is closer to the [2M-PCB23-V3.1](2M-PCB23-V3_1.md)'s, though.

### What works

Tested on one switch, flashed with a CH341A clip:

- It boots and serves the web UI. Chip temperature and the 2 MB flash are detected.
- All four RJ45 ports link at 1G and pass traffic, each on the matching front-panel port. 2.5G
  is not tested yet.
- The LEDs, mapped on this board (see [LEDs](#leds)). Each port's amber LED lights and blinks
  with a 1G link; the green (2.5G) and SFP LEDs are mapped but not yet seen lit by a real link.
- The MAC address is read from the stock firmware's copy at flash offset `0x1FC000`. Keep that
  sector when flashing a full image with a clip: an image padded with `0xFF` erases it, and the
  switch then makes up a locally administered MAC.

- The reset button is on GPIO48, low while pressed (as on the 2M-PCB23-V3.1). A short press does
  nothing, and holding it for more than 3 s restarts the switch (both tested). Held more than
  10 s, it also restores the default settings (not tested). The "S" LED can't blink on
  this board, so nothing shows that the button is held.
- Firmware updates through the web UI work; they leave the MAC sector alone.

- Both SFP+ ports: modules are detected, their EEPROMs and readings are read, and a fibre between
  the two cages (two FS SFP-10GLRM-31) links at 10G and passes frames both ways (see [SFP](#sfp)).

Not known yet: whether PoE works (see [PoE](#poe)).

### PCB overview

**Board markings**
- Top silkscreen: 2M-PCB49-V1.1
- PoE daughterboard: StandardPOE8-V1.0

**Key components**
- RTL8372 (presumably) under the heatsink, with crystal `Y1` next to it
- `U6`: flash, Winbond W25Q16JVSIQ, 2 MiB
- `U5`: an unpopulated SOIC-8 footprint next to `T7`, like `U7` on the 2M-PCB23-V3.1 (an I²C
  EEPROM on the slave interface's bus)
- `U16`: a 2×10 socket for the PoE daughterboard, under its left edge, between the power input
  and the heatsink. Pin 1 is at the front end, towards the RJ45 ports (a triangle on the top
  silkscreen, the square pad on the bottom); pins 19 and 20 are at the back, near `T4`. Its
  solder joints are easiest to reach from the bottom, with the daughterboard plugged in.
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
  of which 4 are used. It plugs into `U16` on the main board. The RTL8238B is the daughterboard's
  only chip: its `U2` footprint is unpopulated, so there is no separate microcontroller on it. The
  daughterboard also has a 4-pad row on its right edge, perhaps for programming or a UART. The
  [Hasivo S1100P](Hasivo-S1100P-8GT-1SX-S.md) also has an RTL8238B, managed there by a separate
  STC8H8K64U microcontroller. (The MokerLink POE-2G080110GS, [2M-PCB51-ML-V1.1](2M-PCB51-ML-V1_1.md),
  uses an RTL8238C.)
- **`U16` pins** (measured with the switch unplugged; diode mode with the black probe on GND):
  - 17, 18: GND, the same net as `J2` (V-).
  - 8: 3V3. Reads about 1.2 V in diode mode.
  - 1-6: signals. Each reads about 1.6 V in diode mode, which is about 0.4 V above the 3V3 rail:
    the ESD diode of a 3.3 V chip pin. 1, 3 and 5 read a few tens of kΩ to 3V3: weak pull-ups,
    or a path through the unpowered chips. 2, 4 and 6 gave no reading.
    None of 1-6 connects to a pin of `T7` or `T9`-`T12`.
  - 7, 9-16, 19, 20: nothing in diode mode, probably not connected on the main board.
  - None of them connects to `J1` (V+), so the PoE supply's positive side reaches the daughterboard
    some other way.
- **The stock firmware manages PoE.** Its web UI has a PoE power consumption readout and a "PSE
  Port Settings" page that turns PoE on and off per port and shows each port's type, power (W),
  voltage (V) and current (mA). With no microcontroller on the daughterboard, that means the RTL8372
  itself talks to the RTL8238B through `U16`, in both directions (the readings come back from it).
- **I²C, going by the stock firmware.** Its strings include `PSE IC Type: RTL8238b` and
  `address: 0x20 pse_init` / `address: 0x21 pse_init`, which point to the RTL8238B on an I²C bus at
  `0x20` and `0x21` rather than a UART. `GPIO50`/`GPIO51` are also the RTL8372's I²C bus 2.
- **Found on I²C bus 0: SDA `GPIO47`, SCL `GPIO46`.** Probed from RTLPlayground with the RTL8372's
  own I²C master (raw transactions through `regset`/`regget` on `0x0418`-`0x0424`, after
  switching the pins to I²C in `PIN_MUX_1`). A scan of 0x08-0x77 gets answers at `0x20`, `0x21`
  and `0x60` only. No other pairing of buses 0 and 2 (`GPIO46/47`, `GPIO50/51`) answers.
- **Registers.** All three addresses return a 256-register map of mostly 16-bit values. `0x20` and
  `0x21` read the same (two halves of the chip, presumably 4 channels each); `0x60` matches them
  except at `0x12`-`0x13` and `0x42`-`0x43`, and reads `0` at `0xf0`-`0xf3` where the others read
  `5a 5a a5 a5`, so it may be a broadcast or global address. The dump, taken with nothing
  connected, is in [data/2M-PCB49-V1.1-rtl8238b-regs.txt](data/2M-PCB49-V1.1-rtl8238b-regs.txt).
  Nothing has been written to the chip.
- **The stock firmware uploads an image to the RTL8238B at every boot.** From a disassembly of the
  stock flash (Keil C51, banked; PSBANK `n` maps to image offset `0x4000 + (n - 1) * 0xc000`):
  - The init (bank 19, `0x89aa`) resets the chip, "disables all the PSE ports" by reading
    register `0x10` of `0x20` and `0x21`, then calls the loader (bank 21, `0x833a`).
  - The loader disables interrupts, writes `03 00 00 00` to register `0xec` of `0x20`, waits
    10 ms and reads `0xec` back. If bit 2 of its first byte is clear, it writes `00 00 20 39` to
    register `0x18`, waits 10 ms and writes `0xec` again. Then it writes the whole image to
    register `0xf4` and reads `0xec` once more: bit 5 of the first byte set means "load ok".
  - The image is one of two blobs stored in bank 21 right after the loader, back to back: the
    first is `0x52ac` bytes at bank-relative `0x8428`, the second `0x1f5c` bytes at `0xd6d4`
    (image offsets `0xf8428` and `0xfd6d4` in a 2 MiB dump of this board). Both start with the
    same four bytes, `00 00 39 82`. The loader reads register `0xe8` and compares it with 2:
    equal picks the `0x1f5c` blob, anything else the `0x52ac` one. This board reads `0xe8` as
    `00 00 00 00`, so it uses the `0x52ac` blob.
  - All of it goes through a bit-banged I²C master on `GPIO46`/`GPIO47` (common code `0x1202`),
    and each write is one transaction: address, register byte, then every data byte, so the image
    goes out as a single 8 or 21 KB write. The RTL8372's I²C master moves at most 16 bytes, so
    RTLPlayground would need its own bit-banged writer to do the same.
  - Payloads go out in memory order, and Keil C51 stores an integer most significant byte first,
    so the 32-bit `0x20` of the reset reaches register `0x1a` as `00 00 00 20`. The payloads for
    `0xec` (`03 00 00 00`) and `0x18` (`00 00 20 39`) are literal byte tables in the stock image
    at xdata `0xb5b0` and `0xb5ac`. Reads come back the other way round, least significant byte
    first, which is where the status bits of `0xec` are.
- **Loading the image with RTLPlayground.** `rtl8238.c` repeats the stock sequence at boot
  (before the saved configuration is applied) over a bit-banged I²C master on `GPIO46`/`GPIO47`,
  on machines with `.pse_rtl8238 = 1`. The image itself is the PoE controller vendor's firmware,
  so it isn't in this repository: a build only loads it when given one,
  `make MACHINE=POE_2G040210GSM PSE_IMAGE=<file>`, where `<file>` is packed from your own copies
  of the image(s) with `tools/mkpseimage.py IMAGE0 IMAGE1 <file>` (image 1 is used when register
  `0xe8` reads 2, image 0 otherwise; `-` for one you don't have). The build places it at
  `0x34000`-`0x3ffff` in the firmware image (48 KiB), between the last code bank and the web UI.
  A web update rewrites `0x00000`-`0x6ffff` only (it stops at `CONFIG_START` to keep the saved
  config), so the region has to stay below that to be carried by one. Without it the
  boot log says "no RTL8238B image in this build" and PoE stays off. The console command `poe`
  reads the chip's status registers over the same I²C.
- **Per-port readings** (from the stock firmware's PoE pages and its driver in bank 19; see
  `rtl8238.h`). Front ports 1-4 are channels 3, 2, 1, 0 of `0x20` (the stock port tables at xdata
  `0xabaa`/`0xabf3`); `0x21` serves no ports on this board.
  - `0x0c + ch`: low nibble detection (4 = valid PoE device, 6 = open; a PC's NIC reads 1), high
    nibble class (`0xc` = class 0, 1-8 = class 1-8).
  - `0x10`, first byte: bit `4 + ch` = port powered.
  - `0x30 + 4 * ch`: low 16 bits current, `raw * 125 / 4096` mA; high 16 bits `& 0x7fff`
    voltage, `raw / 512` V. A small class 0 device on port 1 read 41 mA at 52 V.
  - The web UI's PoE tab (shown when `information.json` has `"poe":1`) reads these through
    `/poe.json` every 3 s and converts them. It is read-only: the stock per-port enable/disable
    write isn't decoded yet, and the controller powers valid devices on its own.
- **Protocol on other boards** (unconfirmed here): RTL8238-based PoE is usually driven over a
  UART, 115200 8N1, 12-byte frames (`cmd, seq, 9 data bytes, checksum`), Realtek's dialect of the
  Broadcom PoE protocol ([realtek-poe#25](https://github.com/Hurricos/realtek-poe/issues/25)).
- **PoE works once the image is loaded.** The chip powers nothing by itself: before the loader
  existed, a PoE-powered device on port 1 left the port without link and changed no register.
  With a build that carries the image, the boot-time load brings the chip up and the device
  powers on and links. Registers seen at `0x20` before and after a load:

  | Register | Before | After |
  | --- | --- | --- |
  | `0xec` | `02 00 00 00` | `24 00 00 00` (bits 2 and 5 set, bit 5 = load ok) |
  | `0x18` | `00 00 00 39` | `00 00 00 3a` |
  | `0x10` | `00 00 ff 0f` | `88 00 ff 0f` |

  `0x21` answers the same except for `0x10`, which stays `00 00 ff 0f`: it is the half with no
  ports wired. Registers come back least significant byte first, so the status bits are in the
  first byte received.

### SFP

Found by moving a module (an Intel AFBR-709DMZ, 10G SR) between the cages while polling the GPIO
inputs and reading the EEPROM from each cage's configured bus:

| | Port 5 (left) | Port 6 (right) |
| --- | --- | --- |
| Module detect (low = module in) | GPIO38 | GPIO37 |
| EEPROM I²C SDA | GPIO39 | GPIO41 |
| EEPROM I²C SCL | GPIO40 | GPIO40 |
| LOS | not found | not found |

Both cages differ from the 2M-PCB43-V1.1 (detect GPIO50 / GPIO30, and the two SDA lines the other
way round). No GPIO input changed with the LOS of a module without fibre, so LOS may not be wired
to a GPIO.

The SerDes mapping (port 5 on SerDes 0, port 6 on SerDes 1) is right: with a module in each cage
and a fibre jumper between them, both ports linked at 10G, and 100 broadcast frames sent out of
each port arrived at the other, with no errors. (For this test, port isolation kept the two ports
from forwarding to each other or to the RJ45 ports, so the loop couldn't flood the switch.)

### LEDs

Each RJ45 port has a green and an amber LED, each SFP port a green one, and there is a system LED
marked "S". The LED pads were
found by routing pads to a lit source in turn, with `regset` on the LED mux registers
(`0x65e0`-`0x65f4`) while RTLPlayground ran:

| LED | Pad | | LED | Pad |
| --- | --- | --- | --- | --- |
| Port 1 amber | 9 | | Port 3 amber | 18 |
| Port 1 green | 10 | | Port 3 green | 21 |
| Port 2 amber | 12 | | Port 4 amber | 24 |
| Port 2 green | 15 | | Port 4 green | 25 |
| Port 5 (left SFP) | 26 | | Port 6 (right SFP) | 6 |

Pads 0-7 are disabled at reset (`LED_GLB_IO_EN` bits 0-7), so `POE_2G040210GSM_init.c` enables
pad 6.

The "S" LED is on the chip's LED 28 output. It lights when `PIN_MUX_0` bit 28 and
`LED_GLB_IO_EN` bit 28 are both set, and is dark if either is clear. It stays lit steadily: setting
the system LED state in `LED_MODE` to blink or off made no difference, so RTLPlayground's
blink patterns (such as the reset button's feedback) don't show on it.

The target lights the green LED for a 2.5G or 10G link and the amber LED for 1G, 100M or 10M, both
blinking on activity. The SFP LEDs show any link and activity. Whether that matches the stock
firmware hasn't been compared.

Pad 10 is also GPIO10, which the 2M-PCB43-V1.1 uses as its left SFP's LOS input, so this target
doesn't use it for LOS.

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
- The RTL chip's marking, to confirm the RTL8372.
- What `U16` carries: continuity from its pins to the RTL8372 side, above all to GPIO50/51
  (UART1) and the I²C buses. A logic analyser on the `U16` pins while the stock web UI changes a
  port's PoE setting would show the link and its protocol.
- Whether the ports supply PoE when the RTL8238B gets no commands (under RTLPlayground).
- The serial console on `T9`: baud rate and the stock firmware's boot log.
- 2.5G links and the green LEDs. Making the "S" LED blink.
- SFP LOS: no GPIO input follows it. These can be read from
  the stock firmware's pin-mux, GPIO and LED registers over `T7` while it runs.
- What `T3` is: a 2-pin header with pin 1 on GND, next to the crystal `Y1`, so perhaps a button or
  jumper input (the reset button?).

## Power supply

An internal supply board, labelled "72W REV:A", runs from mains and feeds the main board through
`J1`/`J2` (`V+`, `V-`). Its output voltage is still to be measured.
