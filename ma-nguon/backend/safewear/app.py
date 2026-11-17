"""Local API; ingestion, SQLite and alarms run for the app lifespan, not the browser."""

import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
from pathlib import Path
import os
import re
import sqlite3
from urllib.parse import urlparse

from fastapi import FastAPI, HTTPException, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
from starlette.middleware.trustedhost import TrustedHostMiddleware
from starlette.requests import ClientDisconnect

from .demo import DemoSource
from .service import Receiver, SerialSource
from .storage import Store
from .access import BrowserAccess, equal, validate_hosts
from .login import login_page, login_submit
from .http_ingest import HttpIngress, MAX_BODY, PATH as INGEST_PATH, UplinkError

ROOT = Path(__file__).resolve().parents[2]


def _same_origin(origin, host):
    if not origin:
        return True
    parsed = urlparse(origin)
    return parsed.scheme in ("http", "https") and parsed.netloc == host


class DemoAction(BaseModel):
    action: str = Field(min_length=1, max_length=24)


def create_app(db_path=None, demo=False, serial_port=None, model_path=None,*,
               http_enabled=False,gateway_token=None,gateway_id="gw-01",web_password=None,
               allowed_hosts=None, bp_model_path=None, bp_calibration_path=None):
    if sum((bool(demo),bool(serial_port),bool(http_enabled))) > 1:
        raise ValueError("Choose exactly one ingestion source: demo, serial or HTTP")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,48}",gateway_id):
        raise ValueError("Invalid gateway ID")
    if http_enabled and (not gateway_token or not gateway_token.isascii()
                         or not 32 <= len(gateway_token) <= 256
                         or any(c.isspace() for c in gateway_token)):
        raise ValueError("HTTP ingestion requires a 32..256 character ASCII gateway token")
    if gateway_token and web_password and equal(gateway_token,web_password):
        raise ValueError("Use separate gateway token and dashboard password")
    browser = BrowserAccess(web_password)
    hosts = validate_hosts(allowed_hosts)
    if any(host not in ("127.0.0.1","localhost","testserver") for host in hosts) and not web_password:
        raise ValueError("LAN dashboard requires a separate web password")
    mode = "demo" if demo else "hardware"
    path = db_path or ROOT / "data" / f"{mode}.sqlite"
    store = Store(path)
    receiver = Receiver(store, mode, model_path, bp_model_path=bp_model_path,
                        bp_calibration_path=bp_calibration_path)
    demo_source = DemoSource(receiver) if demo else None
    source = demo_source if demo else (SerialSource(receiver, serial_port) if serial_port else None)
    if isinstance(source, SerialSource):
        receiver.serial = source
    http_ingress = HttpIngress(receiver,gateway_id) if http_enabled else None

    @asynccontextmanager
    async def lifespan(app):
        task = asyncio.create_task(source.run()) if source else None
        try:
            yield
        finally:
            if task:
                task.cancel()
                with suppress(asyncio.CancelledError):
                    await task
            store.close()

    app = FastAPI(title="SafeWear local", lifespan=lifespan, docs_url=None, redoc_url=None)
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=hosts)
    app.state.receiver = receiver
    app.state.demo = demo_source
    app.state.http_ingress = http_ingress

    @app.get("/api/bp")
    async def bp_history(node_id: int | None = Query(None, ge=1, le=65535),
                         limit: int = Query(100, ge=1, le=1000)):
        return {"experimental": True, "readings": store.bp_readings(node_id, limit)}

    @app.middleware("http")
    async def local_mutations(request: Request, call_next):
        is_ingest = request.url.path == INGEST_PATH
        if not is_ingest and request.url.path not in ("/healthz", "/login") and not browser.allowed(request.headers,request.cookies):
            if request.method in ("GET", "HEAD") and request.url.path == "/":
                return RedirectResponse("/login",status_code=303,headers={"Cache-Control":"no-store"})
            return browser.challenge()
        if is_ingest:
            # Firmware endpoint is never browser-authenticated, even with a session cookie.
            if request.headers.get("origin"):
                return JSONResponse({"detail":"Endpoint chỉ dành cho Gateway"},status_code=403)
            if http_ingress is None:
                return JSONResponse({"detail":"HTTP Gateway chưa được bật"},status_code=403)
            supplied = request.headers.get("authorization","")
            if not equal(supplied,"Bearer "+gateway_token):
                return JSONResponse({"detail":"Gateway chưa xác thực"},status_code=401)
            length = request.headers.get("content-length")
            if length is not None and (not length.isascii() or not length.isdecimal() or len(length)>10 or int(length)>MAX_BODY):
                return JSONResponse({"detail":"Gói HTTP quá dài"},status_code=413)
        if request.method == "POST" and not _same_origin(request.headers.get("origin"), request.headers.get("host")):
            return JSONResponse({"detail": "Chỉ nhận thao tác từ web cục bộ"}, status_code=403)
        response = await call_next(request)
        if browser.password is not None and not is_ingest and browser.basic_ok(request.headers.get("authorization","")):
            browser.set_session(response,request.url.scheme=="https")
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Cache-Control"] = "no-store"
        response.headers.setdefault("Content-Security-Policy", "default-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; script-src 'self'; style-src 'self'; connect-src 'self'")
        return response

    @app.get("/login")
    async def login_form(request: Request):
        if browser.password is None or browser.allowed(request.headers,request.cookies):
            return RedirectResponse("/",status_code=303)
        return login_page()

    @app.post("/login")
    async def login(request: Request):
        return await login_submit(request,browser)

    @app.post(INGEST_PATH)
    async def ingest_gateway(request: Request):
        started = receiver.clock()
        if request.headers.get("content-type","").split(";",1)[0].strip().lower() != "application/json":
            raise HTTPException(415,"Cần Content-Type application/json")
        body = bytearray()
        try:
            async with asyncio.timeout(3):
                async for chunk in request.stream():
                    if len(body)+len(chunk)>MAX_BODY:
                        raise HTTPException(413,"Gói HTTP quá dài")
                    body.extend(chunk)
        except TimeoutError:
            raise HTTPException(408,"Hết thời gian nhận gói") from None
        except ClientDisconnect:
            raise HTTPException(400,"Gateway đã ngắt khi đang gửi gói") from None
        try:
            return http_ingress.accept(bytes(body),body_read_seconds=receiver.clock()-started)
        except UplinkError as exc:
            raise HTTPException(exc.status,exc.detail,
                                headers={"Retry-After":"1"} if exc.status==429 else None) from None

    @app.get("/healthz")
    async def health():
        return {"status": "ok", "mode": mode}

    @app.get("/api/latest")
    async def latest():
        return receiver.state()

    @app.get("/api/events")
    async def events():
        return {"events": receiver.events()}

    @app.post("/api/events/{event_id}/{action}")
    async def operator(event_id: str, action: str):
        if action not in ("ack", "resolve") or not re.fullmatch(r"\d{1,5}:\d{1,10}:\d{1,10}", event_id):
            raise HTTPException(400, "Thao tác hoặc mã sự kiện không hợp lệ")
        try:
            result = receiver.operator_action(event_id, action)
        except (OSError, sqlite3.Error) as exc:
            raise HTTPException(503, "Chưa lưu được thao tác; kiểm tra lưu trữ rồi thử lại") from exc
        if result is None:
            raise HTTPException(404, "Không tìm thấy sự kiện")
        return result

    @app.post("/api/demo/action")
    async def demo_action(body: DemoAction):
        if demo_source is None:
            raise HTTPException(403, "Điều khiển mô phỏng bị tắt trong chế độ phần cứng")
        try:
            demo_source.action(body.action)
        except ValueError as exc:
            raise HTTPException(400, "Tình huống mô phỏng không hợp lệ") from exc
        return receiver.state()

    @app.websocket("/ws")
    async def live(ws: WebSocket):
        host = ws.headers.get("host","").split(":",1)[0].lower()
        if (host not in hosts or not browser.allowed(ws.headers,ws.cookies)
            or not _same_origin(ws.headers.get("origin"), ws.headers.get("host"))):
            await ws.close(code=1008)
            return
        await ws.accept()
        try:
            while True:
                await asyncio.wait_for(ws.send_json(receiver.state()), timeout=3)
                await asyncio.sleep(1)
        except (WebSocketDisconnect, RuntimeError, asyncio.TimeoutError):
            return

    @app.get("/")
    async def index():
        return FileResponse(ROOT / "web" / "index.html")

    app.mount("/static", StaticFiles(directory=ROOT / "web"), name="static")
    return app


def main():
    import uvicorn
    parser = argparse.ArgumentParser(description="SafeWear local receiver")
    parser.add_argument("--demo", action="store_true", help="Mô phỏng, DB riêng, không phải đo thật")
    parser.add_argument("--serial", help="Gateway serial port, for example COM5")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--db")
    parser.add_argument("--model", help="Trusted local RF model bundle only")
    parser.add_argument("--http",action="store_true",help="Receive JSON from one authenticated Gateway")
    parser.add_argument("--host",default="127.0.0.1")
    parser.add_argument("--allowed-host",action="append",default=[])
    parser.add_argument("--gateway-id",default="gw-01")
    args = parser.parse_args()
    if sum((bool(args.demo),bool(args.serial),bool(args.http)))>1:
        parser.error("--demo, --serial và --http không dùng đồng thời")
    if not 1 <= args.port <= 65535:
        parser.error("Port must be between 1 and 65535")
    password = os.getenv("SAFEWEAR_WEB_PASSWORD") or None
    if args.host not in ("127.0.0.1","localhost") and (not password or not args.allowed_host):
        parser.error("LAN binding requires SAFEWEAR_WEB_PASSWORD and explicit --allowed-host")
    try:
        app = create_app(args.db,args.demo,args.serial,args.model,http_enabled=args.http,
                         gateway_token=os.getenv("SAFEWEAR_GATEWAY_TOKEN"),gateway_id=args.gateway_id,
                         web_password=password,allowed_hosts=args.allowed_host,
                         bp_model_path=os.getenv("SAFEWEAR_BP_MODEL"),
                         bp_calibration_path=os.getenv("SAFEWEAR_BP_CALIBRATION"))
    except ValueError as exc:
        parser.error(str(exc))
    uvicorn.run(app,host=args.host,port=args.port,proxy_headers=False,ws_max_size=4096,limit_concurrency=24)


if __name__ == "__main__":
    main()
