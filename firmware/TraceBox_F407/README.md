# LXB407VE-P1 board bring-up

## Keil prerequisite

This project targets `STM32F407VETx`.  For the installed Keil MDK 5.24a
toolchain, install `Keil.STM32F4xx_DFP.2.9.0.pack` before opening the project.
The project intentionally pins this older DFP because current 3.x packs use a
newer pack schema that the 2017 Pack Installer cannot process reliably.

## Connections for tonight

| Function | Board pin | Connect to |
|---|---:|---|
| Debug signals | PA13 / PA14 / RST / GND | ST-Link SWDIO / SWCLK / NRST / GND |
| Target reference | 3V3 | Dedicated ST-Link VTref only, if the probe provides it |
| UART output | PA9 | USB-TTL RX |
| UART input | PA10 | USB-TTL TX |
| UART ground | GND | USB-TTL GND |
| SPI loopback | PA7 | PA6 (one Dupont wire, no other SPI device connected) |
| W25Q64 VCC / GND | 3V3 / GND | Module power; never use 5 V |
| W25Q64 CS / CLK | PB0 / PA5 | Chip select / SPI1 clock |
| W25Q64 DO / DI | PA6 / PA7 | SPI1 MISO / MOSI |

Use a **3.3 V TTL** serial adapter. Do not connect its 5 V output to PA9/PA10.
Power the board from its normal 5 V input/USB-C or from one known-good supply;
do not tie together two unrelated 5 V sources.

Many low-cost ST-Link V2 clones label a **3.3 V power output** rather than a
high-impedance VTref input. If the board is already powered from USB-C, leave that
3.3 V output disconnected; never let the probe and board regulators drive each
other. SWDIO, SWCLK, GND and optionally NRST are enough for such clones.

Serial settings: `115200, 8 data bits, no parity, 1 stop bit, no flow control`.
The USB-C connector on this board is native USB and is not the USART1 console.

The repository does not commit Keil's machine-local `.uvoptx`. For GUI download,
select `ST-Link Debugger` once under **Options for Target -> Debug**, then select
`Use Target Driver for Flash Programming` under **Utilities**. Alternatively run
`tools/flash_stlink.ps1`, which uses the installed STM32CubeProgrammer CLI and
does not depend on Keil's probe setting.

## Expected behavior

- PC0 LED toggles every 500 ms after startup.
- USART1 prints a banner, chip ID, 96-bit UID, reset reason, bus clocks and a
  DWT 10 ms cycle measurement.
- `SELFTEST: PASS clock+dwt` should appear.
- Interactive commands: `help`, `ping`, `info`, `pins`, `stats`, `dwt`,
  `led on`, `led off`, `led toggle`, `led auto`, `spi info`, and
  `spi loop 16/256/4096/all`, `spi dma 16/256/4096/all`, `core selftest`,
  `adc info`, and `adc test`.

For the SPI polling baseline, power the board off before connecting PA7 (MOSI)
directly to PA6 (MISO). After flashing, `spi loop all` must report matching PRBS
data for all three lengths. PB0 toggles as the transaction chip-select even
though it is not connected during loopback. SPI1 starts in mode 0 at 1.3125 MHz.

The DMA path uses SPI1_RX on DMA2 Stream2 Channel3 and SPI1_TX on DMA2 Stream3
Channel3. `spi dma all` returns from each start call immediately; the DMA ISR
only acknowledges flags and publishes completion/error events. The cooperative
main loop owns BSY drain, timeout cleanup, callback, PRBS verification and UART
reporting.

`spi dma timeout` is a deterministic fault-injection command: it starts a
4096-byte transfer with a deliberately impossible 16,800-cycle deadline. The
expected result is `TIMEOUT`, one recovery, and a successful subsequent
`spi dma 4096`. It tests cleanup logic and is not a production transfer mode.

`core selftest` exercises the storage-independent record core on the MCU: a
32-record drop-newest event ring, an eight-slot 256-byte page ownership pool,
explicit little-endian ADC page encoding, commit-byte rejection and the F407
hardware CRC unit. It deliberately fills both rings and corrupts/restores one
sample byte; the expected intentional overflow counters are one and the final
result must be `CORE SELFTEST: PASS`.

`adc test` starts a finite 1.92 s acquisition. TIM2 emits a 10 kHz update TRGO,
ADC1 samples PA0, and DMA2 Stream4 Channel0 alternates between two 96-sample
buffers. Each transfer-complete ISR publishes only buffer index, sequence and
DWT timestamp into a four-entry descriptor ring. The main loop checks 200
blocks/19,200 samples, M0/M1 alternation, sequence continuity, 9.6 ms block
cadence, descriptor overflow, ADC overrun and maximum ISR cycles. PA0 may be
left unconnected for the first digital-path gate; connect a 10 kOhm
potentiometer between 3.3 V/GND with its wiper on PA0 for low/mid/high analog
evidence.

Before connecting W25Q64, power down and remove the PA7-to-PA6 loopback wire.
Connect the module by its printed signal names: `VCC->3V3`, `GND->GND`,
`CS->PB0`, `CLK->PA5`, `DO->PA6`, `DI->PA7`. `nor id` reads JEDEC ID through
SPI DMA. `nor selftest` is destructive only to the detected chip's final 4 KiB
sector: it verifies erase, writes bytes 0..254, programs the commit byte as a
separate operation, reads all 256 bytes back, validates CRC, forces a 1 ms erase
timeout, and asynchronously polls WIP until recovery completes.

`record selftest` is the first end-to-end producer/consumer gate. It erases 13
sectors from data base `0x002000`, records 200 ADC blocks (19,200 samples) into
200 CRC/commit pages while Flash programming runs asynchronously, drains the
eight-slot page pool, then reads and validates all 200 pages and sequences. It
reports capture duration, pack maximum cycles, pool/descriptor high-water,
overflow, ADC/DMA errors and Flash/CRC errors. This command is destructive to
`0x002000..0x00EFFF`.

## Final P0 workflow

The v1.0.0 candidate keeps the short tests and adds the production lifecycle:

1. `prepare 60` identifies the W25Q64 and asynchronously erases 391 sectors;
2. `record 60` captures 6,250 pages/600,000 samples, drains all buffers and
   reads every page back for CRC/sequence/capture-ID verification;
3. `recover` scans the valid prefix without erase; the same scan runs
   automatically at every boot;
4. `dump 460800` announces the exact page count at 115200, switches baud after
   a guarded delay, sends raw 256-byte pages and returns to 115200;
5. `record progress` is the short, acquisition-safe status frame;
   `record metrics` is the full stopped/diagnostic report.

The preferred host command executes the first four data steps and generates
CSV/JSON/Markdown exports:

```powershell
python .\tools\tracebox_cli.py full --port COM9 --seconds 60 --dump-baud 460800
```

`fault torn` writes a body-only page immediately after the recovered valid
prefix. Press reset after it reports `commit_written=0`; boot recovery must
retain the prefix and report `rejected_torn_pages=1`. Use a short three-second
capture for this test so an erased spare page remains in the final prepared
sector. Do not use it after a full 6,250-page capture.

The USART1 ISR only reads `SR/DR`, publishes the byte into a 128-byte power-of-two
ring buffer and updates counters. Parsing, echo and command execution run in the
cooperative main loop.

## V2 Mini-DCM event ingress

V2 keeps the complete V1 ADC/NOR path and adds an independent SPI2 slave input:

| Signal | TraceBox pin | Mini-DCM Tester pin |
|---|---:|---:|
| NSS | PB12 | PB12 |
| SCK | PB13 | PB13 |
| MISO | PB14 | PB14 |
| MOSI | PB15 | PB15 |
| READY | PE0 output | PE1 input |
| Ground | GND | GND |

`trace prepare N` erases capacity for N fixed 32-byte TraceFrames and
`trace record N` arms SPI2 RX DMA. READY is a single end-to-end credit: it is
returned only after the accepted event has been committed to W25Q64, not when
the ingress DMA buffer is merely released. Use `trace metrics` and
`trace ingress` for accepted/rejected/CRC/sequence, descriptor, DMA/SPI,
high-water and ISR-cycle counters.

The V2 source-level walkthrough is `docs/10_v2_minidcm_spi_integration.md` at
the repository root. Do not connect V2 signals to SPI1 PA5/PA6/PA7/PB0; those
pins remain dedicated to the W25Q64 module.

## Fatal LED codes

Repeated groups of pulses indicate startup/fault paths: 2 clock/NMI, 3 UART
configuration, 4 HardFault, 5 generic/Memory fault, 6 BusFault, 7 UsageFault,
8 SPI initialization, 9 CRC initialization, 10 ADC/DMA initialization,
11 NOR state-machine initialization, 12 recorder initialization.
These are fallback signals when the UART is unavailable.
