# Dump one draw's state. Env: RDOC_EIDS=537
import os, traceback
import renderdoc as rd
OUT = os.environ["RDOC_OUT"]; os.makedirs(OUT, exist_ok=True)
eid = int(os.environ["RDOC_EIDS"])
log = open(os.path.join(OUT, f"draw{eid}.txt"), "w")
def p(*a): print(*a, file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    sf = ctrl.GetStructuredFile()
    acts = {}
    def walk(a_list):
        for a in a_list:
            acts[a.eventId] = a; walk(a.children)
    walk(ctrl.GetRootActions())
    act = acts[eid]
    p(f"EID {eid}: {act.GetName(sf)} indices={act.numIndices} instances={act.numInstances}")
    ctrl.SetFrameEvent(eid, True)
    ps = ctrl.GetPipelineState()
    for i, o in enumerate(ps.GetOutputTargets()):
        if o.resource != rd.ResourceId.Null():
            t = texs[o.resource]; p(f"  out{i}: {o.resource} {t.width}x{t.height} {t.format.Name()} view_fmt={o.format.Name()}")
    for i, b in enumerate(ps.GetColorBlends()):
        p(f"  blend{i}: enabled={b.enabled} writeMask={b.writeMask:04b} color={b.colorBlend.source}/{b.colorBlend.destination}/{b.colorBlend.operation}")
    for u in ps.GetReadOnlyResources(rd.ShaderStage.Pixel):
        t = texs.get(u.descriptor.resource)
        if t: p(f"  in: {u.descriptor.resource} {t.width}x{t.height} {t.format.Name()}")
    refl = ps.GetShaderReflection(rd.ShaderStage.Pixel)
    tgt = ctrl.GetDisassemblyTargets(True)[0]
    open(os.path.join(OUT, f"draw{eid}_ps.txt"), "w").write(ctrl.DisassembleShader(ps.GetGraphicsPipelineObject(), refl, tgt))
    for i, cb in enumerate(refl.constantBlocks):
        d = ps.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
        vs = ctrl.GetCBufferVariableContents(ps.GetGraphicsPipelineObject(), refl.resourceId, rd.ShaderStage.Pixel, refl.entryPoint, i, d.resource, d.byteOffset, d.byteSize)
        for v in vs:
            if v.name.startswith("edram") or v.name in ("color_exp_bias", "flags") or cb.name == "xe_uniform_float_constants":
                if v.members:
                    for m in v.members[:8]:
                        p(f"  {cb.name}.{m.name} = f{[round(m.value.f32v[k],4) for k in range(4)]} u{[hex(m.value.u32v[k]) for k in range(4)]}")
                else:
                    n = min(4, max(1, v.columns * max(1, v.rows)))
                    p(f"  {cb.name}.{v.name} = f{[round(v.value.f32v[k],4) for k in range(n)]} u{[hex(v.value.u32v[k]) for k in range(n)]}")
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
