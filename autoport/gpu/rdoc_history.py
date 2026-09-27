import os, traceback
import renderdoc as rd
out = open(os.path.join(os.environ["RDOC_OUT"], "history.txt"), "w")
def p(*a): print(*a, file=out); out.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    eid = int(os.environ["RDOC_EIDS"])
    ctrl.SetFrameEvent(eid, True)
    ps = ctrl.GetPipelineState()
    target = [o.resource for o in ps.GetOutputTargets() if o.resource != rd.ResourceId.Null()][0]
    last = ctrl.GetRootActions()[-1]
    while last.children: last = last.children[-1]
    for (x, y) in [(300, 360), (100, 100), (600, 700)]:
        hist = ctrl.PixelHistory(target, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        p(f"pixel ({x},{y}) on {target}: {len(hist)} modifications")
        for m in hist[:12]:
            so = m.shaderOut.col.floatValue; po = m.postMod.col.floatValue
            p(f"  EID {m.eventId} passed={m.Passed()} shaderOut={[round(v,3) for v in so]} post={[round(v,3) for v in po]} "
              f"flags: scissor={m.scissorClipped} depth={m.depthTestFailed} backface={m.backfaceCulled} unbound={m.unboundPS}")
    # debug one pixel of the bink draw
    trace = ctrl.DebugPixel(300, 360, rd.DebugPixelInputs())
    p(f"debug trace: {'ok' if trace and trace.debugger else 'unavailable'}")
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
