# qrenderdoc --python: dump post-VS outputs for given EIDs (RDOC_EIDS=21,226)
import os, struct, traceback
import renderdoc as rd
out = open(os.path.join(os.environ["RDOC_OUT"], "vsout.txt"), "w")
def p(*a): print(*a, file=out); out.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDOC_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    for eid in [int(x) for x in os.environ["RDOC_EIDS"].split(",")]:
        ctrl.SetFrameEvent(eid, True)
        ps = ctrl.GetPipelineState()
        refl = ps.GetShaderReflection(rd.ShaderStage.Vertex)
        mesh = ctrl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        p(f"EID {eid}: numIndices={mesh.numIndices} stride={mesh.vertexByteStride} outputs={[s.varName for s in refl.outputSignature]}")
        data = ctrl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, mesh.vertexByteStride * max(mesh.numIndices, 1))
        n = mesh.vertexByteStride // 4
        for v in range(min(mesh.numIndices, 6)):
            vals = struct.unpack_from(f"{n}f", data, v * mesh.vertexByteStride)
            p(f"  v{v}: " + " ".join(f"{x:.4f}" for x in vals))
        # vertex inputs: vertex shader constants/fetch
        vsrefl = refl
        for i, cb in enumerate(vsrefl.constantBlocks):
            d = ps.GetConstantBlock(rd.ShaderStage.Vertex, i, 0).descriptor
            vars_ = ctrl.GetCBufferVariableContents(ps.GetGraphicsPipelineObject(), vsrefl.resourceId, rd.ShaderStage.Vertex, vsrefl.entryPoint, i, d.resource, d.byteOffset, d.byteSize)
            if cb.name == "xe_uniform_float_constants":
                for v in vars_:
                    for m in v.members[:8]:
                        p(f"  vs {m.name} = {[round(m.value.f32v[k],4) for k in range(4)]}")
        dis = ctrl.DisassembleShader(ps.GetGraphicsPipelineObject(), vsrefl, ctrl.GetDisassemblyTargets(True)[0])
        open(os.path.join(os.environ["RDOC_OUT"], f"eid{eid}_vs.txt"), "w").write(dis)
    p("done")
except Exception:
    p(traceback.format_exc())
os._exit(0)
