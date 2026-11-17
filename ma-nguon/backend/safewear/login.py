"""Browser form login using the dashboard's existing session cookie."""

import asyncio
import base64
import hashlib
import html
import re
from urllib.parse import parse_qs

from fastapi import Request
from fastapi.responses import HTMLResponse, RedirectResponse
from starlette.requests import ClientDisconnect

MAX_LOGIN_BODY = 4096
_STYLE = """
:root{font-family:Segoe UI,Arial,sans-serif;color:#183a36;background:#eef4f0;color-scheme:light}
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px}
main{width:100%;max-width:420px;background:#fff;border:1px solid #d8e4dc;border-radius:16px;padding:32px;box-shadow:0 12px 40px #1738310d}
.brand{font-size:24px;font-weight:700;color:#205f48}h1{font-size:22px;margin:24px 0 10px}p{line-height:1.6;color:#5c7469;font-size:14px}
label{display:block;font-size:14px;font-weight:600;margin:20px 0 8px}input,button{width:100%;font:inherit;border-radius:8px;padding:12px}
input{border:1px solid #aabeb1;background:#fff;color:#183a36}button{border:0;margin-top:24px;background:#236b4f;color:#fff;font-weight:600;cursor:pointer}
input:focus-visible,button:focus-visible{outline:3px solid #92c8aa;outline-offset:3px}.error{color:#9c352b;background:#fff0ed;border-radius:8px;padding:12px}.note{margin:24px 0 0;font-size:12px}
@media(max-width:480px){body{padding:16px}main{padding:24px}}
"""
_STYLE_HASH = base64.b64encode(hashlib.sha256(_STYLE.encode("utf-8")).digest()).decode("ascii")
_CSP = ("default-src 'none'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; "
        f"script-src 'none'; style-src 'sha256-{_STYLE_HASH}'; img-src data:; form-action 'self'")


def login_page(message="", status_code=200):
    error = f'<p class="error" role="alert">{html.escape(message)}</p>' if message else ""
    return HTMLResponse(
        f"""<!doctype html>
<html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Đăng nhập · SafeWear</title><link rel="icon" href="data:,"><style>{_STYLE}</style></head><body><main>
<div class="brand">SafeWear</div><h1>Đăng nhập trạm giám sát</h1>
<p>Nhập tài khoản của trạm để xem thiết bị và quản lý cảnh báo.</p>{error}
<form action="/login" method="post" accept-charset="UTF-8">
<label for="username">Tên đăng nhập</label>
<input id="username" name="username" autocomplete="username" value="safewear" maxlength="64" required>
<label for="password">Mật khẩu</label>
<input id="password" name="password" type="password" autocomplete="current-password" required autofocus>
<button type="submit">Đăng nhập</button></form>
<p class="note">Dùng mật khẩu dashboard được cấp cho trạm. Phiên đăng nhập có hiệu lực tối đa 8 giờ.</p>
</main></body></html>""",
        status_code=status_code,
        headers={"Content-Security-Policy": _CSP, "Cache-Control": "no-store"},
    )


async def login_submit(request: Request, browser):
    origin = request.headers.get("origin")
    if ((origin is not None and origin != f"{request.url.scheme}://{request.headers.get('host', '')}")
            or request.headers.get("sec-fetch-site") == "cross-site"):
        return login_page("Chỉ nhận đăng nhập từ trang SafeWear của trạm.", 403)
    if browser.password is None:
        return RedirectResponse("/", status_code=303)
    content_type = request.headers.get("content-type", "").split(";", 1)[0].strip().lower()
    if content_type != "application/x-www-form-urlencoded":
        return login_page("Định dạng đăng nhập không hợp lệ.", 415)
    length = request.headers.get("content-length")
    if length is not None:
        if not length.isascii() or not length.isdecimal() or len(length) > 10:
            return login_page("Thông tin đăng nhập không hợp lệ.", 400)
        if int(length) > MAX_LOGIN_BODY:
            return login_page("Thông tin đăng nhập quá dài.", 413)
    body = bytearray()
    try:
        async with asyncio.timeout(3):
            async for chunk in request.stream():
                if len(body) + len(chunk) > MAX_LOGIN_BODY:
                    return login_page("Thông tin đăng nhập quá dài.", 413)
                body.extend(chunk)
    except TimeoutError:
        return login_page("Hết thời gian nhận thông tin đăng nhập. Vui lòng thử lại.", 408)
    except ClientDisconnect:
        return login_page("Kết nối đăng nhập đã ngắt. Vui lòng thử lại.", 400)
    try:
        encoded = body.decode("ascii")
        if re.search(r"%(?![0-9a-fA-F]{2})", encoded):
            raise ValueError("Malformed form escape")
        fields = parse_qs(encoded, keep_blank_values=True, strict_parsing=True,
                          encoding="utf-8", errors="strict", max_num_fields=2)
        if set(fields) != {"username", "password"} or any(len(values) != 1 for values in fields.values()):
            raise ValueError("Unexpected login fields")
        username, password = fields["username"][0], fields["password"][0]
        if not 1 <= len(username) <= 64 or not password:
            raise ValueError("Invalid login field length")
        if any(ord(char) < 32 or ord(char) == 127 for char in username + password):
            raise ValueError("Control character in login fields")
    except (UnicodeError, ValueError):
        return login_page("Thông tin đăng nhập không hợp lệ. Vui lòng nhập lại.", 400)
    if not browser.credentials_ok(username, password):
        return login_page("Tên đăng nhập hoặc mật khẩu chưa đúng.", 401)
    response = RedirectResponse("/", status_code=303)
    browser.set_session(response, request.url.scheme == "https")
    return response
