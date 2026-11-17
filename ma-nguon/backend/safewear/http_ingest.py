"""Bounded HTTP envelope; commit receipt, decoder effects and dedup atomically."""
from copy import deepcopy
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator

from .protocol import EventCode, Flags, ProtocolError, decode_line

SCHEMA = json.loads((Path(__file__).resolve().parents[2] / "docs/http-uplink-v1.schema.json").read_text())
LIMITS = SCHEMA["x-safewear"]
PATH = LIMITS["path"]
MAX_BODY = LIMITS["max_request_bytes"]
PROPERTIES = SCHEMA["properties"]


def event_id_valid(value):
    if not isinstance(value,str) or not re.fullmatch(PROPERTIES["active_event_ids"]["items"]["pattern"],value):
        return False
    node,boot,seq = map(int,value.split(":"))
    return (1 <= node <= 65535 and 0 <= boot <= 0xFFFFFFFF and 0 <= seq <= 0xFFFFFFFF
            and (boot or seq) and value == f"{node}:{boot}:{seq}")


class Envelope(BaseModel):
    model_config = ConfigDict(extra="forbid",strict=True)
    version: Literal[1]
    gateway_id: Annotated[str,Field(pattern=PROPERTIES["gateway_id"]["pattern"],max_length=48)]
    boot_id: Annotated[str,Field(pattern=PROPERTIES["boot_id"]["pattern"],min_length=16,max_length=16)]
    sequence: Annotated[int,Field(ge=1,le=PROPERTIES["sequence"]["maximum"])]
    age_ms: Annotated[int,Field(ge=0,le=PROPERTIES["age_ms"]["maximum"])]
    active_event_ids: Annotated[list[str],Field(max_length=PROPERTIES["active_event_ids"]["maxItems"])]
    message: dict

    @field_validator("version",mode="before")
    @classmethod
    def strict_version(cls,value):
        if type(value) is not int:
            raise ValueError("Version must be an integer")
        return value

    @field_validator("active_event_ids")
    @classmethod
    def valid_ids(cls,values):
        if len(values) != len(set(values)) or not all(event_id_valid(value) for value in values):
            raise ValueError("Invalid event IDs")
        return values


class UplinkError(ValueError):
    def __init__(self,status,detail):
        super().__init__(detail)
        self.status,self.detail = status,detail


def _unique_object(pairs):
    result = {}
    for key,value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def _reject_constant(_):
    raise ValueError("Nonfinite JSON number")


def _float(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("Nonfinite JSON number")
    return result


def parse_packet(body):
    if len(body) > MAX_BODY:
        raise UplinkError(413,"Gói HTTP quá dài")
    try:
        raw = json.loads(body.decode("utf-8"),object_pairs_hook=_unique_object,
                         parse_constant=_reject_constant,parse_float=_float)
        packet = Envelope.model_validate(raw).model_dump()
        line = json.dumps(packet["message"],ensure_ascii=False,sort_keys=True,
                          separators=(",",":"),allow_nan=False).encode()
        if len(line) > LIMITS["max_message_bytes"]:
            raise UplinkError(413,"Thông báo Gateway quá dài")
        metadata,frame = decode_line(line)
        if metadata.get("transport","lora") not in ("lora","wired"):
            raise ValueError("Invalid sensor link")
        if frame and ((frame.kind in ("snapshot", "ppg") and frame.data["flags"] & Flags.SIMULATED)
                      or (frame.kind=="event" and frame.data["reason"] & 128)):
            raise UplinkError(422,"Không nhận mô phỏng trong chế độ HTTP phần cứng")
    except UplinkError:
        raise
    except (ValueError,TypeError,UnicodeError,RecursionError,ValidationError,ProtocolError):
        raise UplinkError(422,"Gói Gateway không hợp lệ") from None
    return packet,line,frame


def _alarm_reference(frame):
    if frame is None:
        return None
    if frame.kind == "event" and frame.data["event_code"] != EventCode.LOCAL_CLEAR.name:
        return (frame.event_id,frame.node_id,frame.boot_id,frame.sequence,
                frame.data["event_code"],frame.uptime_ms,"http_unpersisted",False)
    if frame.kind == "snapshot":
        flags = Flags(frame.data["flags"])
        if flags & (Flags.SOS_ACTIVE|Flags.FALL_ACTIVE):
            boot,seq = frame.data["active_event_boot_id"],frame.data["active_event_sequence"]
            return (f"{frame.node_id}:{boot}:{seq}",frame.node_id,boot,seq,
                    "MANUAL_SOS" if flags & Flags.SOS_ACTIVE else "FALL_SUSPECT",
                    frame.uptime_ms,"http_unpersisted",False)
    return None


class HttpIngress:
    def __init__(self,receiver,gateway_id):
        self.receiver = receiver
        self.store = receiver.store
        self.gateway_id = gateway_id
        self.tokens = 40.0
        self.refill_at = receiver.clock()
        receiver.uplink = "http"

    def _rate_limit(self):
        now = self.receiver.clock()
        self.tokens = min(40.0,self.tokens+max(0,now-self.refill_at)*20)
        self.refill_at = now
        if self.tokens < 1:
            raise UplinkError(429,"Gateway gửi quá nhanh; thử lại sau")
        self.tokens -= 1

    def accept(self,body,body_read_seconds=0.0):
        self._rate_limit()
        packet,line,frame = parse_packet(body)
        if packet["gateway_id"] != self.gateway_id:
            raise UplinkError(403,"Gateway chưa được cấu hình cho trạm này")
        digest = hashlib.sha256(line).hexdigest()
        receiver = self.receiver
        # Processing is synchronous on the app event loop, with no await inside the
        # transaction, so clients cannot observe an uncommitted intermediate state.
        saved = {name:deepcopy(getattr(receiver,name)) for name in
                 ("nodes","last_gateway_seen","radio_ok","transport","gateway_stats","volatile_events")}
        reference = _alarm_reference(frame)
        known = None
        known_checked = False
        try:
            with self.store.transaction():
                if reference:
                    known = self.store.event(reference[0])
                    known_checked = True
                prior = self.store.http_receipt(packet["gateway_id"],packet["boot_id"],packet["sequence"])
                if prior is not None:
                    if prior != digest:
                        raise UplinkError(409,"Mã gói đã được dùng cho nội dung khác")
                    duplicate = True
                else:
                    high = self.store.http_high_sequence(packet["gateway_id"],packet["boot_id"])
                    if packet["sequence"] <= high:
                        raise UplinkError(409,"Mã gói quá cũ hoặc sai thứ tự")
                    duplicate = False
                    failures = receiver.counters["storage_errors"]
                    receiver.ingest(line,received_at=receiver.clock()-packet["age_ms"]/1000-max(0,body_read_seconds))
                    if receiver.counters["storage_errors"] != failures:
                        raise sqlite3.OperationalError("Ingest storage failed")
                    self.store.add_http_receipt(packet,digest)
                commands = self.store.gateway_commands(packet["active_event_ids"])
        except (sqlite3.Error,OSError):
            for name,value in saved.items():
                setattr(receiver,name,value)
            receiver._storage_failed()
            if reference and known_checked and known is None:
                # Visible as explicitly volatile, never as durable transport ACK.
                receiver._volatile_alarm(*reference)
            raise UplinkError(503,"Chưa lưu được gói; Gateway cần gửi lại") from None
        receiver.counters["http_duplicates" if duplicate else "http_accepted"] += 1
        return {"version":1,"accepted":True,"duplicate":duplicate,
                "gateway_id":packet["gateway_id"],"boot_id":packet["boot_id"],
                "sequence":packet["sequence"],"commands":commands}
