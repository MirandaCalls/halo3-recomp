#!/usr/bin/env python3
"""Find Halo 3 instances reachable over ZeroTier (or any LAN).

Every running Halo 3 answers XNet QoS probes on UDP port
net_port_offset + 1023 (37023 by default; see the net_port_offset cvar),
whether or not it is listening for your session. So a probe is both a
reachability check and a "who is on the system link" discovery.

Usage:
  zt-probe.py                 broadcast probe on this PC's ZeroTier links
  zt-probe.py 10.9.94.74      unicast probe to one peer
  zt-probe.py --port 37023 10.9.94.74
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time

MAGIC = 0x58514F53  # 'XQOS'
PROBE = 1
REPLY = 2
HEADER = struct.Struct("<IBBH8sI")


def zt_interfaces():
    """[(ifname, address, broadcast)] for every zt* interface with IPv4."""
    try:
        out = subprocess.run(["ip", "-j", "-4", "addr", "show"],
                             capture_output=True, text=True, check=True).stdout
        links = json.loads(out or "[]")
    except Exception:
        return []
    found = []
    for link in links:
        name = link.get("ifname", "")
        if not name.startswith("zt"):
            continue
        for addr in link.get("addr_info", []):
            if addr.get("family") != "inet":
                continue
            local, prefix = addr.get("local"), addr.get("prefixlen", 24)
            bcast = addr.get("broadcast")
            if not bcast:
                octets = [int(b) for b in local.split(".")]
                ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3]
                mask = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF
                b = (ip & mask) | (~mask & 0xFFFFFFFF)
                bcast = ".".join(str((b >> s) & 0xFF) for s in (24, 16, 8, 0))
            found.append((name, local, bcast))
    return found


def probe(target, port, timeout):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", 0))
    sock.settimeout(0.2)
    packet = HEADER.pack(MAGIC, PROBE, 0, 0, os.urandom(8), int(time.time()) & 0xFFFFFFFF)
    sent_at = time.monotonic()
    sock.sendto(packet, (target, port))
    replies = {}
    deadline = sent_at + timeout
    while time.monotonic() < deadline:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < HEADER.size:
            continue
        magic, kind, enabled, data_len, _xnkid, _nonce = HEADER.unpack_from(data)
        if magic != MAGIC or kind != REPLY:
            continue
        rtt = (time.monotonic() - sent_at) * 1000
        replies[addr[0]] = (bool(enabled), data_len, rtt)
    sock.close()
    return replies


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("peer", nargs="?", help="peer IPv4 (default: broadcast on zt links)")
    parser.add_argument("--port", type=int, default=37023,
                        help="QoS port, net_port_offset + 1023 (default 37023)")
    parser.add_argument("--timeout", type=float, default=2.0, help="reply wait in seconds")
    args = parser.parse_args()

    if args.peer:
        targets = [(None, None, args.peer)]
    else:
        targets = zt_interfaces()
        if not targets:
            print("no ZeroTier IPv4 interface found (is ZeroTier joined?)", file=sys.stderr)
            return 1

    any_reply = False
    for ifname, local, bcast in targets:
        label = f"{bcast}:{args.port}" + (f" via {ifname} ({local})" if ifname else "")
        print(f"probe {label}")
        for ip, (enabled, data_len, rtt) in probe(bcast, args.port, args.timeout).items():
            any_reply = True
            state = f"listening ({data_len} bytes)" if enabled else "not listening"
            print(f"  {ip:<15} Halo 3 running, {state}, rtt {rtt:.1f} ms")
    if not any_reply:
        print("no Halo 3 answered (game not running, not at the multiplayer menus,\n"
              "or the link/firewall blocks UDP {})".format(args.port), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
