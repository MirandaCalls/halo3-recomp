# Save the pixel-shader input textures and output of one draw, plus PS disassembly. Env: RDOC_EIDS
import os, traceback
import renderdoc as rd
OUT = os.environ["RDOC_OUT"]; os.makedirs(OUT, exist_ok=True)
eid = int(os.environ["RDOC_EIDS"])
log = open(os.path.join(OUT, f"inputs{eid}.txt"), "w")
def p(*a): print(*a, file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    ctrl.SetFrameEvent(eid, True)
    ps = ctrl.GetPipelineState()
    def save(rid, name):
        s = rd.TextureSave(); s.resourceId = rid; s.destType = rd.FileType.PNG; s.alpha = rd.AlphaMapping.Discard
        ctrl.SaveTexture(s, os.path.join(OUT, name)); p("saved", name)
    for i, u in enumerate(ps.GetReadOnlyResources(rd.ShaderStage.Pixel)):
        r = u.descriptor.resource
        if r in texs:
            t = texs[r]; save(r, f"eid{eid}_in{i}_{t.width}x{t.height}_{t.format.Name()}.png")
    refl = ps.GetShaderReflection(rd.ShaderStage.Pixel)
    open(os.path.join(OUT, f"eid{eid}_ps.txt"), "w").write(ctrl.DisassembleShader(ps.GetGraphicsPipelineObject(), refl, ctrl.GetDisassemblyTargets(True)[0]))
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
