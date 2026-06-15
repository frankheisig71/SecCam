#!/usr/bin/env python3

from __future__ import annotations

import threading
import argparse
import logging
import signal
import sys
import configparser
import shutil
import time
from datetime import datetime
from queue import Queue, Full, Empty
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

DEFAULT_CONFIG_FILE = "/etc/seccam-receiver.conf"

DEFAULT_OUTPUT_DIR = "captures"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8080
DEFAULT_MAX_IMAGE_SIZE = 500000
DEFAULT_MAX_QUEUE_SIZE = 10
DEFAULT_STAGE_DIR = "/tmp/seccam-receiver-stage"
DEFAULT_BATCH_SIZE = 100
DEFAULT_BATCH_FLUSH_SECONDS = 2 * 60 * 60

LOGGER = logging.getLogger("pi_capture_receiver")
MAX_QUEUE = 10  # schützt RAM & Pi
WRITE_THREADS = 1  # Pi safe default
max_image_size = DEFAULT_MAX_IMAGE_SIZE

capture_queue: Queue
filename_lock = threading.Lock()
shutdown_event = threading.Event()
writer_threads: list[threading.Thread] = []
pending_lock = threading.Lock()
pending_batch: list[dict[str, Any]] = []
pending_flush_deadline: float | None = None

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


def next_capture_filename(capture_mode: str) -> str:
    with filename_lock:
        while True:
            now = datetime.now()
            prefix = "idle_" if capture_mode == "idle" else ""
            file_name = f"{prefix}CAM_{now:%y%m%d_%H%M%S}_{now.microsecond // 1000:03d}.jpg"
            return file_name

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
        "stage_dir": section.get("stage_dir", DEFAULT_STAGE_DIR),
        "host": section.get("host", DEFAULT_HOST),
        "port": section.getint("port", DEFAULT_PORT),
        "max_image_size": section.getint("max_image_size", DEFAULT_MAX_IMAGE_SIZE),
        "max_queue_size": section.getint("max_queue_size", DEFAULT_MAX_QUEUE_SIZE),
        "batch_size": section.getint("batch_size", DEFAULT_BATCH_SIZE),
        "batch_flush_seconds": section.getint("batch_flush_seconds", DEFAULT_BATCH_FLUSH_SECONDS),
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

    file_name = next_capture_filename(capture_mode)
    image_path = output_root / file_name
    metadata_path = output_root / file_name.replace(".jpg", ".json")
    metadata = {
        "file_name": file_name,
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


def write_staged_image(stage_dir: Path, file_name: str, payload: bytes) -> Path:
    stage_dir.mkdir(parents=True, exist_ok=True)
    staged_path = stage_dir / file_name
    tmp_path = staged_path.with_suffix(".part")
    tmp_path.write_bytes(payload)
    tmp_path.replace(staged_path)
    return staged_path


def flush_staged_batch(batch: list[dict[str, Any]]) -> None:
    if not batch:
        return

    total_bytes = 0
    for item in batch:
        staged_path = item["staged_path"]
        try:
            total_bytes += staged_path.stat().st_size
        except FileNotFoundError:
            LOGGER.warning("staged file missing before flush: %s", staged_path)

    flush_started = time.monotonic()
    LOGGER.info("flushing %d captures to NAS (%d bytes)", len(batch), total_bytes)

    for item in batch:
        staged_path = item["staged_path"]
        target_path = item["target_path"]

        target_path.parent.mkdir(parents=True, exist_ok=True)

        try:
            shutil.move(str(staged_path), str(target_path))
        except Exception:
            LOGGER.exception("flush failed for %s", staged_path)
            continue

        # Metadata sidecar output is intentionally disabled for now.
        # metadata["file_name"] = target_path.name
        # metadata["file_size"] = target_path.stat().st_size
        # tmp_meta = metadata_path.with_suffix(metadata_path.suffix + ".tmp")
        # tmp_meta.write_text(json.dumps(metadata), encoding="utf-8")
        # tmp_meta.replace(metadata_path)

    LOGGER.info(
        "flushed %d captures to NAS in %.3f s",
        len(batch),
        time.monotonic() - flush_started,
    )


def queue_staged_capture(item: dict[str, Any], batch_flush_seconds: int) -> int:
    global pending_flush_deadline

    with pending_lock:
        pending_batch.append(item)
        if len(pending_batch) == 1:
            pending_flush_deadline = time.monotonic() + batch_flush_seconds
        return len(pending_batch)


def take_pending_batch_if_due(now_monotonic: float) -> list[dict[str, Any]]:
    global pending_flush_deadline

    with pending_lock:
        if not pending_batch or pending_flush_deadline is None or now_monotonic < pending_flush_deadline:
            return []

        batch = pending_batch[:]
        pending_batch.clear()
        pending_flush_deadline = None
        return batch


def take_pending_batch_if_full(batch_size: int) -> list[dict[str, Any]]:
    global pending_flush_deadline

    with pending_lock:
        if len(pending_batch) < batch_size:
            return []

        batch = pending_batch[:]
        pending_batch.clear()
        pending_flush_deadline = None
        return batch


def take_pending_batch_on_shutdown() -> list[dict[str, Any]]:
    global pending_flush_deadline

    with pending_lock:
        if not pending_batch:
            return []

        batch = pending_batch[:]
        pending_batch.clear()
        pending_flush_deadline = None
        return batch


def writer_worker(queue, stage_dir: Path, batch_size: int, batch_flush_seconds: int):
    while True:
        should_exit = False
        try:
            item = queue.get(timeout=1)
        except Empty:
            due_batch = take_pending_batch_if_due(time.monotonic())
            if due_batch:
                flush_staged_batch(due_batch)

            if shutdown_event.is_set() and not pending_batch:
                return
            continue

        if item is None:
            due_batch = take_pending_batch_on_shutdown()
            if due_batch:
                flush_staged_batch(due_batch)
            should_exit = True
            item = None

        try:
            if item is not None:
                payload, target_path, _metadata_path, _metadata = item
                staged_path = write_staged_image(stage_dir, target_path.name, payload)
                pending_count = queue_staged_capture(
                    {
                        "staged_path": staged_path,
                        "target_path": target_path,
                    },
                    batch_flush_seconds,
                )

                if pending_count >= batch_size:
                    full_batch = take_pending_batch_if_full(batch_size)
                    if full_batch:
                        flush_staged_batch(full_batch)
        except Exception:
            LOGGER.exception("write failed")
        finally:
            queue.task_done()

        if should_exit:
            return


def batch_flush_watcher(batch_flush_seconds: int) -> None:
    while not shutdown_event.is_set() or pending_batch:
        due_batch = take_pending_batch_if_due(time.monotonic())
        if due_batch:
            flush_staged_batch(due_batch)
            continue

        time.sleep(min(5, batch_flush_seconds))

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
            capture_queue.put_nowait((payload, target_path, metadata_path, metadata))
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
    global writer_threads
    watcher_thread = None

    parser = argparse.ArgumentParser(description="Receive ESP captures ans store to dosk")
    parser.add_argument("--config", default=DEFAULT_CONFIG_FILE, help="configuration file")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--output-dir", default=None)
    parser.add_argument("--stage-dir", default=None)
    parser.add_argument("--batch-size", type=int, default=None)
    parser.add_argument("--batch-flush-seconds", type=int, default=None)
    args = parser.parse_args()

    cfg = load_config(args.config)
    host = args.host if args.host is not None else cfg.get("host", DEFAULT_HOST)
    port = args.port if args.port is not None else cfg.get("port", DEFAULT_PORT)
    output_dir = args.output_dir if args.output_dir is not None else cfg.get("output_dir", DEFAULT_OUTPUT_DIR)
    stage_dir = args.stage_dir if args.stage_dir is not None else cfg.get("stage_dir", DEFAULT_STAGE_DIR)
    if not output_dir:
        LOGGER.error("output_dir is not set (config or --output-dir required)")
        sys.exit(1)
    if not stage_dir:
        LOGGER.error("stage_dir is not set (config or --stage-dir required)")
        sys.exit(1)

    max_image_size = cfg.get("max_image_size", DEFAULT_MAX_IMAGE_SIZE)
    queue_size = cfg.get("max_queue_size",  DEFAULT_MAX_QUEUE_SIZE)
    batch_size = args.batch_size if args.batch_size is not None else cfg.get("batch_size", DEFAULT_BATCH_SIZE)
    batch_flush_seconds = (
        args.batch_flush_seconds
        if args.batch_flush_seconds is not None
        else cfg.get("batch_flush_seconds", DEFAULT_BATCH_FLUSH_SECONDS)
    )

    capture_queue = Queue(maxsize=queue_size)

    configure_logging(args.verbose)

    CaptureHandler.output_dir = Path(output_dir)
    CaptureHandler.max_image_size = max_image_size
    stage_path = Path(stage_dir)

    server = CaptureHTTPServer((host, port), CaptureHandler)
    install_signal_handlers(server)

    LOGGER.info("listening on http://%s:%d/api/v1/captures output_dir=%s",
                host,
                port,
                CaptureHandler.output_dir.resolve())
    LOGGER.info("staging captures in %s batch_size=%d batch_flush_seconds=%d",
                stage_path.resolve(),
                batch_size,
                batch_flush_seconds)
    try:
        for _ in range(WRITE_THREADS):
            t = threading.Thread(target=writer_worker, daemon=False, args=(capture_queue, stage_path, batch_size, batch_flush_seconds))
            t.start()
            writer_threads.append(t)
        watcher_thread = threading.Thread(target=batch_flush_watcher, daemon=False, args=(batch_flush_seconds,))
        watcher_thread.start()
        server.serve_forever()
    except KeyboardInterrupt:
        LOGGER.info("keyboard interrupt, shutting down")
    finally:
        shutdown_event.set()
        for _ in writer_threads:
            capture_queue.put(None)
        for thread in writer_threads:
            thread.join(timeout=10)
        if watcher_thread is not None:
            watcher_thread.join(timeout=10)
        remaining_batch = take_pending_batch_on_shutdown()
        if remaining_batch:
            flush_staged_batch(remaining_batch)
        server.server_close()
        LOGGER.info("server stopped")


if __name__ == "__main__":
    main()