# Pixel history for one pixel of one target. Env: RDOC_TARGET_EID (a draw writing the target), RDOC_XY="x,y"
import os, traceback
import renderdoc as rd
OUT = os.environ["RDOC_OUT"]; os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "pixhist.txt"), "w")
def p(*a): print(*a, file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctrl.GetStructuredFile()
    names = {}
    def walk(acts):
        for a in acts:
            names[a.eventId] = a.GetName(sf); walk(a.children)
    walk(ctrl.GetRootActions())
    ctrl.SetFrameEvent(int(os.environ["RDOC_TARGET_EID"]), True)
    ps = ctrl.GetPipelineState()
    target = [o.resource for o in ps.GetOutputTargets() if o.resource != rd.ResourceId.Null()][0]
    last = max(names)
    ctrl.SetFrameEvent(last, True)
    for xy in os.environ["RDOC_XY"].split(";"):
        x, y = map(int, xy.split(","))
        hist = ctrl.PixelHistory(target, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        p(f"pixel ({x},{y}) target {target}: {len(hist)} mods")
        for m in hist:
            if not m.Passed(): continue
            so = [round(v, 3) for v in m.shaderOut.col.floatValue]
            pre = [round(v, 3) for v in m.preMod.col.floatValue]
            po = [round(v, 3) for v in m.postMod.col.floatValue]
            p(f"  EID {m.eventId:5d} pre={pre} shader={so} post={po}")
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
