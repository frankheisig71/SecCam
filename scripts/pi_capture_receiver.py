#!/usr/bin/env python3

from __future__ import annotations

import threading
import time
import argparse
import json
import logging
import signal
import sys
import configparser
from queue import Queue, Full, Empty
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from socketserver import ThreadingMixIn
from typing import Any

DEFAULT_CONFIG_FILE = "/etc/seccam-receiver.conf"

DEFAULT_OUTPUT_DIR = "captures"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8080
DEFAULT_MAX_IMAGE_SIZE = 500000
DEFAULT_MAX_QUEUE_SIZE = 10

LOGGER = logging.getLogger("pi_capture_receiver")
MAX_QUEUE = 10  # schützt RAM & Pi
WRITE_THREADS = 1  # Pi safe default
max_image_size = DEFAULT_MAX_IMAGE_SIZE

capture_queue: Queue

def sanitize_path_component(value: str, fallback: str) -> str:
    sanitized = "".join(char if char.isalnum() or char in ("-", "_", ".") else "_" for char in value.strip())
    sanitized = sanitized.strip("._")
    return sanitized or fallback


def header_value(headers, name: str, default: str) -> str:
    value = headers.get(name)
    return value.strip() if value else default


def integer_header(headers, name: str, default: int) -> int:
    raw_value = header_value(headers, name, str(default))
    try:
        return int(raw_value)
    except ValueError:
        return default

def load_config(path: str) -> dict:
    cfg = configparser.ConfigParser()

    if not Path(path).exists():
        LOGGER.warning("config file not found: %s", path)
        return {}

    cfg.read(path)

    if "receiver" not in cfg:
        return {}

    section = cfg["receiver"]

    return {
        "output_dir": section.get("output_dir", DEFAULT_OUTPUT_DIR),
        "host": section.get("host", DEFAULT_HOST),
        "port": section.getint("port", DEFAULT_PORT),
        "max_image_size": section.getint("max_image_size", DEFAULT_MAX_IMAGE_SIZE),
        "max_queue_size": section.getint("max_queue_size", DEFAULT_MAX_QUEUE_SIZE),
    }

def build_capture_paths(output_root: Path, headers) -> tuple[Path, Path, dict[str, Any]]:
    device_id = sanitize_path_component(header_value(headers, "X-Device-Id", "unknown-device"), "unknown-device")
    capture_mode = sanitize_path_component(header_value(headers, "X-Capture-Mode", "unknown"), "unknown")
    capture_reason = sanitize_path_component(header_value(headers, "X-Capture-Reason", "unknown"), "unknown")
    captured_at_us = integer_header(headers, "X-Captured-At-Us", 0)
    image_width = integer_header(headers, "X-Image-Width", 0)
    image_height = integer_header(headers, "X-Image-Height", 0)
    sequence_index = integer_header(headers, "X-Sequence-Index", 1)
    sequence_size = integer_header(headers, "X-Sequence-Size", 1)

    day_bucket = f"{captured_at_us // 1000000 // 86400:05d}" if captured_at_us > 0 else "unsorted"
    target_dir = output_root / device_id / capture_mode / day_bucket
    file_stem = (
        f"{device_id}_{capture_mode}_{capture_reason}_{captured_at_us}_"
        f"{image_width}x{image_height}_s{sequence_index}of{sequence_size}"
    )
    image_path = target_dir / f"{file_stem}.jpg"
    metadata_path = target_dir / f"{file_stem}.json"
    metadata = {
        "device_id": device_id,
        "capture_mode": capture_mode,
        "capture_reason": capture_reason,
        "captured_at_us": captured_at_us,
        "image_width": image_width,
        "image_height": image_height,
        "sequence_index": sequence_index,
        "sequence_size": sequence_size,
        "content_type": header_value(headers, "Content-Type", "application/octet-stream"),
    }
    return image_path, metadata_path, metadata


def writer_worker(queue):
    while True:
        try:
            item = queue.get(timeout=1)
        except Empty:
            continue

        try:
            payload, target_path, metadata = item

            target_path.parent.mkdir(parents=True, exist_ok=True)

            tmp_img = target_path.with_suffix(".tmp")
            tmp_img.write_bytes(payload)
            tmp_img.replace(target_path)

            metadata["file_name"] = target_path.name
            metadata["file_size"] = len(payload)

            tmp_meta = target_path.with_suffix(".json.tmp")
            tmp_meta.write_text(json.dumps(metadata), encoding="utf-8")
            tmp_meta.replace(target_path.with_suffix(".json"))

        except Exception as e:
            LOGGER.exception("write failed: %s", e)

        finally:
            queue.task_done()

class CaptureHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class CaptureHandler(BaseHTTPRequestHandler):
    output_dir = Path("captures")
    max_image_size = DEFAULT_MAX_IMAGE_SIZE
    protocol_version = "HTTP/1.0"
        
    def setup(self):
        super().setup()
        self.request.settimeout(30)

    def do_POST(self) -> None:
        if self.path != "/api/v1/captures":
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        if content_length <= 0:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing image body")
            return

        if content_length > self.max_image_size:
            self.send_error(413, "file too large")
            return

        payload = self.rfile.read(content_length)
        target_path, metadata_path, metadata = build_capture_paths(self.output_dir, self.headers)

        try:
            capture_queue.put_nowait((payload, target_path, metadata))
        except Full:
            self.send_error(HTTPStatus.SERVICE_UNAVAILABLE, "server overloaded")
            return

        self.send_response(HTTPStatus.ACCEPTED)
        self.end_headers()
        self.wfile.write(b"queued\n")


def configure_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )


def install_signal_handlers(server: CaptureHTTPServer) -> None:
    def _shutdown_handler(signum, _frame) -> None:
        LOGGER.info("received signal %s, shutting down", signum)
        server.shutdown()

    signal.signal(signal.SIGTERM, _shutdown_handler)
    signal.signal(signal.SIGINT, _shutdown_handler)


def main() -> None:

    global capture_queue
    global max_image_size

    parser = argparse.ArgumentParser(description="Receive ESP captures ans store to dosk")
    parser.add_argument("--config", default=DEFAULT_CONFIG_FILE, help="configuration file")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--output-dir", default=None)
    args = parser.parse_args()

    cfg = load_config(args.config)
    host = cfg.get("host", args.host)
    port = cfg.get("port", args.port)
    output_dir = cfg.get("output_dir", args.output_dir)
    if not output_dir:
        LOGGER.error("output_dir is not set (config or --output-dir required)")
        sys.exit(1)

    max_image_size = cfg.get("max_image_size", DEFAULT_MAX_IMAGE_SIZE)
    queue_size = cfg.get("max_queue_size",  DEFAULT_MAX_QUEUE_SIZE)

    capture_queue = Queue(maxsize=queue_size)

    configure_logging(args.verbose)

    CaptureHandler.output_dir = Path(output_dir)
    CaptureHandler.max_image_size = max_image_size

    server = CaptureHTTPServer((host, port), CaptureHandler)
    install_signal_handlers(server)

    LOGGER.info("listening on http://%s:%d/api/v1/captures output_dir=%s",
                host,
                port,
                CaptureHandler.output_dir.resolve())
    try:
        for _ in range(WRITE_THREADS):
            t = threading.Thread(target=writer_worker, daemon=True, args=(capture_queue,))
            t.start()
        server.serve_forever()
    except KeyboardInterrupt:
        LOGGER.info("keyboard interrupt, shutting down")
    finally:
        server.server_close()
        LOGGER.info("server stopped")


if __name__ == "__main__":
    main()