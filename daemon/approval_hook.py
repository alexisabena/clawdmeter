import sys
import os
import json
import time
import socket
from pathlib import Path

ESP32_IP_FILE = Path(os.path.expanduser("~/.claude")) / "clawdmeter_ip.txt"
WHITELIST_FILE = Path(os.path.expanduser("~/.claude")) / "always_allowed_tools.json"
RESPONSE_TIMEOUT = 10.0  # seconds to wait for approval from Clawdmeter

# Only prompt for these dangerous tools in Claude Code.
# All other tools (like Grep, Glob, View) are auto-approved instantly.
DANGER_TOOLS = {
    "Bash",
    "Write",
    "Edit",
}


def load_whitelist() -> set:
    """Load the set of always-allowed tools from disk."""
    if not WHITELIST_FILE.exists():
        return set()
    try:
        data = json.loads(WHITELIST_FILE.read_text(encoding="utf-8"))
        if isinstance(data, list):
            return set(data)
    except Exception:
        pass
    return set()


def add_to_whitelist(tool_name: str):
    """Add a tool to the always-allowed list on disk."""
    whitelist = load_whitelist()
    whitelist.add(tool_name)
    try:
        WHITELIST_FILE.write_text(json.dumps(list(whitelist)), encoding="utf-8")
    except Exception:
        pass


def is_esp32_reachable(ip: str) -> bool:
    """Quick TCP check to see if the ESP32 HTTP server is up."""
    try:
        with socket.create_connection((ip, 80), timeout=0.8):
            return True
    except Exception:
        return False


def poll_esp32_approval(ip: str) -> str:
    """Poll the ESP32 endpoint /api/approval for user confirmation."""
    import urllib.request
    url = f"http://{ip}/api/approval"
    try:
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=0.8) as response:
            if response.status == 200:
                data = json.loads(response.read().decode("utf-8"))
                return data.get("status", "pending")
    except Exception:
        pass
    return "pending"


def main():
    # 1. Read JSON event context from stdin
    raw_input = ""
    try:
        raw_input = sys.stdin.read().strip()
        event_data = json.loads(raw_input) if raw_input else {}
    except Exception:
        event_data = {}

    # Extract tool name using Claude Code's exact keys
    tool_name = event_data.get("tool_name", "unknown")
    tool_input = event_data.get("tool_input", {})
    
    # Check whitelist first (D-09 bypass)
    whitelist = load_whitelist()
    if tool_name in whitelist:
        sys.exit(0)

    # 2. Safe bypass: Auto-approve any non-danger tools instantly
    if tool_name not in DANGER_TOOLS:
        sys.exit(0)

    # 3. Format a descriptive message based on the tool type
    msg = ""
    if tool_name == "Bash":
        cmd = tool_input.get("command", "")
        cmd_clean = cmd.strip()
        msg = f"Run: {cmd_clean[:25]}" if cmd_clean else "Run command"
    elif tool_name in ("Write", "Edit"):
        filepath = tool_input.get("path", "")
        filename = Path(filepath).name if filepath else ""
        action = "Write" if tool_name == "Write" else "Edit"
        msg = f"{action}: {filename[:25]}" if filename else f"{action} file"
    else:
        msg = f"Tool: {tool_name}"

    # Get active ESP32 IP
    esp32_ip = ""
    try:
        esp32_ip = ESP32_IP_FILE.read_text(encoding="utf-8").strip()
    except Exception:
        pass

    # 4. If the ESP32 is not reachable/unknown, fall through immediately (don't block)
    if not esp32_ip or not is_esp32_reachable(esp32_ip):
        sys.exit(0)

    # 5. Write approval request so the daemon can read and display it
    claude_dir = Path(os.path.expanduser("~/.claude"))
    claude_dir.mkdir(parents=True, exist_ok=True)
    request_file = claude_dir / "approval_request.json"

    request_data = {
        "state": "needs_approval",
        "message": msg,
        "timestamp": time.time()
    }
    request_file.write_text(json.dumps(request_data), encoding="utf-8")

    # 6. Poll the ESP32 for approval status
    start_time = time.time()
    approved = True  # safe default: allow if Clawdmeter doesn't respond in time

    try:
        while time.time() - start_time < RESPONSE_TIMEOUT:
            status = poll_esp32_approval(esp32_ip)
            if status == "allow_once":
                approved = True
                break
            elif status == "always_allow":
                add_to_whitelist(tool_name)
                approved = True
                break
            elif status == "deny":
                approved = False
                break
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        # Cleanup request file
        if request_file.exists():
            try:
                request_file.unlink()
            except OSError:
                pass

    # Exit code: 0 = Allow, 2 = Deny
    sys.exit(0 if approved else 2)


if __name__ == "__main__":
    main()
