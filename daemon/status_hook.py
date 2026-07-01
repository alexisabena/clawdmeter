import sys
import os
import json
import time
from pathlib import Path

def main():
    state = sys.argv[1] if len(sys.argv) > 1 else "idle"
    claude_dir = Path(os.path.expanduser("~/.claude"))
    claude_dir.mkdir(parents=True, exist_ok=True)
    request_file = claude_dir / "approval_request.json"
    
    if state == "idle":
        if request_file.exists():
            try:
                request_file.unlink()
            except OSError:
                pass
    else:
        request_data = {
            "state": "working",
            "message": "Thinking...",
            "timestamp": time.time()
        }
        request_file.write_text(json.dumps(request_data), encoding="utf-8")

if __name__ == "__main__":
    main()
