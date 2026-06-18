#!/usr/bin/env python3
import argparse
import json
import select
import sys
import time

import cv2
import numpy as np
import zmq


def parse_args():
    parser = argparse.ArgumentParser(
        description="Receive JPEG images from image_tcp_bridge and send save commands."
    )
    parser.add_argument("--host", default="10.42.0.130", help="device host/IP")
    parser.add_argument("--image-port", type=int, default=5560)
    parser.add_argument("--control-port", type=int, default=5561)
    parser.add_argument("--topic", default="", help="topic prefix, e.g. cam_left")
    parser.add_argument("--no-window", action="store_true", help="print stats only")
    parser.add_argument("--timeout-ms", type=int, default=1000)
    return parser.parse_args()


def send_save_command(req):
    req.send_string("save")
    try:
        reply = req.recv_string()
    except zmq.error.ZMQError as exc:
        print(f"[control] failed: {exc}")
        return
    print(f"[control] {reply}")


def terminal_key_pressed():
    if not sys.stdin.isatty():
        return ""
    readable, _, _ = select.select([sys.stdin], [], [], 0)
    if not readable:
        return ""
    return sys.stdin.readline().strip()


def main():
    args = parse_args()
    context = zmq.Context.instance()

    sub = context.socket(zmq.SUB)
    sub.setsockopt(zmq.RCVHWM, 5)
    sub.setsockopt(zmq.SUBSCRIBE, args.topic.encode("utf-8"))
    sub.connect(f"tcp://{args.host}:{args.image_port}")

    req = context.socket(zmq.REQ)
    req.setsockopt(zmq.LINGER, 0)
    req.setsockopt(zmq.RCVTIMEO, args.timeout_ms)
    req.setsockopt(zmq.SNDTIMEO, args.timeout_ms)
    req.connect(f"tcp://{args.host}:{args.control_port}")

    poller = zmq.Poller()
    poller.register(sub, zmq.POLLIN)

    print(
        f"subscribed tcp://{args.host}:{args.image_port}, "
        f"control tcp://{args.host}:{args.control_port}"
    )
    print("press 's' in the image window, or type 's' then Enter in terminal")

    count = 0
    last_count = 0
    last_print = time.monotonic()
    latest = {}

    try:
        while True:
            events = dict(poller.poll(20))
            if sub in events:
                topic_b, header_b, jpeg_b = sub.recv_multipart()
                topic = topic_b.decode("utf-8", errors="replace")
                try:
                    header = json.loads(header_b.decode("utf-8"))
                except json.JSONDecodeError:
                    header = {"stream": topic}

                arr = np.frombuffer(jpeg_b, dtype=np.uint8)
                image = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                if image is None:
                    print(f"[warn] failed to decode image topic={topic}")
                    continue

                count += 1
                stream = header.get("stream", topic)
                latest[stream] = (header, image)

                if not args.no_window:
                    cv2.imshow(stream, image)
                    key = cv2.waitKey(1) & 0xFF
                    if key == ord("s"):
                        send_save_command(req)
                    elif key == 27 or key == ord("q"):
                        break

            line = terminal_key_pressed()
            if line == "s":
                send_save_command(req)
            elif line in ("q", "quit", "exit"):
                break

            now = time.monotonic()
            if now - last_print >= 1.0:
                cur = count - last_count
                desc = []
                for stream, (header, image) in sorted(latest.items()):
                    ts = f"{header.get('sec', 0)}.{int(header.get('nsec', 0)):09d}"
                    desc.append(
                        f"{stream} {image.shape[1]}x{image.shape[0]} ts={ts}"
                    )
                print(f"[stat] total={count} cur={cur / (now - last_print):.2f}Hz " + " | ".join(desc))
                last_count = count
                last_print = now
    finally:
        if not args.no_window:
            cv2.destroyAllWindows()
        sub.close(0)
        req.close(0)


if __name__ == "__main__":
    main()
