#pragma once
// Copy to secrets.h (gitignored); do not commit credentials or put them in URLs.
#define SAFEWEAR_WIFI_SSID ""
#define SAFEWEAR_WIFI_PASSWORD ""
#define SAFEWEAR_GATEWAY_ID "gw-01"
// Trusted isolated LAN only. Include the exact canonical endpoint below.
#define SAFEWEAR_HTTP_URL "http://192.168.1.50:8000/api/v1/gateway/ingest"
// Generate a separate random token >=32 characters; same value configured on Pi.
#define SAFEWEAR_GATEWAY_TOKEN ""
