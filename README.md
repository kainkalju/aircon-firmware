# Daikin BRP069A42 Firmware Analysis

## Binary facts
- File: `daikin.fw`, 489,248 bytes (extracted from `firmware.post` multipart)
- Version: **3.4.3** (`dkacstm-wlan-F-3_4_3.bin`)
- MCU: **STM32F2xx** (ARM Cortex-M3, flash @0x08000000, SRAM @0x20000000, SP=0x20020000 → 128 KB RAM)
- WiFi: **Murata WLAN module** (Broadcom BCM43362 or similar) connected via **SDIO**
- WLAN driver: Broadcom SDPCMD CDC v221.1.11.0, built 2013-01-24

## Architecture
- Lightweight cooperative/preemptive task kernel (FreeRTOS-style, proprietary API)
- Tasks: AppMain, VerificationSerialTask, serialComm, echonet_task, cloud_task
- Event queues: gQ_PKTRX, gQ_PKTRX_COMPLETE (SDIO packet pipeline)
- Serial to indoor unit: USART1 (TIM3 drives 20 ms polling frames)
- WLAN IRQ: EXTI15_10 on PC11

## Active IRQ handlers
| IRQ | Address | Purpose |
|-----|---------|---------|
| SysTick | 0x080380A4 | 1 ms tick counter |
| HardFault | 0x080380CC | Crash dump + reboot |
| USART1 | 0x08020A9E | Serial RX/TX (linked-list FIFO) |
| TIM3 | 0x08024BB8 | 20 ms serial polling timer |
| EXTI15_10 | 0x080240DC | WLAN packet ready interrupt |

## HTTP API (port 80, HTTP/1.0)
Full route table at 0x08075154. Key endpoints:

### Aircon
- GET/POST `aircon/get_control_info` / `aircon/set_control_info`
- GET `aircon/get_sensor_info` — htemp, hhum, otemp, err, cmpfreq, mompow
- GET `aircon/get_model_info` — capabilities (elec, dmnd, en_scdltmr, en_spmode...)
- GET/POST `aircon/get_timer` / `aircon/set_timer`
- GET/POST `aircon/get_demand_control` / `aircon/set_demand_control`
- GET/POST `aircon/get_scdltimer` / `aircon/set_scdltimer` / `_body`
- GET/POST `aircon/get_price` / `aircon/set_price`
- GET `aircon/get_week_power`, `get_year_power`, `get_month_power_ex`
- POST `aircon/set_special_mode` (streamer/humidifier)
- POST `dkac/system/fwupdate` (OTA update, multipart/form-data)

### Common
- GET `common/basic_info` — full device info
- GET/POST `common/get_wifi_setting` / `set_wifi_setting`
- GET/POST `common/get_remote_method` / `set_remote_method`
- GET `common/start_wifi_scan` + `get_wifi_scan_result`
- POST `common/reboot`
- POST `common/notify_date_time`

## UDP protocol (DAIKIN_UDP, port unknown)
- `DAIKIN_UDP/common/basic_info`
- `DAIKIN_UDP/debug/timeinfo|s_debug_on|s_debug_off|myconsole`
- `DAIKIN_UDP/debug/loglevel=[e|i|v]`
- `DAIKIN_UDP/debug/demandON|demandOFF`
- `DAIKIN_UDP/debug/24h_info`

## ECHONET Lite (UDP port 3610)
- Class 0x0130 (Home Air Conditioner)
- EPC 0x80=power, 0xB0=mode, 0xB3=temp, 0xBB=room_temp, 0xBE=outdoor_temp
- Supports Get/SetC/INF service codes

## Cloud HTTPS
- Primary: `sha2.daikinonlinecontroller.com`
- Alt: `test2.daikindev.com`, `www.daikincloud.net`, `daikinsmartdb.jp`
- TLS with GlobalSign Root CA (embedded at 0x080770E0)
- POSTs to `/aircon/notice` and `/aircon/error_notice`
- Poll interval: `notice_sync_int` (default 3600 s)

## Response format
All responses use Daikin proprietary key=value format:
- `ret=OK,key1=val1,...`
- `ret=PARAM NG`
- `ret=INTERNAL NG,msg=...`

## Reversed source files
| File | Description |
|------|-------------|
| `daikin.h` | All types, constants, prototypes |
| `startup.c` | Vector table, Reset_Handler, HardFault, SysTick |
| `main.c` | AppMain, clock init, task spawning |
| `uart.c` | USART1 driver, TIM3/EXTI ISRs, serial framing |
| `network.c` | SDIO/Broadcom driver, WiFi, UDP dispatch |
| `http_server.c` | HTTP/1.0 server, route table, parser |
| `aircon_api.c` | All aircon/* endpoints |
| `common_api.c` | All common/* endpoints, server name |
| `echonet.c` | ECHONET Lite protocol |
| `cloud.c` | HTTPS cloud client |
| `util.c` | Logging, delays, IP helpers |
