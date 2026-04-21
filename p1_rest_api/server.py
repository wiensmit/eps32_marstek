import json
import os
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SENSOR_ENTITY = os.environ.get("SENSOR_ENTITY", "sensor.marstek_meter_p1_adjusted")
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "8088"))
CACHE_TTL_MS = int(os.environ.get("CACHE_TTL_MS", "1000"))
STALE_FALLBACK_MS = int(os.environ.get("STALE_FALLBACK_MS", "30000"))
LOG_REQUESTS = os.environ.get("LOG_REQUESTS", "false").lower() == "true"
SUPERVISOR_TOKEN = os.environ.get("SUPERVISOR_TOKEN", "")

HA_URL = f"http://supervisor/core/api/states/{SENSOR_ENTITY}"

_cache_ts_ms = 0.0
_cache_value = 0.0
_cache_valid = False
_fetch_errors = 0
_fetch_success = 0


def now_ms() -> float:
    return time.monotonic() * 1000


def fetch_power() -> tuple[float, bool]:
    global _cache_ts_ms, _cache_value, _cache_valid, _fetch_errors, _fetch_success

    t = now_ms()
    if _cache_valid and (t - _cache_ts_ms) < CACHE_TTL_MS:
        return _cache_value, True

    req = urllib.request.Request(
        HA_URL,
        headers={
            "Authorization": f"Bearer {SUPERVISOR_TOKEN}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=2) as resp:
            data = json.loads(resp.read().decode())
            state = data.get("state")
            if state in (None, "unknown", "unavailable", ""):
                raise ValueError(f"sensor state is {state!r}")
            value = float(state)
            _cache_value = value
            _cache_ts_ms = t
            _cache_valid = True
            _fetch_success += 1
            return value, True
    except Exception as exc:
        _fetch_errors += 1
        print(f"[ERR] fetch {SENSOR_ENTITY}: {exc}", file=sys.stderr, flush=True)
        if _cache_valid and (t - _cache_ts_ms) < STALE_FALLBACK_MS:
            return _cache_value, True
        return 0.0, False


def shelly_payload(value: float) -> dict:
    # Minimal shape — only the fields esp_marstek.ino pollMeter() reads.
    # The full Shelly schema overflows the ESP32's 512B JSON buffer.
    return {
        "id": 0,
        "total_act_power": value,
        "a_act_power": value,
        "b_act_power": 0.0,
        "c_act_power": 0.0,
    }


class Handler(BaseHTTPRequestHandler):
    def _reply(self, code: int, body_dict: dict) -> None:
        body = json.dumps(body_dict).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]

        if path == "/rpc/EM.GetStatus":
            value, _ok = fetch_power()
            self._reply(200, shelly_payload(value))
            return

        if path == "/rpc/Shelly.GetDeviceInfo":
            self._reply(200, {
                "id": "shellypro3em-p1proxy",
                "mac": "000000000000",
                "model": "SPEM-003CEBEU",
                "gen": 2,
                "fw_id": "p1proxy-1.0.0",
                "ver": "1.0.0",
                "app": "Pro3EM",
                "auth_en": False,
            })
            return

        if path == "/health":
            age = int(now_ms() - _cache_ts_ms) if _cache_valid else None
            self._reply(200, {
                "status": "ok" if _cache_valid else "no_data",
                "sensor": SENSOR_ENTITY,
                "last_value_w": _cache_value,
                "cache_age_ms": age,
                "fetch_success": _fetch_success,
                "fetch_errors": _fetch_errors,
            })
            return

        self._reply(404, {"error": "Not Found", "path": path})

    def log_message(self, fmt: str, *args) -> None:
        if LOG_REQUESTS:
            super().log_message(fmt, *args)


def main() -> None:
    if not SUPERVISOR_TOKEN:
        print("[FATAL] SUPERVISOR_TOKEN missing — homeassistant_api: true required in config.yaml",
              file=sys.stderr, flush=True)
        sys.exit(1)

    print(f"[INIT] sensor={SENSOR_ENTITY} port={LISTEN_PORT} "
          f"ttl={CACHE_TTL_MS}ms fallback={STALE_FALLBACK_MS}ms",
          flush=True)

    server = ThreadingHTTPServer(("", LISTEN_PORT), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()


if __name__ == "__main__":
    main()
