# Mã nguồn SafeWear

- firmware/wrist: cụm cổ tay.
- firmware/chest: cụm đeo người.
- firmware/gateway: bộ thu và chuyển tiếp dữ liệu.
- firmware/node: mã C dùng chung cho firmware.
- backend/safewear: dịch vụ tiếp nhận dữ liệu và xử lý.
- web: dashboard giám sát.
- docs: định dạng gói tin và yêu cầu HTTP.

## Chạy dashboard mô phỏng trên Windows

Chạy trong PowerShell tại thư mục ma-nguon.

~~~powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
$env:PYTHONPATH = (Resolve-Path .\backend).Path
.\.venv\Scripts\python.exe -m safewear.app --demo
~~~

Mở địa chỉ do chương trình in ra. Dashboard gắn nhãn chế độ mô phỏng khi dùng dữ liệu mẫu. Cấu hình firmware gateway được mô tả trong firmware/gateway/README.md.