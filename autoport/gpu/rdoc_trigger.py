# qrenderdoc --python: connect to a running capture target and trigger a capture after a delay.
# Env: RDOC_LOG=<log file>, RDOC_DELAY=<seconds before triggering>
import os
import time
import traceback

import renderdoc as rd

out = open(os.environ["RDOC_LOG"], "w")


def p(*a):
    print(*a, file=out)
    out.flush()


try:
    time.sleep(float(os.environ.get("RDOC_DELAY", "60")))
    ident = rd.EnumerateRemoteTargets("localhost", 0)
    p("target ident", ident)
    tc = rd.CreateTargetControl("localhost", ident, "autoport", True)
    p("connected", tc.GetTarget() if tc else None)
    tc.TriggerCapture(1)
    t0 = time.time()
    while time.time() - t0 < 60:
        msg = tc.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            p("capture", msg.newCapture.path)
            break
        time.sleep(0.2)
    tc.Shutdown()
except Exception:
    p(traceback.format_exc())
os._exit(0)
