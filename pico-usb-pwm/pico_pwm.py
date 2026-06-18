#!/usr/bin/env python3

import argparse
import glob
import sys
import time

import serial


DEFAULT_BAUD = 115200


def auto_port():
    ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))

    if not ports:
        raise RuntimeError("Ingen seriell port hittad. Kontrollera att Pico är inkopplad.")

    return ports[0]


def open_serial(port, baud):
    ser = serial.Serial(
        port=port,
        baudrate=baud,
        timeout=1.0,
        write_timeout=1.0,
    )

    # Ge CDC-porten en kort chans att bli redo.
    time.sleep(0.2)

    # Töm gammal text.
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    return ser


def send_command(ser, command):
    if not command.endswith("\n"):
        command += "\n"

    ser.write(command.encode("ascii"))
    ser.flush()

    line = ser.readline().decode("utf-8", errors="replace").strip()
    return line


def main():
    parser = argparse.ArgumentParser(
        description="Control Raspberry Pi Pico PWM over USB CDC serial."
    )

    parser.add_argument(
        "--port",
        default=None,
        help="Serial port, e.g. /dev/ttyACM0. Default: auto-detect.",
    )

    parser.add_argument(
        "--baud",
        type=int,
        default=DEFAULT_BAUD,
        help="Baudrate. USB CDC ignores this mostly, default 115200.",
    )

    group = parser.add_mutually_exclusive_group(required=True)

    group.add_argument(
        "--ping",
        action="store_true",
        help="Ping Pico.",
    )

    group.add_argument(
        "--get",
        action="store_true",
        help="Read current PWM status.",
    )

    group.add_argument(
        "--set",
        type=int,
        metavar="VALUE",
        help="Set 16-bit PWM level, 0..65535.",
    )

    group.add_argument(
        "--set8",
        type=int,
        metavar="VALUE",
        help="Set 8-bit PWM value, 0..255.",
    )

    group.add_argument(
        "--percent",
        type=float,
        metavar="PERCENT",
        help="Set PWM duty percent, 0..100.",
    )

    group.add_argument(
        "--freq",
        type=int,
        metavar="HZ",
        help="Set PWM frequency in Hz.",
    )

    group.add_argument(
        "--ramp8",
        nargs=3,
        metavar=("START", "STOP", "STEP"),
        type=int,
        help="Ramp 8-bit PWM from START to STOP using STEP.",
    )

    parser.add_argument(
        "--delay",
        type=float,
        default=0.02,
        help="Delay between ramp steps in seconds. Default 0.02.",
    )

    args = parser.parse_args()

    try:
        port = args.port or auto_port()

        with open_serial(port, args.baud) as ser:
            if args.ping:
                print(send_command(ser, "PING"))
                return 0

            if args.get:
                print(send_command(ser, "GET"))
                return 0

            if args.set is not None:
                if args.set < 0 or args.set > 65535:
                    raise ValueError("--set must be 0..65535")
                print(send_command(ser, f"SET {args.set}"))
                return 0

            if args.set8 is not None:
                if args.set8 < 0 or args.set8 > 255:
                    raise ValueError("--set8 must be 0..255")
                print(send_command(ser, f"SET8 {args.set8}"))
                return 0

            if args.percent is not None:
                if args.percent < 0.0 or args.percent > 100.0:
                    raise ValueError("--percent must be 0..100")
                print(send_command(ser, f"PERCENT {args.percent}"))
                return 0

            if args.freq is not None:
                if args.freq < 1 or args.freq > 1000000:
                    raise ValueError("--freq must be 1..1000000")
                print(send_command(ser, f"FREQ {args.freq}"))
                return 0

            if args.ramp8 is not None:
                start, stop, step = args.ramp8

                if not (0 <= start <= 255 and 0 <= stop <= 255):
                    raise ValueError("START and STOP must be 0..255")

                if step == 0:
                    raise ValueError("STEP must not be 0")

                if start < stop and step < 0:
                    step = -step

                if start > stop and step > 0:
                    step = -step

                value = start

                while True:
                    response = send_command(ser, f"SET8 {value}")
                    print(response)
                    sys.stdout.flush()

                    if value == stop:
                        break

                    next_value = value + step

                    if step > 0 and next_value > stop:
                        next_value = stop

                    if step < 0 and next_value < stop:
                        next_value = stop

                    value = next_value
                    time.sleep(args.delay)

                return 0

        return 0

    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
