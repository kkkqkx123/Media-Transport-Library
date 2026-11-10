#!/usr/bin/env python3
"""dvs_ipc_probe: minimal dvs_media IPC client (stdlib only).

Usage:
  dvs_ipc_probe.py --sock PATH --type hello|capability|query_state|<verb>
                   [--body JSON] [--request-id ID] [--timeout 3]

Sends one JSON line {"type":..,"requestId":..} plus body fields, reads one
response line, asserts the envelope carries ok/status/code/msg/data/requestId.
Exit 0 on ok:true, 2 on ok:false (honest rejection, prints envelope),
1 on transport/parse failure. No third-party deps; board-runnable.
"""
import argparse
import json
import socket
import sys


def parse_args():
    p = argparse.ArgumentParser(description="probe dvs_media IPC socket")
    p.add_argument("--sock", required=True, help="unix socket path")
    p.add_argument("--type", required=True, help="request type verb")
    p.add_argument("--body", default="", help="extra JSON object merged top-level")
    p.add_argument("--request-id", default="probe-1", help="requestId to send")
    p.add_argument("--timeout", type=float, default=3.0, help="socket timeout seconds")
    return p.parse_args()


def main():
    a = parse_args()
    req = {"type": a.type, "requestId": a.request_id}
    if a.body:
        try:
            extra = json.loads(a.body)
        except json.JSONDecodeError as e:
            print(f"FAIL: bad --body json: {e}", file=sys.stderr)
            return 1
        if not isinstance(extra, dict):
            print("FAIL: --body must be a JSON object", file=sys.stderr)
            return 1
        req.update(extra)
    line = json.dumps(req) + "\n"
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(a.timeout)
        s.connect(a.sock)
        s.sendall(line.encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            if len(buf) > 1 << 20:
                break
        s.close()
    except FileNotFoundError:
        print(f"FAIL: socket not found: {a.sock}", file=sys.stderr)
        return 1
    except ConnectionRefusedError:
        print(f"FAIL: connection refused: {a.sock}", file=sys.stderr)
        return 1
    except socket.timeout:
        print("FAIL: recv timeout", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"FAIL: socket error: {e}", file=sys.stderr)
        return 1
    if not buf:
        print("FAIL: empty response", file=sys.stderr)
        return 1
    try:
        env = json.loads(buf.decode().strip().split("\n")[0])
    except json.JSONDecodeError as e:
        print(f"FAIL: malformed json response: {e}", file=sys.stderr)
        return 1
    for k in ("ok", "status", "code", "msg", "data", "requestId"):
        if k not in env:
            print(f"FAIL: envelope missing field: {k}", file=sys.stderr)
            print(json.dumps(env, ensure_ascii=False)[:2000])
            return 1
    print(json.dumps(env, ensure_ascii=False)[:4000])
    return 0 if env.get("ok") is True else 2


if __name__ == "__main__":
    sys.exit(main())
