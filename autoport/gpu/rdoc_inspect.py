# Run with: QT_QPA_PLATFORM=offscreen qrenderdoc --python rdoc_inspect.py
# Env: RDOC_CAPTURE=<file.rdc>, RDOC_OUT=<dir>
# Lists draws, then dumps inputs/outputs/shader/constants for draws that sample
# single-channel textures (e.g. Bink YUV planes).
import os
import sys

import renderdoc as rd

cap_path = os.environ["RDOC_CAPTURE"]
out_dir = os.environ["RDOC_OUT"]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "report.txt"), "w")


def p(*a):
    print(*a, file=log)
    log.flush()


rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
status = cap.OpenFile(cap_path, "", None)
if status != rd.ResultCode.Succeeded:
    p("open failed", status)
    os._exit(1)
status, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if status != rd.ResultCode.Succeeded:
    p("replay failed", status)
    os._exit(1)

textures = {t.resourceId: t for t in ctrl.GetTextures()}
resources = {r.resourceId: r.name for r in ctrl.GetResources()}


def tex_desc(rid):
    t = textures.get(rid)
    if not t:
        return f"{rid} (not a texture)"
    return f"{resources.get(rid, rid)} {t.width}x{t.height} {t.format.Name()}"


def save_tex(rid, name):
    s = rd.TextureSave()
    s.resourceId = rid
    s.destType = rd.FileType.PNG
    s.mip = 0
    s.slice.sliceIndex = 0
    s.alpha = rd.AlphaMapping.Discard
    ctrl.SaveTexture(s, os.path.join(out_dir, name))


def walk(actions, out):
    for a in actions:
        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            out.append(a)
        walk(a.children, out)


import traceback
try:
  draws = []
  walk(ctrl.GetRootActions(), draws)
  p(f"{len(draws)} draws/dispatches")

  interesting = []
  for a in draws:
      ctrl.SetFrameEvent(a.eventId, False)
      st = ctrl.GetPipelineState()
      ro = st.GetReadOnlyResources(rd.ShaderStage.Pixel)
      bound = []
      for used in ro:
          rid = used.descriptor.resource
          if rid != rd.ResourceId.Null() and rid in textures:
              bound.append(rid)
      outs = [o.resource for o in st.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
      p(f"EID {a.eventId} {a.GetName(ctrl.GetStructuredFile())} idx={a.numIndices}")
      for rid in bound:
          p(f"    in : {tex_desc(rid)}")
      for rid in outs:
          p(f"    out: {tex_desc(rid)}")
      single_channel = [rid for rid in bound if textures[rid].format.compCount == 1
                        and textures[rid].width >= 640]
      if single_channel:
          interesting.append((a, bound, outs))

  p(f"\n{len(interesting)} draws sample large single-channel textures")
  for n, (a, bound, outs) in enumerate(interesting[:3]):
      ctrl.SetFrameEvent(a.eventId, False)
      st = ctrl.GetPipelineState()
      p(f"\n=== EID {a.eventId}")
      for i, rid in enumerate(bound):
          save_tex(rid, f"eid{a.eventId}_in{i}.png")
          p(f"  saved in{i}: {tex_desc(rid)}")
      for i, rid in enumerate(outs):
          save_tex(rid, f"eid{a.eventId}_out{i}.png")
          p(f"  saved out{i}: {tex_desc(rid)}")
      refl = st.GetShaderReflection(rd.ShaderStage.Pixel)
      if refl:
          targets = ctrl.GetDisassemblyTargets(True)
          dis = ctrl.DisassembleShader(st.GetGraphicsPipelineObject(), refl, targets[0])
          open(os.path.join(out_dir, f"eid{a.eventId}_ps.txt"), "w").write(dis)
          for i, cb in enumerate(refl.constantBlocks):
              desc = st.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
              vars_ = ctrl.GetCBufferVariableContents(st.GetGraphicsPipelineObject(), refl.resourceId,
                                                     rd.ShaderStage.Pixel, refl.entryPoint, i,
                                                     desc.resource, desc.byteOffset, desc.byteSize)
              p(f"  cbuffer {i} {cb.name}:")
              def dump(vs, pre="    ", depth=0):
                  for v in vs:
                      if v.members:
                          p(f"{pre}{v.name}:")
                          if depth < 2:
                              dump(v.members[:16], pre + "  ", depth + 1)
                      else:
                          n = min(4, v.columns * max(1, v.rows))
                          p(f"{pre}{v.name} = f{[round(v.value.f32v[k], 4) for k in range(n)]} u{[hex(v.value.u32v[k]) for k in range(n)]}")
              dump(vars_)


  p("done")
except Exception:
  p(traceback.format_exc())
ctrl.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
os._exit(0)
