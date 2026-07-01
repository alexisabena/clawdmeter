#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import calendar
import datetime
import getpass
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 1
SCAN_TIMEOUT = 8.0
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

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


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
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

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
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
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
    clock = read_clock_setting()
    if clock == "off":
        return
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


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
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

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
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
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


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


def get_local_ip() -> str:
    """Get the primary local network IP address of this computer"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """Handle incoming approval responses from the ESP32 touch buttons"""
    try:
        data = await asyncio.wait_for(reader.read(1024), timeout=5.0)
    except Exception:
        writer.close()
        await writer.wait_closed()
        return

    req = data.decode('utf-8', errors='ignore')
    if "POST /api/response" in req:
        parts = req.split("\r\n\r\n", 1)
        if len(parts) > 1:
            try:
                body = json.loads(parts[1])
                approved = body.get("approved", False)
                log(f"Received Wi-Fi approval response from ESP32: {approved}")
                response_path = Path(os.path.expanduser('~/.claude/approval_response.json'))
                response_path.write_text(json.dumps({"approved": approved}), encoding="utf-8")
            except Exception as e:
                log(f"Failed to parse Wi-Fi approval response JSON: {e}")
        
        resp = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n\r\n"
            '{"status":"ok"}'
        )
        writer.write(resp.encode())
        await writer.drain()
    else:
        resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n"
        writer.write(resp.encode())
        await writer.drain()
        
    writer.close()
    try:
        await writer.wait_closed()
    except Exception:
        pass


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (Wi-Fi, macOS/Linux) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    server = await asyncio.start_server(handle_client, host='0.0.0.0', port=18989)
    log("Started local HTTP server on port 18989 for remote approvals")

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
                else:
                    try:
                        claude_payload = await poll_api(token)
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
            payload["d_url"] = f"http://{get_local_ip()}:18989/api/response"

            target_host = read_esp32_ip()
            esp_url = f"http://{target_host}/api/payload"

            if state_changed or elapsed >= POLL_INTERVAL or last_sent_agent_state == -1:
                if payload.get("ok"):
                    log(f"Sending payload to ESP32: {payload} -> {esp_url}")
                    try:
                        async with httpx.AsyncClient(timeout=4.0) as http:
                            resp = await http.post(esp_url, json=payload)
                        if resp.status_code == 200:
                            last_sent_agent_state = agent_state
                            last_sent_agent_msg = agent_msg
                            backoff = 1
                        else:
                            log(f"ESP32 returned HTTP {resp.status_code}")
                            raise Exception("Bad HTTP status")
                    except Exception as e:
                        log(f"Failed to send payload to ESP32: {e}")
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
        server.close()
        await server.wait_closed()
        log("HTTP server closed")


if __name__ == "__main__":
    import socket
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
