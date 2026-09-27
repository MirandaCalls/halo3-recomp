# Read one pixel of the target after each event that writes it; report value changes.
# Env: RDOC_TARGET_EID, RDOC_XY="x,y", RDOC_RANGE="a,b"
import os, traceback
import renderdoc as rd
OUT = os.environ["RDOC_OUT"]; os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "pick.txt"), "w")
def p(*a): print(*a, file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    ctrl.SetFrameEvent(int(os.environ["RDOC_TARGET_EID"]), True)
    target = [o.resource for o in ctrl.GetPipelineState().GetOutputTargets() if o.resource != rd.ResourceId.Null()][0]
    t = texs[target]; p(f"target {target} {t.width}x{t.height} {t.format.Name()} msaa={t.msSamp}")
    x, y = map(int, os.environ["RDOC_XY"].split(","))
    a0, a1 = map(int, os.environ["RDOC_RANGE"].split(","))
    acts = []
    want = rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear | rd.ActionFlags.Copy | rd.ActionFlags.Resolve
    def walk(l):
        for a in l:
            if a.flags & want: acts.append(a)
            walk(a.children)
    walk(ctrl.GetRootActions())
    prev = None
    for a in acts:
        if not (a0 <= a.eventId <= a1): continue
        ctrl.SetFrameEvent(a.eventId, True)
        for s in range(max(1, t.msSamp)):
            v = ctrl.PickPixel(target, x, y, rd.Subresource(0, 0, s), rd.CompType.Typeless).floatValue
            v = tuple(round(c, 3) for c in v)
            if s == 0 and v != prev:
                p(f"EID {a.eventId} {a.GetName(ctrl.GetStructuredFile())[:26]:26s} sample{s} = {v}")
                prev = v
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
