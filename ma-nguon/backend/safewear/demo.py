"""Explicit simulation only; synthesizes valid wire bytes, never a validated ML dataset."""

import asyncio
import json
import secrets
import time

from .protocol import EventCode, Flags, UNKNOWN, encode_event, encode_snapshot, rx_line


class DemoSource:
    def __init__(self, receiver):
        self.receiver = receiver
        self.boot = secrets.randbits(32) or 1
        self.sequence = 0
        self.started = time.monotonic()
        self.paused = False
        self.ppg_valid = True
        self.active = None
        self.motion = "idle"

    def _seq(self):
        self.sequence += 1
        return self.sequence

    def _tick(self):
        return int((time.monotonic() - self.started) * 1000) & 0xFFFFFFFF

    def snapshot(self):
        flags = Flags.IMU_VALID | Flags.SIMULATED
        if self.ppg_valid:
            flags |= Flags.HR_VALID | Flags.SPO2_VALID
        if self.active:
            flags |= Flags.SOS_ACTIVE if self.active[1] == EventCode.MANUAL_SOS else Flags.FALL_ACTIVE
        features = ([0, 0, 1000, 12, 970, 1030, 1001, 30, 150, 12] if self.motion == "idle"
                    else [60, 90, 950, 180, 700, 1400, 1030, 850, 2500, 160])
        data = encode_snapshot(boot=self.boot, sequence=self._seq(), uptime_ms=self._tick(), flags=flags,
                               hr=760 if self.ppg_valid else UNKNOWN,
                               spo2=9800 if self.ppg_valid else UNKNOWN,
                               hr_age=0 if self.ppg_valid else UNKNOWN,
                               spo2_age=0 if self.ppg_valid else UNKNOWN,
                               active_boot=self.boot if self.active else 0,
                               active_sequence=self.active[0] if self.active else 0, features=features)
        self.receiver.ingest(rx_line(data, gateway_ms=self._tick()))

    def action(self, action):
        if action in ("sos", "fall"):
            code = EventCode.MANUAL_SOS if action == "sos" else EventCode.FALL_SUSPECT
            seq = self._seq()
            self.active = (seq, code)
            data = encode_event(boot=self.boot, sequence=seq, uptime_ms=self._tick(), code=code,
                                reason=128 | (2 if action == "sos" else 1))
            for _ in range(3):
                self.receiver.ingest(rx_line(data, gateway_ms=self._tick()))
        elif action == "invalid":
            self.ppg_valid = False
        elif action == "restore":
            self.ppg_valid, self.paused = True, False
        elif action == "pause":
            self.paused = True
        elif action == "local_clear":
            if self.active:
                self.receiver.ingest(rx_line(encode_event(boot=self.boot, sequence=self.active[0],
                    uptime_ms=self._tick(), code=EventCode.LOCAL_CLEAR, reason=128)))
                self.active = None
        elif action in ("idle", "walking"):
            self.motion = action
        else:
            raise ValueError("Unknown demo action")
        if not self.paused:
            self.snapshot()

    async def run(self):
        count = 0
        while True:
            self.receiver.ingest((json.dumps({"kind": "heartbeat", "gateway_ms": self._tick(),
                "radio_ok": True, "transport": "lora", "rx_total": self.sequence}) + "\n").encode())
            if not self.paused and count % 5 == 0:
                self.snapshot()
            count += 1
            await asyncio.sleep(1)
