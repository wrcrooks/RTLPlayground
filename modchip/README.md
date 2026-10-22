# Mod chip

The mod chip is an RP2350 board that takes over the switch's management plane. It lives in its own
repository, [wrcrooks/rtlplayground-modchip](https://github.com/wrcrooks/rtlplayground-modchip), which
has the hardware, the RP2350-side tools and the design document.

This repository keeps only the 8051 side, the bridge firmware in [`bridge/`](bridge/README.md), because
it shares RTLPlayground's chip-level modules.
