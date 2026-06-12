#!/usr/bin/env python3

from __future__ import annotations

import argparse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def header_value(headers, name: str, default: str) -> str:
    value = headers.get(name)
    return value.strip() if value else default


class CaptureHandler(BaseHTTPRequestHandler):
    output_dir = Path("captures")

    def do_POST(self) -> None:
        if self.path != "/api/v1/captures":
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        if content_length <= 0:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing image body")
            return

        payload = self.rfile.read(content_length)
        device_id = header_value(self.headers, "X-Device-Id", "unknown-device")
        capture_mode = header_value(self.headers, "X-Capture-Mode", "unknown")
        captured_at_us = header_value(self.headers, "X-Captured-At-Us", "0")
        sequence_index = header_value(self.headers, "X-Sequence-Index", "1")
        sequence_size = header_value(self.headers, "X-Sequence-Size", "1")

        file_name = f"{device_id}_{capture_mode}_{captured_at_us}_s{sequence_index}of{sequence_size}.jpg"
        target_path = self.output_dir / file_name
        target_path.parent.mkdir(parents=True, exist_ok=True)
        target_path.write_bytes(payload)

        self.send_response(HTTPStatus.CREATED)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.end_headers()
        self.wfile.write(f"stored {target_path.name}\n".encode("utf-8"))

    def log_message(self, format: str, *args) -> None:
        return


def main() -> None:
    parser = argparse.ArgumentParser(description="Receive ESP32 training captures and store them on disk.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--output-dir", default="captures")
    args = parser.parse_args()

    CaptureHandler.output_dir = Path(args.output_dir)
    server = ThreadingHTTPServer((args.host, args.port), CaptureHandler)
    print(f"Listening on http://{args.host}:{args.port}/api/v1/captures")
    server.serve_forever()


if __name__ == "__main__":
    main()