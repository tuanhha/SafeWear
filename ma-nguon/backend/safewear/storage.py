"""One serialized SQLite writer; operator alarms outlive browser/receiver restarts."""

from datetime import datetime, timezone
from contextlib import contextmanager
import json
from pathlib import Path
import sqlite3
import threading


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class Store:
    def __init__(self, path: str | Path):
        if str(path) != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self._transaction_depth = 0
        self.db = sqlite3.connect(str(path), check_same_thread=False, timeout=3)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS frames(
                id INTEGER PRIMARY KEY, received_at TEXT NOT NULL,
                node_id INTEGER NOT NULL, boot_id INTEGER NOT NULL, sequence INTEGER NOT NULL,
                frame_kind TEXT NOT NULL, raw_hex TEXT NOT NULL, metadata TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS frames_node ON frames(node_id,id);
            CREATE TABLE IF NOT EXISTS events(
                event_id TEXT PRIMARY KEY,node_id INTEGER NOT NULL,boot_id INTEGER NOT NULL,
                sequence INTEGER NOT NULL,kind TEXT NOT NULL,state TEXT NOT NULL,
                received_at TEXT NOT NULL,last_seen_at TEXT NOT NULL,source_uptime_ms INTEGER NOT NULL,
                origin TEXT NOT NULL,simulated INTEGER NOT NULL,local_cleared INTEGER NOT NULL DEFAULT 0,
                acknowledged_at TEXT,resolved_at TEXT);
            CREATE TABLE IF NOT EXISTS http_receipts(
                id INTEGER PRIMARY KEY,gateway_id TEXT NOT NULL,boot_id TEXT NOT NULL,
                sequence INTEGER NOT NULL,message_hash TEXT NOT NULL,packet_json TEXT NOT NULL,
                received_at TEXT NOT NULL,UNIQUE(gateway_id,boot_id,sequence));
            CREATE TABLE IF NOT EXISTS http_sessions(
                gateway_id TEXT NOT NULL,boot_id TEXT NOT NULL,high_sequence INTEGER NOT NULL,
                PRIMARY KEY(gateway_id,boot_id));
            CREATE TABLE IF NOT EXISTS bp_readings(
                id INTEGER PRIMARY KEY, received_at TEXT NOT NULL,
                node_id INTEGER NOT NULL, boot_id INTEGER NOT NULL, sequence INTEGER NOT NULL,
                features_json TEXT NOT NULL, result_json TEXT NOT NULL,
                UNIQUE(node_id,boot_id,sequence));
        """)
        self.db.commit()

    def _commit(self):
        self.db.commit()

    @contextmanager
    def transaction(self):
        """Nested store writes share one commit, including HTTP receipt + effects."""
        with self.lock:
            outer = self._transaction_depth == 0
            if outer:
                self.db.execute("BEGIN IMMEDIATE")
            self._transaction_depth += 1
            try:
                yield
                if outer:
                    self._commit()
            except BaseException:
                if outer:
                    self.db.rollback()
                raise
            finally:
                self._transaction_depth -= 1

    def add_frame(self, frame, metadata):
        with self.transaction():
            self.db.execute(
                "INSERT INTO frames(received_at,node_id,boot_id,sequence,frame_kind,raw_hex,metadata) VALUES(?,?,?,?,?,?,?)",
                (utc_now(), frame.node_id, frame.boot_id, frame.sequence, frame.kind,
                 frame.raw.hex(), json.dumps(metadata, ensure_ascii=False)),
            )
            last_id = self.db.execute("SELECT last_insert_rowid()").fetchone()[0]
            if last_id % 1000 == 0:
                self.db.execute("DELETE FROM frames WHERE id < ?", (last_id - 100000,))

    def add_bp(self, frame, result, received_at):
        with self.transaction():
            self.db.execute(
                "INSERT OR IGNORE INTO bp_readings(received_at,node_id,boot_id,sequence,features_json,result_json) "
                "VALUES(?,?,?,?,?,?)", (received_at, frame.node_id, frame.boot_id, frame.sequence,
                                        json.dumps(frame.data), json.dumps(result, allow_nan=False)))

    def bp_readings(self, node_id=None, limit=100):
        with self.lock:
            query = "SELECT * FROM bp_readings"
            args = []
            if node_id is not None:
                query += " WHERE node_id=?"
                args.append(node_id)
            query += " ORDER BY id DESC LIMIT ?"
            args.append(max(1, min(1000, limit)))
            return [{**dict(row), "features": json.loads(row["features_json"]),
                     "result": json.loads(row["result_json"])} for row in self.db.execute(query, args)]

    def ensure_event(self, event_id, node_id, boot_id, sequence, kind,
                     source_uptime_ms, origin, simulated):
        now = utc_now()
        with self.transaction():
            self.db.execute("""
                INSERT INTO events(event_id,node_id,boot_id,sequence,kind,state,received_at,
                  last_seen_at,source_uptime_ms,origin,simulated)
                VALUES(?,?,?,?,?,'ALARM',?,?,?,?,?)
                ON CONFLICT(event_id) DO UPDATE SET last_seen_at=excluded.last_seen_at
                """, (event_id, node_id, boot_id, sequence, kind, now, now,
                       source_uptime_ms, origin, int(simulated)))
            return self.event(event_id)

    def event(self, event_id):
        with self.lock:
            row = self.db.execute("SELECT * FROM events WHERE event_id=?", (event_id,)).fetchone()
            return dict(row) if row else None

    def events(self, limit=100):
        with self.lock:
            # Open alarms must never disappear behind a page of resolved history.
            active = self.db.execute("SELECT * FROM events WHERE state!='RESOLVED' ORDER BY received_at DESC")
            records = [dict(row) for row in active]
            closed = self.db.execute("SELECT * FROM events WHERE state='RESOLVED' ORDER BY received_at DESC LIMIT ?", (limit,))
            return records + [dict(row) for row in closed]

    def recover_event(self, event):
        columns = ("event_id", "node_id", "boot_id", "sequence", "kind", "state", "received_at",
                   "last_seen_at", "source_uptime_ms", "origin", "simulated", "local_cleared",
                   "acknowledged_at", "resolved_at")
        with self.transaction():
            self.db.execute("INSERT OR IGNORE INTO events(" + ",".join(columns) + ") VALUES(" +
                            ",".join("?" for _ in columns) + ")", tuple(event.get(key) for key in columns))
            current = self.event(event["event_id"])
            if event["state"] == "RESOLVED" and current["state"] != "RESOLVED":
                self.update(event["event_id"], "resolve")
            elif event["state"] == "ACKNOWLEDGED" and current["state"] == "ALARM":
                self.update(event["event_id"], "ack")
            if event.get("local_cleared"):
                self.update(event["event_id"], "local_clear")

    def update(self, event_id, action):
        if action not in ("ack", "resolve", "local_clear"):
            raise ValueError("Unknown event action")
        with self.transaction():
            event = self.event(event_id)
            if event is None:
                return None
            if action == "local_clear":
                self.db.execute("UPDATE events SET local_cleared=1 WHERE event_id=?", (event_id,))
            elif action == "ack" and event["state"] == "ALARM":
                self.db.execute("UPDATE events SET state='ACKNOWLEDGED',acknowledged_at=? WHERE event_id=?",
                                (utc_now(), event_id))
            elif action == "resolve" and event["state"] != "RESOLVED":
                self.db.execute("UPDATE events SET state='RESOLVED',resolved_at=? WHERE event_id=?",
                                (utc_now(), event_id))
            return self.event(event_id)

    def http_receipt(self, gateway_id, boot_id, sequence):
        with self.lock:
            row = self.db.execute(
                "SELECT message_hash FROM http_receipts WHERE gateway_id=? AND boot_id=? AND sequence=?",
                (gateway_id,boot_id,sequence)).fetchone()
            return row[0] if row else None

    def add_http_receipt(self, packet, message_hash):
        with self.transaction():
            cursor = self.db.execute(
                "INSERT INTO http_receipts(gateway_id,boot_id,sequence,message_hash,packet_json,received_at) VALUES(?,?,?,?,?,?)",
                (packet["gateway_id"],packet["boot_id"],packet["sequence"],message_hash,
                 json.dumps(packet,ensure_ascii=False,separators=(",",":"),allow_nan=False),utc_now()))
            row_id = cursor.lastrowid
            self.db.execute(
                "INSERT INTO http_sessions(gateway_id,boot_id,high_sequence) VALUES(?,?,?) "
                "ON CONFLICT(gateway_id,boot_id) DO UPDATE SET high_sequence=max(high_sequence,excluded.high_sequence)",
                (packet["gateway_id"],packet["boot_id"],packet["sequence"]))
            if row_id % 1000 == 0:
                self.db.execute("DELETE FROM http_receipts WHERE id < ?",(row_id-100000,))

    def http_high_sequence(self, gateway_id, boot_id):
        with self.lock:
            row = self.db.execute("SELECT high_sequence FROM http_sessions WHERE gateway_id=? AND boot_id=?",
                                  (gateway_id,boot_id)).fetchone()
            return row[0] if row else 0

    def gateway_commands(self, event_ids):
        commands = []
        for event_id in event_ids:
            event = self.event(event_id)
            if event and event["state"] in ("ACKNOWLEDGED","RESOLVED"):
                commands.append({"kind":"alarm_resolve" if event["state"]=="RESOLVED" else "alarm_ack",
                                 "event_id":event_id})
        return commands

    def close(self):
        with self.lock:
            self.db.close()
