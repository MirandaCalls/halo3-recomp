# Dump & decode Xenos texture fetch constants bound for one draw's pixel shader. Env: RDOC_EIDS
import os, traceback
import renderdoc as rd
OUT = os.environ["RDOC_OUT"]; eid = int(os.environ["RDOC_EIDS"])
log = open(os.path.join(OUT, f"fetch{eid}.txt"), "w")
def p(*a): print(*a, file=log); log.flush()
FMT = {6:'k_8_8_8_8',7:'k_2_10_10_10',10:'k_10_11_11',11:'k_11_11_10',25:'k_16_16_16_16',26:'k_16_16_EXPAND',31:'k_16_16_16_16_FLOAT',
       32:'k_32_FLOAT',34:'k_32_32_32_32_FLOAT',54:'k_2_10_10_10_AS_16_16_16_16',57:'k_16_16_16_16_FLOAT(?)',48:'k_8_8_8_8_AS_16_16_16_16',
       62:'k_2_10_10_10_FLOAT_AS_16_16_16_16?',63:'k_2_10_10_10_FLOAT?'}
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(eid, True)
    ps = ctrl.GetPipelineState()
    refl = ps.GetShaderReflection(rd.ShaderStage.Pixel)
    for i, cb in enumerate(refl.constantBlocks):
        if cb.name != "xe_uniform_fetch_constants": continue
        d = ps.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
        vs = ctrl.GetCBufferVariableContents(ps.GetGraphicsPipelineObject(), refl.resourceId, rd.ShaderStage.Pixel, refl.entryPoint, i, d.resource, d.byteOffset, d.byteSize)
        words = []
        for v in vs:
            for m in (v.members or [v]):
                words += [m.value.u32v[k] for k in range(4)]
        for t in range(len(words) // 6):
            w = words[t*6:(t+1)*6]
            if not any(w): continue
            ftype = w[0] & 3; fmt = w[1] & 0x3F; endian = (w[1] >> 6) & 3; base = (w[1] >> 12) << 12
            p(f"fetch slot {t}: {' '.join(f'{x:08X}' for x in w)}  type={ftype} format={fmt}({FMT.get(fmt,'?')}) endian={endian} base={base:08X} exp_adjust(w3)={((w[3]>>13)&0x3F) - (64 if (w[3]>>18)&1 else 0)} num_format(w3b0)={w[3]&1}")
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
