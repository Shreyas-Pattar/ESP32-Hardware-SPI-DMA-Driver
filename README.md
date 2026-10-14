# Timer-Driven SPI Master DMA Streaming on ESP32

ESP32 firmware (Arduino core, built with PlatformIO) that streams a 1 KB buffer out of the SPI2 master using the ESP-IDF SPI driver with DMA. A 100 Hz periodic timer triggers a worker task, which queues one DMA transaction per tick and waits for it to complete. The firmware measures the CPU cycles spent queuing each transfer.

## What it demonstrates

- **SPI master with DMA:** `spi_bus_initialize()` with `SPI_DMA_CH_AUTO`, a device configured for SPI mode 0 at 10 MHz with a hardware chip-select, and a 7-deep transaction queue.
- **DMA-capable memory:** the 1,024-byte transmit buffer is allocated with `heap_caps_malloc(..., MALLOC_CAP_DMA)` and filled with a known byte pattern (`i & 0xFF`).
- **Queued transactions:** each burst uses `spi_device_queue_trans()` followed by `spi_device_get_trans_result()`.
- **Periodic dispatch:** an `esp_timer` periodic timer fires every 10 ms and releases a binary semaphore. The `DMAStreamer` task (Core 0, priority 3) takes it and starts a burst.
- **Cycle profiling:** the time spent in `spi_device_queue_trans()` is measured with the CPU cycle counter (`xthal_get_ccount()`).

## Architecture

```text
 esp_timer (10 ms)          Core 0
┌─────────────────┐  sem  ┌───────────────────────────────┐   SPI2 (DMA)    ┌─────────┐
│ timer callback  │ ────▶ │ DMAStreamer (prio 3)          │ ──────────────▶ │ MOSI/CLK│
│ gives semaphore │       │ queue_trans → get_trans_result│  1024 B @ 10 MHz│  /CS    │
└─────────────────┘       └───────────────────────────────┘                 └─────────┘
```

| Signal | GPIO |
|---|---|
| MOSI | 23 |
| MISO | 19 |
| SCLK | 18 |
| CS | 5 |

The transmit buffer is 1,024 bytes (8,192 bits). At 10 MHz, the bus time per burst is about 819 µs (calculated, not measured), roughly 8% of the 10 ms period.

## Results

Captured over UART at 115200 baud on an ESP32 WROOM-32 dev board. The firmware logs every 10th burst:

```text
[SPI-DMA] Controller initialized at 10 MHz with active DMA Channel.
[TIMER] Hardware periodic interrupt active (100 Hz dispatch rate).
[DMA BURST #0010] 1024 Bytes dispatched over SPI | CPU enqueuing cost: 1245 cycles
[DMA BURST #0020] 1024 Bytes dispatched over SPI | CPU enqueuing cost: 1245 cycles
...
[DMA BURST #0070] 1024 Bytes dispatched over SPI | CPU enqueuing cost: 1253 cycles
...
[DMA BURST #3860] 1024 Bytes dispatched over SPI | CPU enqueuing cost: 1245 cycles
```

| Measurement | Value |
|---|---|
| Enqueue cost (steady state) | 1,245 cycles (about 5.2 µs at 240 MHz) |
| Enqueue cost (occasional) | 1,253 cycles (+8), seen every 70th burst in the log |
| Bursts in the capture | about 3,860 (about 39 s at 100 Hz), no errors logged |

## Limitations

- **No receiver is attached, and nothing is received.** The firmware does not check that the transmitted bytes are correct; it only confirms the driver completes each transaction without an error.
- **The timer is not a hardware ISR.** With the default `esp_timer` dispatch method, the callback runs in the `esp_timer` task, so the title's old "ISR" wording does not apply.
- **No baseline.** The enqueue cost is not compared against a blocking (polling) transfer, so this project does not measure how much CPU time DMA saves.
- **Overruns are not detected.** A binary semaphore merges triggers that arrive while a burst is in progress, so a late burst would not be counted.
- **Sparse logging.** Only every 10th burst is printed, with no min / max / average and no period jitter.
- **Init errors are not fatal.** If SPI or buffer setup fails, the worker task still starts.

## Build and run

Requirements: PlatformIO and an ESP32 dev board (`esp32dev`).

```bash
pio run -t upload
pio device monitor      # 115200 baud
```

```text
├── platformio.ini
└── src/
    └── main.cpp
```

## Roadmap

- [ ] Loopback test: jumper GPIO23 to GPIO19, receive into a buffer, `memcmp` each burst and count mismatches
- [ ] Add a baseline using `spi_device_polling_transmit()` and compare CPU time against DMA (with a low-priority counter task running)
- [ ] Count overruns and report min / max / average cost and period jitter
- [ ] Use an ISR-dispatched timer or rename it as a timer task
- [ ] Stop on init failure instead of starting the worker task
