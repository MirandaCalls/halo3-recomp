# Save the image each render target holds after the last draw that writes it.
import os, traceback
import renderdoc as rd
OUT = os.environ["RDOC_OUT"]; os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "passes.txt"), "w")
def p(*a): print(*a, file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    draws = []
    def walk(acts):
        for a in acts:
            if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch): draws.append(a)
            walk(a.children)
    walk(ctrl.GetRootActions())
    p(f"{len(draws)} draws")
    last_writer = {}
    order = []
    for a in draws:
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        for o in ps.GetOutputTargets():
            if o.resource != rd.ResourceId.Null():
                if o.resource not in last_writer: order.append(o.resource)
                last_writer[o.resource] = a.eventId
        for u in ps.GetReadWriteResources(rd.ShaderStage.Compute) if a.flags & rd.ActionFlags.Dispatch else []:
            r = u.descriptor.resource
            if r in texs:
                if r not in last_writer: order.append(r)
                last_writer[r] = a.eventId
    for i, rid in enumerate(order):
        eid = last_writer[rid]; t = texs[rid]
        ctrl.SetFrameEvent(eid, True)
        s = rd.TextureSave(); s.resourceId = rid; s.destType = rd.FileType.PNG; s.alpha = rd.AlphaMapping.Discard
        name = f"pass{i:02d}_eid{eid}_{t.width}x{t.height}_{t.format.Name()}.png"
        ctrl.SaveTexture(s, os.path.join(OUT, name))
        p(f"{name}  first/last writer eid {eid}")
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
