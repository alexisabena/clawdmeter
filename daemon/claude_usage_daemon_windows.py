#!/usr/bin/env python3
"""Claude Usage Tracker Daemon — Windows (Phase 2).

Reads the Claude OAuth token from the native-Windows credentials path and
polls the Anthropic API for rate-limit utilization data. BLE glue added in
later plans.
"""

import asyncio
import calendar
import datetime
import json
import logging
import logging.handlers
import os
import re
import signal
import subprocess
import sys
import socket
import threading
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 1
SCAN_TIMEOUT = 8.0
CONNECT_RETRIES = 3        # D-01: attempts before giving up on a device
CONNECT_RETRY_DELAY = 2.0  # D-01: seconds between failed connect attempts
ZOMBIE_BREAK_LIMIT = 1     # D-03: consecutive write failures before abandoning a half-open link
                           # N=1: breaks at T=60s, leaves ~60s headroom for reconnect+poll inside 120s SLA
                           # N=2 would bust the 120s budget before reconnect even begins
RECONNECT_BACKOFF_CAP = 8  # D-05: fast-reconnect cap (seconds); keeps stacked retries inside 120s SLA
                           # ~5–10s band per CONTEXT.md Claude's Discretion; 8 chosen as middle ground

# Optional reset chime.
# Optional clock display. 
# Config lives under the same Clawdmeter dir as daemon.log.
CONFIG_FILE = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter" / "config"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def _build_file_logger() -> logging.Logger | None:
    """Create a rotating file logger for field diagnostics, or None.

    Autostart launches the tray under pythonw.exe, which has no console — stdout
    is discarded (and is in fact None, making print() unsafe). A rotating file is
    then the ONLY trail when the daemon stalls in the field. Windows-only: on the
    Linux dev box / CI the console print() suffices, and gating to win32 keeps the
    pure-helper unit tests from writing stray log files.
    """
    if sys.platform != "win32":
        return None
    logger = logging.getLogger("clawdmeter.daemon")
    if logger.handlers:
        return logger  # idempotent across re-import (tray imports this module)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    path = base / "Clawdmeter" / "daemon.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            path, maxBytes=512 * 1024, backupCount=3, encoding="utf-8"
        )
    except OSError:
        return None  # best-effort — logging setup must never stop the daemon
    handler.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%d %H:%M:%S"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    return logger


_FILE_LOGGER = _build_file_logger()


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    # Under pythonw sys.stdout is None and print() would raise — guard it so a
    # missing console can never crash the daemon thread (the silent-freeze mode).
    try:
        print(line, flush=True)
    except (OSError, ValueError, AttributeError, RuntimeError):
        pass
    if _FILE_LOGGER is not None:
        _FILE_LOGGER.info(msg)


class AuthError(Exception):
    """Raised by poll_api on a genuine 401/403 — the token really is expired or
    invalid and the user must re-run `claude login`. Distinct from a None return,
    which means a TRANSIENT failure (network/DNS, timeout, rate-limit, 5xx) that
    must NOT be mislabeled as a token problem (SC#5: a boot-time `getaddrinfo
    failed` DNS blip wrongly fired the 'token expired' toast)."""


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        raise AuthError(resp.status_code)
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd)."""
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30, "rd": ""}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30, "rd": ""}
    pct_val = (now - period_start) / period_len * 100
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": int(round(period_len / 86400)),
        "rd": f"{dt_end.strftime('%b')} {dt_end.day}",
    }


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob."""
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _windows_credential_candidates() -> list[Path]:
    """Return the ordered list of credential file paths to probe."""
    if override := os.environ.get("CLAUDE_CREDENTIALS_PATH"):
        return [Path(override)]
    if config_dir := os.environ.get("CLAUDE_CONFIG_DIR"):
        return [Path(config_dir) / ".credentials.json"]
    home = Path.home()
    local_appdata = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local"))
    appdata = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
    return [
        home / ".claude" / ".credentials.json",
        local_appdata / "Claude" / ".credentials.json",
        appdata / "Claude" / ".credentials.json",
    ]


def read_token() -> str | None:
    """Read the Claude OAuth access token from the first available credential file."""
    for path in _windows_credential_candidates():
        try:
            return _extract_access_token(path.read_text(encoding="utf-8"))
        except OSError:
            continue
    return None


def _read_expiry() -> str:
    """Return human-readable expiry from the first-hit credentials file."""
    for path in _windows_credential_candidates():
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError:
            continue
        try:
            data = json.loads(raw)
            oauth = data.get("claudeAiOauth", {})
            expires_ms = oauth.get("expiresAt")
            if expires_ms is None:
                return "expiry unknown"
            dt = datetime.datetime.fromtimestamp(
                expires_ms / 1000, tz=datetime.timezone.utc
            )
            return dt.strftime("%Y-%m-%d %H:%M UTC")
        except (TypeError, ValueError, OSError, AttributeError, json.JSONDecodeError):
            continue
    return "expiry unknown"


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" so the device stays silent until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" so existing setups keep showing "Usage" until opted in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection on Windows via the registry. Returns 12 or 24."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Control Panel\International") as k:
            # iTime: "1" = 24-hour, "0" = 12-hour.
            val, _ = winreg.QueryValueEx(k, "iTime")
            return 24 if str(val).strip() == "1" else 12
    except (ImportError, OSError):
        return 24


def add_clock_fields(payload: dict) -> None:
    """Add "t" (local wall-clock epoch) + "tf" (12|24) when the config opts in."""
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


def read_gemini_api_key() -> str | None:
    if key := os.environ.get("GEMINI_API_KEY"):
        return key.strip()
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text(encoding="utf-8").splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k.strip().lower() == "gemini_api_key":
                    return v.strip()
    except OSError:
        pass
    return None


def read_gemini_project_id() -> str | None:
    if pid := os.environ.get("GEMINI_PROJECT_ID"):
        return pid.strip()
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text(encoding="utf-8").splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k.strip().lower() == "gemini_project_id":
                    return v.strip()
    except OSError:
        pass
    return None


async def poll_gemini_usage(api_key: str, project_id: str) -> dict | None:
    now_dt = datetime.datetime.now(datetime.timezone.utc)
    start_dt = now_dt - datetime.timedelta(minutes=15)
    
    start_str = start_dt.strftime("%Y-%m-%dT%H:%M:%SZ")
    end_str = now_dt.strftime("%Y-%m-%dT%H:%M:%SZ")
    
    url = f"https://monitoring.googleapis.com/v3/projects/{project_id}/timeSeries"
    
    params_use = {
        "filter": 'metric.type = "serviceruntime.googleapis.com/quota/rate/use"',
        "interval.startTime": start_str,
        "interval.endTime": end_str,
        "key": api_key
    }
    params_limit = {
        "filter": 'metric.type = "serviceruntime.googleapis.com/quota/limit"',
        "interval.startTime": start_str,
        "interval.endTime": end_str,
        "key": api_key
    }
    
    try:
        async with httpx.AsyncClient(timeout=15.0) as http:
            resp_use = await http.get(url, params=params_use)
            resp_limit = await http.get(url, params=params_limit)
    except httpx.HTTPError as e:
        log(f"Gemini Monitoring API call failed: {e}")
        return None
        
    if resp_use.status_code != 200 or resp_limit.status_code != 200:
        log(f"Gemini API returned error status. Use: {resp_use.status_code}, Limit: {resp_limit.status_code}")
        return None
        
    try:
        use_json = resp_use.json()
        limit_json = resp_limit.json()
    except Exception as e:
        log(f"Failed to parse Gemini monitoring JSON: {e}")
        return None

    def extract_latest_values(data: dict) -> dict:
        metrics = {}
        for ts in data.get("timeSeries", []):
            qm = ts.get("metric", {}).get("labels", {}).get("quota_metric")
            if not qm or not qm.startswith("generativelanguage.googleapis.com/"):
                continue
            points = ts.get("points", [])
            if not points:
                continue
            latest_point = points[0]
            val_dict = latest_point.get("value", {})
            val = 0.0
            if "int64Value" in val_dict:
                val = float(val_dict["int64Value"])
            elif "doubleValue" in val_dict:
                val = float(val_dict["doubleValue"])
            metrics[qm] = val
        return metrics

    use_metrics = extract_latest_values(use_json)
    limit_metrics = extract_latest_values(limit_json)
    
    rpm_metric = "generativelanguage.googleapis.com/generate_content_free_tier_requests"
    rpm_paid_metric = "generativelanguage.googleapis.com/generate_content_requests"
    rpm_use = use_metrics.get(rpm_metric) or use_metrics.get(rpm_paid_metric) or 0.0
    rpm_limit = limit_metrics.get(rpm_metric) or limit_metrics.get(rpm_paid_metric) or 15.0
    
    tpm_metric = "generativelanguage.googleapis.com/generate_content_free_tier_input_token_count"
    tpm_paid_metric = "generativelanguage.googleapis.com/generate_content_input_token_count"
    tpm_use = use_metrics.get(tpm_metric) or use_metrics.get(tpm_paid_metric) or 0.0
    tpm_limit = limit_metrics.get(tpm_metric) or limit_metrics.get(tpm_paid_metric) or 1000000.0
    
    rpd_metric = "generativelanguage.googleapis.com/generate_content_requests_per_day"
    rpd_use = use_metrics.get(rpd_metric) or 0.0
    rpd_limit = limit_metrics.get(rpd_metric) or 1500.0
    
    rpm_pct = int(round((rpm_use / rpm_limit * 100.0))) if rpm_limit > 0 else 0
    tpm_pct = int(round((tpm_use / tpm_limit * 100.0))) if tpm_limit > 0 else 0
    rpd_pct = int(round((rpd_use / rpd_limit * 100.0))) if rpd_limit > 0 else 0
    
    session_pct = max(rpm_pct, tpm_pct)
    weekly_pct = rpd_pct
    
    seconds_to_midnight = ((24 - now_dt.hour - 1) * 3600) + ((60 - now_dt.minute - 1) * 60) + (60 - now_dt.second)
    rpd_reset_mins = max(0, int(seconds_to_midnight / 60))
    
    return {
        "s": min(100, max(0, session_pct)),
        "sr": 1,
        "w": min(100, max(0, weekly_pct)),
        "wr": rpd_reset_mins,
        "st": "allowed",
        "ok": True
    }


def build_ble_payload(claude_payload: dict | None, gemini_payload: dict | None, agent_state: int = 0, agent_msg: str = "") -> dict:
    payload = {"ok": False}
    payload["a_st"] = agent_state
    if agent_msg:
        payload["a_msg"] = agent_msg[:32]

    if claude_payload and claude_payload.get("ok"):
        payload["ok"] = True
        payload["c_s"] = claude_payload.get("s", 0)
        payload["c_sr"] = claude_payload.get("sr", 0)
        payload["c_w"] = claude_payload.get("w", 0)
        payload["c_wr"] = claude_payload.get("wr", 0)
        payload["c_st"] = claude_payload.get("st", "unknown")
        payload["c_acct"] = claude_payload.get("acct", "pro")
        if "c" in claude_payload:
            payload["c"] = claude_payload["c"]
        if "t" in claude_payload:
            payload["t"] = claude_payload["t"]
        if "tf" in claude_payload:
            payload["tf"] = claude_payload["tf"]
    else:
        payload["c_s"] = 0
        payload["c_sr"] = 0
        payload["c_w"] = 0
        payload["c_wr"] = 0
        payload["c_st"] = "error"
        payload["c_acct"] = "pro"

    if gemini_payload and gemini_payload.get("ok"):
        payload["ok"] = True
        payload["g_s"] = gemini_payload.get("s", 0)
        payload["g_sr"] = gemini_payload.get("sr", 0)
        payload["g_w"] = gemini_payload.get("w", 0)
        payload["g_wr"] = gemini_payload.get("wr", 0)
        payload["g_st"] = gemini_payload.get("st", "unknown")
    else:
        payload["g_s"] = 0
        payload["g_sr"] = 0
        payload["g_w"] = 0
        payload["g_wr"] = 0
        payload["g_st"] = "disabled" if not (read_gemini_api_key() and read_gemini_project_id()) else "error"
        
    return payload


def read_esp32_ip() -> str:
    """Read custom ESP32 IP address if specified in config, otherwise default to mDNS clawdmeter.local"""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "esp32_ip":
                    return val.strip()
    except OSError:
        pass
    return "clawdmeter.local"


async def main(tray_state=None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                try:
                    signal.signal(sig, _stop)
                except ValueError:
                    pass

    log("=== Claude Usage Tracker Daemon (Wi-Fi, Windows) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    last_poll = 0.0
    
    claude_payload_cache = None
    gemini_payload_cache = None
    
    last_sent_agent_state = -1
    last_sent_agent_msg = ""
    
    backoff = 1
    
    try:
        while not stop_event.is_set():
            agent_state = 0
            agent_msg = ""
            request_path = Path(os.path.expanduser('~/.claude/approval_request.json'))
            if request_path.exists():
                try:
                    req_data = json.loads(request_path.read_text(encoding="utf-8"))
                    req_state = req_data.get("state")
                    if req_state == "needs_approval":
                        agent_state = 2
                        agent_msg = req_data.get("message", "Permission request")
                    elif req_state == "working":
                        agent_state = 1
                        agent_msg = req_data.get("message", "Working...")
                except Exception:
                    pass

            state_changed = (agent_state != last_sent_agent_state) or (agent_msg != last_sent_agent_msg)
            
            now = time.time()
            elapsed = now - last_poll
            
            if elapsed >= POLL_INTERVAL or last_poll == 0.0 or not claude_payload_cache:
                token = read_token()
                claude_payload = None
                if not token:
                    log("No Claude token; skipping Claude poll")
                    if tray_state:
                        tray_state.set_error("token expired — run claude login")
                else:
                    try:
                        claude_payload = await poll_api(token)
                    except AuthError:
                        if tray_state:
                            tray_state.set_error("token expired — run claude login")
                        claude_payload = None
                    except Exception as e:
                        log(f"Claude poll exception: {e}")
                        claude_payload = None
                
                gemini_key = read_gemini_api_key()
                gemini_pid = read_gemini_project_id()
                gemini_payload = None
                if gemini_key and gemini_pid:
                    try:
                        gemini_payload = await poll_gemini_usage(gemini_key, gemini_pid)
                    except Exception as e:
                        log(f"Gemini poll exception: {e}")
                        gemini_payload = None
                else:
                    log("Gemini API key or project ID not configured; skipping Gemini poll")
                    
                claude_payload_cache = claude_payload
                gemini_payload_cache = gemini_payload
                last_poll = time.time()

            payload = build_ble_payload(claude_payload_cache, gemini_payload_cache, agent_state, agent_msg)

            target_host = read_esp32_ip()
            esp_url = f"http://{target_host}/api/payload"

            if state_changed or elapsed >= POLL_INTERVAL or last_sent_agent_state == -1:
                if payload.get("ok"):
                    log(f"Sending payload to ESP32: {payload} -> {esp_url}")
                    try:
                        async with httpx.AsyncClient(timeout=4.0) as http:
                            resp = await http.post(esp_url, json=payload)
                        if resp.status_code == 200:
                            # Save active IP for approval hook discovery
                            try:
                                ip_file = Path(os.path.expanduser('~/.claude/clawdmeter_ip.txt'))
                                ip_file.parent.mkdir(parents=True, exist_ok=True)
                                ip_file.write_text(target_host, encoding="utf-8")
                            except Exception:
                                pass
                            
                            last_sent_agent_state = agent_state
                            last_sent_agent_msg = agent_msg
                            if tray_state:
                                tray_state.set_connected(time.time())
                            backoff = 1
                        else:
                            log(f"ESP32 returned HTTP {resp.status_code}")
                            raise Exception("Bad HTTP status")
                    except Exception as e:
                        log(f"Failed to send payload to ESP32: {e}")
                        if tray_state:
                            tray_state.set_scanning()
                        try:
                            await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                        except asyncio.TimeoutError:
                            pass
                        backoff = min(backoff * 2, 60)
                        continue
                else:
                    log("Both Claude and Gemini polls failed; skipping write")

            try:
                await asyncio.wait_for(stop_event.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        pass


if __name__ == "__main__":
    import socket
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
