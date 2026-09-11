# ESP-NOW Harness

ESP32-C5 を同一 firmware の汎用 node として使い、Windows host から role と scenario を割り当てる ESP-NOW benchmark harness です。small-packet PPS、RTT/tail latency、jitter、loss、duplicate、out-of-order、application goodput、RSSI、および latency-under-load を測定します。

```text
                            Windows PC
                         espnow-harness host
                         /                 \
                    USB serial          USB serial
                       /                     \
              ESP32-C5 Node A <--- ESP-NOW ---> ESP32-C5 Node B
```

両 node へ同一 binary を書き込みます。host が run ごとに `generator` / `sink` / `ping_initiator` / `echo_responder` を設定します。

## ESP32-C5 / ESP-IDF API decisions

この実装は **ESP-IDF v6.1** の ESP32-C5 headers、Programming Guide、公式 `examples/wifi/espnow` に合わせています。

- Wi-Fi STA を `esp_wifi_start()` した後に `esp_now_init()` し、`esp_now_register_recv_cb()` / `esp_now_register_send_cb()` を登録します。
- v6.0 では旧 `esp_wifi_config_espnow_rate()` は削除済みです。peer を `esp_now_add_peer()` した後、`esp_now_set_peer_rate_config()` と `wifi_tx_rate_config_t { phymode, rate, ersu, dcm }` を使います。
- C5 は single-radio dual-band です。同時 dual-band ではありません。scenario の ISO country code を `esp_wifi_set_country_code(..., false)` で固定してから、`esp_wifi_set_band_mode(WIFI_BAND_MODE_*_ONLY)`、`esp_wifi_set_channel()` の順で設定します。v1 scenario validation は 2.4 GHz ch 1–14 と、非 DFS の 5 GHz ch 36/40/44/48/149/153/157/161/165 に限定しますが、country ごとの最終的な可否は IDF が判定します。日本での最初の試験は `country = "JP"`, ch 36 を推奨します。
- receive callback の `esp_now_recv_info_t::rx_ctrl->rssi` を packet RSSI として取得します。
- `esp_wifi_set_max_tx_power()` は 0.25 dBm 単位です。scenario は dBm で指定し、firmware へ渡す前に 4 倍します。IDF header の範囲は 2–20 dBm です。
- power save は既定で無効 (`WIFI_PS_NONE`)。比較時のみ `WIFI_PS_MIN_MODEM` を選択します。
- control transport は ESP32-C5 内蔵 USB Serial/JTAG の bidirectional console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`) です。USB-UART bridge の COM port も、同じ stdin/stdout へ接続される board configuration なら利用できます。
- ESP-NOW v1 peer との互換性と VR の small packet 用途を優先し、application frame は 28–250 bytes に制限します。`packet_size` は benchmark header と payload を含み、`esp_now_send()` に渡す全長そのものです。

Primary references: [ESP-NOW API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c5/api-reference/network/esp_now.html), [ESP32-C5 Wi-Fi overview](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c5/api-guides/wifi-driver/overview.html), [USB Serial/JTAG console](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c5/api-guides/usb-serial-jtag-console.html), [official ESP-NOW example](https://github.com/espressif/esp-idf/tree/v6.1/examples/wifi/espnow), [`esp_now.h` v6.1](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_wifi/include/esp_now.h), [`esp_wifi.h` v6.1](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_wifi/include/esp_wifi.h).

## Architecture and timing path

The wire header is 28 bytes, big-endian, and manually encoded—there is no compiler struct cast or padding dependency:

```text
magic:u32 | version:u8 | type:u8 | flags:u16 | run_id:u32 |
stream_id:u16 | packet_len:u16 | seq:u32 | timestamp_us:u64
```

RX/TX callbacks only capture `esp_timer_get_time()`, RSSI, completion status and a bounded frame copy into a statically allocated FreeRTOS queue. Parsing, echo, sequence tracking, statistics and JSON occur in lower-priority tasks. There is no per-packet logging and no heap allocation in the measurement send/receive path.

The generator uses absolute deadlines (`start + index * 1_000_000 / rate`) so delay does not accumulate. It sleeps for the coarse part and uses a short microsecond delay only near the deadline. Streams are round-robin interleaved. Saturation keeps one ESP-NOW send outstanding and proceeds on send completion, matching Espressif's ordering guidance and avoiding an unbounded driver queue.

RTT uses only the initiator's `esp_timer` clock: its timestamp is echoed unchanged. Up to 8192 RTT samples are retained. Beyond that, deterministic bounded reservoir replacement is used. Percentiles are nearest-rank estimates over the reservoir; exact min/max, count, mean and standard deviation are accumulated over all samples. Jitter is reported as mean/max absolute difference between consecutive RTT samples. One-way latency is intentionally not reported.

Sequence detection uses a 64-packet sliding bitmap per stream. It detects recent duplicates and reordering; packets arriving more than 63 behind the current highest sequence are conservatively classified out-of-order. Host aggregate loss uses submitted TX versus unique RX, which also captures trailing loss that receiver-side gap counting cannot infer.

State transitions are explicit:

```text
BOOT -> IDLE -> CONFIGURED -> ARMED -> RUNNING -> FINISHED
                                      \-------> ERROR
```

Every data frame and state-changing command is checked against `run_id`; stale frames never enter current-run measurements. Invalid state transitions return structured errors.

## Prerequisites

- Windows 10/11 and two ESP32-C5 evaluation boards with a 2.4/5 GHz-capable antenna/RF path.
- ESP-IDF v6.1 installed with ESP32-C5 tools. Open an ESP-IDF PowerShell so `idf.py` and the RISC-V compiler are on `PATH`.
- Stable Rust toolchain (tested here with rustc/cargo 1.97.0).
- A data-capable USB cable for each board. Native USB Serial/JTAG uses ESP32-C5 GPIO14 D+ and GPIO13 D-. Boards with a bridge may require that bridge vendor's VCP driver. Espressif Installation Manager can install Espressif USB/JTAG drivers.

## Build and flash firmware

In an ESP-IDF v6.1 PowerShell:

```powershell
cd firmware
idf.py set-target esp32c5
idf.py build
idf.py -p COM5 flash
idf.py -p COM6 flash
```

Flash the same `firmware/build/espnow_harness.bin` project image to both boards. Do not leave `idf.py monitor` attached while the host owns the COM ports. If native USB does not enumerate on first flash, hold BOOT, tap RESET, release BOOT, then flash the newly appearing COM port.

## Build and test host

```powershell
cargo build --release -p espnow-harness
cargo test --workspace
```

Pure firmware wire-code can also be tested with any desktop CMake/C compiler:

```powershell
cmake -S firmware/test -B firmware/test/build
cmake --build firmware/test/build
ctest --test-dir firmware/test/build --output-on-failure
```

## First benchmark

1. Connect both nodes and discover only devices that answer the non-mutating versioned `info` probe:

   ```powershell
   target\release\espnow-harness.exe devices
   ```

2. Validate a scenario without touching hardware:

   ```powershell
   target\release\espnow-harness.exe validate scenarios\basic.toml
   ```

3. Run the basic stream and RTT scenarios. Explicit ports avoid ambiguity if other harness boards are connected:

   ```powershell
   target\release\espnow-harness.exe run scenarios\basic.toml --node-a COM5 --node-b COM6
   target\release\espnow-harness.exe run scenarios\latency.toml --node-a COM5 --node-b COM6
   ```

4. Run saturation throughput and load-latency tests:

   ```powershell
   target\release\espnow-harness.exe run scenarios\throughput.toml --node-a COM5 --node-b COM6
   target\release\espnow-harness.exe run scenarios\latency-under-load.toml --node-a COM5 --node-b COM6
   ```

If exactly two harness nodes are connected, the `--node-a/--node-b` arguments may be omitted. Discovery writes only an `info` JSON line and ignores unrelated boot text; it never configures or resets an unknown serial device.

## Scenario schema

See [`scenarios/`](scenarios). Unknown TOML fields are rejected. `duration` accepts values such as `500ms`, `10s`, or `2m`. `radio.country` is a two-letter uppercase ISO country code and is part of saved reproducibility metadata. Valid modes are `ping`, `stream`, `multi-stream`, `saturation`, and `latency-under-load`. v1 exposes these PHY labels: `1m` (2.4 GHz only), `6m`, `24m`, `54m`, `mcs0`, `mcs7`, `he-mcs0`, `he-mcs7`.

For encryption, add `key = "00112233445566778899aabbccddeeff"`; it is used as the shared 16-byte PMK and LMK in this two-node benchmark. Do not reuse a production secret in result-producing experiments.

`latency-under-load.toml` uses stream 15 for probes and streams 0–14 for background. Its 1000 × 250-byte frames/s request 2 Mbit/s of application traffic. Requested bandwidth excludes Wi-Fi/ESP-NOW overhead.

## Control protocol

Control is JSON Lines, version 1. Each request carries an `id`; each response echoes it and has `ok`, structured `code/message` on failure, and optional `state`, `info`, or `result`.

```json
{"version":1,"id":1,"cmd":"info"}
{"version":1,"id":2,"cmd":"configure","run_id":42,"role":"generator","peer_mac":"aa:bb:cc:dd:ee:ff","radio":{},"traffic":{},"duration_us":10000000}
{"version":1,"id":3,"cmd":"arm","run_id":42}
{"version":1,"id":4,"cmd":"start","run_id":42}
{"version":1,"id":5,"cmd":"result","run_id":42}
```

The host performs configure A/B, arm receiver, arm generator, start receiver, start generator, stop, then result collection. Errors cover incompatible version, invalid packet size/config, invalid state, radio/channel/rate failure, run mismatch, serial timeout/disconnect, peer/send failure, queue pressure, and stale/invalid frame counters. A reboot normally appears as a command timeout or loss of configured state.

## Results and interpretation

Each run creates:

```text
results/2026-09-12T001234Z-basic/
  scenario.toml
  metadata.json
  result.json
  summary.csv
```

Metadata includes host/firmware/IDF versions, optional Git commit, node MAC/capabilities, full radio/scenario configuration, start time and requested duration. JSON retains per-node/per-stream counters, RSSI histogram (-127 through 0 dBm), RTT distribution and aggregate cross-node loss. CSV flattens scalar fields for comparison.

- RTT is A→B→A application echo time, not one-way delay. p99 means 99% of retained RTT samples are at or below that value; inspect p99.9/max for tracking-relevant stalls.
- Packet loss is submitted frames minus unique received frames. Send callback success is MAC-layer delivery, not proof that the application processed the frame.
- Goodput is received benchmark application bytes per second, including the 28-byte harness header but excluding ESP-NOW/Wi-Fi overhead.
- RSSI is per received ESP-NOW frame. Compare distributions, not only the mean.
- Requested PPS and achieved TX/RX PPS differ when airtime, completion serialization, CPU load, or the driver limits the run.

## Current scope and extension points

Implemented: identical configurable firmware; 16 logical streams; ping, constant-rate stream, interleaved multi-stream, completion-limited saturation, latency-under-load; radio band/channel/rate/power/power-save/encryption configuration; bounded metrics; discovery/orchestration; JSON/CSV/human results; protocol/scenario/sequence/percentile/serialization tests.

Not implemented in v1: trace replay, synchronized one-way latency, GPIO sync, 3+ node orchestration, automatic sweeps, GUI/graphs. A future trace-driven generator should emit the same `{deadline, packet_size, stream_id, type}` scheduling items as the constant-rate generator; no wire-format change is required. A future protocol version can add clock-domain and sync metadata without reinterpreting the v1 timestamp.

Before trusting numbers, verify the actual evaluation board has a suitable dual-band RF path, use identical power supplies/cables, fix node placement/orientation, record interference/channel/regulatory settings, perform a warm-up run, and repeat each scenario several times.
