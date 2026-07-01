import sys
import os
import json
import time
import socket
from pathlib import Path

ESP32_IP_FILE = Path(os.path.expanduser("~/.claude")) / "clawdmeter_ip.txt"
RESPONSE_TIMEOUT = 10.0  # seconds to wait for approval from Clawdmeter

# Only prompt for these dangerous tools. Read-only/utility tools are auto-approved.
DANGER_TOOLS = {
    "run_command",
    "write_to_file",
    "replace_file_content",
    "multi_replace_file_content",
}


def is_esp32_reachable() -> bool:
    """Quick TCP check to see if the ESP32 HTTP server is up."""
    try:
        ip = ESP32_IP_FILE.read_text(encoding="utf-8").strip()
        if not ip:
            return False
        with socket.create_connection((ip, 80), timeout=1.0):
            return True
    except Exception:
        return False


def main():
    # 1. Read JSON event context from stdin
    raw_input = ""
    try:
        raw_input = sys.stdin.read().strip()
        event_data = json.loads(raw_input) if raw_input else {}
    except Exception:
        event_data = {}

    # Debug log the incoming event payload to inspect structure
    try:
        log_dir = Path(os.path.expanduser("~/AppData/Local/Clawdmeter"))
        log_dir.mkdir(parents=True, exist_ok=True)
        debug_file = log_dir / "hook_debug.json"
        debug_file.write_text(
            json.dumps({"raw": raw_input, "parsed": event_data}, indent=2),
            encoding="utf-8"
        )
    except Exception:
        pass

    # Extract tool/command details
    tool_name = event_data.get("tool", "unknown")
    arguments = event_data.get("arguments", {})
    
    # 2. Safe bypass: Auto-approve any non-danger tools instantly
    if tool_name not in DANGER_TOOLS:
        sys.exit(0)

    # 3. Format a descriptive message based on the tool type
    msg = ""
    if tool_name == "run_command":
        cmd = arguments.get("CommandLine") or arguments.get("command") or event_data.get("command") or ""
        # Strip path or keep short
        msg = f"Run: {cmd[:25]}" if cmd else "Run command"
    elif tool_name in ("write_to_file", "replace_file_content", "multi_replace_file_content"):
        filepath = arguments.get("TargetFile") or arguments.get("path") or ""
        filename = Path(filepath).name if filepath else ""
        action = "Write" if tool_name == "write_to_file" else "Edit"
        msg = f"{action}: {filename[:25]}" if filename else f"{action} file"
    else:
        msg = f"Tool: {tool_name}"

    # 4. If the ESP32 is not reachable, fall through immediately (don't block)
    if not is_esp32_reachable():
        sys.exit(0)

    # 5. Write approval request
    claude_dir = Path(os.path.expanduser("~/.claude"))
    claude_dir.mkdir(parents=True, exist_ok=True)

    request_file = claude_dir / "approval_request.json"
    response_file = claude_dir / "approval_response.json"

    # Clean up any stale response file
    if response_file.exists():
        try:
            response_file.unlink()
        except OSError:
            pass

    request_data = {
        "state": "needs_approval",
        "message": msg,
        "timestamp": time.time()
    }
    request_file.write_text(json.dumps(request_data), encoding="utf-8")

    # 6. Wait for approval with timeout — default to APPROVE if no response
    start_time = time.time()
    approved = True  # safe default: allow if Clawdmeter doesn't respond in time

    try:
        while time.time() - start_time < RESPONSE_TIMEOUT:
            if response_file.exists():
                try:
                    resp = json.loads(response_file.read_text(encoding="utf-8"))
                    approved = resp.get("approved", True)
                    break
                except Exception:
                    pass
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        for f in (request_file, response_file):
            if f.exists():
                try:
                    f.unlink()
                except OSError:
                    pass

    # Exit code: 0 = Allow, 2 = Deny
    sys.exit(0 if approved else 2)


if __name__ == "__main__":
    main()
