# SafeWear ESP32 Gateway

Firmware ESP32 nhận gói SafeWear v1, cảnh báo tại trạm và chuyển dữ liệu đã kiểm tra sang máy chủ bằng USB hoặc HTTP trên mạng nội bộ. Hợp đồng dữ liệu ở `../../docs/protocol-v1.md` và `../../docs/http-uplink-v1.md`. Đây là mã có thể biên dịch; chưa được thử trên phần cứng thực và không tự nạp firmware.

## Bốn target

* `gateway_lora`: nhận LoRa bằng Ra-02/SX1278; cần một module tại STM32 và một module tại ESP32, mỗi module có anten phù hợp.
* `gateway_wired`: kiểm tra nối dây bằng USART1 STM32 → Serial2 ESP32; không khởi tạo hoặc phát RF. Heartbeat ghi rõ `transport="wired"`, `radio_ok=false`. Đây là kiểm tra luồng dữ liệu thực qua dây, không chứng minh vùng phủ LoRa.
* `gateway_lora_http`: thu LoRa, worker riêng gửi HTTP lên Pi; vẫn xuất USB để kiểm tra.
* `gateway_wired_http`: nhận UART từ STM32, worker riêng gửi HTTP lên Pi; ghi rõ `transport="wired"`.

## Chân mặc định — phải đối chiếu board trước khi nạp

| ESP32 dev board | Ra-02/SX1278 hoặc thiết bị |
| --- | --- |
| GPIO18 | SCK |
| GPIO19 | MISO |
| GPIO23 | MOSI |
| GPIO5 | NSS/CS |
| GPIO14 | RESET |
| GPIO26 | DIO0 |
| 3V3, GND | Nguồn 3,3 V ổn định, chung GND |
| GPIO25 | Điều khiển còi chủ động qua transistor, active HIGH |
| GPIO32 RX2 | Nhận từ STM32 PA9 TX1 ở chế độ nối dây |
| GPIO33 TX2 | Dự phòng; không cần nối trong bản v1 |

Ra-02 và chân tín hiệu ESP32 dùng mức logic 3,3 V; không đưa tín hiệu 5 V trực tiếp vào chúng. Không cấp còi tải lớn từ GPIO. GPIO5 là chân ảnh hưởng cấu hình khởi động trên ESP32 cổ điển: xác minh module và điện trở ngoài không cản trở boot. Không nối tắt đường nguồn hai board khi chưa kiểm tra cách cấp nguồn.

Ở chế độ nối dây, chỉ cần STM32 PA9 TX → ESP32 GPIO32 RX và GND chung. Không cần đường TX trả về STM32. USB của ESP32 kết nối với PC; tránh mở cùng cổng USB đồng thời trong ứng dụng PC và Serial Monitor.

## Biên dịch

Từ thư mục gốc chuẩn `F:\SafeWear`, luôn nạp cấu hình môi trường trước khi chạy. Build, package, cache và file tạm đều nằm trên ổ F:

```powershell
Set-Location F:/SafeWear
. F:/SafeWear/tools/enter-environment.ps1
F:/SafeWear/.venv/Scripts/python.exe -m platformio run -d firmware/gateway -e gateway_lora
F:/SafeWear/.venv/Scripts/python.exe -m platformio run -d firmware/gateway -e gateway_wired
F:/SafeWear/.venv/Scripts/python.exe -m platformio run -d firmware/gateway -e gateway_lora_http -e gateway_wired_http
```

Không có lệnh upload tự động trong quy trình này. Platform được khóa `espressif32@6.10.0`, board `esp32dev`, Arduino core từ platform; RadioLib `7.8.1`, ArduinoJson `6.21.5`. Build output nằm tại `F:\SafeWear\.cache\build\gateway`; thư viện tải về tại `F:\SafeWear\.cache\libdeps\gateway`; toolchain tại `F:\SafeWear\.cache\platformio`.

## Wi-Fi / HTTP tới Pi

Sao chép `include/secrets.example.h` thành `include/secrets.h` rồi điền SSID, mật khẩu Wi-Fi, URL Pi và token Gateway thật. `secrets.h` được bỏ khỏi Git; không chia sẻ file này trong bản nộp công khai. Token Gateway riêng, ngẫu nhiên, ít nhất 32 ký tự; không dùng mật khẩu Dashboard làm token. Không có credential mặc định hoặc credential giả trong mã. Chưa điền đủ thì target HTTP vẫn biên dịch nhưng `http_enabled=false`, `http_status=-1000`; Wi-Fi chưa được bật. Cấu hình Wi-Fi hỗ trợ PSK 8–64 ký tự; `gateway_id` mặc định `gw-01` phải khớp cấu hình Pi.

URL phải có dạng `http://<địa-chỉ-Pi>:<cổng>/api/v1/gateway/ingest`, không query/fragment/credential trong URL. Bản này dùng HTTP trên LAN tách biệt/WPA2 đáng tin cậy; không hỗ trợ HTTPS hoặc kết nối Internet công khai. Muốn qua mạng không đáng tin cậy cần thiết kế TLS/VPN riêng. Pi phải bật HTTP ingestion; chỉ chọn **một nguồn USB hoặc HTTP** trong máy chủ, dù firmware vẫn giữ USB mirror để kiểm tra.

Worker FreeRTOS trên core0 thực hiện Wi-Fi/HTTP; vòng chính trên core1 nhận LoRa/UART và điều khiển AlarmBook/còi. Connect timeout 1.500 ms, read timeout 2.000 ms; đọc body vào bộ đệm cố định 4.096 byte, có deadline đọc body 6 giây. Không dùng `getString`, không redirect; yêu cầu Content-Length 1–4.096 và từ chối chunked/Transfer-Encoding; Connection:close. Các timeout này không phải kết quả đo độ trễ cảnh báo thực tế.

Hai hàng đợi HTTP cố định, mỗi hàng 8 gói. Sự kiện EVENT ưu tiên và được giữ để retry trong RAM; telemetry/heartbeat quá 15 giây bị bỏ, hàng thường đầy bỏ bản cũ. Nếu có EVENT mới trong lúc gói thường chờ retry, bỏ gói thường đó rồi gửi EVENT; không giữ gói có sequence thấp để gửi sau sequence cao. Worker chỉ có một request đang chờ; cấp sequence khi chọn gửi, retry giữ nguyên boot/sequence/message. Boot ID ngẫu nhiên 64 bit dạng 16 ký tự hex mỗi lần khởi động, đổi sau khi hết uint32 và request cuối đã xử lý. `age_ms` tính lại bằng đồng hồ monotonic 64 bit mỗi lần gửi; `active_event_ids` lấy bản sao từ vòng chính, gồm cả ID đã ACK chờ resolve, tối đa 32 ID.

Retry lỗi mạng/429/5xx tăng dần 1–30 giây; 401/403 khóa thử xác thực ít nhất 60 giây, kể cả khi gói thường hết hạn. Các 4xx không thể retry (gồm 409) và redirect bị bỏ, tăng `http_drops`. Không ghi token trong log. Heartbeat USB chứa `http_enabled`, `http_status`, `http_accepted`, `http_drops`. Drop/timeout/overflow không tắt còi. Các hàng đợi không bền vững qua mất nguồn.

Chỉ phản hồi 200 JSON đúng identity, `accepted=true`, `duplicate` kiểu bool và danh sách command hợp lệ mới hoàn thành gói. Lệnh chỉ áp dụng cho ID đã quảng bá trong request đó. Worker đẩy command qua hàng đợi hữu hạn; vòng chính áp dụng ACK/RESOLVE lặp an toàn, không phát diagnostic trả lại cho từng lệnh. **Máy chủ đã nhận bền vững không đồng nghĩa người quản lý đã ACK và không tự tắt còi.** Hàng đợi command đầy khiến request được retry cùng identity.

`include/http_contract.h` được sinh từ schema và SHA256 fixture chung bằng `tools/http_contract.py --generate`. Mỗi lần PlatformIO build đều kiểm tra drift; chỉ sinh lại sau khi đã xem thay đổi hợp đồng do owner cung cấp.

## Hành vi

Radio: 433,92 MHz, SF7, BW500 kHz, CR4/5, syncword `0x12`, preamble 8, explicit header, hardware CRC bật. Gateway chỉ thu; không có RF ACK/downlink. ISR chỉ đặt cờ; đọc SPI, giải mã và tạo JSON đều chạy ngoài ISR. Lỗi khởi tạo không khóa chương trình: `radio_ok=false`, chẩn đoán có mã trạng thái, thử lại mỗi 5 giây. Bộ thu yên lặng được tái kích hoạt sau 30 giây; `radio_ok` chỉ phản ánh kết quả thao tác phần mềm, không xác nhận vùng phủ/anten hay liên lạc với node.

USB: 115200 baud, một JSON/LF, mỗi dòng đầu ra tối đa 512 byte tính cả LF. Dòng đầu vào trên 512 byte hoặc có NUL bị bỏ đến LF kế tiếp. Mỗi lượt đọc tối đa 128 byte để không giữ luồng. ESP32 ROM có thể phát thông báo boot trước khi ứng dụng chạy; ứng dụng chỉ phát NDJSON, bộ nhận PC cần bỏ các dòng boot không phải JSON.

Hai hàng đợi cố định, mỗi hàng 8 dòng; sự kiện/cảnh báo được ưu tiên. Khi hàng thường đầy, bỏ bản cũ; khi hàng khẩn đầy, tăng `queue_drops`. Còi đã được cập nhật trước bước xếp hàng nên cảnh báo tại trạm không phụ thuộc PC. Heartbeat tạo mỗi giây, chứa bộ đếm lỗi/mất hàng đợi và số cảnh báo đang mở. USB không có cơ chế xác nhận PC đã lưu dữ liệu; khi PC mất kết nối, lịch sử chưa lưu có thể mất.

CRC16 CCITT-FALSE và độ dài/version/type được kiểm tra; snapshot phải dùng schema1/10 đặc trưng/reserved zero. EVENT trùng ID không mở lại hoặc chuyển lại thành một sự kiện mới. SNAPSHOT mang ID sự kiện đang hoạt động vẫn được chuyển để PC phục hồi một EVENT bị mất. `FALL_ACTIVE` riêng cũng kích hoạt cảnh báo; khi có cả hai cờ, MANUAL_SOS ưu tiên.

PPG_FEATURES (type 3, 70 byte) dùng schema 1/12 đặc trưng, cửa sổ 10 giây và reserved zero theo giao thức. Gateway kiểm cờ chất lượng, giới hạn mẫu và điều kiện valid, sau đó chuyển nguyên payload qua USB/HTTP. Gói này luôn là telemetry thường trong hàng đợi hữu hạn, không mở hay xóa cảnh báo. HTTP loại telemetry quá 15 giây; thử nghiệm RF và độ ổn định trên phần cứng vẫn chưa nghiệm thu.

`LOCAL_CLEAR` chỉ báo wearable đã bỏ latch, không tắt cảnh báo điều hành. Command USB chỉ chấp nhận `alarm_ack` hoặc `alarm_resolve` kèm ID `node:boot:sequence` hợp lệ. ACK tắt âm cho đúng ID nhưng vẫn giữ cảnh báo mở; RESOLVE đóng đúng ID. ID chưa biết không thể bị pre-resolve. Không command nào được chuyển qua RF/UART về wearable.

Lưu tối đa 32 cảnh báo mở và 64 ID vừa resolve trong RAM. Không ghi đè cảnh báo mở khi đầy; lỗi quá dung lượng giữ còi cho tới khi khởi động lại và phát chẩn đoán. Bộ nhớ chống mở lại trên Gateway có giới hạn 64 lần resolve và bị xóa khi mất nguồn; cơ sở dữ liệu PC là nơi lưu lịch sử bền vững. Sau Gateway reset, node có thể báo lại sự kiện đang latch; cần PC xác nhận/resolve lại nếu đã xử lý. Không được coi Gateway RAM là nhật ký tai nạn bền vững.

## Kiểm tra trước buổi demo

1. Biên dịch đúng chế độ; đối chiếu chân và nguồn trước khi nạp thủ công.
2. Khởi động chưa gắn radio: heartbeat phải tiếp tục và `radio_ok=false`.
3. Nhận gói đúng CRC; làm sai một byte và kiểm tra gói bị loại/bộ đếm CRC tăng.
4. SOS thật: còi Gateway hoạt động khi PC chưa mở; giữ cùng nút không sinh vô hạn sự kiện.
5. Bỏ EVENT, gửi snapshot có active reference: còi vẫn kích hoạt đúng ID, kể cả FALL-only.
6. ACK/RESOLVE đúng ID; lặp EVENT/snapshot cùng ID không làm còi mở lại trong phạm vi cache. LOCAL_CLEAR không resolve.
7. Gửi dòng quá dài rồi dòng hợp lệ: bộ nhận phải phục hồi tại LF.
8. Với LoRa, đo mất gói/độ trễ thực tế; với nối dây, xác nhận UI ghi rõ chế độ nối dây.

`tests/core_tests.cpp` kiểm tra mã lõi độc lập với Arduino: CRC chuẩn, giới hạn ID, kiểm tra frame, snapshot phục hồi, ACK/resolve, duplicate và quá dung lượng. Test source được giữ trong dự án; executable/cache sinh ra đặt dưới `.cache`. Với Zig cài cục bộ dưới cache:

```powershell
Set-Location F:/SafeWear
. F:/SafeWear/tools/enter-environment.ps1
New-Item -ItemType Directory -Force F:/SafeWear/.cache/test-results/gateway | Out-Null
F:/SafeWear/.cache/tools/zig/ziglang/zig.exe c++ -std=c++11 -I firmware/gateway/include firmware/gateway/tests/core_tests.cpp -o F:/SafeWear/.cache/test-results/gateway/core_tests.exe
F:/SafeWear/.cache/test-results/gateway/core_tests.exe
```

Việc test host không thay thế kiểm thử radio/phần cứng.

Đối chiếu fixture chung với chính bộ giải mã C++ của Gateway:

```powershell
F:/SafeWear/.cache/tools/zig/ziglang/zig.exe c++ -std=c++11 -Wall -Wextra -Werror -I firmware/gateway/include firmware/gateway/tests/codec_probe.cpp -o F:/SafeWear/.cache/test-results/gateway/codec_probe.exe
F:/SafeWear/.venv/Scripts/python.exe firmware/gateway/tests/verify_vectors.py
```

Luôn chạy trong cùng phiên PowerShell đã nạp `enter-environment.ps1`. Fixture chuẩn nằm ở `tests/fixtures/protocol-v1.json` tại gốc dự án; probe so sánh độ hợp lệ, header và tham chiếu cảnh báo với kỳ vọng chung.

HTTP core có test host cho queue/expiry/preemption, retry policy, age, response sai/duplicate key/32 command và tách biệt transport ACK với còi:

```powershell
F:/SafeWear/.venv/Scripts/python.exe firmware/gateway/tools/http_contract.py
F:/SafeWear/.cache/tools/zig/ziglang/zig.exe c++ -std=c++11 -Wall -Wextra -Werror -I firmware/gateway/include -I .cache/libdeps/gateway/gateway_lora/ArduinoJson/src firmware/gateway/tests/http_tests.cpp -o .cache/test-results/gateway/http_tests.exe
F:/SafeWear/.cache/test-results/gateway/http_tests.exe
F:/SafeWear/.cache/tools/zig/ziglang/zig.exe c++ -std=c++11 -Wall -Wextra -Werror -I firmware/gateway/include -I .cache/libdeps/gateway/gateway_lora/ArduinoJson/src firmware/gateway/tests/http_probe.cpp -o .cache/test-results/gateway/http_probe.exe
F:/SafeWear/.venv/Scripts/python.exe firmware/gateway/tests/verify_http_vectors.py
```

Cần thử trên board thật: mất Wi-Fi/Pi trong khi nhận LoRa, queue đầy, Pi khởi động lại sau commit trước response, token sai, response quá dài/sai ID, còi khi PC/Pi mất kết nối và ACK/RESOLVE từ web. Build và test host chưa chứng minh các tình huống phần cứng đó.

### Raw PPG capture qua ESP32 (opt-in)

Build `gateway_ppg_capture` và ghép với node `node_ppg_capture`. Nối STM32 PA9 (TX) tới ESP32 GPIO32 (RX), nối GND chung, logic 3.3 V; không cần USB-UART rời. MAX30102 dùng PB10/PB11 trên node; kiểm pinout node trước khi cấp nguồn. USB ESP32 xuất NDJSON `ppg_raw` gồm `sample_ms`, `red`, `ir`, `motion_ok` để công cụ ghi dữ liệu đọc.

Target này chỉ wired, không HTTP, không mô phỏng. Chỉ chấp nhận đúng năm trường (motion_ok boolean), dòng tối đa 128 byte, timestamp uint32 và RED/IR 18 bit. Gói sai tăng `input_errors`; đầy hàng đợi tăng `queue_drops` trong heartbeat. Raw dùng hàng đợi thường hữu hạn, EVENT ưu tiên; không coi capture không mất mẫu nếu các bộ đếm tăng. Các target chuẩn tiếp tục từ chối raw. Chưa nghiệm thu thu mẫu qua phần cứng; không tự nạp firmware.
