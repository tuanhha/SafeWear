"""Protocol v1: strict, little-endian 20/70-byte radio frames; see docs/protocol-v1.md."""

from dataclasses import dataclass
from enum import IntEnum, IntFlag
import json
import math
import re
import struct


class ProtocolError(ValueError):
    pass


class Flags(IntFlag):
    HR_VALID = 1
    SPO2_VALID = 2
    SOS_ACTIVE = 4
    IMU_VALID = 8
    PPG_FAULT = 16
    BATTERY_LOW = 32
    FALL_ACTIVE = 64
    SENSOR_GAP = 128
    SIMULATED = 256


class EventCode(IntEnum):
    MANUAL_SOS = 1
    FALL_SUSPECT = 2
    LOCAL_CLEAR = 3


HEADER = struct.Struct("<BBHIII")
SNAPSHOT = struct.Struct("<10H2I2B10hH")
UNKNOWN = 65535


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True)
class Frame:
    kind: str
    node_id: int
    boot_id: int
    sequence: int
    uptime_ms: int
    data: dict
    raw: bytes

    @property
    def event_id(self) -> str:
        return f"{self.node_id}:{self.boot_id}:{self.sequence}"


def decode_frame(raw: bytes) -> Frame:
    if len(raw) not in (20, 70):
        raise ProtocolError("Độ dài khung không hợp lệ")
    if crc16(raw[:-2]) != struct.unpack_from("<H", raw, len(raw) - 2)[0]:
        raise ProtocolError("CRC không hợp lệ")
    version, kind, node, boot, seq, tick = HEADER.unpack_from(raw)
    if version != 1 or kind not in (1, 2, 3) or node == 0:
        raise ProtocolError("Phiên bản, loại gói hoặc mã thiết bị không hợp lệ")
    if kind == 3:
        if len(raw) != 70:
            raise ProtocolError("Khung PPG phải dài 70 byte")
        flags, window, count, lost, schema, size = struct.unpack_from("<4H2B", raw, 16)
        if (flags & ~0x103 or window != 10000 or count > 250 or schema != 1 or size != 12
                or struct.unpack_from("<H", raw, 50)[0] > 2000 or any(raw[52:68])
                or (flags & 1 and (count < 225 or lost or flags & 2))):
            raise ProtocolError("Schema hoặc chất lượng PPG không hợp lệ")
        return Frame("ppg", node, boot, seq, tick, {
            "flags": flags, "window_ms": window, "sample_count": count,
            "lost_ppg_samples": lost, "feature_schema": schema,
            "age_ms": struct.unpack_from("<H", raw, 50)[0],
            "features": list(struct.unpack_from("<12h", raw, 26)),
        }, raw)
    if kind == 2:
        if len(raw) != 20 or raw[16] not in tuple(EventCode):
            raise ProtocolError("Sự kiện không hợp lệ")
        data = {"event_code": EventCode(raw[16]).name, "reason": raw[17]}
        return Frame("event", node, boot, seq, tick, data, raw)
    if len(raw) != 70:
        raise ProtocolError("Khung số đo phải dài 70 byte")
    values = SNAPSHOT.unpack_from(raw, 16)
    flags, hr, spo2, battery, hr_age, spo2_age, window, count, lost_imu, lost_ppg = values[:10]
    event_boot, event_seq, schema, feature_count = values[10:14]
    if schema != 1 or feature_count != 10 or window != 5000 or values[-1] != 0:
        raise ProtocolError("Schema đặc trưng không tương thích")
    if flags & ~0x1FF or count > 250:
        raise ProtocolError("Cờ hoặc số mẫu không hợp lệ")
    if flags & (Flags.SOS_ACTIVE | Flags.FALL_ACTIVE) and not (event_boot or event_seq):
        raise ProtocolError("Cảnh báo thiếu mã sự kiện gốc")
    data = {
        "flags": flags, "hr_raw": hr, "spo2_raw": spo2, "battery_mv": battery,
        "hr_age_ms": hr_age, "spo2_age_ms": spo2_age, "window_ms": window,
        "sample_count": count, "lost_imu_samples": lost_imu, "lost_ppg_samples": lost_ppg,
        "active_event_boot_id": event_boot, "active_event_sequence": event_seq,
        "feature_schema": schema, "features": list(values[14:24]),
    }
    return Frame("snapshot", node, boot, seq, tick, data, raw)


def _finish(data: bytes) -> bytes:
    return data + struct.pack("<H", crc16(data))


def encode_event(node=1, boot=1, sequence=1, uptime_ms=0,
                 code=EventCode.MANUAL_SOS, reason=2) -> bytes:
    return _finish(HEADER.pack(1, 2, node, boot, sequence, uptime_ms) + bytes([code, reason]))


def encode_snapshot(node=1, boot=1, sequence=1, uptime_ms=0,
                    flags=Flags.HR_VALID | Flags.SPO2_VALID | Flags.IMU_VALID,
                    hr=720, spo2=9800, battery=3900, hr_age=0, spo2_age=0,
                    sample_count=250, lost_imu=0, lost_ppg=0,
                    active_boot=0, active_sequence=0, features=None) -> bytes:
    if features is None:
        features = [0, 0, 1000, 10, 980, 1020, 1000, 10, 20, 10]
    if len(features) != 10:
        raise ProtocolError("Cần đúng 10 đặc trưng")
    body = SNAPSHOT.pack(int(flags), hr, spo2, battery, hr_age, spo2_age, 5000,
                         sample_count, lost_imu, lost_ppg, active_boot, active_sequence,
                         1, 10, *features, 0)
    return _finish(HEADER.pack(1, 1, node, boot, sequence, uptime_ms) + body)


def rx_line(frame: bytes, rssi=-63.0, snr=8.0, gateway_ms=0) -> bytes:
    return (json.dumps({"kind": "rx", "payload": frame.hex(), "rssi": rssi,
                        "snr": snr, "gateway_ms": gateway_ms}, separators=(",", ":")) + "\n").encode()


def encode_ppg(node=1, boot=1, sequence=1, uptime_ms=0, *, flags=0,
               sample_count=0, lost=0, features=None, age_ms=0) -> bytes:
    """Feature-only research packet. Defaults deliberately do not assert validity."""
    values = [0] * 12 if features is None else features
    if len(values) != 12:
        raise ProtocolError("Cần đúng 12 đặc trưng PPG")
    body = struct.pack("<4H2B12hH16x", flags, 10000, sample_count, lost, 1, 12, *values, age_ms)
    raw = _finish(HEADER.pack(1, 3, node, boot, sequence, uptime_ms) + body)
    decode_frame(raw)
    return raw


def decode_line(line: bytes) -> tuple[dict, Frame | None]:
    if len(line) > 512:
        raise ProtocolError("Dòng USB quá dài")
    try:
        obj = json.loads(line.decode("utf-8"), parse_constant=_reject_constant, parse_float=_finite_float)
    except (ValueError, UnicodeError) as exc:
        raise ProtocolError("USB không phải NDJSON hợp lệ") from exc
    if not isinstance(obj, dict) or obj.get("kind") not in ("rx", "heartbeat", "diagnostic"):
        raise ProtocolError("Loại thông báo Gateway không hợp lệ")
    if obj["kind"] != "rx":
        if obj["kind"] == "heartbeat":
            if not isinstance(obj.get("radio_ok"), bool):
                raise ProtocolError("Heartbeat thiếu trạng thái radio")
            if obj.get("transport", "lora") not in ("lora", "wired"):
                raise ProtocolError("Loại đường truyền không hợp lệ")
            for name in ("gateway_ms", "rx_total", "crc_errors", "queue_drops"):
                value = obj.get(name, 0)
                if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
                    raise ProtocolError("Bộ đếm Gateway không hợp lệ")
        elif not isinstance(obj.get("message", ""), str):
            raise ProtocolError("Thông báo chẩn đoán không hợp lệ")
        return obj, None
    payload = obj.get("payload")
    if not isinstance(payload, str) or not re.fullmatch(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{140})", payload):
        raise ProtocolError("Payload USB không hợp lệ")
    for name in ("rssi", "snr"):
        value = obj.get(name)
        lower, upper = (-200, 20) if name == "rssi" else (-100, 100)
        if value is not None and (type(value) not in (int, float) or not lower <= value <= upper):
            raise ProtocolError("Chất lượng sóng không hợp lệ")
    return obj, decode_frame(bytes.fromhex(payload))


def _reject_constant(value):
    raise ProtocolError("JSON chứa số không hữu hạn")


def _finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        raise ProtocolError("JSON chứa số không hữu hạn")
    return result


class LineFramer:
    """Bound memory and discard an entire overlong line rather than parse its tail."""

    def __init__(self):
        self.buffer = bytearray()
        self.discarding = False
        self.dropped_lines = 0

    def feed(self, data: bytes) -> list[bytes]:
        lines = []
        for value in data:
            if value == 10:
                if not self.discarding and self.buffer:
                    lines.append(bytes(self.buffer))
                self.buffer.clear()
                self.discarding = False
            elif not self.discarding:
                self.buffer.append(value)
                if len(self.buffer) > 512:
                    self.buffer.clear()
                    self.discarding = True
                    self.dropped_lines += 1
        return lines
