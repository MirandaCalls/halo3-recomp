# List draws in an EID range with their output targets / depth target / inputs. Env: RDOC_RANGE="a,b"
import os, traceback
import renderdoc as rd
OUT = os.environ["RDOC_OUT"]; os.makedirs(OUT, exist_ok=True)
a0, a1 = map(int, os.environ["RDOC_RANGE"].split(","))
log = open(os.path.join(OUT, f"range_{a0}_{a1}.txt"), "w")
def p(*a): print(*a, file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    sf = ctrl.GetStructuredFile()
    acts = []
    def walk(l):
        for a in l:
            if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear | rd.ActionFlags.Copy): acts.append(a)
            walk(a.children)
    walk(ctrl.GetRootActions())
    def d(r):
        t = texs.get(r); return f"{r}:{t.width}x{t.height}:{t.format.Name()}" if t else str(r)
    for a in acts:
        if not (a0 <= a.eventId <= a1): continue
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        outs = [d(o.resource) for o in ps.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        dt = ps.GetDepthTarget(); dts = d(dt.resource) if dt.resource != rd.ResourceId.Null() else "-"
        ins = [d(u.descriptor.resource) for u in ps.GetReadOnlyResources(rd.ShaderStage.Pixel) if u.descriptor.resource in texs]
        p(f"EID {a.eventId} {a.GetName(sf)[:28]:28s} n={a.numIndices} out={outs} depth={dts} in={ins[:3]}")
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
