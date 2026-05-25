from __future__ import annotations

import argparse
import json
import re
import time
from collections import Counter
from dataclasses import dataclass
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock, Thread
from typing import Any

import serial


COLOUR_MAP = {
    "R": "red",
    "G": "green",
    "B": "blue",
    "U": "unknown",
}

COMMANDS = {
    "unknown": "0x00",
    "red": "0x01",
    "green": "0x02",
    "blue": "0x03",
}

ANGLES = {
    "unknown": 180,
    "red": 0,
    "green": 60,
    "blue": 120,
}

RX_RE = re.compile(r"RX color=([RGBU])\s+seq=(\d+)\s+rssi=(-?\d+)")
MOTOR_RE = re.compile(r"MOTOR COMMAND:\s+([A-Z/]+).*angle=(\d+)")


@dataclass
class Event:
    timestamp: str
    colour: str
    command: str
    angle: int
    sequence: str
    rssi: str
    line: str


class State:
    def __init__(self) -> None:
        self.lock = Lock()
        self.current_colour = "unknown"
        self.current_command = "0x00"
        self.current_angle = 90
        self.serial_status = "starting"
        self.last_sequence = "-"
        self.last_rssi = "-"
        self.last_line = ""
        self.counts: Counter = Counter()
        self.events: list[Event] = []

    def set_serial_status(self, status: str) -> None:
        with self.lock:
            self.serial_status = status

    def record_line(self, line: str) -> None:
        line = line.strip()
        if not line:
            return

        with self.lock:
            self.last_line = line

        match = RX_RE.search(line)
        if not match:
            return

        colour = COLOUR_MAP[match.group(1)]
        sequence = match.group(2)
        rssi = match.group(3)
        event = Event(
            timestamp=time.strftime("%H:%M:%S"),
            colour=colour,
            command=COMMANDS[colour],
            angle=ANGLES[colour],
            sequence=sequence,
            rssi=rssi,
            line=line,
        )

        with self.lock:
            self.current_colour = colour
            self.current_command = COMMANDS[colour]
            self.current_angle = ANGLES[colour]
            self.last_sequence = sequence
            self.last_rssi = rssi
            self.counts[colour] += 1
            self.events.append(event)
            self.events = self.events[-50:]

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return {
                "serial_status": self.serial_status,
                "current_colour": self.current_colour,
                "current_command": self.current_command,
                "current_angle": self.current_angle,
                "last_sequence": self.last_sequence,
                "last_rssi": self.last_rssi,
                "last_line": self.last_line,
                "counts": {name: int(self.counts[name]) for name in COMMANDS},
                "events": [event.__dict__ for event in self.events],
            }


def read_serial(port: str, baudrate: int, state: State) -> None:
    while True:
        try:
            state.set_serial_status(f"opening {port}")
            with serial.Serial(port, baudrate, timeout=1) as ser:
                state.set_serial_status(f"connected {port}")
                while True:
                    raw = ser.readline()
                    if raw:
                        state.record_line(raw.decode("utf-8", errors="replace"))
        except serial.SerialException as error:
            state.set_serial_status(f"serial error: {error}")
            time.sleep(1)


def serve(host: str, port: int, state: State, root: Path) -> ThreadingHTTPServer:
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args: Any, **kwargs: Any) -> None:
            super().__init__(*args, directory=str(root), **kwargs)

        def do_GET(self) -> None:
            if self.path == "/api/status":
                self.write_json(state.snapshot())
                return
            if self.path == "/":
                self.path = "/dashboard.html"
            super().do_GET()

        def write_json(self, payload: dict[str, Any]) -> None:
            data = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, format: str, *args: Any) -> None:
            return

    server = ThreadingHTTPServer((host, port), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    return server


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Receiver XIAO serial dashboard")
    parser.add_argument("--serial-port", required=True, help="Example: /dev/cu.usbmodem14101")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    state = State()
    root = Path(__file__).resolve().parent

    Thread(target=read_serial, args=(args.serial_port, args.baudrate, state), daemon=True).start()
    server = serve(args.host, args.port, state, root)
    print(f"Dashboard: http://{args.host}:{args.port}/")
    print(f"Reading serial: {args.serial_port} @ {args.baudrate}")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
