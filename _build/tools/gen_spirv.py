#!/usr/bin/env python3
# tools/gen_spirv.py -- erzeugt die SPIR-V-Shader fuer Phase 3 (T3.4+), OHNE externe
# Abhaengigkeiten (kein glslang noetig): ein minimaler, handgeschriebener SPIR-V-
# Assembler (Spez: SPIR-V 1.0, Magic 0x07230203). Ausgaben:
#   user/vk_vert.spv / user/vk_frag.spv  -- Demo-Shader (MVP-Transform + Gouraud)
#   user/vk_shaders.h                    -- eingebettete Wort-Arrays + Testvektoren
# Die Testvektoren (Eingaben + erwartete Ausgaben, in Python referenz-berechnet)
# beweisen den Interpreter semantisch (Toleranz 1e-4 wegen float32/float64-Rundung).
import struct, sys, math

MAGIC = 0x07230203
VERSION = 0x00010000            # SPIR-V 1.0

# --- Opcodes (SPIR-V-Spez, Unified1) ---
OpExtInstImport=11; OpExtInst=12; OpMemoryModel=14; OpEntryPoint=15; OpExecutionMode=16
OpCapability=17; OpTypeVoid=19; OpTypeBool=20; OpTypeInt=21; OpTypeFloat=22
OpTypeVector=23; OpTypeMatrix=24; OpTypeStruct=30; OpTypePointer=32; OpTypeFunction=33
OpConstant=43; OpConstantComposite=44; OpFunction=54; OpFunctionEnd=56
OpFunctionParameter=55; OpFunctionCall=57; OpReturnValue=254   # V2.3
OpVariable=59; OpLoad=61; OpStore=62; OpAccessChain=65
OpDecorate=71; OpMemberDecorate=72
OpVectorExtractDynamic=77; OpVectorInsertDynamic=78   # V2.6
OpVectorShuffle=79; OpCompositeConstruct=80; OpCompositeExtract=81; OpCompositeInsert=82   # V2.5
OpFNegate=127; OpFAdd=129; OpFSub=131; OpFMul=133; OpFDiv=136
OpConvertSToF=111
OpTypeRuntimeArray=29; OpIMul=132     # V1.7
OpIAdd=128; OpUDiv=134; OpUMod=137; OpShiftLeftLogical=196; OpBitwiseOr=197; OpBitwiseXor=198; OpBitwiseAnd=199  # V2.2
OpTypeImage=25; OpTypeSampledImage=27; OpImageSampleImplicitLod=87
OpImageFetch=95; OpImageGather=96; OpImageRead=98; OpImageWrite=99; OpImage=100; OpImageQuerySizeLod=103; OpISub=130; OpBitcast=124   # V2.6
OpAtomicIAdd=234   # V2.6
OpBitFieldUExtract=203; OpBitReverse=204; OpBitCount=205   # V2.6
OpConstantComposite2=44
OpVectorTimesScalar=142; OpMatrixTimesVector=145; OpDot=148
OpSelect=169; OpFOrdGreaterThan=186
OpPhi=245; OpSelectionMerge=247; OpLabel=248; OpBranch=249; OpBranchConditional=250
OpReturn=253
# Dekorationen / Enums
DecBlock=2; DecColMajor=5; DecMatrixStride=7; DecArrayStride=6; DecBuiltIn=11; DecLocation=30; DecOffset=35
DecBinding=33; DecDescriptorSet=34
SCUniform=2; SCUniformConstant=0; SCStorageBuffer=12    # V1.7
DimImage2D=1
BuiltInPosition=0; BuiltInInstanceIndex=43; BuiltInGlobalInvocationId=28   # V1.7
BuiltInSubgroupSize=36; BuiltInSubgroupLocalInvocationId=41                # V3b
SCInput=1; SCOutput=3; SCPushConstant=9
ExecModelVertex=0; ExecModelFragment=4; ExecModelGLCompute=5   # V1.7
GLSL_Normalize=69
# V3b: Subgroup/GroupNonUniform-Ops + Capabilities
OpGroupNonUniformElect=333; OpGroupNonUniformIAdd=349
CapGroupNonUniform=61; CapGroupNonUniformArithmetic=63; ScopeSubgroup=3; GroupOpReduce=0

def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]

def words_str(s):
    b = s.encode('ascii') + b'\x00'
    while len(b) % 4: b += b'\x00'
    return list(struct.unpack('<%dI' % (len(b)//4), b))

class Asm:
    def __init__(self):
        self.caps=[]; self.pre=[]; self.deco=[]; self.types=[]; self.funcs=[]
        self.next_id=1
    def nid(self):
        i=self.next_id; self.next_id+=1; return i
    def emit(self, sec, op, ops):
        sec.append(((len(ops)+1)<<16)|op); sec.extend(ops)
    def cap(self, c):           self.emit(self.caps, OpCapability, [c])
    def extimport(self, name):
        i=self.nid(); self.emit(self.pre, OpExtInstImport, [i]+words_str(name)); return i
    def memmodel(self):         self.emit(self.pre, OpMemoryModel, [0,1])   # Logical GLSL450
    def entry(self, model, fn, name, iface):
        self.emit(self.pre, OpEntryPoint, [model, fn]+words_str(name)+iface)
    def execmode(self, fn, mode, extra=None):
        self.emit(self.pre, OpExecutionMode, [fn, mode]+(extra or []))
    def dec(self, tgt, d, extra=None):  self.emit(self.deco, OpDecorate, [tgt,d]+(extra or []))
    def mdec(self, st, m, d, extra=None): self.emit(self.deco, OpMemberDecorate, [st,m,d]+(extra or []))
    def ty(self, op, ops):
        i=self.nid(); self.emit(self.types, op, [i]+ops); return i
    def const(self, t, words):
        i=self.nid(); self.emit(self.types, OpConstant, [t,i]+words); return i
    def var(self, ptr_t, sc):
        i=self.nid(); self.emit(self.types, OpVariable, [ptr_t,i,sc]); return i
    def ins(self, op, t, ops):          # Instruktion mit Result-Id
        i=self.nid(); self.emit(self.funcs, op, [t,i]+ops); return i
    def insv(self, op, ops):            # Instruktion ohne Result
        self.emit(self.funcs, op, ops)
    def label(self):
        i=self.nid(); self.emit(self.funcs, OpLabel, [i]); return i
    def label_id(self):                 # Id VORAB reservieren (Vorwaertsspruenge)
        return self.nid()
    def label_place(self, i):
        self.emit(self.funcs, OpLabel, [i])
    def binary(self):
        w=[MAGIC, VERSION, 0, self.next_id, 0]
        for s in (self.caps, self.pre, self.deco, self.types, self.funcs): w.extend(s)
        return w

def build_vert():
    """layout(push_constant) uniform PC { mat4 mvp; };
       layout(location=0) in vec3 inPos;  layout(location=1) in vec3 inColor;
       layout(location=0) out vec3 vColor;  gl_Position (BuiltIn Position, standalone)
       main: gl_Position = mvp * vec4(inPos,1); vColor = inColor;"""
    a=Asm(); a.cap(1); glsl=a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v3=a.ty(OpTypeVector,[flt,3]); v4=a.ty(OpTypeVector,[flt,4])
    m4=a.ty(OpTypeMatrix,[v4,4])
    st=a.ty(OpTypeStruct,[m4])
    p_pc_st=a.ty(OpTypePointer,[SCPushConstant,st]); p_pc_m4=a.ty(OpTypePointer,[SCPushConstant,m4])
    p_in_v3=a.ty(OpTypePointer,[SCInput,v3]); p_out_v3=a.ty(OpTypePointer,[SCOutput,v3])
    p_out_v4=a.ty(OpTypePointer,[SCOutput,v4])
    i32=a.ty(OpTypeInt,[32,1]); c0=a.const(i32,[0]); c1f=a.const(flt,[f32(1.0)])
    pc=a.var(p_pc_st,SCPushConstant)
    inPos=a.var(p_in_v3,SCInput); inCol=a.var(p_in_v3,SCInput)
    vCol=a.var(p_out_v3,SCOutput); glpos=a.var(p_out_v4,SCOutput)
    a.dec(st,DecBlock); a.mdec(st,0,DecColMajor); a.mdec(st,0,DecOffset,[0]); a.mdec(st,0,DecMatrixStride,[16])
    a.dec(inPos,DecLocation,[0]); a.dec(inCol,DecLocation,[1]); a.dec(vCol,DecLocation,[0])
    a.dec(glpos,DecBuiltIn,[BuiltInPosition])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    ac=a.ins(OpAccessChain,p_pc_m4,[pc,c0])
    mvp=a.ins(OpLoad,m4,[ac])
    p3=a.ins(OpLoad,v3,[inPos])
    x=a.ins(OpCompositeExtract,flt,[p3,0]); y=a.ins(OpCompositeExtract,flt,[p3,1]); z=a.ins(OpCompositeExtract,flt,[p3,2])
    p4=a.ins(OpCompositeConstruct,v4,[x,y,z,c1f])
    clip=a.ins(OpMatrixTimesVector,v4,[mvp,p4])
    a.insv(OpStore,[glpos,clip])
    col=a.ins(OpLoad,v3,[inCol]); a.insv(OpStore,[vCol,col])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelVertex,fn,"main",[inPos,inCol,vCol,glpos])
    return a.binary()

def build_frag_ubo():
    """V1.3 Fragment-Shader mit Uniform-Buffer (set 0, binding 0):
       layout(set=0,binding=0) uniform UBO { vec4 color; };
       layout(location=0) out vec4 outColor;  main: outColor = color;"""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v4=a.ty(OpTypeVector,[flt,4]); i32=a.ty(OpTypeInt,[32,1])
    st=a.ty(OpTypeStruct,[v4])
    p_uni_st=a.ty(OpTypePointer,[SCUniform,st]); p_uni_v4=a.ty(OpTypePointer,[SCUniform,v4])
    p_out_v4=a.ty(OpTypePointer,[SCOutput,v4])
    c0=a.const(i32,[0])
    ubo=a.var(p_uni_st,SCUniform); outc=a.var(p_out_v4,SCOutput)
    a.dec(st,DecBlock); a.mdec(st,0,DecOffset,[0])
    a.dec(ubo,DecDescriptorSet,[0]); a.dec(ubo,DecBinding,[0]); a.dec(outc,DecLocation,[0])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    ac=a.ins(OpAccessChain,p_uni_v4,[ubo,c0])
    c=a.ins(OpLoad,v4,[ac])
    a.insv(OpStore,[outc,c])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelFragment,fn,"main",[ubo,outc])
    a.execmode(fn,7)   # OriginUpperLeft
    return a.binary()

def build_frag_tex():
    """V1.4 Fragment-Shader mit Textur (combined image sampler, set0/binding0):
       layout(set=0,binding=0) uniform sampler2D tex;
       layout(location=0) out vec4 outColor;
       main: outColor = texture(tex, vec2(0.25, 0.75));   (konstante Koordinate)"""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v2=a.ty(OpTypeVector,[flt,2]); v4=a.ty(OpTypeVector,[flt,4])
    # OpTypeImage: SampledType Dim Depth Arrayed MS Sampled Format
    img=a.ty(OpTypeImage,[flt, DimImage2D, 0, 0, 0, 1, 0])
    simg=a.ty(OpTypeSampledImage,[img])
    p_uc=a.ty(OpTypePointer,[SCUniformConstant,simg]); p_out_v4=a.ty(OpTypePointer,[SCOutput,v4])
    cu=a.const(flt,[f32(0.25)]); cv=a.const(flt,[f32(0.75)])
    coord=a.nid(); a.emit(a.types,OpConstantComposite2,[v2,coord,cu,cv])
    tex=a.var(p_uc,SCUniformConstant); outc=a.var(p_out_v4,SCOutput)
    a.dec(tex,DecDescriptorSet,[0]); a.dec(tex,DecBinding,[0]); a.dec(outc,DecLocation,[0])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    si=a.ins(OpLoad,simg,[tex])
    c=a.ins(OpImageSampleImplicitLod,v4,[si,coord])
    a.insv(OpStore,[outc,c])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelFragment,fn,"main",[tex,outc])
    a.execmode(fn,7)
    return a.binary()

def build_frag_fetch():
    """V2.6 Fragment: textureSize + texelFetch (Integer-Bild-Ops).
       layout(set=0,binding=0) uniform sampler2D tex;  out vec4 outColor;
       ivec2 sz = textureSize(tex, 0);                       // (2,2)
       outColor = texelFetch(tex, ivec2(sz.x - 1, 1), 0);    // Texel(1,1) = gelb 0xFFFFFF00
       Bindet QuerySizeLod (sz.x) UND ImageFetch aneinander: falsche Groesse -> Fetch(-1,1)
       -> geklemmt auf (0,1) = blau; falscher Fetch -> andere Farbe. -> diskriminierend."""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v4=a.ty(OpTypeVector,[flt,4])
    i32=a.ty(OpTypeInt,[32,1]); v2i=a.ty(OpTypeVector,[i32,2])
    img=a.ty(OpTypeImage,[flt, DimImage2D, 0, 0, 0, 1, 0])
    simg=a.ty(OpTypeSampledImage,[img])
    p_uc=a.ty(OpTypePointer,[SCUniformConstant,simg]); p_out_v4=a.ty(OpTypePointer,[SCOutput,v4])
    c0=a.const(i32,[0]); c1=a.const(i32,[1])
    tex=a.var(p_uc,SCUniformConstant); outc=a.var(p_out_v4,SCOutput)
    a.dec(tex,DecDescriptorSet,[0]); a.dec(tex,DecBinding,[0]); a.dec(outc,DecLocation,[0])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    si=a.ins(OpLoad,simg,[tex])
    im=a.ins(OpImage,img,[si])
    sz=a.ins(OpImageQuerySizeLod,v2i,[im,c0])        # ivec2 (w,h)
    sx=a.ins(OpCompositeExtract,i32,[sz,0])          # sz.x
    sx1=a.ins(OpISub,i32,[sx,c1])                     # sz.x - 1
    coord=a.ins(OpCompositeConstruct,v2i,[sx1,c1])   # ivec2(sz.x-1, 1)
    im2=a.ins(OpImage,img,[si])
    texel=a.ins(OpImageFetch,v4,[im2,coord])         # Texel(1,1) = gelb
    a.insv(OpStore,[outc,texel])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelFragment,fn,"main",[tex,outc])
    a.execmode(fn,7)
    return a.binary()

def build_frag_gather():
    """V2.6 Fragment: textureGather. sampler2D tex; out vec4 outColor;
       outColor = textureGather(tex, vec2(0.5,0.5), 0);  // R-Kanal der 4 Zentrums-Texel
       2x2 (rot/gruen/blau/gelb): R-Werte (0,0)=1 (1,0)=0 (0,1)=0 (1,1)=1.
       Reihenfolge (i0,j1),(i1,j1),(i1,j0),(i0,j0) = blau,gelb,gruen,rot -> R=(0,1,0,1)
       -> outColor=(0,1,0,1) -> 0xFF00FF00 (gruen). Falsche Reihenfolge/Komponente -> andere Farbe."""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v2=a.ty(OpTypeVector,[flt,2]); v4=a.ty(OpTypeVector,[flt,4])
    i32=a.ty(OpTypeInt,[32,1])
    img=a.ty(OpTypeImage,[flt, DimImage2D, 0, 0, 0, 1, 0])
    simg=a.ty(OpTypeSampledImage,[img])
    p_uc=a.ty(OpTypePointer,[SCUniformConstant,simg]); p_out_v4=a.ty(OpTypePointer,[SCOutput,v4])
    ch=a.const(flt,[f32(0.5)]); comp0=a.const(i32,[0])
    coord=a.nid(); a.emit(a.types,OpConstantComposite,[v2,coord,ch,ch])
    tex=a.var(p_uc,SCUniformConstant); outc=a.var(p_out_v4,SCOutput)
    a.dec(tex,DecDescriptorSet,[0]); a.dec(tex,DecBinding,[0]); a.dec(outc,DecLocation,[0])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    si=a.ins(OpLoad,simg,[tex])
    g=a.ins(OpImageGather,v4,[si,coord,comp0])
    a.insv(OpStore,[outc,g])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelFragment,fn,"main",[tex,outc])
    a.execmode(fn,7)
    return a.binary()

def build_frag_mrt():
    """V1.5 MRT-Fragment-Shader (2 Farb-Ausgaben):
       layout(location=0) out vec4 o0; layout(location=1) out vec4 o1;
       main: o0 = vec4(1,0,0,1) [rot]; o1 = vec4(0,1,0,1) [gruen];"""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v4=a.ty(OpTypeVector,[flt,4])
    p_out=a.ty(OpTypePointer,[SCOutput,v4])
    c0=a.const(flt,[f32(0.0)]); c1=a.const(flt,[f32(1.0)])
    red=a.nid(); a.emit(a.types,OpConstantComposite2,[v4,red,c1,c0,c0,c1])
    grn=a.nid(); a.emit(a.types,OpConstantComposite2,[v4,grn,c0,c1,c0,c1])
    o0=a.var(p_out,SCOutput); o1=a.var(p_out,SCOutput)
    a.dec(o0,DecLocation,[0]); a.dec(o1,DecLocation,[1])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    a.insv(OpStore,[o0,red]); a.insv(OpStore,[o1,grn])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelFragment,fn,"main",[o0,o1])
    a.execmode(fn,7)
    return a.binary()

def build_compute():
    """V1.7 Compute (local_size_x=1) + V2.2 Integer-Ops-Voll + V2.3 OpFunctionCall:
       layout(set=0,binding=0) buffer B { uint data[]; };
       uint combine(uint a, uint x){ return (a/3u)+(a%5u)+(x<<2)+(x&6u)+(x|1u)+(x^3u)
                                            + bitCount(x) + bitfieldReverse(x) + bitfieldExtract(x,0,2); }
       void main(){ uint x=gl_GlobalInvocationID.x; data[x] = combine(x*7u, x); }
       Uebt UDiv/UMod/ShiftLeft/BitAnd/BitOr/BitXor/IAdd (V2.2), OpFunctionCall
       (2 Wert-Parameter) + OpReturnValue-Inlining (V2.3) UND V2.6-Bitfeld-Ops
       (BitCount/BitReverse/BitFieldUExtract) aus. data[i] wird identisch referenz-berechnet."""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    u32=a.ty(OpTypeInt,[32,0])                    # unsigned int
    v3u=a.ty(OpTypeVector,[u32,3])
    rta=a.ty(OpTypeRuntimeArray,[u32])            # uint data[]
    st=a.ty(OpTypeStruct,[rta])                   # buffer B { uint data[]; }
    p_ssbo_st=a.ty(OpTypePointer,[SCStorageBuffer,st])
    p_ssbo_u=a.ty(OpTypePointer,[SCStorageBuffer,u32])
    p_in_v3u=a.ty(OpTypePointer,[SCInput,v3u]); p_in_u=a.ty(OpTypePointer,[SCInput,u32])
    ufn=a.ty(OpTypeFunction,[u32,u32,u32])        # uint combine(uint, uint)
    c0=a.const(u32,[0]); c1=a.const(u32,[1]); c2=a.const(u32,[2]); c3=a.const(u32,[3])
    c5=a.const(u32,[5]); c6=a.const(u32,[6]); c7=a.const(u32,[7])
    ssbo=a.var(p_ssbo_st,SCStorageBuffer); gid=a.var(p_in_v3u,SCInput)
    a.dec(rta,DecArrayStride,[4])
    a.dec(st,DecBlock); a.mdec(st,0,DecOffset,[0])
    a.dec(ssbo,DecDescriptorSet,[0]); a.dec(ssbo,DecBinding,[0])
    a.dec(gid,DecBuiltIn,[BuiltInGlobalInvocationId])
    # Helfer-Funktion + Parameter-Ids vorab reservieren (main referenziert sie im Call).
    combine=a.nid(); pa=a.nid(); px=a.nid()
    # --- main ---
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    gidx_ptr=a.ins(OpAccessChain,p_in_u,[gid,c0])   # &gl_GlobalInvocationID.x
    x=a.ins(OpLoad,u32,[gidx_ptr])
    av=a.ins(OpIMul,u32,[x,c7])                     # a = x*7
    s=a.ins(OpFunctionCall,u32,[combine,av,x])      # combine(a, x)
    dst=a.ins(OpAccessChain,p_ssbo_u,[ssbo,c0,x])   # &data[x]
    a.insv(OpStore,[dst,s])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    # --- Helfer: uint combine(uint a, uint x) ---
    a.emit(a.funcs,OpFunction,[u32,combine,0,ufn])
    a.emit(a.funcs,OpFunctionParameter,[u32,pa])
    a.emit(a.funcs,OpFunctionParameter,[u32,px])
    a.label()
    b=a.ins(OpUDiv,u32,[pa,c3])                     # a/3
    c=a.ins(OpUMod,u32,[pa,c5])                     # a%5
    d=a.ins(OpShiftLeftLogical,u32,[px,c2])         # x<<2
    e=a.ins(OpBitwiseAnd,u32,[px,c6])               # x&6
    f=a.ins(OpBitwiseOr,u32,[px,c1])                # x|1
    g=a.ins(OpBitwiseXor,u32,[px,c3])               # x^3
    bc=a.ins(OpBitCount,u32,[px])                   # popcount(x)
    br=a.ins(OpBitReverse,u32,[px])                 # bits von x gespiegelt
    be=a.ins(OpBitFieldUExtract,u32,[px,c0,c2])     # (x>>0)&3
    s2=a.ins(OpIAdd,u32,[b,c]); s2=a.ins(OpIAdd,u32,[s2,d]); s2=a.ins(OpIAdd,u32,[s2,e])
    s2=a.ins(OpIAdd,u32,[s2,f]); s2=a.ins(OpIAdd,u32,[s2,g])
    s2=a.ins(OpIAdd,u32,[s2,bc]); s2=a.ins(OpIAdd,u32,[s2,br]); s2=a.ins(OpIAdd,u32,[s2,be])
    a.insv(OpReturnValue,[s2])
    a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelGLCompute,fn,"main",[gid,ssbo])
    a.execmode(fn,17,[1,1,1])                        # LocalSize 1 1 1
    return a.binary()

def build_compute_atomic():
    """V2.6 Compute + Atomics: buffer B { uint data[]; };
       void main(){ atomicAdd(data[0], gl_GlobalInvocationID.x + 1u); }
       Dispatch(8) seriell -> data[0] == 1+2+...+8 == 36. Beweist OpAtomicIAdd
       (Read-Modify-Write ueber das persistente SSBO-Backing zwischen Invokationen)."""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    u32=a.ty(OpTypeInt,[32,0]); v3u=a.ty(OpTypeVector,[u32,3])
    rta=a.ty(OpTypeRuntimeArray,[u32]); st=a.ty(OpTypeStruct,[rta])
    p_ssbo_st=a.ty(OpTypePointer,[SCStorageBuffer,st])
    p_ssbo_u=a.ty(OpTypePointer,[SCStorageBuffer,u32])
    p_in_v3u=a.ty(OpTypePointer,[SCInput,v3u]); p_in_u=a.ty(OpTypePointer,[SCInput,u32])
    c0=a.const(u32,[0]); c1=a.const(u32,[1]); c_scope=a.const(u32,[1]); c_sem=a.const(u32,[0])
    ssbo=a.var(p_ssbo_st,SCStorageBuffer); gid=a.var(p_in_v3u,SCInput)
    a.dec(rta,DecArrayStride,[4]); a.dec(st,DecBlock); a.mdec(st,0,DecOffset,[0])
    a.dec(ssbo,DecDescriptorSet,[0]); a.dec(ssbo,DecBinding,[0])
    a.dec(gid,DecBuiltIn,[BuiltInGlobalInvocationId])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    gidx=a.ins(OpAccessChain,p_in_u,[gid,c0]); x=a.ins(OpLoad,u32,[gidx])
    inc=a.ins(OpIAdd,u32,[x,c1])                       # gid.x + 1
    dst=a.ins(OpAccessChain,p_ssbo_u,[ssbo,c0,c0])     # &data[0]
    a.ins(OpAtomicIAdd,u32,[dst,c_scope,c_sem,inc])    # alten Wert verwerfen
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelGLCompute,fn,"main",[gid,ssbo])
    a.execmode(fn,17,[1,1,1])
    return a.binary()

def build_compute_image():
    """V2.6 Compute + Storage-Image: layout(binding=0, rgba8) uniform image2D img; (4x1)
       void main(){ ivec2 p=ivec2(gid.x,0); vec4 c=imageLoad(img,p); imageStore(img,p, c.bgra); }
       Vertauscht R<->B je Pixel (Kanaele nur 0x00/0xFF -> byte-exakt, kein Float-Rundungsrisiko).
       Beweist OpImageRead + OpImageWrite + ganzzahlige Koordinaten-Adressierung."""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v4=a.ty(OpTypeVector,[flt,4])
    u32=a.ty(OpTypeInt,[32,0]); i32=a.ty(OpTypeInt,[32,1])
    v3u=a.ty(OpTypeVector,[u32,3]); v2i=a.ty(OpTypeVector,[i32,2])
    img=a.ty(OpTypeImage,[flt, DimImage2D, 0, 0, 0, 2, 0])   # Sampled=2 (Storage-Image)
    p_uc=a.ty(OpTypePointer,[SCUniformConstant,img])
    p_in_v3u=a.ty(OpTypePointer,[SCInput,v3u]); p_in_u=a.ty(OpTypePointer,[SCInput,u32])
    c0u=a.const(u32,[0]); c0i=a.const(i32,[0])
    im=a.var(p_uc,SCUniformConstant); gid=a.var(p_in_v3u,SCInput)
    a.dec(im,DecDescriptorSet,[0]); a.dec(im,DecBinding,[0])
    a.dec(gid,DecBuiltIn,[BuiltInGlobalInvocationId])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    gidx=a.ins(OpAccessChain,p_in_u,[gid,c0u]); x=a.ins(OpLoad,u32,[gidx])
    xi=a.ins(OpBitcast,i32,[x])                     # gid.x (uint) -> int
    p=a.ins(OpCompositeConstruct,v2i,[xi,c0i])      # ivec2(x, 0)
    imgv=a.ins(OpLoad,img,[im])
    c=a.ins(OpImageRead,v4,[imgv,p])                # vec4 = imageLoad(img, p)
    sw=a.ins(OpVectorShuffle,v4,[c,c,2,1,0,3])      # c.bgra (R<->B)
    imgv2=a.ins(OpLoad,img,[im])
    a.insv(OpImageWrite,[imgv2,p,sw])               # imageStore(img, p, c.bgra)
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelGLCompute,fn,"main",[gid,im])
    a.execmode(fn,17,[1,1,1])
    return a.binary()

def build_compute_subgroup():
    """V3b Compute + Subgroup-Ops (GroupNonUniform): buffer B { uint data[]; };
       void main(){ uint x=gid.x;
                    data[x] = subgroupAdd(x+5u) + gl_SubgroupSize*1000u
                              + gl_SubgroupInvocationID + (subgroupElect()?1000000u:0u); }
       Interpreter faehrt 1 Lane -> subgroupAdd(v)=v, SubgroupSize=1, InvocationID=0, Elect=true.
       => data[x] == (x+5) + 1000 + 0 + 1000000 == x + 1001005. Dispatch(8) referenz-berechnet.
       Beweist OpGroupNonUniformIAdd (Reduce) + OpGroupNonUniformElect + Subgroup-Builtins."""
    a=Asm(); a.cap(1); a.cap(CapGroupNonUniform); a.cap(CapGroupNonUniformArithmetic)
    a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    u32=a.ty(OpTypeInt,[32,0]); bl=a.ty(OpTypeBool,[]); v3u=a.ty(OpTypeVector,[u32,3])
    rta=a.ty(OpTypeRuntimeArray,[u32]); st=a.ty(OpTypeStruct,[rta])
    p_ssbo_st=a.ty(OpTypePointer,[SCStorageBuffer,st]); p_ssbo_u=a.ty(OpTypePointer,[SCStorageBuffer,u32])
    p_in_v3u=a.ty(OpTypePointer,[SCInput,v3u]); p_in_u=a.ty(OpTypePointer,[SCInput,u32])
    c0=a.const(u32,[0]); c5=a.const(u32,[5]); c1000=a.const(u32,[1000])
    cbig=a.const(u32,[1000000]); csub=a.const(u32,[ScopeSubgroup])
    ssbo=a.var(p_ssbo_st,SCStorageBuffer); gid=a.var(p_in_v3u,SCInput)
    sgsz=a.var(p_in_u,SCInput); sginv=a.var(p_in_u,SCInput)
    a.dec(rta,DecArrayStride,[4]); a.dec(st,DecBlock); a.mdec(st,0,DecOffset,[0])
    a.dec(ssbo,DecDescriptorSet,[0]); a.dec(ssbo,DecBinding,[0])
    a.dec(gid,DecBuiltIn,[BuiltInGlobalInvocationId])
    a.dec(sgsz,DecBuiltIn,[BuiltInSubgroupSize])
    a.dec(sginv,DecBuiltIn,[BuiltInSubgroupLocalInvocationId])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    gidx=a.ins(OpAccessChain,p_in_u,[gid,c0]); x=a.ins(OpLoad,u32,[gidx])
    xp5=a.ins(OpIAdd,u32,[x,c5])
    sa=a.ins(OpGroupNonUniformIAdd,u32,[csub,GroupOpReduce,xp5])   # subgroupAdd(x+5)
    sz=a.ins(OpLoad,u32,[sgsz]); szk=a.ins(OpIMul,u32,[sz,c1000])  # gl_SubgroupSize*1000
    inv=a.ins(OpLoad,u32,[sginv])                                  # gl_SubgroupInvocationID
    el=a.ins(OpGroupNonUniformElect,bl,[csub])                    # subgroupElect()
    esel=a.ins(OpSelect,u32,[el,cbig,c0])
    t1=a.ins(OpIAdd,u32,[sa,szk]); t2=a.ins(OpIAdd,u32,[t1,inv]); t3=a.ins(OpIAdd,u32,[t2,esel])
    dst=a.ins(OpAccessChain,p_ssbo_u,[ssbo,c0,x]); a.insv(OpStore,[dst,t3])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelGLCompute,fn,"main",[gid,ssbo,sgsz,sginv])
    a.execmode(fn,17,[1,1,1])
    return a.binary()

def build_instanced():
    """V1.2 Instancing-Vertexshader (nutzt gl_InstanceIndex + ConvertSToF):
       in vec3 inPos(0), inColor(1); in int gl_InstanceIndex; out vec3 vColor(0); gl_Position.
       gl_Position = vec4(inPos.x + float(gl_InstanceIndex)*0.5 - 0.25, inPos.y, inPos.z, 1);
       vColor = inColor;  -> Instanz 0 nach links (-0.25), Instanz 1 nach rechts (+0.25)."""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); i32=a.ty(OpTypeInt,[32,1])
    v3=a.ty(OpTypeVector,[flt,3]); v4=a.ty(OpTypeVector,[flt,4])
    p_in_v3=a.ty(OpTypePointer,[SCInput,v3]); p_in_i=a.ty(OpTypePointer,[SCInput,i32])
    p_out_v3=a.ty(OpTypePointer,[SCOutput,v3]); p_out_v4=a.ty(OpTypePointer,[SCOutput,v4])
    c1f=a.const(flt,[f32(1.0)]); chalf=a.const(flt,[f32(0.5)]); cq=a.const(flt,[f32(0.25)])
    inPos=a.var(p_in_v3,SCInput); inCol=a.var(p_in_v3,SCInput); iid=a.var(p_in_i,SCInput)
    vCol=a.var(p_out_v3,SCOutput); glpos=a.var(p_out_v4,SCOutput)
    a.dec(inPos,DecLocation,[0]); a.dec(inCol,DecLocation,[1]); a.dec(vCol,DecLocation,[0])
    a.dec(iid,DecBuiltIn,[BuiltInInstanceIndex]); a.dec(glpos,DecBuiltIn,[BuiltInPosition])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    p3=a.ins(OpLoad,v3,[inPos])
    ii=a.ins(OpLoad,i32,[iid])
    fi=a.ins(OpConvertSToF,flt,[ii])
    off1=a.ins(OpFMul,flt,[fi,chalf])
    off=a.ins(OpFSub,flt,[off1,cq])
    x=a.ins(OpCompositeExtract,flt,[p3,0]); y=a.ins(OpCompositeExtract,flt,[p3,1]); z=a.ins(OpCompositeExtract,flt,[p3,2])
    x2=a.ins(OpFAdd,flt,[x,off])
    p4=a.ins(OpCompositeConstruct,v4,[x2,y,z,c1f])
    a.insv(OpStore,[glpos,p4])
    col=a.ins(OpLoad,v3,[inCol]); a.insv(OpStore,[vCol,col])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelVertex,fn,"main",[inPos,inCol,iid,vCol,glpos])
    return a.binary()

def build_frag():
    """layout(location=0) in vec3 vColor; layout(location=0) out vec4 outColor;
       main: outColor = vec4(vColor, 1.0);"""
    a=Asm(); a.cap(1); a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); v3=a.ty(OpTypeVector,[flt,3]); v4=a.ty(OpTypeVector,[flt,4])
    p_in_v3=a.ty(OpTypePointer,[SCInput,v3]); p_out_v4=a.ty(OpTypePointer,[SCOutput,v4])
    c1f=a.const(flt,[f32(1.0)])
    vCol=a.var(p_in_v3,SCInput); outc=a.var(p_out_v4,SCOutput)
    a.dec(vCol,DecLocation,[0]); a.dec(outc,DecLocation,[0])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    c=a.ins(OpLoad,v3,[vCol])
    r=a.ins(OpCompositeExtract,flt,[c,0]); g=a.ins(OpCompositeExtract,flt,[c,1]); b=a.ins(OpCompositeExtract,flt,[c,2])
    o=a.ins(OpCompositeConstruct,v4,[r,g,b,c1f])
    a.insv(OpStore,[outc,o])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelFragment,fn,"main",[vCol,outc])
    a.execmode(fn,7)   # OriginUpperLeft
    return a.binary()

def build_test():
    """Kontrollfluss-/ExtInst-Testshader (ExecModel Vertex, rein ueber Locations):
       in  vec4 tin (loc 0); out vec4 tout (loc 0)
       a = tin.x
       if (a > 1.0) y = a*2;  else y = a+3;          (Branch + Phi)
       n = normalize(tin.yzw);                        (Shuffle + ExtInst)
       sel = (a > 1.0) ? 2.0 : 3.0;                   (OpSelect)
       -- V2.5: mat2 M = [[a,y],[n.x,n.y]] (column-major); me = M[1][0] (2-Index-Extract)
          == n.x. Ausgabe wird ueber 4 verkettete OpCompositeInsert gebaut:
          tout = insert(sel,3, insert(n.y,2, insert(me,1, insert(y,0, vec4(0)))))
                = vec4(y, n.x, n.y, sel)  -- Referenz UNVERAENDERT, aber ein falscher
          Matrix-Stride (row- statt column-major) oder Insert-Offset faellt sofort auf.
       -- Review-Fix: n.x/n.y statt dot(n,n)==1.0 -> richtungsabhaengig, diskriminierend."""
    a=Asm(); a.cap(1); glsl=a.extimport("GLSL.std.450"); a.memmodel()
    void=a.ty(OpTypeVoid,[]); fnv=a.ty(OpTypeFunction,[void])
    flt=a.ty(OpTypeFloat,[32]); bl=a.ty(OpTypeBool,[]); i32=a.ty(OpTypeInt,[32,1])
    v2=a.ty(OpTypeVector,[flt,2]); v3=a.ty(OpTypeVector,[flt,3]); v4=a.ty(OpTypeVector,[flt,4])
    m2=a.ty(OpTypeMatrix,[v2,2])
    p_in=a.ty(OpTypePointer,[SCInput,v4]); p_out=a.ty(OpTypePointer,[SCOutput,v4])
    c0=a.const(flt,[f32(0.0)]); c1=a.const(flt,[f32(1.0)]); c2=a.const(flt,[f32(2.0)]); c3=a.const(flt,[f32(3.0)])
    i0=a.const(i32,[0]); i1=a.const(i32,[1])            # V2.6: dynamische Komponenten-Indizes
    tin=a.var(p_in,SCInput); tout=a.var(p_out,SCOutput)
    a.dec(tin,DecLocation,[0]); a.dec(tout,DecLocation,[0])
    fn=a.nid(); a.emit(a.funcs,OpFunction,[void,fn,0,fnv]); a.label()
    tv=a.ins(OpLoad,v4,[tin])
    av=a.ins(OpCompositeExtract,flt,[tv,0])
    cond=a.ins(OpFOrdGreaterThan,bl,[av,c1])
    l_then=a.label_id(); l_else=a.label_id(); l_merge=a.label_id()
    a.insv(OpSelectionMerge,[l_merge,0])
    a.insv(OpBranchConditional,[cond,l_then,l_else])
    a.label_place(l_then)
    y1=a.ins(OpFMul,flt,[av,c2]); a.insv(OpBranch,[l_merge])
    a.label_place(l_else)
    y2=a.ins(OpFAdd,flt,[av,c3]); a.insv(OpBranch,[l_merge])
    a.label_place(l_merge)
    y=a.ins(OpPhi,flt,[y1,l_then,y2,l_else])
    sw=a.ins(OpVectorShuffle,v3,[tv,tv,1,2,3])          # tin.yzw
    nrm=a.ins(OpExtInst,v3,[glsl,GLSL_Normalize,sw])    # normalize(tin.yzw)
    nx=a.ins(OpVectorExtractDynamic,flt,[nrm,i0])       # V2.6: nrm[0] dynamisch -- n.x
    ny=a.ins(OpVectorExtractDynamic,flt,[nrm,i1])       # V2.6: nrm[1] dynamisch -- n.y
    sel=a.ins(OpSelect,flt,[cond,c2,c3])
    # V2.5: mat2 (column-major) + 2-Index-Extract -> me == n.x
    col0=a.ins(OpCompositeConstruct,v2,[av,y])          # Spalte 0 = (a, y)
    # V2.6: Spalte 1 = (n.x, n.y) via dynamisches Insert in vec2(0,0)
    z2=a.ins(OpCompositeConstruct,v2,[c0,c0])
    ci0=a.ins(OpVectorInsertDynamic,v2,[z2,nx,i0])      # [nx, 0]
    col1=a.ins(OpVectorInsertDynamic,v2,[ci0,ny,i1])    # [nx, ny]
    mat=a.ins(OpCompositeConstruct,m2,[col0,col1])
    me=a.ins(OpCompositeExtract,flt,[mat,1,0])          # M[Spalte1][Zeile0] == n.x
    # V2.5: Ausgabe via 4 verkettete CompositeInsert (Basis = vec4(0))
    base=a.ins(OpCompositeConstruct,v4,[c0,c0,c0,c0])
    o0=a.ins(OpCompositeInsert,v4,[y,base,0])
    o1=a.ins(OpCompositeInsert,v4,[me,o0,1])            # me == n.x
    o2=a.ins(OpCompositeInsert,v4,[ny,o1,2])
    o=a.ins(OpCompositeInsert,v4,[sel,o2,3])
    a.insv(OpStore,[tout,o])
    a.insv(OpReturn,[]); a.emit(a.funcs,OpFunctionEnd,[])
    a.entry(ExecModelVertex,fn,"main",[tin,tout])
    return a.binary()

def ref_test(tin):
    a=tin[0]
    y = a*2.0 if a>1.0 else a+3.0
    n=tin[1:4]; ln=math.sqrt(sum(v*v for v in n))
    nn=[v/ln for v in n]                                # normalize(yzw)
    sel = 2.0 if a>1.0 else 3.0
    return [y, nn[0], nn[1], sel]                       # n.x/n.y diskriminieren Shuffle+Normalize

def ref_vert(mvp_cols, pos):
    p=[pos[0],pos[1],pos[2],1.0]
    return [sum(mvp_cols[c][r]*p[c] for c in range(4)) for r in range(4)]

def c_f(v):
    s = "%.9g" % v
    if not any(c in s for c in ".eE"):
        s += ".0"
    return s + "f"

def c_array(name, words):
    out=["static const unsigned %s[] = {" % name]
    for i in range(0,len(words),8):
        out.append("    "+", ".join("0x%08xu"%w for w in words[i:i+8])+",")
    out.append("};")
    return "\n".join(out)

def main():
    vert=build_vert(); frag=build_frag(); test=build_test(); inst=build_instanced(); fragubo=build_frag_ubo(); fragtex=build_frag_tex(); fragmrt=build_frag_mrt(); comp=build_compute(); fragfetch=build_frag_fetch(); fraggather=build_frag_gather(); compatomic=build_compute_atomic(); compimage=build_compute_image(); compsubgroup=build_compute_subgroup()
    with open("user/vk_vert.spv","wb") as f: f.write(struct.pack('<%dI'%len(vert),*vert))
    with open("user/vk_frag.spv","wb") as f: f.write(struct.pack('<%dI'%len(frag),*frag))

    # Referenz-Testvektoren
    mvp=[[1,0,0,0],[0,2,0,0],[0,0,1,0],[3,4,5,1]]   # Spalten (column-major): Skalierung y*2 + Translation (3,4,5)
    pos=[1.0,2.0,3.0]; col=[0.25,0.5,0.75]
    exp_pos=ref_vert(mvp,pos)
    # t1[0]=3.0 (NICHT 2.0): 2*2 == 2+2 wuerde eine FMul->FAdd-Mutation im Interpreter
    # ueberleben lassen -- der Mutations-Proof fand genau diese Testvektor-Schwaeche.
    t1=[3.0,1.0,2.0,2.0]; e1=ref_test(t1)
    t2=[0.5,0.0,3.0,4.0]; e2=ref_test(t2)

    h=["/* AUTOGENERIERT von tools/gen_spirv.py -- NICHT von Hand editieren. */",
       "#ifndef RPI_RTOS_VK_SHADERS_H","#define RPI_RTOS_VK_SHADERS_H","",
       c_array("spv_vert_words",vert),"",
       c_array("spv_frag_words",frag),"",
       c_array("spv_test_words",test),"",
       c_array("spv_inst_words",inst),"",
       c_array("spv_fragubo_words",fragubo),"",
       c_array("spv_fragtex_words",fragtex),"",
       c_array("spv_fragmrt_words",fragmrt),"",
       c_array("spv_comp_words",comp),"",
       c_array("spv_fragfetch_words",fragfetch),"",
       c_array("spv_fraggather_words",fraggather),"",
       c_array("spv_compatomic_words",compatomic),"",
       c_array("spv_compimage_words",compimage),"",
       c_array("spv_compsubgroup_words",compsubgroup),"",
       "/* Testvektoren (Python-Referenz; Vergleich mit Toleranz 1e-4). */",
       "static const float spv_tv_mvp_cols[16] = {%s};"%",".join(c_f(mvp[c][r]) for c in range(4) for r in range(4)),
       "static const float spv_tv_pos[3] = {%s};"%",".join(c_f(v) for v in pos),
       "static const float spv_tv_col[3] = {%s};"%",".join(c_f(v) for v in col),
       "static const float spv_tv_exp_pos[4] = {%s};"%",".join(c_f(v) for v in exp_pos),
       "static const float spv_tv_t1[4] = {%s};"%",".join(c_f(v) for v in t1),
       "static const float spv_tv_e1[4] = {%s};"%",".join(c_f(v) for v in e1),
       "static const float spv_tv_t2[4] = {%s};"%",".join(c_f(v) for v in t2),
       "static const float spv_tv_e2[4] = {%s};"%",".join(c_f(v) for v in e2),
       "","#endif",""]
    with open("user/vk_shaders.h","w") as f: f.write("\n".join(h))
    print("OK: vk_vert.spv (%d W), vk_frag.spv (%d W), vk_test (%d W), vk_inst (%d W), vk_shaders.h" %
          (len(vert),len(frag),len(test),len(inst)))

if __name__=="__main__":
    main()
