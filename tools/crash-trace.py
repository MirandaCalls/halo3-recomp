#!/usr/bin/env python3
"""Print the crashing thread's backtrace from a macOS crash report (.ips).

    tools/crash-trace.py                 # newest halo3 report
    tools/crash-trace.py path/to.ips     # a specific report
"""

import glob
import json
import os
import sys

REPORTS = os.path.expanduser("~/Library/Logs/DiagnosticReports")


def newest_report(name="halo3"):
    paths = glob.glob(os.path.join(REPORTS, f"{name}*.ips"))
    return max(paths, key=os.path.getmtime) if paths else None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else newest_report()
    if not path:
        sys.exit(f"no halo3 crash reports in {REPORTS}")
    with open(path) as f:
        f.readline()  # one-line JSON header; the report body follows
        report = json.load(f)

    images = report.get("usedImages", [])
    exc = report.get("exception", {})
    print(f"report:    {path}")
    print(f"exception: {exc.get('type', '?')} {exc.get('signal', '')} {report.get('termination', {}).get('indicator', '')}".rstrip())
    asi = report.get("asi")
    if asi:
        for lines in asi.values():
            for line in lines:
                print(f"message:   {line}")

    index = report.get("faultingThread", 0)
    thread = report["threads"][index]
    print(f"thread {index}: {thread.get('name') or thread.get('queue') or ''}")
    for n, frame in enumerate(thread.get("frames", [])[:40]):
        image = images[frame["imageIndex"]] if frame.get("imageIndex", -1) < len(images) else {}
        name = os.path.basename(image.get("name") or image.get("path") or "?")
        symbol = frame.get("symbol", f"+0x{frame.get('imageOffset', 0):x}")
        where = f" ({os.path.basename(frame['sourceFile'])}:{frame.get('sourceLine', '?')})" if "sourceFile" in frame else ""
        offset = f" + {frame['symbolLocation']}" if "symbolLocation" in frame else ""
        print(f"  {n:2} {name:28} {symbol}{offset}{where}")


if __name__ == "__main__":
    main()
