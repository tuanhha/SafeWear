# SafeWear protocol v1 — implementation contract

Locked 2026-10-15. Node STM32F103C8 (HAL), SX1278 -> ESP32 RadioLib -> USB NDJSON -> Python -> local web. All multibyte integers LITTLE ENDIAN. Signed fields two's complement. CRC16 CCITT-FALSE (poly 0x1021, init 0xFFFF, refin/refout false, xorout 0), over all bytes except final two CRC bytes. CRC stored little endian. Radio hardware CRC also enabled. No RF ACK/downlink in v1.

Extension 2026-10-23: type 3 PPG features below. Types 1/2 retain their byte layout. Update all three decoders together before using type 3; older receivers correctly reject unknown types.

## Experimental PPG feature extension

PPG exactly 70 bytes, existing 16-byte header with type=3. Source uptime is the
10-second window end, not the transmit time. Layout (little endian):

| Offset | Type | Meaning |
|---|---|---|
| 16 | u16 | flags: bit0 QUALITY_VALID, bit1 GAP, bit8 SIMULATED; all others rejected |
| 18 | u16 | window_ms=10000 |
| 20 | u16 | sample_count<=250 |
| 22 | u16 | lost_ppg_samples, saturated at65535 |
| 24 | u8 | feature_schema=1 |
| 25 | u8 | feature_count=12 |
| 26 | int16[12] | encoded PPG features |
| 50 | u16 | age_ms at dispatch, maximum2000 |
| 52 | byte[16] | reserved zero |
| 68 | u16 | CRC16 over bytes0..67 |

QUALITY_VALID requires >=225 samples, no losses/GAP, and passing the experimental
optical and motion gate. Invalid windows are still reported with zero features;
missing samples are never filled. PPG frames do not create or resolve alarms.
Hardware mode rejects SIMULATED. Existing 140-hex-character USB/HTTP payload
envelope fits unchanged. PPG has a separate latest-only bounded telemetry slot;
EVENT always preempts it. At dispatch, telemetry receives a shared sequence ID
and recomputed CRC, preserving receive order. Events retain their immutable IDs.
Snapshot HR/SpO2 ages include node queue delay. Discard telemetry waiting >2s.

Schema1 uses 25Hz FIFO output (MAX100Hz, averaging4), half-open 10-second
windows. First window starts2.5s after initialization to stagger PPG against5s
snapshots. FIFO timestamps are reconstructed from configured cadence and observed
drain time; they are not hardware timestamp measurements. Excess observed age,
FIFO overflow, bus fault, >80ms gap or invalid/moving IMU invalidates the window.

Feature order: mean IR/10; mean red/10; IR population std/DC*10000; red
population std/DC*10000; red/IR correlation*10000; mean trough interval ms;
population std of intervals ms; mean trough-to-peak rise ms; mean peak-to-trough
decay ms; mean width above half amplitude ms; mean normalized maximum rise
slope per second*100; mean normalized maximum decay slope per second*100.
Quantization is float32 then half-away-from-zero, saturated int16.

Require all optical values1000..249999, both CVs .001.. .2, correlation>=.8,
at least3 complete beats. Trough is the rightmost point of a plateau; between
successive troughs select the first maximum. Accept duration320..2000ms,
interior maximum, amplitude>=0.5*window IR std; baseline is mean endpoint IR.
Width integrates adjacent sample intervals whose mean IR is >=baseline+amp/2.
Slope uses actual sample delta and divides by beat amplitude. This feature gate
does not establish physiological accuracy. C/Python extraction must agree on
encoded values; see node portable checks and docs/bp-data.md.

`nodes[].blood_pressure` adds nullable systolic_mmhg/diastolic_mmhg, status,
experimental=true, age_ms and received_at. Age includes node queue delay and
Gateway/HTTP delay; >15s clears numbers even when other telemetry continues.
Source uptime/window length and transport ages are retained in history.
`GET /api/bp?node_id=<optional>&limit=100` returns persisted experimental
records (limit1..1000), protected by the dashboard authentication. Receipt time
is server wall time; source uptime is not synchronized UTC. Model and local
node-to-person calibration are administrator-owned files, never HTTP uploads.
No model or calibration => no BP numbers; estimates never drive alarms.

## Radio header and frames

Common 16-byte header: version u8 @0 =1; type u8 @1 (1=snapshot,2=event); node_id u16 @2; boot_id u32 @4; sequence u32 @8; source_uptime_ms u32 @12. boot_id changes each boot. sequence increments for a new logical frame; retransmitted EVENT bytes and sequence remain identical. Source uptime wraps modulo 2^32. Reject unknown version/type/length/CRC.

EVENT exactly20 bytes: header; event_code u8 @16 (1=MANUAL_SOS,2=FALL_SUSPECT,3=LOCAL_CLEAR); reason u8 @17 (bit0 threshold,bit1 button,bit7 simulated); CRC u16 @18. Event ID is node_id:boot_id:sequence. LOCAL_CLEAR header boot/sequence identifies the original event, not a new event; source uptime may be current, and code differs. It only reports the wearable latch was cleared, never resolves the operator alarm.

SNAPSHOT exactly70 bytes: header; flags u16 @16; hr_bpm_x10 u16 @18; spo2_percent_x100 u16 @20; battery_mv u16 @22; hr_age_ms u16 @24; spo2_age_ms u16 @26; imu_window_ms u16 @28 (=5000); imu_sample_count u16 @30; lost_imu_samples u16 @32; lost_ppg_samples u16 @34; active_event_boot_id u32 @36; active_event_sequence u32 @40; feature_schema u8 @44 (=1); feature_count u8 @45 (=10); features int16[10] @46; reserved zero u16 @66; CRC u16 @68. Unknown numeric HR/SpO2/battery/ages =65535. Event reference zero/zero when none. Only one active local event; MANUAL_SOS takes priority over FALL_SUSPECT; an escalation creates a new event ID.

flags: bit0 HR_VALID;bit1 SPO2_VALID;bit2 SOS_ACTIVE;bit3 IMU_VALID;bit4 PPG_FAULT;bit5 BATTERY_LOW;bit6 FALL_ACTIVE;bit7 SENSOR_GAP;bit8 SIMULATED. Invalid values are not replaced with previous normal readings. Flags validity and per-value age are both required; server uses last receive age as well.

## IMU feature schema1

50Hz accelerometer, fixed5s half-openwindow [start,start+5000),250 nominal samples; final1s has timestamps>=start+4000. Acceleration measured in g. Require >=225 valid samples and no intersamplegap>60ms to set IMU_VALID. Feature order (int16, round half away from zero like C lroundf, saturate [-32768,32767]): mean_x*1000,mean_y*1000,mean_z*1000,std(norm)*1000,min(norm)*1000,max(norm)*1000,rms(norm)*1000,max(abs(diff(norm))/dt_seconds)*10,range(acos(z/norm) degrees)*100,std(norm in final1s)*1000. Population standard deviation. Clamp acos input [-1,1]; zero norm tilt=0. Jerk uses actual delta between samples; invalid/large gaps invalidate window. Clamp lost counters at65535. Model input is precisely these10 encoded integers; training and inference must match, not a raw CNN.

## Radio profile and scheduler

433.92MHz,SF7,BW500kHz,CR4/5,preamble8,explicit header,CRC on,syncword0x12,TX+7dBmmax,documentedantenna<=2.15dBi,noamplifier.70B airtime32.064ms;20B14.144ms. Snapshot target5s. After any transmission wait >=99*actual airtime (snapshot3175ms,event1401ms roundedup). Boot conservative cooldown>=3200ms. Events priority, at most3 transmissions total per event; coalesce held button/persistent cause; latest-only bounded snapshotqueue. TX has deadline, never block sampling. Send active-event reference in snapshots for lost-event recovery. No duty burst credits. Hardware reception/coverage/conformity remain unverified.

## USB to Python

115200baud,UTF8 NDJSON one object per LF,max512bytes/line; discard overlongline until nextLF. No unstructured debug on gateway USB.

RX example {"kind":"rx","payload":"<40or140hexchars>","rssi":-70.5,"snr":8.0,"gateway_ms":1234}. Heartbeat every1s {"kind":"heartbeat","gateway_ms":1234,"radio_ok":true,"rx_total":0,"crc_errors":0,"queue_drops":0}. Diagnostics {"kind":"diagnostic","message":"boundedtext"}. Serial queue bounded, count drops, preserve event priority. PC does not infer delivery from TX_DONE.

PC->Gateway optional local-only USB commands NDJSON {"kind":"alarm_ack","event_id":"node:boot:sequence"} and {"kind":"alarm_resolve","event_id":"node:boot:sequence"}; clear/silence Gateway alarm only for that event, no RF forwarding. Gateway rejects commands >512bytes, stores a bounded recent event set to avoid reopening resolved IDs. Node feedback confirms local acceptance only, never receipt by manager.

## SOS semantics and state

Debounce30ms,hold1s to activate,local feedback<=250ms AFTER qualification. Continued holding original press cannot cancel. While active, release fully then a NEW hold5s clears wearable latch; preserve remote alarm until operator resolve. Persist active kind+original boot/sequence over reboot; journal writes only on transitions. ALARM -> ACKNOWLEDGED -> RESOLVED on server; ACK doesn't resolve, disconnect doesn't resolve, late packets don't reopen a resolved same ID. New distinct event IDs remain actionable. FALL uses a temporal threshold baseline, not one high-g sample; ML is an annotation, never prerequisite for SOS/basic alarm.

## Backend/frontend public interface

GET /api/latest gives {mode,source_status,diagnostics,nodes,events,model}. GET /api/events lists persistent records, POST /api/events/{event_id}/ack and /resolve mutate operator state. WebSocket /ws broadcasts full latest object at1Hz, including staleness recalculation even if no packets arrive. Node list includes id,connection,hr,spo2,battery_mv,hr_quality,spo2_quality,hr_age_ms,spo2_age_ms,last_seen_age_ms,activity,activity_source,active_event_id. Human text Vietnamese. Heartbeat stale>3s means gateway stale;node>15s offline;HR/SpO2>10s or invalidbit =>null/invalid. Empty startup has no fabricated values. Demo has persistent visible mode banner and SIMULATED bits, and separate DB. Demo API controls disabled outside demo mode.

Backend binds127.0.0.1 by default, no external services. Model absent=>modelstatus untrained and rule baseline labeled; never claim synthetic training as validated worker model. Browser closure must not stop receiver/storage/alarm worker. SQLite writes serialized. All queues bounded. Event history survives PC restart and dedup by event_id; raw frame bytes and source/receive times available for audit.

## Ownership and hardware pin defaults

Root owns backend,codec,web,tests,integrationdocs. Radio agent owns firmware/node only. Gateway agent owns firmware/gateway only. ML agent owns backend/safewear/ml.py,tools/train_model.py,tests/test_ml.py,docs/ml-data.md and ML third-party notices only. No files outside owner without coordination.

Node defaults: SPI1 PA5SCK/PA6MISO/PA7MOSI,PA4NSS,PB0radioRESET,PB1DIO0;MPU I2C1 PB6/PB7 400kHz;MAX I2C2 PB10/PB11 initial80kHz cable<=50cm;SOS PB8 pullup; buzzer PB9 via driver;LED PC13 active-low; USART1 PA9TX/PA10RX debug; battery PA0 ADC divider2.0 only after actual board check, otherwise unknown. Reserve final2KiB Flash for journal;do not assume128KiBchip.

Gateway defaults ESP32dev: SPI18SCK/19MISO/23MOSI,5NSS,14RESET,26DIO0,buzzer25 via driver;USB115200. Both boards/pinout need physical verification before flashing. No automatic flash/programming in this work.

## Optional wired bench mode (not a LoRa coverage demonstration)

One LoRa module alone cannot provide an RF transmit/receive pair. Separate node_wired/gateway_wired build environments define SAFEWEAR_WIRED_ONLY=1. Node emits the same CRC-checked EVENT/SNAPSHOT frames wrapped in RX NDJSON on USART1 PA9TX115200; no RF initialization/transmission in this mode. Connect STM32PA9TX -> ESP32GPIO32RX (Serial2) plus commonGND,3.3Vlogic. ESP32 USB sends bounded forwarded RX lines and own heartbeat with transport='wired',radio_ok=false. Normalgatewayheartbeattransport='lora'. Serial/root UI explicitly distinguishes wired from LoRa;wiredmeasurements remain actualmeasurements,notSIMULATED. No newUSBTTLadapterrequiredwhenusingESP32asbridge. Do not bridge powerrails blindly; verifybothboardsupply/GND. NodeUARTTXbounded/nonblocking; avoidmixingdebugtextwithNDJSON.
