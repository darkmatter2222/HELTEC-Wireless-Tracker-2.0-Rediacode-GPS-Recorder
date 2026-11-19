"""Paced TCP sink that emulates the LTE-hotspot ingest endpoint.

Accepts one HTTP POST, drains the body at a fixed byte/sec pace, then
answers 200 with a tiny JSON body. The pacing is what reproduces the
field condition: the body transfer alone takes ~N/pace seconds, so a
60s task-watchdog window is crossed inside HTTPClient's write loop.

usage: python scripts/sink_server.py [port] [bytes_per_sec]
"""
import json
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
PACE = int(sys.argv[2]) if len(sys.argv) > 2 else 500


def handle(conn):
    conn.settimeout(120)
    # One accept per connection, but the connection may carry several
    # keep-alive requests (the firmware POSTs chunked bodies there).
    carry = b""
    while True:
        buf = carry
        carry = b""
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                conn.close()
                return
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        content_length = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                content_length = int(line.split(b":")[1].strip())
        body = bytearray(rest)
        t0 = time.time()
        while len(body) < content_length:
            want = max(1, min(1400, PACE))
            chunk = conn.recv(want)
            if not chunk:
                break
            body += chunk
            time.sleep(max(0.0, want / PACE))
        got = len(body)
        dt = time.time() - t0
        print(json.dumps({
            "event": "complete",
            "content_length": content_length,
            "got_bytes": got,
            "recv_seconds": round(dt, 2),
            "kbps": round(content_length * 8 / max(dt, 0.001) / 1000, 2),
        }), flush=True)
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     b"Content-Length: 13\r\n\r\n{\"ok\":true}\n\n")
        # Any bytes beyond this request's body belong to the next one.
        carry = bytes(body[content_length:])


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(4)
    print(json.dumps({"event": "listening", "port": PORT, "pace_bps": PACE}),
          flush=True)
    while True:
        conn, _ = srv.accept()
        handle(conn)


if __name__ == "__main__":
    main()
