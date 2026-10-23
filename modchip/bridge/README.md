# Mod-chip bridge firmware (8051)

A minimal firmware for the RTL8372/RTL8373's built-in 8051. It hands the switch's management plane to
an external processor, the RP2350 on the mod chip, over the UART on header T5.

The mod chip itself lives in its own repository,
[wrcrooks/rtlplayground-modchip](https://github.com/wrcrooks/rtlplayground-modchip). That repository
has the hardware, the RP2350-side tools and the full design document. This directory has only the
8051 side, because it shares RTLPlayground's chip-level modules.

Nothing here has run on hardware yet.

## Building

From the repository root:

```
make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact   # 512 KiB flash
make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_MANAGED                    # 2 MiB flash
```

## Tests

- **Framing.** `make -C test` runs `test_bridge_link`, which compiles `link.c` unchanged and checks its
  framing.
- **Golden frames.** `test/build/test_bridge_link --emit` prints the frames the mod-chip repository's
  Python test checks against. Regenerate them there when the framing changes.
- **Helper drift.** `check_hal.py` checks that the helpers copied from `rtlplayground.c` haven't
  drifted.

## Why the 8051 stays

The 8051 is part of the RTL8372 die. Three things are reachable only through its special function
registers, so it can't be removed or replaced:

- **the CPU port's packet buffers**, which it reads and writes by DMA (`SFR_NIC_*`). Every frame to or
  from the management plane passes through it;
- **PHY (MDIO) and SerDes access**, which are SFR commands (`SFR_EXEC_READ_SMI`,
  `SFR_EXEC_READ_SDS`);
- **the SPI flash** holding its own code.

Everything else is an ordinary 32-bit switch register. An external host reaches those through the
slave interface on header T3 (I²C, address 0x5C).

The design therefore splits the work. The 8051 runs a small **bridge firmware**: it initialises the
chip, keeps the SerDes and SFPs working, and tunnels frames and SFR-only operations to the RP2350 over
its UART. The RP2350 does everything else.

## The link: UART on header T5

| T5 pin | Signal | Mod chip |
|---|---|---|
| 1 (square) | U0TXD, switch output | RP2350 GP1 (UART0 RX) |
| 2 | GND | GND |
| 3 | U0RXD, switch input | RP2350 GP0 (UART0 TX), 100 Ω series |
| 4 | 3.3 V | not connected |

- **Wiring.** T5 is about 90 mm from T3 (*photo*). The board outline reaches it with a rigid tail; see
  Mechanical. A 3-wire harness is the fallback if the tail's fit turns out to be impractical.
- **Baud rate.** The bridge runs UART0 at 976 562 baud (`LINK_TH1 0xfe`), which the 125 MHz 8051
  generates exactly.
- **RP2350 side.** From its default 150 MHz `clk_peri`, the RP2350's fractional divider comes within
  0.07 % of that rate, well inside UART tolerance. Running `clk_sys` at 125 MHz instead makes it exact.
- **Faster option.** 1 953 125 baud (`0xff`; again within 0.07 % on the RP2350) is the next step, if the 8051 keeps up
  with its per-byte CRC and COBS work. Measure it on hardware.
- **Throughput.** About 97 KB/s each way at 976 562 baud. A 1500-byte frame takes 15 ms.

## Firmware

The output is `output/bridge-<MACHINE>[-compact]/bridge-<git>-<MACHINE>[-compact].bin`, plus
`…-flash512k.bin` for a programmer writing a whole 512 KiB chip.

| File | Contents |
|---|---|
| `bridge.c` | `main()`, the ISRs, the request handlers, the frame tunnel, link and SFP events |
| `link.c`, `link.h` | COBS framing and CRC, compiled unchanged by `test/test_bridge_link.c` |
| `bridge_proto.h` | message types and status codes |
| `hal.c` | RTLPlayground's register, PHY, SerDes and print helpers, copied unchanged. `check_hal.py` reports any drift from `rtlplayground.c` |
| `Makefile` | links the bridge with the shared chip-level modules: `boot.c`, `sfp.c`, `rtl837x_init.c`, `rtl837x_phy.c`, `rtl837x_pins.c`, `rtl837x_leds.c`, `rtl837x_flash.c`, `rtl837x_nic.c`, `rtl837x_port.c`, `rtl837x_trap.c`, the machine definitions |

The image is about 34 KB of code, against about 142 KB for RTLPlayground, and uses 7.4 KB of the
48 KB XRAM. It leaves out uIP, the web UI, the command line, the config replay, DHCP, DNS, NTP,
syslog, STP, LACP, IGMP and LLDP. Its layout matches an RTLPlayground image, so the boot ROM, a
programmer and the staged update path treat it the same way.

The main loop:

1. **Every tick (200 Hz).** Poll the SFP pins every 50 ms; `handle_sfp()` reconfigures the SerDes for an
   inserted module. Once a second, re-check links, time the host out, and send a beacon while no host
   is present.
2. **On a link interrupt.** Adjust the SerDes as RTLPlayground does, and report the change.
3. **On a NIC interrupt.** DMA up to 4 frames out of the RX ring and send each to the host, or drop
   it if no host is present.
4. **Always.** Decode queued UART bytes and act on one complete request. Flush buffered log text.

The ISRs only latch events and queue UART bytes, as in RTLPlayground. All register access stays in the
main loop.

## Link protocol (v1)

**Framing.** Each message is `type, seq, payload…, CRC-16` encoded with COBS and ended by `0x00`.

- The CRC is CRC-16/MODBUS: reflected polynomial 0xA001, initial value 0xFFFF, appended low byte
  first. It's the same table `crc16.asm` already uses for image checks.
- Multi-byte fields are little endian.
- The largest message is a TX frame: 1 + 1 + 1 + 1556 + 2 bytes.

**Flow control.** The host sends one request, waits for the reply (same `seq`, type `request | 0x80`,
status byte first), then sends the next. That keeps the 8051's 2 KiB receive ring from overflowing
without any credit scheme. The RP2350 receives into a DMA ring and needs no flow control. If the ISR
does lose a byte, the message is dropped and counted in `link_errors`.

| Request | Payload | Reply payload | Notes |
|---|---|---|---|
| `0x01 HELLO` | | hello | Marks the host present; followed by `LINK` and `SFP` |
| `0x02 PING` | | `uptime_s:4 rx_dropped:2 link_errors:2` | Send at least every 3 s (`HOST_TIMEOUT_S`) |
| `0x10 TX_FRAME` | `flags:1 frame` | | `flags` is the NIC descriptor's checksum byte, normally 0x07. The frame runs from the destination MAC, without FCS; insert an 0x8899 CPU tag to direct it (`doc/CpuPort.md`) |
| `0x20 REG_READ` | `reg:2` | `value:4` | The same register the slave interface reaches |
| `0x21 REG_WRITE` | `reg:2 value:4` | | |
| `0x22 PHY_READ` | `phy:1 dev:1 reg:2` | `value:2` | Clause 45; `phy` < 16 |
| `0x23 PHY_WRITE` | `phy:1 dev:1 reg:2 value:2` | | |
| `0x24 SDS_READ` | `sds:1 page:1 reg:1` | `value:2` | `sds` ≤ 1, `page` < 128 |
| `0x25 SDS_WRITE` | `sds:1 page:1 reg:1 value:2` | | |
| `0x26 SFP_READ` | `slot:1 offset:1 len:1` | `data` | `len` 1-16; bit 7 of `offset` selects A2h (diagnostics) |
| `0x27 TBL_READ` | `ctrl:4 count:1` | per entry: `ctrl:4 out_a:4 out_b:4 out_c:4 data_0:4` | Runs `count` (1-64) table reads, from the entry in `ctrl` (`TBL_CTRL` as written, read command) onwards: `TBL_CTRL` after the read, `L2_DATA_OUT_A/B/C` and `TBL_DATA_0`. Saves the host a write, a poll and the reads per entry |
| `0x28 SDS_CONFIG` | `sds:1 mode:1` | | `sds_config()`: SerDes `sds` (≤ 1) to `SDS_*` mode `mode` |
| `0x29 STAT_READ` | `pmask:2 n:1 counter:n` | per port in `pmask`, ascending, per counter: `value:8` | MIB counters (`STAT_GET`), `n` 1-16, each the 64-bit value of `STAT_V_HIGH`:`STAT_V_LOW`. Saves the host four round trips per counter |
| `0x30 FLASH_READ` | `addr:4 len:2` | `data` | `len` 1-512; `addr` 4-byte aligned |
| `0x31 FLASH_ERASE` | `addr:4` | | One 4 KiB sector, at or above `upload_start` |
| `0x32 FLASH_WRITE` | `addr:4 data` | | 1-256 bytes within one page, at or above `upload_start` |
| `0x33 FLASH_ID` | `what:1 addr:4` | text | Runs `flash_read_jedecid()` (`'j'`), `flash_read_uid()` (`'u'`) or `flash_read_security()` on 40 bytes at `addr` (`'s'`) and returns what it prints, up to 128 bytes |
| `0x3E SET_LED` | `state:1` | | System LED: 0 off, 1 fast, 2 slow, 3 on |
| `0x3F REBOOT` | | | Replies, then resets the chip |

| Unsolicited | Payload |
|---|---|
| `0x40 BEACON` | hello, once a second while no host is present |
| `0x41 RX_FRAME` | the 8-byte NIC RX descriptor (port in byte 3, length in bytes 4-5), then the frame with its CPU tag |
| `0x42 LINK` | `links_sts:4 links:4 links_89:4` (registers 0x63e8, 0x63f0, 0x63f4) |
| `0x43 SFP` | `sfp_pins:1`, as `sfp_pins_last` in `sfp.c` |
| `0x44 LOG` | text the shared modules print |

The **hello** payload holds, in order:

- protocol version, 1 byte;
- chip ID, 4 bytes;
- MAC, 6 bytes: from flash at `machine.mac_flash_offset`, otherwise the derived locally administered
  address RTLPlayground uses;
- flash size, 4 bytes;
- image size, 4 bytes;
- upload start, 4 bytes;
- CPU port, minimum port, maximum port and SFP count, 1 byte each;
- flags, 1 byte: RTL8373, N variant, compact layout;
- machine name, 30 bytes;
- firmware version, the rest of the message.

Status codes: 0 ok, 1 bad length, 2 bad argument, 3 failed, 4 unknown request.

**Flash writes stay above the running image.** Writes are allowed only at or above `upload_start`,
which covers the staging area, the saved config sector and any MAC slot above the image. To update the
bridge, the RP2350 erases and writes a complete image at `upload_start`, reads it back to verify, then
sends `REBOOT`. The 8051 checks the CRC at boot and installs the image, as RTLPlayground does for a
web upload. Expect the same slow writes, one Page Program per 4 bytes: about 2-3 minutes for a
compact image on the W25Q40.

## Register ownership after `HELLO`

Several engines can be reached from both sides, so each has one owner.

| Engine | Owner | Host access |
|---|---|---|
| CPU-port NIC (0x7840-0x7890), interrupt status/mask (0x5f34, 0x5f84, 0x5f88, 0x7854) | 8051 | none |
| SerDes, PHY (MDIO), flash | 8051 (SFR only) | through requests |
| SoC I²C master (SFP EEPROMs), SFP GPIOs | 8051 | `SFP_READ` |
| System LED | either | `SET_LED`, or the register directly when the 8051 isn't using it |
| Everything else: VLAN, L2, ACL and MIB table engines, port, trap, mirror, storm and bandwidth registers | RP2350 | I²C on T3, or `REG_READ`/`REG_WRITE` |

The 8051 doesn't touch the RP2350's registers after boot, so the host may use either path.

- **Before `HELLO`.** The host must not write registers. The 8051 answers `HELLO` only after its
  initialisation has finished, because it starts serving requests only then.
- **One path at a time.** The table engines (`TBL_CTRL` and friends) take several register accesses
  per operation. The host must not interleave an I²C sequence and an RPC sequence on the same engine.

## Failure behaviour

| Event | Result |
|---|---|
| No RP2350, or it stops sending | After 3 s the bridge drops CPU-port frames and sends beacons. Forwarding continues with the registers as last written |
| 8051 stops answering | The RP2350 sees `PING` time out and pulses the reset line (if wired). The switch reboots to defaults, which interrupts traffic for a few seconds; the RP2350 then re-applies its configuration after `HELLO` |
| Corrupt or lost bytes on the UART | The message fails its CRC and is counted in `link_errors`. The host retries after its reply timeout |
| Bad staged image | The 8051 refuses it at boot (CRC) and erases the staging area |
| Bricked flash | A programmer (CH341A) on the SPI flash, as today; the mod chip doesn't change that |
