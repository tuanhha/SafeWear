"""Receiver and alarm lifecycle independent of browser connections."""

import asyncio
from collections import Counter, deque
from dataclasses import dataclass
import json
import logging
import queue
import re
import sqlite3
import threading
import time

from .protocol import EventCode, Flags, LineFramer, ProtocolError, UNKNOWN, decode_line
from .storage import Store, utc_now

LOG = logging.getLogger(__name__)


@dataclass
class NodeState:
    node_id: int
    boot_id: int
    seen: float
    sequence: int | None = None
    snapshot: dict | None = None
    snapshot_seen: float = 0
    rssi: float | None = None
    snr: float | None = None
    retired_boots: deque | None = None
    ppg: dict | None = None
    ppg_seen: float = 0
    bp: dict | None = None
    bp_received_at: str | None = None


class Receiver:
    def __init__(self, store: Store, mode="hardware", model_path=None, clock=time.monotonic,
                 *, bp_model_path=None, bp_calibration_path=None):
        from .ml import ActivityClassifier
        from .bp import BloodPressureEstimator
        self.store = store
        self.mode = mode
        self.clock = clock
        self.model = ActivityClassifier(model_path)
        self.bp_model = BloodPressureEstimator(bp_model_path, bp_calibration_path)
        self.nodes: dict[int, NodeState] = {}
        self.counters = Counter()
        self.last_gateway_seen: float | None = None
        self.radio_ok = False
        self.transport = "lora"
        self.gateway_connected = False
        self.last_error = None
        self.gateway_stats = {}
        self.serial = None
        self.uplink = "simulation" if mode == "demo" else "usb"
        self.volatile_events = {}

    def _storage_failed(self):
        self.counters["storage_errors"] += 1
        self.last_error = "Lỗi lưu trữ: cảnh báo mới chỉ được giữ tạm; chưa lưu bền"

    def _flush_volatile(self):
        for event_id, event in list(self.volatile_events.items()):
            try:
                self.store.recover_event(event)
            except (sqlite3.Error, OSError):
                break
            else:
                del self.volatile_events[event_id]

    def _alarm(self, event_id, node_id, boot_id, seq, kind, uptime, origin, simulated):
        previous = None
        try:
            previous = self.store.event(event_id)
            result = self.store.ensure_event(event_id, node_id, boot_id, seq, kind, uptime, origin, simulated)
            if previous:
                self.counters["duplicate_events"] += 1
            return result
        except (sqlite3.Error, OSError):
            self._storage_failed()
            # A failed last_seen write cannot downgrade a known durable operator state.
            if previous is not None:
                return previous
            return self._volatile_alarm(event_id,node_id,boot_id,seq,kind,uptime,origin,simulated)

    def _volatile_alarm(self, event_id, node_id, boot_id, seq, kind, uptime, origin, simulated):
        if event_id in self.volatile_events:
            return self.volatile_events[event_id]
        if len(self.volatile_events) >= 128:
            self.counters["volatile_alarm_overflow"] += 1
            return None
        now = utc_now()
        event = dict(event_id=event_id,node_id=node_id,boot_id=boot_id,sequence=seq,kind=kind,
                     state="ALARM",received_at=now,last_seen_at=now,source_uptime_ms=uptime,
                     origin=origin,simulated=int(simulated),local_cleared=0,
                     acknowledged_at=None,resolved_at=None,volatile=True)
        self.volatile_events[event_id] = event
        return event

    def events(self):
        try:
            records = self.store.events()
        except (sqlite3.Error, OSError):
            self._storage_failed()
            records = []
        merged = {event["event_id"]:event for event in records}
        for event_id,event in self.volatile_events.items():
            merged.setdefault(event_id,event)
        return sorted(merged.values(),key=lambda e:(e["state"]=="RESOLVED",e["received_at"]))

    def ingest(self, line: bytes, received_at=None):
        try:
            metadata, frame = decode_line(line)
        except ProtocolError as exc:
            self.counters["rejected_frames"] += 1
            self.last_error = str(exc)
            return False
        self._flush_volatile()
        now = self.clock() if received_at is None else min(self.clock(),received_at)
        self.last_gateway_seen = max(now, self.last_gateway_seen or now)
        if metadata["kind"] == "heartbeat":
            self.radio_ok = metadata["radio_ok"]
            self.transport = metadata.get("transport", "lora")
            self.gateway_stats = {key: metadata.get(key, 0) for key in ("rx_total", "crc_errors", "queue_drops")}
            return True
        if frame is None:
            self.last_error = str(metadata.get("message", ""))[:180]
            return True
        self.counters["received_frames"] += 1
        if self.mode != "demo" and ((frame.kind in ("snapshot", "ppg") and frame.data["flags"] & Flags.SIMULATED)
                                    or (frame.kind == "event" and frame.data["reason"] & 128)):
            self.counters["rejected_frames"] += 1
            self.last_error = "Dữ liệu mô phỏng bị từ chối trong chế độ phần cứng"
            return False
        node = self.nodes.get(frame.node_id)
        if node is None:
            if len(self.nodes) >= 64:
                self.counters["node_limit_drops"] += 1
            else:
                node = NodeState(frame.node_id, frame.boot_id, now, retired_boots=deque(maxlen=8))
                self.nodes[frame.node_id] = node
        # Events from a previous boot remain actionable, but must not replace current telemetry.
        telemetry_accepted = True
        if frame.kind in ("snapshot", "ppg"):
            telemetry_accepted = node is not None and self._snapshot(node,frame,metadata,now)
        elif node is not None and frame.boot_id == node.boot_id:
            node.seen = max(node.seen, now)
        try:
            self.store.add_frame(frame, metadata)
        except (sqlite3.Error, OSError):
            self._storage_failed()
        if frame.kind == "event":
            if frame.data["event_code"] == EventCode.LOCAL_CLEAR.name:
                try:
                    self.store.update(frame.event_id, "local_clear")
                except (sqlite3.Error, OSError):
                    self._storage_failed()
                    if frame.event_id in self.volatile_events:
                        self.volatile_events[frame.event_id]["local_cleared"] = 1
            else:
                result = self._alarm(frame.event_id,frame.node_id,frame.boot_id,frame.sequence,
                                     frame.data["event_code"],frame.uptime_ms,"event",bool(frame.data["reason"]&128))
                self._reconcile_gateway(result)
        elif frame.kind == "snapshot":
            flags = Flags(frame.data["flags"])
            if flags & (Flags.SOS_ACTIVE | Flags.FALL_ACTIVE):
                boot,seq = frame.data["active_event_boot_id"],frame.data["active_event_sequence"]
                kind = "MANUAL_SOS" if flags & Flags.SOS_ACTIVE else "FALL_SUSPECT"
                result = self._alarm(f"{frame.node_id}:{boot}:{seq}",frame.node_id,boot,seq,kind,
                                     frame.uptime_ms,"snapshot_recovery",bool(flags&Flags.SIMULATED))
                self._reconcile_gateway(result)
        return telemetry_accepted

    def _snapshot(self,node,frame,metadata,now):
        if frame.boot_id != node.boot_id:
            # Priority events can overtake telemetry queued before a reboot.
            if now < node.seen or frame.boot_id in node.retired_boots:
                self.counters["late_snapshots"] += 1
                return False
            node.retired_boots.append(node.boot_id)
            node.boot_id,node.sequence = frame.boot_id,None
            node.snapshot = node.ppg = node.bp = None
            node.bp_received_at = None
        if node.sequence is not None:
            delta = (frame.sequence-node.sequence) & 0xFFFFFFFF
            if delta == 0 or delta >= 0x80000000:
                self.counters["late_snapshots"] += 1
                return False
        node.sequence = frame.sequence
        if frame.kind == "ppg":
            node.ppg, node.ppg_seen = frame.data, now - frame.data["age_ms"] / 1000
            node.bp_received_at = utc_now()
            node.bp = self.bp_model.predict(
                frame.data["features"], valid=bool(frame.data["flags"] & 1),
                node_id=node.node_id, simulated=bool(frame.data["flags"] & 256))
            node.bp.update(source_uptime_ms=frame.uptime_ms, window_ms=10000,
                           transport_age_ms=max(0, int((self.clock() - now) * 1000)),
                           node_queue_age_ms=frame.data["age_ms"])
            try:
                self.store.add_bp(frame, node.bp, node.bp_received_at)
            except (sqlite3.Error, OSError):
                self._storage_failed()
        else:
            node.snapshot = frame.data
            node.snapshot_seen = now
        node.seen = max(node.seen,now)
        node.rssi,node.snr = metadata.get("rssi"),metadata.get("snr")
        return True

    def _reconcile_gateway(self, event):
        if self.serial and event and event["state"] in ("ACKNOWLEDGED","RESOLVED"):
            action = "alarm_resolve" if event["state"]=="RESOLVED" else "alarm_ack"
            self.serial.command({"kind":action,"event_id":event["event_id"]})

    def state(self):
        now = self.clock()
        nodes = []
        for node in self.nodes.values():
            data = node.snapshot or {}
            flags = Flags(data.get("flags", 0))
            elapsed = max(0, int((now - node.snapshot_seen) * 1000)) if node.snapshot else UNKNOWN
            last_seen = max(0, int((now - node.seen) * 1000))
            online = last_seen <= 15000
            hr_age = data.get("hr_age_ms", UNKNOWN) + elapsed
            spo2_age = data.get("spo2_age_ms", UNKNOWN) + elapsed
            hr_raw, spo2_raw = data.get("hr_raw", UNKNOWN), data.get("spo2_raw", UNKNOWN)
            hr_ok = bool(online and flags & Flags.HR_VALID and hr_age <= 10000 and 0 < hr_raw <= 4000)
            spo2_ok = bool(online and flags & Flags.SPO2_VALID and spo2_age <= 10000 and 0 < spo2_raw <= 10000)
            imu_ok = bool(online and elapsed <= 10000 and flags & Flags.IMU_VALID
                          and not flags & Flags.SENSOR_GAP and data.get("sample_count", 0) >= 225)
            activity = self.model.predict(data.get("features", [0] * 10), valid=imu_ok)
            active_id = None
            bp_age = max(0, int((now - node.ppg_seen) * 1000)) if node.ppg else None
            bp = dict(node.bp or {"status": "no_signal", "systolic_mmhg": None,
                                 "diastolic_mmhg": None, "experimental": True})
            if node.snapshot and (flags & (Flags.PPG_FAULT | Flags.SENSOR_GAP)
                                  or not flags & Flags.IMU_VALID):
                bp.update(status="signal_invalid", systolic_mmhg=None, diastolic_mmhg=None)
            if not online or (bp_age is not None and bp_age > 15000):
                bp.update(status="stale", systolic_mmhg=None, diastolic_mmhg=None)
            bp.update(age_ms=bp_age, received_at=node.bp_received_at)
            if flags & (Flags.SOS_ACTIVE | Flags.FALL_ACTIVE):
                active_id = f"{node.node_id}:{data.get('active_event_boot_id',0)}:{data.get('active_event_sequence',0)}"
            nodes.append({
                "id": node.node_id, "name": f"Người đeo {node.node_id:02}",
                "boot_id": node.boot_id, "connection": "online" if online else "offline",
                "sample_sequence": node.sequence,
                "hr": hr_raw / 10 if hr_ok else None, "spo2": spo2_raw / 100 if spo2_ok else None,
                "hr_quality": "valid" if hr_ok else "invalid", "spo2_quality": "valid" if spo2_ok else "invalid",
                "hr_age_ms": hr_age if node.snapshot else None,
                "spo2_age_ms": spo2_age if node.snapshot else None,
                "last_seen_age_ms": last_seen, "battery_mv": None if data.get("battery_mv", UNKNOWN) == UNKNOWN else data["battery_mv"],
                "rssi": node.rssi, "snr": node.snr, "activity": activity.get("label", "unknown"),
                "activity_source": activity.get("source", "untrained"), "activity_detail": activity,
                "active_event_id": active_id, "simulated": bool(flags & Flags.SIMULATED),
                "lost_imu_samples": data.get("lost_imu_samples", 0),
                "lost_ppg_samples": data.get("lost_ppg_samples", 0),
                "blood_pressure": bp,
            })
        gateway_age = None if self.last_gateway_seen is None else int((now - self.last_gateway_seen) * 1000)
        gateway_fresh = gateway_age is not None and gateway_age <= 3000
        return {"mode": self.mode, "server_time": utc_now(), "nodes": nodes,
                "events": self.events(), "model": self.model.status(),
                "bp_model": self.bp_model.status(),
                "source_status": {"gateway": "online" if gateway_fresh else "offline",
                                  "uplink": self.uplink,
                                  "transport": self.transport, "radio_ok": self.radio_ok,
                                  "serial_connected": self.gateway_connected, "age_ms": gateway_age},
                "diagnostics": {**dict(self.counters), "gateway": self.gateway_stats,
                                "volatile_alarms": len(self.volatile_events),
                                "last_error": self.last_error}}

    def operator_action(self, event_id, action):
        self._flush_volatile()
        if event_id in self.volatile_events:
            raise OSError("Cảnh báo chưa được lưu bền; chưa thể xác nhận thao tác")
        event = self.store.update(event_id, action)
        if event and self.serial:
            self.serial.command({"kind": f"alarm_{action}", "event_id": event_id})
        return event


class SerialSource:
    """Blocking serial is isolated in one thread; receiver consumes bounded queues."""

    def __init__(self, receiver, port):
        self.receiver = receiver
        self.port = port
        receiver.uplink = "uart" if re.fullmatch(r"/dev/(?:serial[01]|tty(?:S|AMA)[0-9]+)", port) else "usb"
        self.events = queue.Queue(maxsize=64)
        self.normal = queue.Queue(maxsize=128)
        self.commands = queue.Queue(maxsize=32)
        self.stop = threading.Event()
        self.thread = None

    def command(self, obj):
        try:
            self.commands.put_nowait((json.dumps(obj, separators=(",", ":")) + "\n").encode())
        except queue.Full:
            self.receiver.counters["command_drops"] += 1

    def _enqueue(self, line):
        critical = False
        try:
            obj = json.loads(line)
            critical = obj.get("kind") == "rx" and obj.get("payload", "")[2:4] == "02"
        except (ValueError, TypeError, AttributeError):
            pass
        target = self.events if critical else self.normal
        try:
            target.put_nowait((line, self.receiver.clock()))
        except queue.Full:
            self.receiver.counters["serial_queue_drops"] += 1
            if not critical:
                try:
                    target.get_nowait()
                    target.put_nowait((line, self.receiver.clock()))
                except (queue.Empty, queue.Full):
                    pass

    def _read(self):
        import serial
        while not self.stop.is_set():
            try:
                with serial.Serial(self.port, 115200, timeout=0.2, write_timeout=0.2) as connection:
                    connection.reset_input_buffer()
                    self.receiver.gateway_connected = True
                    framer = LineFramer()
                    while not self.stop.is_set():
                        data = connection.read(512)
                        for line in framer.feed(data):
                            self._enqueue(line)
                        if framer.dropped_lines:
                            self.receiver.counters["overlong_lines"] += framer.dropped_lines
                            framer.dropped_lines = 0
                        try:
                            command = self.commands.get_nowait()
                        except queue.Empty:
                            command = None
                        if command:
                            connection.write(command)
            except (serial.SerialException, OSError):
                self.receiver.gateway_connected = False
                self.receiver.last_error = f"Chưa kết nối được Gateway tại {self.port}"
                self.stop.wait(1)
            finally:
                self.receiver.gateway_connected = False

    async def run(self):
        self.thread = threading.Thread(target=self._read, name="gateway-serial", daemon=True)
        self.thread.start()
        try:
            while True:
                line = None
                for q in (self.events, self.normal):
                    try:
                        line = q.get_nowait()
                        break
                    except queue.Empty:
                        pass
                if line is not None:
                    raw, received_at = line
                    self.receiver.ingest(raw, received_at=received_at)
                    await asyncio.sleep(0)
                else:
                    await asyncio.sleep(0.02)
        finally:
            self.stop.set()
            await asyncio.to_thread(self.thread.join, 2)
