import os, traceback
import renderdoc as rd
out = open(os.path.join(os.environ["RDOC_OUT"], "sampler.txt"), "w")
def p(*a): print(*a, file=out); out.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    ctrl.SetFrameEvent(int(os.environ["RDOC_EIDS"]), True)
    ps = ctrl.GetPipelineState()
    for u in ps.GetReadOnlyResources(rd.ShaderStage.Pixel):
        d = u.descriptor; t = texs.get(d.resource)
        if t:
            p(f"img {d.resource} {t.width}x{t.height} mips={t.mips} arr={t.arraysize} view firstMip={d.firstMip} numMips={d.numMips}")
    for u in ps.GetSamplers(rd.ShaderStage.Pixel):
        s = u.sampler
        p(f"sampler filter={s.filter.minify}/{s.filter.magnify}/{s.filter.mip} minLOD={s.minLOD} maxLOD={s.maxLOD} bias={s.mipBias} addr={s.addressU},{s.addressV}")
    # pixel history-free check: sample Y image mips
    for u in ps.GetReadOnlyResources(rd.ShaderStage.Pixel):
        t = texs.get(u.descriptor.resource)
        if t and t.width == 1280:
            for m in range(t.mips):
                sub = rd.Subresource(m, 0, 0)
                mm = ctrl.GetMinMax(t.resourceId, sub, rd.CompType.Typeless)
                p(f"  Y mip{m}: min={mm[0].floatValue[0]:.3f} max={mm[1].floatValue[0]:.3f}")
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
