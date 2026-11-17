"""LAN browser access kept separate from machine-to-machine ingestion auth."""
import base64
import binascii
import hmac
import hashlib
import re
import secrets
import time

from fastapi.responses import JSONResponse

COOKIE = "safewear_session"


def equal(left,right):
    return hmac.compare_digest(left.encode("utf-8"),right.encode("utf-8"))


class BrowserAccess:
    def __init__(self,password=None):
        if password is not None and len(password) < 12:
            raise ValueError("Web password must contain at least 12 characters")
        self.password = password
        self.session = secrets.token_urlsafe(32)

    def cookie_ok(self,cookies):
        value = cookies.get(COOKIE,"")
        try:
            expiry,signature = value.split(".",1)
            if not expiry.isascii() or not expiry.isdecimal() or len(expiry)>12:
                return False
            expected = hmac.new(self.session.encode(),expiry.encode(),hashlib.sha256).hexdigest()
            return equal(signature,expected) and time.time() <= int(expiry)
        except ValueError:
            return False

    def basic_ok(self,authorization):
        if not authorization.startswith("Basic "):
            return False
        try:
            user,password = base64.b64decode(authorization[6:],validate=True).decode("utf-8").split(":",1)
        except (ValueError,UnicodeError,binascii.Error):
            return False
        return self.credentials_ok(user,password)

    def credentials_ok(self,user,password):
        user_ok = equal(user,"safewear")
        password_ok = equal(password,self.password or "")
        return self.password is not None and user_ok and password_ok

    def allowed(self,headers,cookies):
        return self.password is None or self.cookie_ok(cookies) or self.basic_ok(headers.get("authorization",""))

    def challenge(self):
        return JSONResponse({"detail":"Cần đăng nhập dashboard"},status_code=401,
                            headers={"WWW-Authenticate":'Basic realm="SafeWear", charset="UTF-8"'})

    def set_session(self,response,secure=False):
        expiry = str(int(time.time())+8*60*60)
        signature = hmac.new(self.session.encode(),expiry.encode(),hashlib.sha256).hexdigest()
        response.set_cookie(COOKIE,expiry+"."+signature,max_age=8*60*60,httponly=True,
                            samesite="strict",secure=secure)


def validate_hosts(hosts):
    result = ["127.0.0.1","localhost","testserver"]
    for host in hosts or []:
        # Exact IPv4/DNS names only; no wildcard, port, scheme or path.
        if (not isinstance(host,str) or len(host)>253 or
            not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?",host)
            or ".." in host):
            raise ValueError("Allowed host must be an exact IPv4 address or DNS name")
        if host.lower() not in result:
            result.append(host.lower())
    return result
