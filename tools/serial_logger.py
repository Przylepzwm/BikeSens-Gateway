#!/usr/bin/env python3
import argparse
import os
import signal
import sys
import termios
import time
from datetime import datetime


def configure_port(fd: int, baud: int) -> None:
    baud_map = {
        9600: termios.B9600,
        19200: termios.B19200,
        38400: termios.B38400,
        57600: termios.B57600,
        115200: termios.B115200,
        230400: termios.B230400,
    }
    if baud not in baud_map:
        raise ValueError(f"Unsupported baud: {baud}")

    attrs = termios.tcgetattr(fd)
    attrs[0] = termios.IGNPAR
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = baud_map[baud]
    attrs[5] = baud_map[baud]
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcflush(fd, termios.TCIFLUSH)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def is_mostly_printable(text: str) -> bool:
    if not text:
        return False
    printable = sum(1 for ch in text if ch == "\t" or ch == "\r" or ch == "\n" or 32 <= ord(ch) <= 126)
    return printable / max(len(text), 1) >= 0.8


def main() -> int:
    parser = argparse.ArgumentParser(description="Record ESP32 serial logs to a file.")
    parser.add_argument("port", help="Serial port, e.g. /dev/cu.usbserial-0001")
    parser.add_argument("output", help="Output log file")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument(
        "--raw",
        action="store_true",
        help="Do not filter gibberish lines; write everything that decodes.",
    )
    args = parser.parse_args()

    stop = False

    def handle_sigint(signum, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_sigint)

    fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY)
    try:
        configure_port(fd, args.baud)
        with open(args.output, "a", encoding="utf-8") as out:
            sys.stderr.write(f"Listening on {args.port} at {args.baud}, saving to {args.output}\n")
            sys.stderr.flush()
            buf = ""
            while not stop:
                data = os.read(fd, 256)
                if not data:
                    time.sleep(0.05)
                    continue
                chunk = data.decode("utf-8", errors="replace")
                buf += chunk
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.rstrip("\r")
                    if not args.raw and line and not is_mostly_printable(line):
                        continue
                    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    record = f"{ts} {line}\n"
                    sys.stdout.write(record)
                    sys.stdout.flush()
                    out.write(record)
                    out.flush()
    finally:
        os.close(fd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
