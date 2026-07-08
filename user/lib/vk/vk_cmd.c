/*
 * user/lib/vk/vk_cmd.c  --  Vulkan-Draw-Pfad: ShaderModule/PipelineLayout/
 * RenderPass/Framebuffer/GraphicsPipeline/CommandPool/CommandBuffer/Fence/Submit.
 *
 * vkQueueSubmit fuehrt SYNCHRON aus (Fences signalisieren beim Submit -- spez-legal,
 * Ausfuehrung erscheint einfach schon abgeschlossen): Command-Replay ->
 *   BeginRenderPass:  loadOp CLEAR fuellt Farb-/Tiefen-Attachment
 *   Draw:             SPIR-V-VERTEX-Shader je Vertex (vk_spirv.c) -> Clip-Space ->
 *                     r3d-Pipeline (Clipping/Top-Left-Rasterung/Depth, user/lib/r3d.c)
 *                     -> SPIR-V-FRAGMENT-Shader je Fragment -> Attachment-Write.
 * Subset (ehrlich): 1 Subpass (1 Farb- + optional 1 Tiefen-Attachment), Topologie
 * TRIANGLE_LIST, 1 Vertex-Binding (<=4 Attribute, R32G32B32(A)_SFLOAT), statischer
 * Viewport/Scissor, DepthCompare LESS, kein Blending/Index-Draw/Secondary-CB.
 * MIT FP kompiliert.
 */
#include "vk_rtos.h"
#include "vk_spirv.h"
#include "../r3d.h"

void *memset(void *dst, int c, unsigned long n);

/* ---- Pools ---- */
#define RT_MAX_SHADER   6
#define RT_SHADER_WORDS 512
#define RT_MAX_LAYOUT   4
#define RT_MAX_RP       4
#define RT_MAX_FB       4
#define RT_MAX_PIPE     4
#define RT_MAX_CMDPOOL  2
#define RT_MAX_CB       4
#define RT_MAX_CMDS     96
#define RT_MAX_VATTR    4
#define RT_PUSH_MAX     256   /* V1.10: 1.4-Pflicht maxPushConstantsSize=256 (war 128) */

typedef struct {
    int used;
    unsigned words[RT_SHADER_WORDS];   /* Kopie (pCode darf nach Create freigegeben werden) */
    unsigned nwords;
    spv_mod_t mod;                     /* geparst bei vkCreateShaderModule */
} rt_shader_t;

typedef struct { int used; unsigned push_bytes; } rt_layout_t;

#define RT_MAX_COLOR_ATT 8             /* V1.5: MRT (1.4-Limit maxColorAttachments=8) */

typedef struct {
    int used;
    int n_color;                       /* Anzahl Farb-Attachments (MRT) */
    VkFormat color_fmt[RT_MAX_COLOR_ATT];
    int color_clear[RT_MAX_COLOR_ATT]; /* loadOp == CLEAR je Farb-Attachment */
    int color_att[RT_MAX_COLOR_ATT];   /* Attachment-Index (fuer pClearValues[index]) */
    VkFormat depth_fmt;                /* UNDEFINED -> kein Depth */
    int depth_clear, depth_att;
} rt_rp_t;

typedef struct {
    int used;
    int n_color;
    rt_view_t *color[RT_MAX_COLOR_ATT];
    rt_view_t *depth;                  /* depth == 0 erlaubt */
    unsigned width, height;
} rt_fb_t;

typedef struct {
    int used;
    rt_shader_t *vs, *fs;
    unsigned stride;                   /* Vertex-Binding 0 */
    struct { unsigned loc, off, ncomp; } attr[RT_MAX_VATTR];
    unsigned nattr;
    float vp_x, vp_y, vp_w, vp_h, vp_minz, vp_maxz;
    int   sc_x, sc_y, sc_w, sc_h;
    int   cull_mode, front_ccw;
    int   depth_test, depth_write;
    int   depth_compare;               /* V3b: VkCompareOp (0-7); Default LESS */
    /* V1.1 Farb-Blending (Attachment 0). blend_enable==0 -> Overwrite. */
    int   blend_enable;
    unsigned src_col, dst_col, col_op;     /* VkBlendFactor/VkBlendOp */
    unsigned src_alpha, dst_alpha, alpha_op;
    unsigned write_mask;                   /* VkColorComponentFlags (R=1,G=2,B=4,A=8) */
    float blend_const[4];
    /* V1.7: Compute-Pipeline (is_compute==1 -> cs statt vs/fs, kein Rasterizer-Zustand). */
    int   is_compute;
    rt_shader_t *cs;
    int   samples;                         /* V1.6 MSAA: rasterizationSamples (1 oder 4) */
} rt_pipe_t;

typedef struct { int used; } rt_cmdpool_t;

typedef struct {
    unsigned op; unsigned long long a, b;
    float cclear[RT_MAX_COLOR_ATT][4]; float dclear;   /* Clear-Farbe je Farb-Attachment */
    int rx, ry, rw, rh;                /* renderArea (BeginRenderPass) */
    /* Draw-Parameter (C_DRAW): a=vertexCount/indexCount, b=firstVertex/firstIndex. */
    unsigned inst_count, first_inst;   /* Instancing */
    int      indexed, vertex_offset;   /* Indexed-Draw (V1.2) */
    unsigned itype;                    /* Index-Typ bei C_BINDIB (16/32/8) */
    unsigned long long d;              /* generisch (C_BINDDESC: Descriptor-Set-Handle) */
} rt_cmd_t;
enum { C_BEGINRP = 1, C_ENDRP, C_BINDPIPE, C_BINDVB, C_BINDIB, C_BINDDESC, C_PUSH, C_DRAW,
       C_FILLBUF, C_COPYBUF, C_CLEARCOLIMG,     /* V1.9: Transfer/Fill/Clear */
       C_COPYBUF2IMG, C_IMG2BUF, C_IMG2IMG, C_BLITIMG,  /* V3: Buffer<->Image, Image->Image, Blit */
       C_DRAWINDIRECT,                           /* V3: Indirect-Draw (+Count) */
       C_SETCULL, C_SETFRONT, C_SETDTEST, C_SETDWRITE,  /* V3: Extended-Dynamic-State */
       C_SETDISCARD, C_SETVP, C_SETSC, C_SETBLENDEN, C_SETWMASK,  /* V3b: EDS/EDS2/EDS3 */
       C_SETDCOMPARE,    /* V3b: dynamischer depthCompareOp */
       C_BEGINDYNR,      /* V3b: Dynamic-Rendering mit echtem Draw (transientes FB/RP) */
       C_BEGINQUERY, C_ENDQUERY, C_RESETQUERY, C_WRITETS, C_SETEVENT, C_RESETEVENT,  /* V1.8 */
       C_DISPATCH,       /* V1.7: Compute */
       C_RESOLVEIMG };   /* V1.6: MSAA-Resolve */

typedef struct {
    int used, recording;
    rt_cmd_t cmd[RT_MAX_CMDS];
    int ncmd;
    unsigned char push[RT_PUSH_MAX];              /* aktueller Push-Zustand beim Aufzeichnen */
    unsigned char dpush[RT_MAX_CMDS][RT_PUSH_MAX];/* Push-SNAPSHOT je Draw (Spez: Push gilt pro Draw) */
} rt_cb_t;

typedef struct { int used; int signaled; } rt_fence_t;

/* V1.3: Descriptor-Objekte. Layout/Pool sind blosse Handles; der Set haelt die aufgeloesten
 * Buffer je Binding (aus vkUpdateDescriptorSets). Subset: 1 Set (Set 0). */
#define RT_MAX_DSLAYOUT 4
#define RT_MAX_DPOOL    2
#define RT_MAX_DSET     8
#define RT_MAX_SAMPLER  4
typedef struct { int used; } rt_dslayout_t;
typedef struct { int used; } rt_dpool_t;
typedef struct { int used; int filter; int wrap; } rt_sampler_t;   /* V1.4 */
typedef struct {
    int used;
    const void *base[SPV_MAX_DESCRIPTOR];      /* Buffer-Memory je Binding (0 = nicht gesetzt) */
    unsigned    bytes[SPV_MAX_DESCRIPTOR];
    spv_tex_t   tex[SPV_MAX_DESCRIPTOR];        /* V1.4: gebundene Textur je Binding */
} rt_dset_t;

/* V1.8: Query-Pools (OCCLUSION zaehlt tiefentest-bestandene Fragmente, TIMESTAMP ein monotoner
 * Zaehler) + Events (einfache Signal-Flags). avail = Verfuegbarkeit fuer vkGetQueryPoolResults. */
#define RT_MAX_QUERYPOOL 4
#define RT_MAX_QUERIES   32
#define RT_MAX_EVENT     8
typedef struct {
    int used;
    unsigned type;                             /* VkQueryType (OCCLUSION/TIMESTAMP) */
    unsigned count;
    unsigned long long result[RT_MAX_QUERIES];
    unsigned char avail[RT_MAX_QUERIES];       /* 1 nach EndQuery/WriteTimestamp; 0 nach Reset */
} rt_querypool_t;
typedef struct { int used; int signaled; } rt_event_t;

static rt_shader_t  g_shader[RT_MAX_SHADER];
static rt_layout_t  g_layout[RT_MAX_LAYOUT];
static rt_rp_t      g_rp[RT_MAX_RP];
static rt_fb_t      g_fb[RT_MAX_FB];
static rt_pipe_t    g_pipe[RT_MAX_PIPE];
static rt_cmdpool_t g_cmdpool[RT_MAX_CMDPOOL];
static rt_cb_t      g_cb[RT_MAX_CB];
static rt_fence_t   g_fence[RT_MAX_CB];
static rt_dslayout_t g_dslayout[RT_MAX_DSLAYOUT];
static rt_dpool_t    g_dpool[RT_MAX_DPOOL];
static rt_dset_t     g_dset[RT_MAX_DSET];
static rt_sampler_t  g_sampler[RT_MAX_SAMPLER];
static rt_querypool_t g_querypool[RT_MAX_QUERYPOOL];   /* V1.8 */
static rt_event_t     g_event[RT_MAX_EVENT];           /* V1.8 */

/* V1.8: aktiver Occlusion-Zaehler (0 = keine Occlusion-Query offen) + monotoner Timestamp-Zaehler.
 * Einzelthread-EL0-Ausfuehrung (synchrones Submit) -> globaler Zustand ist unkritisch. */
static unsigned long long *g_occl_counter;
static unsigned long long  g_ts_counter;

#define ALLOC_SLOT(arr, n, out) do { (out) = 0; \
    for (int i_ = 0; i_ < (n); i_++) { if (!(arr)[i_].used) { (arr)[i_].used = 1; (out) = &(arr)[i_]; break; } } } while (0)

/* ---------------- Shader-Module ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pShaderModule) { return VK_ERROR_INITIALIZATION_FAILED; }
    unsigned nw = (unsigned)(pCreateInfo->codeSize / 4);
    if (nw == 0 || nw > RT_SHADER_WORDS) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    rt_shader_t *s;
    ALLOC_SLOT(g_shader, RT_MAX_SHADER, s);
    if (!s) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for (unsigned i = 0; i < nw; i++) { s->words[i] = pCreateInfo->pCode[i]; }
    s->nwords = nw;
    if (spv_parse(&s->mod, s->words, nw) != 0) {
        s->used = 0;
        return VK_ERROR_OUT_OF_HOST_MEMORY;            /* ungueltiges SPIR-V: fail-loud */
    }
    *pShaderModule = (VkShaderModule)(void *)s;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
    VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (shaderModule) { ((rt_shader_t *)(void *)shaderModule)->used = 0; }
}

/* ---------------- PipelineLayout ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout)
{
    (void)device; (void)pAllocator;
    rt_layout_t *l;
    ALLOC_SLOT(g_layout, RT_MAX_LAYOUT, l);
    if (!l) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    l->push_bytes = 0;
    if (pCreateInfo && pCreateInfo->pushConstantRangeCount >= 1) {
        const VkPushConstantRange *r = &pCreateInfo->pPushConstantRanges[0];
        l->push_bytes = r->offset + r->size;
        if (l->push_bytes > RT_PUSH_MAX) { l->used = 0; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    *pPipelineLayout = (VkPipelineLayout)(void *)l;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (pipelineLayout) { ((rt_layout_t *)(void *)pipelineLayout)->used = 0; }
}

/* ---------------- RenderPass ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pRenderPass || pCreateInfo->subpassCount != 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    rt_rp_t *rp;
    ALLOC_SLOT(g_rp, RT_MAX_RP, rp);
    if (!rp) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    rp->depth_fmt = VK_FORMAT_UNDEFINED;
    rp->depth_clear = 0; rp->depth_att = 1;
    rp->n_color = 0;
    const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[0];
    unsigned nc = sp->colorAttachmentCount;
    if (nc > RT_MAX_COLOR_ATT) { nc = RT_MAX_COLOR_ATT; }
    for (unsigned i = 0; i < nc; i++) {
        unsigned ai = sp->pColorAttachments[i].attachment;
        const VkAttachmentDescription *a = &pCreateInfo->pAttachments[ai];
        rp->color_fmt[i]   = a->format;
        rp->color_clear[i] = (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
        rp->color_att[i]   = (int)ai;             /* pClearValues nach Attachment-INDEX */
    }
    rp->n_color = (int)nc;
    if (sp->pDepthStencilAttachment) {
        unsigned ai = sp->pDepthStencilAttachment->attachment;
        const VkAttachmentDescription *a = &pCreateInfo->pAttachments[ai];
        rp->depth_fmt   = a->format;
        rp->depth_clear = (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
        rp->depth_att   = (int)ai;
    }
    *pRenderPass = (VkRenderPass)(void *)rp;
    return VK_SUCCESS;
}

/* V3: Core-1.2 vkCreateRenderPass2 -- uebersetzt VkRenderPassCreateInfo2 (AttachmentDescription2/
 * SubpassDescription2/AttachmentReference2, sType/pNext + gleiche Kernfelder) auf die verifizierte
 * 1.0-Erzeugung. Subset wie v1: 1 Subpass. */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(
    VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
    if (!pCreateInfo || !pRenderPass || pCreateInfo->subpassCount != 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkAttachmentDescription atts[RT_MAX_COLOR_ATT + 1];
    uint32_t na = pCreateInfo->attachmentCount;
    if (na > RT_MAX_COLOR_ATT + 1) { na = RT_MAX_COLOR_ATT + 1; }
    for (uint32_t i = 0; i < na; i++) {
        const VkAttachmentDescription2 *s = &pCreateInfo->pAttachments[i];
        memset(&atts[i], 0, sizeof(atts[i]));
        atts[i].format = s->format;               atts[i].samples = s->samples;
        atts[i].loadOp = s->loadOp;               atts[i].storeOp = s->storeOp;
        atts[i].stencilLoadOp = s->stencilLoadOp; atts[i].stencilStoreOp = s->stencilStoreOp;
        atts[i].initialLayout = s->initialLayout; atts[i].finalLayout = s->finalLayout;
    }
    const VkSubpassDescription2 *s2 = &pCreateInfo->pSubpasses[0];
    VkAttachmentReference colorRefs[RT_MAX_COLOR_ATT];
    uint32_t nc = s2->colorAttachmentCount;
    if (nc > RT_MAX_COLOR_ATT) { nc = RT_MAX_COLOR_ATT; }
    for (uint32_t i = 0; i < nc; i++) {
        colorRefs[i].attachment = s2->pColorAttachments[i].attachment;
        colorRefs[i].layout     = s2->pColorAttachments[i].layout;
    }
    VkAttachmentReference depthRef;
    VkSubpassDescription sp; memset(&sp, 0, sizeof(sp));
    sp.pipelineBindPoint = s2->pipelineBindPoint;
    sp.colorAttachmentCount = nc; sp.pColorAttachments = colorRefs;
    if (s2->pDepthStencilAttachment) {
        depthRef.attachment = s2->pDepthStencilAttachment->attachment;
        depthRef.layout     = s2->pDepthStencilAttachment->layout;
        sp.pDepthStencilAttachment = &depthRef;
    }
    VkRenderPassCreateInfo ci; memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = na; ci.pAttachments = atts;
    ci.subpassCount = 1; ci.pSubpasses = &sp;
    return vkCreateRenderPass(device, &ci, pAllocator, pRenderPass);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
    VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (renderPass) { ((rt_rp_t *)(void *)renderPass)->used = 0; }
}

/* ---------------- Framebuffer ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pFramebuffer || pCreateInfo->attachmentCount < 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    rt_fb_t *fb;
    ALLOC_SLOT(g_fb, RT_MAX_FB, fb);
    if (!fb) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    fb->n_color = 0; fb->depth = 0;
    for (int i = 0; i < RT_MAX_COLOR_ATT; i++) { fb->color[i] = 0; }
    fb->width = pCreateInfo->width; fb->height = pCreateInfo->height;
    /* Attachments in Reihenfolge: Farb-Views (D32 -> Depth). MRT: mehrere Farb-Attachments. */
    for (unsigned i = 0; i < pCreateInfo->attachmentCount; i++) {
        rt_view_t *v = rt_view_from_handle(pCreateInfo->pAttachments[i]);
        if (v && v->image) {
            if (v->format == VK_FORMAT_D32_SFLOAT) { fb->depth = v; }
            else if (fb->n_color < RT_MAX_COLOR_ATT) { fb->color[fb->n_color++] = v; }
        }
    }
    if (fb->n_color == 0) { fb->used = 0; return VK_ERROR_INITIALIZATION_FAILED; }
    *pFramebuffer = (VkFramebuffer)(void *)fb;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
    VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (framebuffer) { ((rt_fb_t *)(void *)framebuffer)->used = 0; }
}

/* ---------------- GraphicsPipeline ---------------- */
static unsigned fmt_ncomp(VkFormat f)
{
    if (f == VK_FORMAT_R32_SFLOAT)          { return 1; }
    if (f == VK_FORMAT_R32G32_SFLOAT)       { return 2; }
    if (f == VK_FORMAT_R32G32B32_SFLOAT)    { return 3; }
    if (f == VK_FORMAT_R32G32B32A32_SFLOAT) { return 4; }
    return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    (void)device; (void)pipelineCache; (void)pAllocator;
    if (!pCreateInfos || !pPipelines || createInfoCount != 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[0];
    rt_pipe_t *p;
    ALLOC_SLOT(g_pipe, RT_MAX_PIPE, p);
    if (!p) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memset(&p->vs, 0, sizeof(*p) - (unsigned long)((unsigned char *)&p->vs - (unsigned char *)p));

    for (unsigned i = 0; i < ci->stageCount; i++) {
        rt_shader_t *s = (rt_shader_t *)(void *)ci->pStages[i].module;
        if (ci->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT)   { p->vs = s; }
        if (ci->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) { p->fs = s; }
    }
    if (!p->vs || !p->fs) { p->used = 0; return VK_ERROR_INITIALIZATION_FAILED; }

    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    p->stride = 0; p->nattr = 0;
    if (vi && vi->vertexBindingDescriptionCount >= 1) {
        p->stride = vi->pVertexBindingDescriptions[0].stride;
    }
    if (vi) {
        for (unsigned i = 0; i < vi->vertexAttributeDescriptionCount && i < RT_MAX_VATTR; i++) {
            const VkVertexInputAttributeDescription *a = &vi->pVertexAttributeDescriptions[i];
            unsigned nc = fmt_ncomp(a->format);
            if (nc == 0 || a->location >= SPV_MAX_LOC) { p->used = 0; return VK_ERROR_FEATURE_NOT_PRESENT; }
            p->attr[p->nattr].loc = a->location;
            p->attr[p->nattr].off = a->offset;
            p->attr[p->nattr].ncomp = nc;
            p->nattr++;
        }
    }
    if (!ci->pInputAssemblyState ||
        ci->pInputAssemblyState->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
        p->used = 0;
        return VK_ERROR_FEATURE_NOT_PRESENT;           /* Subset: TRIANGLE_LIST */
    }
    const VkPipelineViewportStateCreateInfo *vp = ci->pViewportState;
    if (vp && vp->viewportCount >= 1 && vp->pViewports) {
        const VkViewport *v = &vp->pViewports[0];
        p->vp_x = v->x; p->vp_y = v->y; p->vp_w = v->width; p->vp_h = v->height;
        p->vp_minz = v->minDepth; p->vp_maxz = v->maxDepth;
    }
    if (vp && vp->scissorCount >= 1 && vp->pScissors) {
        const VkRect2D *s = &vp->pScissors[0];
        p->sc_x = s->offset.x; p->sc_y = s->offset.y;
        p->sc_w = (int)s->extent.width; p->sc_h = (int)s->extent.height;
    }
    const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
    p->cull_mode = R3D_CULL_NONE; p->front_ccw = 1;
    if (rs) {
        if (rs->cullMode == VK_CULL_MODE_BACK_BIT)  { p->cull_mode = R3D_CULL_BACK; }
        if (rs->cullMode == VK_CULL_MODE_FRONT_BIT) { p->cull_mode = R3D_CULL_FRONT; }
        p->front_ccw = (rs->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    }
    const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;
    p->depth_test = 0; p->depth_write = 0;
    p->depth_compare = VK_COMPARE_OP_LESS;             /* V3b: Default; alle 8 CompareOps unterstuetzt */
    if (ds) {
        p->depth_test  = (ds->depthTestEnable == VK_TRUE);
        p->depth_write = (ds->depthWriteEnable == VK_TRUE);
        if ((unsigned)ds->depthCompareOp <= VK_COMPARE_OP_ALWAYS) {
            p->depth_compare = (int)ds->depthCompareOp;
        }
    }
    /* V1.1: Farb-Blending (Attachment 0). Ohne State oder blendEnable=FALSE -> Overwrite. */
    p->blend_enable = 0;
    p->write_mask = 0xF;                                /* RGBA default */
    p->src_col = VK_BLEND_FACTOR_ONE; p->dst_col = VK_BLEND_FACTOR_ZERO;
    p->col_op = VK_BLEND_OP_ADD;
    p->src_alpha = VK_BLEND_FACTOR_ONE; p->dst_alpha = VK_BLEND_FACTOR_ZERO;
    p->alpha_op = VK_BLEND_OP_ADD;
    p->blend_const[0] = p->blend_const[1] = p->blend_const[2] = p->blend_const[3] = 0.0f;
    const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
    if (cb && cb->attachmentCount >= 1 && cb->pAttachments) {
        const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[0];
        p->write_mask   = a->colorWriteMask;
        p->blend_enable = (a->blendEnable == VK_TRUE);
        p->src_col = a->srcColorBlendFactor; p->dst_col = a->dstColorBlendFactor;
        p->col_op  = a->colorBlendOp;
        p->src_alpha = a->srcAlphaBlendFactor; p->dst_alpha = a->dstAlphaBlendFactor;
        p->alpha_op  = a->alphaBlendOp;
        for (int i = 0; i < 4; i++) { p->blend_const[i] = cb->blendConstants[i]; }
        /* Subset: nur die additiven Blend-Ops (ADD/SUBTRACT/REVERSE_SUBTRACT/MIN/MAX). */
        if ((p->col_op > VK_BLEND_OP_MAX) || (p->alpha_op > VK_BLEND_OP_MAX)) {
            p->used = 0;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    /* V1.6 MSAA: rasterizationSamples (1 oder 4). */
    p->samples = 1;
    if (ci->pMultisampleState && ci->pMultisampleState->rasterizationSamples == VK_SAMPLE_COUNT_4_BIT) {
        p->samples = 4;
    }
    pPipelines[0] = (VkPipeline)(void *)p;
    return VK_SUCCESS;
}

/* V1.7: Compute-Pipeline -- 1 Compute-Stage, kein Rasterizer-Zustand. */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    (void)device; (void)pipelineCache; (void)pAllocator;
    if (!pCreateInfos || !pPipelines || createInfoCount != 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkComputePipelineCreateInfo *ci = &pCreateInfos[0];
    rt_pipe_t *p;
    ALLOC_SLOT(g_pipe, RT_MAX_PIPE, p);
    if (!p) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memset(&p->vs, 0, sizeof(*p) - (unsigned long)((unsigned char *)&p->vs - (unsigned char *)p));
    if (ci->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT) { p->used = 0; return VK_ERROR_INITIALIZATION_FAILED; }
    p->cs = (rt_shader_t *)(void *)ci->stage.module;
    if (!p->cs) { p->used = 0; return VK_ERROR_INITIALIZATION_FAILED; }
    p->is_compute = 1;
    pPipelines[0] = (VkPipeline)(void *)p;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(
    VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (pipeline) { ((rt_pipe_t *)(void *)pipeline)->used = 0; }
}

/* ---------------- Sampler (V1.4) ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
    (void)device; (void)pAllocator;
    rt_sampler_t *s;
    ALLOC_SLOT(g_sampler, RT_MAX_SAMPLER, s);
    if (!s) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    s->filter = (pCreateInfo && pCreateInfo->magFilter == VK_FILTER_LINEAR) ? 1 : 0;
    s->wrap   = (pCreateInfo && pCreateInfo->addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) ? 1 : 0;
    *pSampler = (VkSampler)(void *)s;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(
    VkDevice device, VkSampler sampler, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (sampler) { ((rt_sampler_t *)(void *)sampler)->used = 0; }
}

/* ---------------- Descriptor-Sets (V1.3) ---------------- */
static rt_cmd_t *cb_push(VkCommandBuffer h);   /* unten definiert (Command-Recording) */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    rt_dslayout_t *l;
    ALLOC_SLOT(g_dslayout, RT_MAX_DSLAYOUT, l);
    if (!l) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    *pSetLayout = (VkDescriptorSetLayout)(void *)l;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (descriptorSetLayout) { ((rt_dslayout_t *)(void *)descriptorSetLayout)->used = 0; }
}

/* V3: Core-1.1 vkGetDescriptorSetLayoutSupport -- meldet ehrlich, ob dieses Layout im Subset
 * erzeugbar ist (bis SPV_MAX_DESCRIPTOR Bindings, Set 0). */
VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSupport(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
    VkDescriptorSetLayoutSupport *pSupport)
{
    (void)device;
    if (!pSupport) { return; }
    pSupport->supported = (pCreateInfo && pCreateInfo->bindingCount <= SPV_MAX_DESCRIPTOR)
                              ? VK_TRUE : VK_FALSE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    rt_dpool_t *p;
    ALLOC_SLOT(g_dpool, RT_MAX_DPOOL, p);
    if (!p) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    *pDescriptorPool = (VkDescriptorPool)(void *)p;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (descriptorPool) { ((rt_dpool_t *)(void *)descriptorPool)->used = 0; }
    /* Alle Sets des Pools freigeben (Subset: alle). */
    for (int i = 0; i < RT_MAX_DSET; i++) { g_dset[i].used = 0; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *pAllocateInfo, VkDescriptorSet *pDescriptorSets)
{
    (void)device;
    if (!pAllocateInfo || !pDescriptorSets) { return VK_ERROR_INITIALIZATION_FAILED; }
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        rt_dset_t *s;
        ALLOC_SLOT(g_dset, RT_MAX_DSET, s);
        if (!s) { return VK_ERROR_OUT_OF_POOL_MEMORY; }
        for (int b = 0; b < SPV_MAX_DESCRIPTOR; b++) { s->base[b] = 0; s->bytes[b] = 0; }
        memset(s->tex, 0, sizeof(s->tex));
        pDescriptorSets[i] = (VkDescriptorSet)(void *)s;
    }
    return VK_SUCCESS;
}

/* Einen VkWriteDescriptorSet auf einen konkreten rt_dset_t anwenden (aus vkUpdateDescriptorSets
 * UND vkCmdPushDescriptorSetKHR genutzt -- identische Aufloesung von Buffer/Textur je Binding). */
static void dset_apply_write(rt_dset_t *s, const VkWriteDescriptorSet *w)
{
    if (!s || w->dstBinding >= SPV_MAX_DESCRIPTOR) { return; }
    if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && w->pImageInfo) {
        const VkDescriptorImageInfo *ii = &w->pImageInfo[0];
        rt_view_t *view = rt_view_from_handle(ii->imageView);
        rt_sampler_t *smp = (rt_sampler_t *)(void *)ii->sampler;
        spv_tex_t *tx = &s->tex[w->dstBinding];
        tx->pixels = 0;
        if (view && view->image && view->image->mem) {
            tx->pixels   = rt_image_pixels(view->image);
            tx->w        = view->image->extent.width;
            tx->h        = view->image->extent.height;
            tx->pitch_px = (unsigned)(view->image->row_pitch / 4);
            tx->filter   = smp ? smp->filter : 0;
            tx->wrap     = smp ? smp->wrap : 0;
        }
        return;
    }
    if (w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && w->pImageInfo) {
        const VkDescriptorImageInfo *ii = &w->pImageInfo[0];
        rt_view_t *view = rt_view_from_handle(ii->imageView);
        spv_tex_t *tx = &s->tex[w->dstBinding];
        tx->pixels = 0;
        if (view && view->image && view->image->mem) {
            tx->pixels   = rt_image_pixels(view->image);
            tx->w        = view->image->extent.width;
            tx->h        = view->image->extent.height;
            tx->pitch_px = (unsigned)(view->image->row_pitch / 4);
            tx->filter   = 0;
            tx->wrap     = 0;
        }
        return;
    }
    if (!w->pBufferInfo) { return; }
    const VkDescriptorBufferInfo *bi = &w->pBufferInfo[0];
    rt_buffer_t *b = rt_buffer_from_handle(bi->buffer);
    if (!b || !b->mem) { return; }
    VkDeviceSize range = (bi->range == VK_WHOLE_SIZE) ? (b->size - bi->offset) : bi->range;
    s->base[w->dstBinding]  = b->mem->base + b->off + bi->offset;
    s->bytes[w->dstBinding] = (unsigned)range;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites,
    uint32_t descriptorCopyCount, const VkCopyDescriptorSet *pDescriptorCopies)
{
    (void)device; (void)descriptorCopyCount; (void)pDescriptorCopies;
    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
        dset_apply_write((rt_dset_t *)(void *)w->dstSet, w);
    }
}

/* V3b: Core-1.4/KHR_push_descriptor -- vkCmdPushDescriptorSetKHR. Ohne Set-Objekt: die Writes
 * werden in einen transienten rt_dset_t (rotierender Pool) aufgeloest und wie ein gebundenes Set
 * fuer die folgenden Draws verwendet. Subset: Set 0; jeder Push startet ein frisches transientes
 * Set (kein Merge mehrerer Push-Aufrufe -- dokumentierte Subset-Grenze). Der Pool ueberlebt bis
 * zum Submit (Werte werden erst dort gelesen). */
#define RT_MAX_PUSHDSET 4
static rt_dset_t g_pushdset[RT_MAX_PUSHDSET];
static int       g_pushdset_idx;
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites)
{
    (void)pipelineBindPoint; (void)layout;
    if (set != 0 || descriptorWriteCount < 1 || !pDescriptorWrites) { return; }   /* Subset: Set 0 */
    rt_dset_t *s = &g_pushdset[g_pushdset_idx];
    g_pushdset_idx = (g_pushdset_idx + 1) % RT_MAX_PUSHDSET;
    memset(s, 0, sizeof(*s));
    s->used = 1;
    for (uint32_t i = 0; i < descriptorWriteCount; i++) { dset_apply_write(s, &pDescriptorWrites[i]); }
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_BINDDESC; c->d = (unsigned long long)(unsigned long)(void *)s; }
}
/* Core-1.4-Name (Supplement-Header) -> auf die KHR-Variante gemappt. */
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites)
{
    vkCmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set,
                              descriptorWriteCount, pDescriptorWrites);
}

/* V3: Core-1.1 Descriptor-Update-Templates -- speichern die Entries und wenden sie an, indem
 * pro Entry aus pData (offset/stride) ein VkWriteDescriptorSet gebaut + an das verifizierte
 * vkUpdateDescriptorSets gereicht wird. */
typedef struct {
    int used;
    uint32_t count;
    VkDescriptorUpdateTemplateEntry entries[SPV_MAX_DESCRIPTOR];
} rt_updtempl_t;
static rt_updtempl_t g_updtempl[4];

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pDescriptorUpdateTemplate) { return VK_ERROR_INITIALIZATION_FAILED; }
    for (int i = 0; i < 4; i++) {
        if (!g_updtempl[i].used) {
            rt_updtempl_t *t = &g_updtempl[i];
            uint32_t n = pCreateInfo->descriptorUpdateEntryCount;
            if (n > SPV_MAX_DESCRIPTOR) { n = SPV_MAX_DESCRIPTOR; }
            t->used = 1; t->count = n;
            for (uint32_t k = 0; k < n; k++) { t->entries[k] = pCreateInfo->pDescriptorUpdateEntries[k]; }
            *pDescriptorUpdateTemplate = (VkDescriptorUpdateTemplate)(void *)t;
            return VK_SUCCESS;
        }
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (descriptorUpdateTemplate) { ((rt_updtempl_t *)(void *)descriptorUpdateTemplate)->used = 0; }
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplate updateTemplate,
    const void *pData)
{
    rt_updtempl_t *t = (rt_updtempl_t *)(void *)updateTemplate;
    if (!t || !t->used || !pData) { return; }
    for (uint32_t i = 0; i < t->count; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        for (uint32_t j = 0; j < e->descriptorCount; j++) {
            const char *src = (const char *)pData + e->offset + (unsigned long)j * e->stride;
            VkWriteDescriptorSet w; memset(&w, 0, sizeof(w));
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = descriptorSet; w.dstBinding = e->dstBinding;
            w.dstArrayElement = e->dstArrayElement + j;
            w.descriptorCount = 1; w.descriptorType = e->descriptorType;
            switch (e->descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                w.pBufferInfo = (const VkDescriptorBufferInfo *)(const void *)src; break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_SAMPLER:
                w.pImageInfo = (const VkDescriptorImageInfo *)(const void *)src; break;
            default: continue;
            }
            vkUpdateDescriptorSets(device, 1, &w, 0, 0);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets)
{
    (void)pipelineBindPoint; (void)layout; (void)dynamicOffsetCount; (void)pDynamicOffsets;
    if (firstSet != 0 || descriptorSetCount < 1) { return; }   /* Subset: Set 0 */
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_BINDDESC; c->d = (unsigned long long)(unsigned long)(void *)pDescriptorSets[0]; }
}

/* ---------------- CommandPool / CommandBuffer ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    rt_cmdpool_t *cp;
    ALLOC_SLOT(g_cmdpool, RT_MAX_CMDPOOL, cp);
    if (!cp) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    *pCommandPool = (VkCommandPool)(void *)cp;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (commandPool) { ((rt_cmdpool_t *)(void *)commandPool)->used = 0; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
    VkCommandBuffer *pCommandBuffers)
{
    (void)device;
    if (!pAllocateInfo || pAllocateInfo->commandBufferCount != 1) {
        return VK_ERROR_INITIALIZATION_FAILED;         /* Subset: 1 je Aufruf */
    }
    rt_cb_t *cb;
    ALLOC_SLOT(g_cb, RT_MAX_CB, cb);
    if (!cb) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    cb->ncmd = 0; cb->recording = 0;
    pCommandBuffers[0] = (VkCommandBuffer)(void *)cb;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
    const VkCommandBuffer *pCommandBuffers)
{
    (void)device; (void)commandPool;
    for (uint32_t i = 0; i < commandBufferCount; i++) {
        if (pCommandBuffers[i]) { ((rt_cb_t *)(void *)pCommandBuffers[i])->used = 0; }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
    (void)pBeginInfo;
    rt_cb_t *cb = (rt_cb_t *)(void *)commandBuffer;
    cb->ncmd = 0;
    cb->recording = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    ((rt_cb_t *)(void *)commandBuffer)->recording = 0;
    return VK_SUCCESS;
}

static rt_cmd_t *cb_push(VkCommandBuffer h)
{
    rt_cb_t *cb = (rt_cb_t *)(void *)h;
    if (!cb->recording || cb->ncmd >= RT_MAX_CMDS) { return 0; }
    rt_cmd_t *c = &cb->cmd[cb->ncmd++];
    memset(c, 0, sizeof(*c));
    return c;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
    VkSubpassContents contents)
{
    (void)contents;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (!c || !pRenderPassBegin) { return; }
    c->op = C_BEGINRP;
    c->a = (unsigned long long)(unsigned long)(void *)pRenderPassBegin->renderPass;
    c->b = (unsigned long long)(unsigned long)(void *)pRenderPassBegin->framebuffer;
    for (int a = 0; a < RT_MAX_COLOR_ATT; a++) {
        c->cclear[a][0] = 0; c->cclear[a][1] = 0; c->cclear[a][2] = 0; c->cclear[a][3] = 1;
    }
    c->dclear = 1.0f;
    /* renderArea (Spez: loadOp CLEAR fuellt NUR die renderArea). */
    c->rx = pRenderPassBegin->renderArea.offset.x;
    c->ry = pRenderPassBegin->renderArea.offset.y;
    c->rw = (int)pRenderPassBegin->renderArea.extent.width;
    c->rh = (int)pRenderPassBegin->renderArea.extent.height;
    const rt_rp_t *rp = (const rt_rp_t *)(void *)pRenderPassBegin->renderPass;
    /* ClearValues nach ATTACHMENT-INDEX (Spez), je Farb-Attachment (MRT). */
    if (rp) {
        for (int a = 0; a < rp->n_color; a++) {
            if (rp->color_att[a] >= 0 && (unsigned)rp->color_att[a] < pRenderPassBegin->clearValueCount) {
                for (int i = 0; i < 4; i++) {
                    c->cclear[a][i] = pRenderPassBegin->pClearValues[rp->color_att[a]].color.float32[i];
                }
            }
        }
        if (rp->depth_fmt != VK_FORMAT_UNDEFINED && rp->depth_att >= 0 &&
            (unsigned)rp->depth_att < pRenderPassBegin->clearValueCount) {
            c->dclear = pRenderPassBegin->pClearValues[rp->depth_att].depthStencil.depth;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_ENDRP; }
}

/* V3: Core-1.2 RenderPass2-Kommandos -- Begin/End nutzen die identische VkRenderPassBeginInfo;
 * die Subpass-Begin/End-Infos tragen nur Contents (1 Subpass -> NextSubpass2 No-op). */
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
    const VkSubpassBeginInfo *pSubpassBeginInfo)
{
    vkCmdBeginRenderPass(commandBuffer, pRenderPassBegin,
                         pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE);
}
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(
    VkCommandBuffer commandBuffer, const VkSubpassBeginInfo *pB, const VkSubpassEndInfo *pE)
{
    (void)commandBuffer; (void)pB; (void)pE;           /* Subset: 1 Subpass */
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(
    VkCommandBuffer commandBuffer, const VkSubpassEndInfo *pSubpassEndInfo)
{
    (void)pSubpassEndInfo;
    vkCmdEndRenderPass(commandBuffer);
}

/* V3b: Core-1.3 Dynamic Rendering -- ohne RenderPass/Framebuffer-Objekte direkt in Image-Views,
 * MIT echter Draw-Anbindung. C_BEGINDYNR baut zur Submit-Zeit ein transientes FB/RP (Farb-
 * Attachment 0 + optional Depth) auf, ueber das C_DRAW dann genauso rendert wie im RenderPass.
 * loadOp CLEAR wird beim Begin auf die renderArea angewandt; storeOp ist implizit (synchrone
 * CPU-Impl -> direkter Store). Subset: der Draw schreibt Farb-Attachment 0 (+ Depth); weitere
 * Farb-Attachments (MRT-via-Dynamic-Rendering) werden geclbt, aber nicht bezeichnet -- MRT-Draws
 * laufen ueber den regulaeren RenderPass-Pfad (dokumentierte Subset-Grenze). */
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(
    VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo)
{
    if (!pRenderingInfo) { return; }
    rt_view_t *cv0 = 0, *dv = 0;
    int clear_c0 = 0, clear_d = 0;
    float ccol[4] = { 0, 0, 0, 0 }, cdep = 1.0f;
    if (pRenderingInfo->colorAttachmentCount >= 1 && pRenderingInfo->pColorAttachments) {
        const VkRenderingAttachmentInfo *at = &pRenderingInfo->pColorAttachments[0];
        if (at->imageView) {
            cv0 = rt_view_from_handle(at->imageView);
            clear_c0 = (at->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
            for (int k = 0; k < 4; k++) { ccol[k] = at->clearValue.color.float32[k]; }
        }
    }
    if (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView) {
        const VkRenderingAttachmentInfo *at = pRenderingInfo->pDepthAttachment;
        dv = rt_view_from_handle(at->imageView);
        clear_d = (at->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
        cdep = at->clearValue.depthStencil.depth;
    }
    if (!cv0 || !cv0->image) { return; }
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_BEGINDYNR;
        c->a = (unsigned long long)(unsigned long)(void *)cv0;
        c->b = (unsigned long long)(unsigned long)(void *)dv;
        c->d = (unsigned)((clear_c0 ? 1u : 0u) | (clear_d ? 2u : 0u));
        for (int k = 0; k < 4; k++) { c->cclear[0][k] = ccol[k]; }
        c->dclear = cdep;
        c->rx = pRenderingInfo->renderArea.offset.x;
        c->ry = pRenderingInfo->renderArea.offset.y;
        c->rw = (int)pRenderingInfo->renderArea.extent.width;
        c->rh = (int)pRenderingInfo->renderArea.extent.height;
    }
    /* Zusaetzliche Farb-Attachments (1..n-1): nur loadOp CLEAR anwenden (kein Draw-Ziel). */
    for (uint32_t i = 1; i < pRenderingInfo->colorAttachmentCount; i++) {
        const VkRenderingAttachmentInfo *at = &pRenderingInfo->pColorAttachments[i];
        if (at->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR || !at->imageView) { continue; }
        rt_view_t *v = rt_view_from_handle(at->imageView);
        if (!v || !v->image) { continue; }
        rt_cmd_t *cc = cb_push(commandBuffer);
        if (!cc) { return; }
        cc->op = C_CLEARCOLIMG;
        cc->a = (unsigned long long)(unsigned long)(void *)v->image;
        for (int k = 0; k < 4; k++) { cc->cclear[0][k] = at->clearValue.color.float32[k]; }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_ENDRP; }                       /* FB/RP zuruecksetzen (synchroner Store) */
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_BINDPIPE; c->a = (unsigned long long)(unsigned long)(void *)pipeline;
        c->b = (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) ? 1 : 0;   /* V1.7 */
    }
}

/* V1.7: Compute-Dispatch aufzeichnen (Gruppenzahl je Dimension). */
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(
    VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_DISPATCH; c->a = groupCountX; c->rx = (int)groupCountY; c->ry = (int)groupCountZ; }
}

/* V3: Core-1.1 Device-Group-Kommandos. Ein-Geraet-System: SetDeviceMask ist No-op, und
 * DispatchBase wird nur mit Basis 0 aufgerufen (kein Multi-Device-Split) -> = vkCmdDispatch. */
VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask)
{
    (void)commandBuffer; (void)deviceMask;
}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBase(
    VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ,
    uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    (void)baseGroupX; (void)baseGroupY; (void)baseGroupZ;   /* Basis 0 im Ein-Geraet-Subset */
    vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer *pBuffers, const VkDeviceSize *pOffsets)
{
    /* Subset: nur Binding 0 wird gezeichnet. Ein Bind, der Binding 0 NICHT einschliesst,
     * darf ihn NICHT still ueberschreiben -> ignorieren (statt Binding 1 als 0 zu binden). */
    if (firstBinding > 0 || bindingCount < 1) { return; }
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_BINDVB;
        c->a = (unsigned long long)(unsigned long)(void *)pBuffers[0];
        c->b = pOffsets ? pOffsets[0] : 0;
    }
}

/* V3: Core-1.3 Extended-Dynamic-State -- vkCmdBindVertexBuffers2 delegiert an die 1.0-Variante;
 * pSizes/pStrides sind im Subset irrelevant (Vertex-Stride kommt aus der Pipeline). */
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(
    VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer *pBuffers, const VkDeviceSize *pOffsets,
    const VkDeviceSize *pSizes, const VkDeviceSize *pStrides)
{
    (void)pSizes; (void)pStrides;
    vkCmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(
    VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags,
    uint32_t offset, uint32_t size, const void *pValues)
{
    (void)layout; (void)stageFlags;
    rt_cb_t *cb = (rt_cb_t *)(void *)commandBuffer;
    if (!cb->recording || offset + size > RT_PUSH_MAX) { return; }
    const unsigned char *src = (const unsigned char *)pValues;
    for (uint32_t i = 0; i < size; i++) { cb->push[offset + i] = src[i]; }
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_PUSH; }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_BINDIB;
        c->a = (unsigned long long)(unsigned long)(void *)buffer;
        c->b = offset;
        /* 16=VK_INDEX_TYPE_UINT16(0), 32=UINT32(1), 8=UINT8(1000265000). Normieren auf 2/4/1. */
        c->itype = (indexType == VK_INDEX_TYPE_UINT32) ? 4u
                 : (indexType == VK_INDEX_TYPE_UINT16) ? 2u : 1u;
    }
}

/* V3.4: Core-1.4 vkCmdBindIndexBuffer2 -- wie 1.0 + Groessen-Parameter; im Subset ganzer Puffer
 * (robustBufferAccess klemmt Index-Zugriffe), size ignoriert. */
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
    VkIndexType indexType)
{
    (void)size;
    vkCmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);
}
/* V3.4: Core-1.4 vkCmdPushConstants2 -- Info-Struct auf die 1.0-Variante uebersetzt. */
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2(
    VkCommandBuffer commandBuffer, const VkPushConstantsInfo *pInfo)
{
    if (pInfo) {
        vkCmdPushConstants(commandBuffer, pInfo->layout, pInfo->stageFlags,
                           pInfo->offset, pInfo->size, pInfo->pValues);
    }
}
/* V3.4: Core-1.4 vkCmdBindDescriptorSets2 -- Bindpoint aus dem Subset (ignoriert), Rest aus dem Info. */
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2(
    VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo *pInfo)
{
    if (pInfo) {
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pInfo->layout,
                                pInfo->firstSet, pInfo->descriptorSetCount, pInfo->pDescriptorSets,
                                pInfo->dynamicOffsetCount, pInfo->pDynamicOffsets);
    }
}
/* V3.4: Core-1.4 vkCmdSetLineStipple -- Subset zeichnet keine Linien-Primitive (nur Dreiecke) ->
 * der Stipple-Zustand hat keinen Render-Effekt (vacuously korrekt). */
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStipple(
    VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern)
{
    (void)commandBuffer; (void)lineStippleFactor; (void)lineStipplePattern;
}

/* V3: Core-1.3 Extended-Dynamic-State -- Setter zeichnen ihren Wert als Kommando auf; der Executor
 * setzt den passenden g_dyn_*-Override, der ab dem naechsten Draw den Pipeline-State ueberschreibt. */
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETCULL; c->a = cullMode; }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETFRONT; c->a = (unsigned)frontFace; }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETDTEST; c->a = depthTestEnable ? 1u : 0u; }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETDWRITE; c->a = depthWriteEnable ? 1u : 0u; }
}

/* V3b: Extended-Dynamic-State (Core 1.3) -- rasterizerDiscard: bei TRUE werden keine Fragmente
 * erzeugt (die Rasterisierung wird verworfen). Beobachtbarer Effekt: der Draw hinterlaesst nichts. */
VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(
    VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETDISCARD; c->a = rasterizerDiscardEnable ? 1u : 0u; }
}

/* V3b: dynamischer Viewport (Core 1.0 vkCmdSetViewport + Core 1.3 WithCount). Subset: Viewport 0.
 * x,y,w,h + minz,maxz werden im Kommando gefuehrt und ueberschreiben den gebackenen Pipeline-Viewport. */
static void set_viewport_record(VkCommandBuffer commandBuffer, const VkViewport *v)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_SETVP;
        c->cclear[0][0] = v->x; c->cclear[0][1] = v->y;
        c->cclear[0][2] = v->width; c->cclear[0][3] = v->height;
        c->cclear[1][0] = v->minDepth; c->cclear[1][1] = v->maxDepth;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
    VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
    const VkViewport *pViewports)
{
    if (firstViewport > 0 || viewportCount < 1 || !pViewports) { return; }  /* Subset: Viewport 0 */
    set_viewport_record(commandBuffer, &pViewports[0]);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(
    VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport *pViewports)
{
    if (viewportCount < 1 || !pViewports) { return; }
    set_viewport_record(commandBuffer, &pViewports[0]);
}

/* V3b: dynamisches Scissor (Core 1.0 vkCmdSetScissor + Core 1.3 WithCount). Subset: Scissor 0. */
static void set_scissor_record(VkCommandBuffer commandBuffer, const VkRect2D *s)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_SETSC;
        c->rx = s->offset.x; c->ry = s->offset.y;
        c->rw = (int)s->extent.width; c->rh = (int)s->extent.height;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
    VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount,
    const VkRect2D *pScissors)
{
    if (firstScissor > 0 || scissorCount < 1 || !pScissors) { return; }     /* Subset: Scissor 0 */
    set_scissor_record(commandBuffer, &pScissors[0]);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(
    VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D *pScissors)
{
    if (scissorCount < 1 || !pScissors) { return; }
    set_scissor_record(commandBuffer, &pScissors[0]);
}

/* V3b: EDS3 (VK_EXT_extended_dynamic_state3) dynamisches Blending fuer Attachment 0.
 * SetColorBlendEnable schaltet das Blending, SetColorWriteMask die Kanalmaske; beide
 * ueberschreiben den gebackenen Pipeline-Zustand ab dem naechsten Draw. */
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEnableEXT(
    VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
    const VkBool32 *pColorBlendEnables)
{
    if (firstAttachment > 0 || attachmentCount < 1 || !pColorBlendEnables) { return; }  /* Attachment 0 */
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETBLENDEN; c->a = pColorBlendEnables[0] ? 1u : 0u; }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteMaskEXT(
    VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
    const VkColorComponentFlags *pColorWriteMasks)
{
    if (firstAttachment > 0 || attachmentCount < 1 || !pColorWriteMasks) { return; }
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETWMASK; c->a = (unsigned)pColorWriteMasks[0]; }
}

/* V3b: dynamischer depthCompareOp (Core 1.3). Greift REAL: r3d vergleicht nach VkCompareOp. */
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c && (unsigned)depthCompareOp <= VK_COMPARE_OP_ALWAYS) { c->op = C_SETDCOMPARE; c->a = (unsigned)depthCompareOp; }
}

/* V3b: Rest der Extended-Dynamic-State-Setter (Core 1.3/EDS2/EDS3). Im Dreiecks-Subset ohne
 * Stencil-/Depth-Bounds-/Depth-Bias-/Linien-Zustand haben diese KEINEN Render-Effekt (vacuously
 * korrekt) -- sie vervollstaendigen die API-Oberflaeche und schlucken ihren Zustand folgenlos.
 * primitiveTopology bleibt TRIANGLE_LIST (Subset); primitiveRestart betrifft nur Strips. */
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t) { (void)cb; (void)t; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 e) { (void)cb; (void)e; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 e) { (void)cb; (void)e; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer cb, float a, float b, float c) { (void)cb; (void)a; (void)b; (void)c; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer cb, VkBool32 e) { (void)cb; (void)e; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer cb, float a, float b) { (void)cb; (void)a; (void)b; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 e) { (void)cb; (void)e; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp c, VkCompareOp d) { (void)cb; (void)f; (void)a; (void)b; (void)c; (void)d; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) { (void)cb; (void)f; (void)m; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) { (void)cb; (void)f; (void)m; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t r) { (void)cb; (void)f; (void)r; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer cb, float w) { (void)cb; (void)w; }
/* EDS3-Blending: dynamisch greifen colorBlendEnable + colorWriteMask (s.o.); Blend-Equation und
 * Blend-Constants nutzen den gebackenen Pipeline-Wert (dokumentierte Subset-Grenze). */
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEquationEXT(VkCommandBuffer cb, uint32_t fa, uint32_t ac, const VkColorBlendEquationEXT *e) { (void)cb; (void)fa; (void)ac; (void)e; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) { (void)cb; (void)c; }

/* vkCmdBindIndexBuffer2 (1.4/maintenance5) folgt mit dem Header-Bump in Block V3.4. */

static void draw_record(VkCommandBuffer commandBuffer, int indexed, unsigned count,
                        unsigned first, unsigned inst_count, unsigned first_inst, int vertex_offset)
{
    rt_cb_t *cb = (rt_cb_t *)(void *)commandBuffer;
    if (!cb->recording || inst_count == 0 || count == 0) { return; }
    int idx = cb->ncmd;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_DRAW; c->a = count; c->b = first;
        c->indexed = indexed; c->inst_count = inst_count;
        c->first_inst = first_inst; c->vertex_offset = vertex_offset;
        /* Push-Constants PRO Draw einfrieren. */
        for (int i = 0; i < RT_PUSH_MAX; i++) { cb->dpush[idx][i] = cb->push[i]; }
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(
    VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance)
{
    draw_record(commandBuffer, 0, vertexCount, firstVertex, instanceCount, firstInstance, 0);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    draw_record(commandBuffer, 1, indexCount, firstIndex, instanceCount, firstInstance, vertexOffset);
}

/* V3: Core-1.0/1.2 Indirect-Draws -- Draw-Parameter (+optionaler drawCount) aus einem Buffer.
 * a=Param-Buffer, b=offset, inst_count=drawCount/maxDrawCount, first_inst=stride, indexed-Flag,
 * d=Count-Buffer (0=direkte Variante), rx=countOffset. Push-Constants pro Draw eingefroren. */
static void draw_indirect_record(VkCommandBuffer commandBuffer, int indexed, VkBuffer buffer,
                                 unsigned long long offset, unsigned drawCount, unsigned stride,
                                 VkBuffer countBuffer, unsigned long long countOffset)
{
    rt_cb_t *cb = (rt_cb_t *)(void *)commandBuffer;
    if (!cb->recording) { return; }
    int idx = cb->ncmd;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_DRAWINDIRECT;
        c->a = (unsigned long long)(unsigned long)(void *)buffer;
        c->b = offset;
        c->indexed = indexed;
        c->inst_count = drawCount; c->first_inst = stride;
        c->d = (unsigned long long)(unsigned long)(void *)countBuffer;
        c->rx = (int)countOffset;
        for (int i = 0; i < RT_PUSH_MAX; i++) { cb->dpush[idx][i] = cb->push[i]; }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
    draw_indirect_record(commandBuffer, 0, buffer, offset, drawCount, stride, VK_NULL_HANDLE, 0);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
    draw_indirect_record(commandBuffer, 1, buffer, offset, drawCount, stride, VK_NULL_HANDLE, 0);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{
    draw_indirect_record(commandBuffer, 0, buffer, offset, maxDrawCount, stride, countBuffer, countBufferOffset);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{
    draw_indirect_record(commandBuffer, 1, buffer, offset, maxDrawCount, stride, countBuffer, countBufferOffset);
}

/* ---------------- V1.9: Transfer/Fill/Clear (ausserhalb RenderPass) ---------------- */
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(
    VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
    VkDeviceSize size, uint32_t data)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_FILLBUF; c->a = (unsigned long long)(unsigned long)(void *)dstBuffer;
        c->rx = (int)dstOffset; c->rw = (int)size; c->d = data;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(
    VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
    uint32_t regionCount, const VkBufferCopy *pRegions)
{
    for (uint32_t i = 0; i < regionCount; i++) {
        rt_cmd_t *c = cb_push(commandBuffer);
        if (!c) { return; }
        c->op = C_COPYBUF;
        c->a = (unsigned long long)(unsigned long)(void *)srcBuffer;
        c->b = (unsigned long long)(unsigned long)(void *)dstBuffer;
        c->rx = (int)pRegions[i].srcOffset; c->ry = (int)pRegions[i].dstOffset;
        c->rw = (int)pRegions[i].size;
    }
}

/* V3: Core-1.0/1.3 Buffer->Image-Kopie (B8G8R8A8, 4 B/Pixel). Subset: bufferRowLength=0
 * (dicht gepackt = imageExtent.width). Region-Felder in a/b/d/rx/ry/rw/rh. */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(
    VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout,
    uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
    (void)dstImageLayout;
    for (uint32_t i = 0; i < regionCount; i++) {
        rt_cmd_t *c = cb_push(commandBuffer);
        if (!c) { return; }
        c->op = C_COPYBUF2IMG;
        c->a = (unsigned long long)(unsigned long)(void *)srcBuffer;
        c->b = (unsigned long long)(unsigned long)(void *)dstImage;
        c->d = pRegions[i].bufferOffset;
        c->rx = pRegions[i].imageOffset.x; c->ry = pRegions[i].imageOffset.y;
        c->rw = (int)pRegions[i].imageExtent.width; c->rh = (int)pRegions[i].imageExtent.height;
    }
}
/* V3b: Core 1.4 / VK_EXT_host_image_copy -- synchrone Host<->Image-Kopie OHNE Command-Buffer und
 * OHNE Queue-Submit (Host-Timeline). B8G8R8A8 (4 Byte/Pixel); dicht gepackt oder via
 * memoryRowLength. vkTransitionImageLayout ist ein No-op (Layouts werden nicht getrackt). */
VKAPI_ATTR VkResult VKAPI_CALL vkCopyMemoryToImage(
    VkDevice device, const VkCopyMemoryToImageInfo *info)
{
    (void)device;
    if (!info || !info->pRegions) { return VK_ERROR_INITIALIZATION_FAILED; }
    rt_image_t *img = rt_image_from_handle(info->dstImage);
    unsigned *dp = img ? (unsigned *)(void *)rt_image_pixels(img) : 0;
    if (!dp) { return VK_ERROR_MEMORY_MAP_FAILED; }
    unsigned wpr = (unsigned)(img->row_pitch / 4);
    for (uint32_t r = 0; r < info->regionCount; r++) {
        const VkMemoryToImageCopy *rg = &info->pRegions[r];
        const unsigned char *src = (const unsigned char *)rg->pHostPointer;
        if (!src) { continue; }
        unsigned rowlen = rg->memoryRowLength ? rg->memoryRowLength : rg->imageExtent.width;
        for (unsigned y = 0; y < rg->imageExtent.height; y++) {
            for (unsigned x = 0; x < rg->imageExtent.width; x++) {
                const unsigned char *sp = src + (unsigned long long)((y * rowlen) + x) * 4;
                unsigned val = (unsigned)sp[0] | ((unsigned)sp[1] << 8) |
                               ((unsigned)sp[2] << 16) | ((unsigned)sp[3] << 24);
                dp[(unsigned)(rg->imageOffset.y + (int)y) * wpr + (unsigned)(rg->imageOffset.x + (int)x)] = val;
            }
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCopyImageToMemory(
    VkDevice device, const VkCopyImageToMemoryInfo *info)
{
    (void)device;
    if (!info || !info->pRegions) { return VK_ERROR_INITIALIZATION_FAILED; }
    rt_image_t *img = rt_image_from_handle(info->srcImage);
    const unsigned *sp = img ? (const unsigned *)(void *)rt_image_pixels(img) : 0;
    if (!sp) { return VK_ERROR_MEMORY_MAP_FAILED; }
    unsigned wpr = (unsigned)(img->row_pitch / 4);
    for (uint32_t r = 0; r < info->regionCount; r++) {
        const VkImageToMemoryCopy *rg = &info->pRegions[r];
        unsigned char *dst = (unsigned char *)rg->pHostPointer;
        if (!dst) { continue; }
        unsigned rowlen = rg->memoryRowLength ? rg->memoryRowLength : rg->imageExtent.width;
        for (unsigned y = 0; y < rg->imageExtent.height; y++) {
            for (unsigned x = 0; x < rg->imageExtent.width; x++) {
                unsigned val = sp[(unsigned)(rg->imageOffset.y + (int)y) * wpr + (unsigned)(rg->imageOffset.x + (int)x)];
                unsigned char *dpb = dst + (unsigned long long)((y * rowlen) + x) * 4;
                dpb[0] = (unsigned char)(val & 0xFF);         dpb[1] = (unsigned char)((val >> 8) & 0xFF);
                dpb[2] = (unsigned char)((val >> 16) & 0xFF); dpb[3] = (unsigned char)((val >> 24) & 0xFF);
            }
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkTransitionImageLayout(
    VkDevice device, uint32_t transitionCount, const VkHostImageLayoutTransitionInfo *pTransitions)
{
    (void)device; (void)transitionCount; (void)pTransitions;   /* Layouts nicht getrackt -> No-op */
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(
    VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2 *pInfo)
{
    if (!pInfo) { return; }
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        VkBufferImageCopy r; memset(&r, 0, sizeof(r));
        r.bufferOffset      = pInfo->pRegions[i].bufferOffset;
        r.imageSubresource  = pInfo->pRegions[i].imageSubresource;
        r.imageOffset       = pInfo->pRegions[i].imageOffset;
        r.imageExtent       = pInfo->pRegions[i].imageExtent;
        vkCmdCopyBufferToImage(commandBuffer, pInfo->srcBuffer, pInfo->dstImage,
                               pInfo->dstImageLayout, 1, &r);
    }
}

/* V3: Core-1.0/1.3 Image->Buffer-Kopie (Umkehrung von CopyBufferToImage). */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(
    VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
    (void)srcImageLayout;
    for (uint32_t i = 0; i < regionCount; i++) {
        rt_cmd_t *c = cb_push(commandBuffer);
        if (!c) { return; }
        c->op = C_IMG2BUF;
        c->a = (unsigned long long)(unsigned long)(void *)srcImage;
        c->b = (unsigned long long)(unsigned long)(void *)dstBuffer;
        c->d = pRegions[i].bufferOffset;
        c->rx = pRegions[i].imageOffset.x; c->ry = pRegions[i].imageOffset.y;
        c->rw = (int)pRegions[i].imageExtent.width; c->rh = (int)pRegions[i].imageExtent.height;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2(
    VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2 *pInfo)
{
    if (!pInfo) { return; }
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        VkBufferImageCopy r; memset(&r, 0, sizeof(r));
        r.bufferOffset     = pInfo->pRegions[i].bufferOffset;
        r.imageSubresource = pInfo->pRegions[i].imageSubresource;
        r.imageOffset      = pInfo->pRegions[i].imageOffset;
        r.imageExtent      = pInfo->pRegions[i].imageExtent;
        vkCmdCopyImageToBuffer(commandBuffer, pInfo->srcImage, pInfo->srcImageLayout,
                               pInfo->dstBuffer, 1, &r);
    }
}

/* V3: Core-1.0/1.3 Image->Image-Kopie (gleiche Groesse, kein Scaling). srcOffset in rx/ry,
 * dstOffset in inst_count/first_inst, extent in rw/rh. */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(
    VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy *pRegions)
{
    (void)srcImageLayout; (void)dstImageLayout;
    for (uint32_t i = 0; i < regionCount; i++) {
        rt_cmd_t *c = cb_push(commandBuffer);
        if (!c) { return; }
        c->op = C_IMG2IMG;
        c->a = (unsigned long long)(unsigned long)(void *)srcImage;
        c->b = (unsigned long long)(unsigned long)(void *)dstImage;
        c->rx = pRegions[i].srcOffset.x; c->ry = pRegions[i].srcOffset.y;
        c->inst_count = (unsigned)pRegions[i].dstOffset.x; c->first_inst = (unsigned)pRegions[i].dstOffset.y;
        c->rw = (int)pRegions[i].extent.width; c->rh = (int)pRegions[i].extent.height;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(
    VkCommandBuffer commandBuffer, const VkCopyImageInfo2 *pInfo)
{
    if (!pInfo) { return; }
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        VkImageCopy r; memset(&r, 0, sizeof(r));
        r.srcSubresource = pInfo->pRegions[i].srcSubresource; r.srcOffset = pInfo->pRegions[i].srcOffset;
        r.dstSubresource = pInfo->pRegions[i].dstSubresource; r.dstOffset = pInfo->pRegions[i].dstOffset;
        r.extent = pInfo->pRegions[i].extent;
        vkCmdCopyImage(commandBuffer, pInfo->srcImage, pInfo->srcImageLayout,
                       pInfo->dstImage, pInfo->dstImageLayout, 1, &r);
    }
}

/* V3: Core-1.0/1.3 vkCmdBlitImage -- skalierende Kopie (Subset: nearest-Filter). srcOffset[0]
 * rx/ry, src-Groesse rw/rh, dst-Groesse inst_count/first_inst, dstOffset[0] vertex_offset/itype. */
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(
    VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount,
    const VkImageBlit *pRegions, VkFilter filter)
{
    (void)srcImageLayout; (void)dstImageLayout; (void)filter;   /* Subset: nearest */
    for (uint32_t i = 0; i < regionCount; i++) {
        const VkImageBlit *r = &pRegions[i];
        rt_cmd_t *c = cb_push(commandBuffer);
        if (!c) { return; }
        c->op = C_BLITIMG;
        c->a = (unsigned long long)(unsigned long)(void *)srcImage;
        c->b = (unsigned long long)(unsigned long)(void *)dstImage;
        c->rx = r->srcOffsets[0].x; c->ry = r->srcOffsets[0].y;
        c->rw = r->srcOffsets[1].x - r->srcOffsets[0].x;
        c->rh = r->srcOffsets[1].y - r->srcOffsets[0].y;
        c->inst_count = (unsigned)(r->dstOffsets[1].x - r->dstOffsets[0].x);
        c->first_inst = (unsigned)(r->dstOffsets[1].y - r->dstOffsets[0].y);
        c->vertex_offset = r->dstOffsets[0].x; c->itype = (unsigned)r->dstOffsets[0].y;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(
    VkCommandBuffer commandBuffer, const VkBlitImageInfo2 *pInfo)
{
    if (!pInfo) { return; }
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        VkImageBlit r; memset(&r, 0, sizeof(r));
        r.srcSubresource = pInfo->pRegions[i].srcSubresource;
        r.srcOffsets[0]  = pInfo->pRegions[i].srcOffsets[0]; r.srcOffsets[1] = pInfo->pRegions[i].srcOffsets[1];
        r.dstSubresource = pInfo->pRegions[i].dstSubresource;
        r.dstOffsets[0]  = pInfo->pRegions[i].dstOffsets[0]; r.dstOffsets[1] = pInfo->pRegions[i].dstOffsets[1];
        vkCmdBlitImage(commandBuffer, pInfo->srcImage, pInfo->srcImageLayout,
                       pInfo->dstImage, pInfo->dstImageLayout, 1, &r, pInfo->filter);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(
    VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
    const VkClearColorValue *pColor, uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
    (void)imageLayout; (void)rangeCount; (void)pRanges;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c && pColor) {
        c->op = C_CLEARCOLIMG; c->a = (unsigned long long)(unsigned long)(void *)image;
        for (int i = 0; i < 4; i++) { c->cclear[0][i] = pColor->float32[i]; }
    }
}

/* V1.6: MSAA-Resolve -- Multisample-Quelle (samples-major) in ein Single-Sample-Ziel mitteln. */
VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(
    VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageResolve *pRegions)
{
    (void)srcImageLayout; (void)dstImageLayout; (void)regionCount; (void)pRegions;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) {
        c->op = C_RESOLVEIMG;
        c->a = (unsigned long long)(unsigned long)(void *)srcImage;
        c->b = (unsigned long long)(unsigned long)(void *)dstImage;
    }
}

/* V3: Core-1.3 Copy2-Kommandos -- uebersetzen die *Info2-Strukturen (sType/pNext + gleiche
 * Region-Felder) auf die verifizierten 1.0-Kommandos. */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2(
    VkCommandBuffer commandBuffer, const VkCopyBufferInfo2 *pInfo)
{
    if (!pInfo) { return; }
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        VkBufferCopy r;
        r.srcOffset = pInfo->pRegions[i].srcOffset;
        r.dstOffset = pInfo->pRegions[i].dstOffset;
        r.size      = pInfo->pRegions[i].size;
        vkCmdCopyBuffer(commandBuffer, pInfo->srcBuffer, pInfo->dstBuffer, 1, &r);
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2(
    VkCommandBuffer commandBuffer, const VkResolveImageInfo2 *pInfo)
{
    if (!pInfo) { return; }
    for (uint32_t i = 0; i < pInfo->regionCount; i++) {
        VkImageResolve r;
        r.srcSubresource = pInfo->pRegions[i].srcSubresource;
        r.srcOffset      = pInfo->pRegions[i].srcOffset;
        r.dstSubresource = pInfo->pRegions[i].dstSubresource;
        r.dstOffset      = pInfo->pRegions[i].dstOffset;
        r.extent         = pInfo->pRegions[i].extent;
        vkCmdResolveImage(commandBuffer, pInfo->srcImage, pInfo->srcImageLayout,
                          pInfo->dstImage, pInfo->dstImageLayout, 1, &r);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers)
{
    (void)commandBuffer; (void)srcStageMask; (void)dstStageMask; (void)dependencyFlags;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;
    (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
    /* Synchron ausfuehrende CPU-Implementierung: Layout-Uebergaenge sind No-ops. */
}

/* V3: Core-1.3 Synchronization2 -- vkCmdPipelineBarrier2 ist No-op wie oben (synchron in-order). */
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(
    VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)
{
    (void)commandBuffer; (void)pDependencyInfo;
}

/* ---------------- V1.8: Query-Pools (Occlusion + Timestamp) ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(
    VkDevice device, const VkQueryPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || pCreateInfo->queryCount > RT_MAX_QUERIES) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    rt_querypool_t *qp;
    ALLOC_SLOT(g_querypool, RT_MAX_QUERYPOOL, qp);
    if (!qp) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    qp->type = pCreateInfo->queryType;
    qp->count = pCreateInfo->queryCount;
    for (unsigned i = 0; i < RT_MAX_QUERIES; i++) { qp->result[i] = 0; qp->avail[i] = 0; }
    *pQueryPool = (VkQueryPool)(void *)qp;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(
    VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (queryPool) { ((rt_querypool_t *)(void *)queryPool)->used = 0; }
}

/* Host-Reset (1.2 core, ex VK_EXT_host_query_reset). */
VKAPI_ATTR void VKAPI_CALL vkResetQueryPool(
    VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{
    (void)device;
    rt_querypool_t *qp = (rt_querypool_t *)(void *)queryPool;
    if (!qp) { return; }
    for (uint32_t k = 0; k < queryCount && firstQuery + k < qp->count; k++) {
        qp->result[firstQuery + k] = 0; qp->avail[firstQuery + k] = 0;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
    VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount,
    size_t dataSize, void *pData, VkDeviceSize stride, VkQueryResultFlags flags)
{
    (void)device; (void)dataSize;
    rt_querypool_t *qp = (rt_querypool_t *)(void *)queryPool;
    if (!qp || !pData) { return VK_ERROR_UNKNOWN; }
    int wide = (flags & VK_QUERY_RESULT_64_BIT) != 0;
    int with_avail = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
    VkResult ret = VK_SUCCESS;
    unsigned char *bytes = (unsigned char *)pData;
    for (uint32_t k = 0; k < queryCount; k++) {
        uint32_t q = firstQuery + k;
        unsigned char *row = bytes + (size_t)k * (size_t)stride;
        int avail = (q < qp->count) ? qp->avail[q] : 0;
        /* WAIT_BIT: synchrone Runtime -> nach Submit stets verfuegbar. Ohne WAIT + nicht
         * verfuegbar -> VK_NOT_READY (Spez), Ergebnis dann undefiniert (wir schreiben 0). */
        if (!avail && !(flags & VK_QUERY_RESULT_WAIT_BIT)) { ret = VK_NOT_READY; }
        unsigned long long val = (q < qp->count) ? qp->result[q] : 0;
        if (wide) { *(unsigned long long *)(void *)row = val; row += 8; }
        else      { *(unsigned *)(void *)row = (unsigned)val; row += 4; }
        if (with_avail) {
            unsigned long long a = (unsigned long long)(avail ? 1 : 0);
            if (wide) { *(unsigned long long *)(void *)row = a; }
            else      { *(unsigned *)(void *)row = (unsigned)a; }
        }
    }
    return ret;
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(
    VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_RESETQUERY; c->a = (unsigned long long)(unsigned long)(void *)queryPool;
             c->b = firstQuery; c->rw = (int)queryCount; }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(
    VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags)
{
    (void)flags;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_BEGINQUERY; c->a = (unsigned long long)(unsigned long)(void *)queryPool; c->b = query; }
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(
    VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
{
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_ENDQUERY; c->a = (unsigned long long)(unsigned long)(void *)queryPool; c->b = query; }
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(
    VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage,
    VkQueryPool queryPool, uint32_t query)
{
    (void)pipelineStage;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_WRITETS; c->a = (unsigned long long)(unsigned long)(void *)queryPool; c->b = query; }
}

/* V3: Core-1.3 Synchronization2 -- vkCmdWriteTimestamp2 delegiert an die 1.0-Variante
 * (die 64-bit-Stage-Maske ist in der synchronen In-Order-Impl bedeutungslos). */
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool, uint32_t query)
{
    (void)stage;
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, query);
}

/* ---------------- V1.8: Events (Signal-Flags) ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(
    VkDevice device, const VkEventCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkEvent *pEvent)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    rt_event_t *e;
    ALLOC_SLOT(g_event, RT_MAX_EVENT, e);
    if (!e) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    e->signaled = 0;
    *pEvent = (VkEvent)(void *)e;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(
    VkDevice device, VkEvent event, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (event) { ((rt_event_t *)(void *)event)->used = 0; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice device, VkEvent event)
{
    (void)device;
    rt_event_t *e = (rt_event_t *)(void *)event;
    if (!e) { return VK_ERROR_UNKNOWN; }
    return e->signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent event)
{
    (void)device;
    rt_event_t *e = (rt_event_t *)(void *)event;
    if (e) { e->signaled = 1; }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent event)
{
    (void)device;
    rt_event_t *e = (rt_event_t *)(void *)event;
    if (e) { e->signaled = 0; }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(
    VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    (void)stageMask;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_SETEVENT; c->a = (unsigned long long)(unsigned long)(void *)event; }
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(
    VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    (void)stageMask;
    rt_cmd_t *c = cb_push(commandBuffer);
    if (c) { c->op = C_RESETEVENT; c->a = (unsigned long long)(unsigned long)(void *)event; }
}

/* Synchrone in-order Ausfuehrung: gewartete Events wurden im selben Strom bereits gesetzt. */
VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(
    VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
    VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers)
{
    (void)commandBuffer; (void)eventCount; (void)pEvents; (void)srcStageMask; (void)dstStageMask;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;
    (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
}

/* V3: Core-1.3 Synchronization2 Event-Kommandos -- die VkDependencyInfo-Stage-Masken sind in der
 * synchronen In-Order-Impl bedeutungslos; Set/Reset delegieren an die verifizierten Event-Kommandos. */
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(
    VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo *pDependencyInfo)
{
    (void)pDependencyInfo;
    vkCmdSetEvent(commandBuffer, event, 0);
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2(
    VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask)
{
    (void)stageMask;
    vkCmdResetEvent(commandBuffer, event, 0);
}
VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2(
    VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
    const VkDependencyInfo *pDependencyInfos)
{
    (void)pDependencyInfos;
    vkCmdWaitEvents(commandBuffer, eventCount, pEvents, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* ---------------- Fences ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
    VkDevice device, const VkFenceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFence *pFence)
{
    (void)device; (void)pAllocator;
    rt_fence_t *f;
    ALLOC_SLOT(g_fence, RT_MAX_CB, f);
    if (!f) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    f->signaled = (pCreateInfo && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT)) ? 1 : 0;
    *pFence = (VkFence)(void *)f;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (fence) { ((rt_fence_t *)(void *)fence)->used = 0; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences)
{
    (void)device;
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) { ((rt_fence_t *)(void *)pFences[i])->signaled = 0; }
    }
    return VK_SUCCESS;
}

void rt_fence_signal(VkFence f)
{
    if (f) { ((rt_fence_t *)(void *)f)->signaled = 1; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence)
{
    (void)device;
    return ((rt_fence_t *)(void *)fence)->signaled ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice device, uint32_t fenceCount, const VkFence *pFences, VkBool32 waitAll, uint64_t timeout)
{
    (void)device; (void)timeout;
    /* Submit fuehrt synchron aus -> Fences sind entweder signalisiert oder werden es nie.
     * waitAll respektieren: TRUE -> ALLE muessen signalisiert sein; FALSE -> MINDESTENS EINE. */
    if (waitAll) {
        for (uint32_t i = 0; i < fenceCount; i++) {
            if (!((rt_fence_t *)(void *)pFences[i])->signaled) { return VK_TIMEOUT; }
        }
        return VK_SUCCESS;
    }
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (((rt_fence_t *)(void *)pFences[i])->signaled) { return VK_SUCCESS; }
    }
    return (fenceCount == 0) ? VK_SUCCESS : VK_TIMEOUT;
}

/* T3-Review #16: von vkDestroyDevice gerufen -> die vk_cmd-eigenen Pools zuruecksetzen
 * (sonst ueberleben ShaderModule/Pipeline/CB/... die Device-Zerstoerung -> Handle-Leck bei
 * Device-Neuerstellung, z.B. VKTEST erstellt 2x Device). */
void rt_cmd_reset_pools(void)
{
    memset(g_shader, 0, sizeof(g_shader));
    memset(g_layout, 0, sizeof(g_layout));
    memset(g_rp, 0, sizeof(g_rp));
    memset(g_fb, 0, sizeof(g_fb));
    memset(g_pipe, 0, sizeof(g_pipe));
    memset(g_cmdpool, 0, sizeof(g_cmdpool));
    memset(g_cb, 0, sizeof(g_cb));
    memset(g_fence, 0, sizeof(g_fence));
    memset(g_dslayout, 0, sizeof(g_dslayout));
    memset(g_dpool, 0, sizeof(g_dpool));
    memset(g_dset, 0, sizeof(g_dset));
    memset(g_sampler, 0, sizeof(g_sampler));
    memset(g_querypool, 0, sizeof(g_querypool));   /* V1.8 */
    memset(g_event, 0, sizeof(g_event));
    g_occl_counter = 0; g_ts_counter = 0;
}

/* ---------------- Executor (vkQueueSubmit) ---------------- */
/* Farbe [0,1]^4 -> 0xAARRGGBB (B8G8R8A8_UNORM, little-endian im Speicher = B,G,R,A). PRO Kanal
 * geklemmt. V1.1: ECHTES Alpha wird jetzt gespeichert (fuer Blending + korrektes Readback). Der
 * Framebuffer-Scanout ignoriert das obere Byte -> fuer die Swapchain optisch unveraendert. */
static unsigned pack_rgba(float r, float g, float b, float a)
{
    int ri = (int)(r * 255.0f + 0.5f), gi = (int)(g * 255.0f + 0.5f);
    int bi = (int)(b * 255.0f + 0.5f), ai = (int)(a * 255.0f + 0.5f);
    if (ri < 0) { ri = 0; } if (ri > 255) { ri = 255; }
    if (gi < 0) { gi = 0; } if (gi > 255) { gi = 255; }
    if (bi < 0) { bi = 0; } if (bi > 255) { bi = 255; }
    if (ai < 0) { ai = 0; } if (ai > 255) { ai = 255; }
    return ((unsigned)ai << 24) | ((unsigned)ri << 16) | ((unsigned)gi << 8) | (unsigned)bi;
}
static void unpack_rgba(unsigned w, float *out)
{
    out[0] = (float)((w >> 16) & 0xFF) / 255.0f;   /* R */
    out[1] = (float)((w >> 8) & 0xFF) / 255.0f;    /* G */
    out[2] = (float)(w & 0xFF) / 255.0f;           /* B */
    out[3] = (float)((w >> 24) & 0xFF) / 255.0f;   /* A */
}

/* V1.1: einen VkBlendFactor fuer Kanal ch (0..2=RGB, 3=A) auswerten. */
static float blend_factor(unsigned f, const float *s, const float *d, const float *k, int ch)
{
    switch (f) {
    case VK_BLEND_FACTOR_ZERO:                     return 0.0f;
    case VK_BLEND_FACTOR_ONE:                      return 1.0f;
    case VK_BLEND_FACTOR_SRC_COLOR:                return s[ch];
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:      return 1.0f - s[ch];
    case VK_BLEND_FACTOR_DST_COLOR:                return d[ch];
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:      return 1.0f - d[ch];
    case VK_BLEND_FACTOR_SRC_ALPHA:                return s[3];
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:      return 1.0f - s[3];
    case VK_BLEND_FACTOR_DST_ALPHA:                return d[3];
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:      return 1.0f - d[3];
    case VK_BLEND_FACTOR_CONSTANT_COLOR:           return k[ch];
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return 1.0f - k[ch];
    case VK_BLEND_FACTOR_CONSTANT_ALPHA:           return k[3];
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return 1.0f - k[3];
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: {
        if (ch == 3) { return 1.0f; }
        float t = 1.0f - d[3];
        return s[3] < t ? s[3] : t;
    }
    default: return 0.0f;                           /* Dual-Source (SRC1_*) nicht im Subset */
    }
}
static float blend_op(unsigned op, float s, float d, float sf, float df)
{
    switch (op) {
    case VK_BLEND_OP_ADD:              return s * sf + d * df;
    case VK_BLEND_OP_SUBTRACT:         return s * sf - d * df;
    case VK_BLEND_OP_REVERSE_SUBTRACT: return d * df - s * sf;
    case VK_BLEND_OP_MIN:              return s < d ? s : d;
    default:                           return s > d ? s : d;   /* VK_BLEND_OP_MAX */
    }
}

typedef struct {
    const rt_pipe_t *pipe;
    spv_mod_t *fsmod;
    unsigned   fs_fn;
    const unsigned char *push;
    const void *ubo[SPV_MAX_DESCRIPTOR];       /* V1.3: gebundene Descriptor (Set 0) */
    unsigned    ubo_bytes[SPV_MAX_DESCRIPTOR];
    spv_tex_t   tex[SPV_MAX_DESCRIPTOR];        /* V1.4: gebundene Texturen */
    int         n_color;                       /* V1.5: MRT -- Anzahl Farb-Attachments */
    unsigned   *mrt[RT_MAX_COLOR_ATT];         /* Sekundaer-Attachments (1..n-1); [0]=Primaer via r3d */
    int         mrt_pitch[RT_MAX_COLOR_ATT];
} fs_ctx_t;

static spv_io_t g_vs_io, g_fs_io;      /* getrennt: FS-Callback laeuft waehrend VS-Schleife */

/* V3: Extended-Dynamic-State (Core 1.3) -- pro Command-Buffer gesetzte Overrides, die den
 * gebackenen Pipeline-State zur Draw-Zeit ueberschreiben. -1 = nicht gesetzt (Pipeline gilt).
 * Vor fs_shade_dst deklariert, da dort das dynamische Blending/Write-Mask gelesen wird. */
static int g_dyn_cull = -1, g_dyn_front = -1, g_dyn_dtest = -1, g_dyn_dwrite = -1;
static int g_dyn_dcompare = -1;                       /* V3b: dynamischer depthCompareOp (VkCompareOp 0-7) */
static int g_dyn_discard = -1;                        /* V3b: rasterizerDiscardEnable (1 -> keine Fragmente) */
static int g_dyn_vp_set = 0;                          /* V3b: dynamischer Viewport gesetzt? */
static float g_dyn_vp_x, g_dyn_vp_y, g_dyn_vp_w, g_dyn_vp_h, g_dyn_vp_minz, g_dyn_vp_maxz;
static int g_dyn_sc_set = 0;                          /* V3b: dynamisches Scissor gesetzt? */
static int g_dyn_sc_x, g_dyn_sc_y, g_dyn_sc_w, g_dyn_sc_h;
/* V3b: EDS3 dynamisches Blending (Attachment 0). -1 = Pipeline gilt. */
static int g_dyn_blend_en = -1, g_dyn_wmask = -1;

/* Fragment-Shader ausfuehren, Blending + Write-Mask anwenden, fertiges 0xAARRGGBB liefern.
 * dst_word = aktueller Pixel (fuer Blending/Write-Mask). */
static unsigned fs_shade_dst(void *user, int px, int py, float z, const float *attr, unsigned dst_word)
{
    fs_ctx_t *fc = (fs_ctx_t *)user;
    const rt_pipe_t *p = fc->pipe;
    spv_io_t *io = &g_fs_io;
    memset(io->out_loc, 0, sizeof(io->out_loc));
    for (int i = 0; i < 4; i++) {
        io->in_loc[0][i] = attr[i];
        io->in_loc[1][i] = attr[4 + i];
    }
    io->frag_coord[0] = (float)px + 0.5f;
    io->frag_coord[1] = (float)py + 0.5f;
    io->frag_coord[2] = z;
    io->frag_coord[3] = 1.0f;
    io->push = fc->push;
    io->push_bytes = RT_PUSH_MAX;
    for (int d = 0; d < SPV_MAX_DESCRIPTOR; d++) {
        io->ubo[d] = fc->ubo[d]; io->ubo_bytes[d] = fc->ubo_bytes[d]; io->tex[d] = fc->tex[d];
    }
    io->discarded = 0;                                                     /* V2.3: pro Fragment zuruecksetzen */
    if (spv_exec(fc->fsmod, fc->fs_fn, io) != 0) { return 0x00FF00FFu; }   /* fail-loud pink */
    if (io->discarded) { return dst_word; }                               /* V2.3: OpKill -> Ziel-Pixel unveraendert */

    /* V1.5 MRT: Sekundaer-Attachments (Location 1..n-1) direkt schreiben (Overwrite; Blending
     * nur fuer Attachment 0 via r3d -- dokumentierte Subset-Grenze fuer Sekundaere). */
    for (int a = 1; a < fc->n_color; a++) {
        if (!fc->mrt[a]) { continue; }
        fc->mrt[a][(long)py * fc->mrt_pitch[a] + px] =
            pack_rgba(io->out_loc[a][0], io->out_loc[a][1], io->out_loc[a][2], io->out_loc[a][3]);
    }

    float src[4] = { io->out_loc[0][0], io->out_loc[0][1], io->out_loc[0][2], io->out_loc[0][3] };
    float dst[4]; unpack_rgba(dst_word, dst);
    float res[4];
    /* V3b: EDS3 dynamisches Blending/Write-Mask ueberschreibt den gebackenen Pipeline-State. */
    int blend_en    = (g_dyn_blend_en >= 0) ? g_dyn_blend_en : p->blend_enable;
    unsigned wmask  = (g_dyn_wmask    >= 0) ? (unsigned)g_dyn_wmask : p->write_mask;
    if (blend_en) {
        for (int ch = 0; ch < 3; ch++) {
            float sf = blend_factor(p->src_col, src, dst, p->blend_const, ch);
            float df = blend_factor(p->dst_col, src, dst, p->blend_const, ch);
            res[ch] = blend_op(p->col_op, src[ch], dst[ch], sf, df);
        }
        float saf = blend_factor(p->src_alpha, src, dst, p->blend_const, 3);
        float daf = blend_factor(p->dst_alpha, src, dst, p->blend_const, 3);
        res[3] = blend_op(p->alpha_op, src[3], dst[3], saf, daf);
    } else {
        res[0] = src[0]; res[1] = src[1]; res[2] = src[2]; res[3] = src[3];
    }
    /* colorWriteMask: nicht geschriebene Kanaele behalten den Zielwert (Bits R=1,G=2,B=4,A=8). */
    if (!(wmask & VK_COLOR_COMPONENT_R_BIT)) { res[0] = dst[0]; }
    if (!(wmask & VK_COLOR_COMPONENT_G_BIT)) { res[1] = dst[1]; }
    if (!(wmask & VK_COLOR_COMPONENT_B_BIT)) { res[2] = dst[2]; }
    if (!(wmask & VK_COLOR_COMPONENT_A_BIT)) { res[3] = dst[3]; }
    return pack_rgba(res[0], res[1], res[2], res[3]);
}

/* r3d-Callback-Signatur (kein dst-Zugang) -> die Ziel-Pixel-Adresse liegt in fs_ctx (color+pitch);
 * r3d schreibt den Rueckgabewert an dieselbe Stelle, die wir hier lesen. */
static unsigned *g_fs_color; static int g_fs_pitch;   /* aktuelles Farb-Target fuer fs_shade */
static unsigned fs_shade(void *user, int px, int py, float z, const float *attr)
{
    /* V1.8 Occlusion: r3d ruft diesen Callback NUR fuer tiefentest-bestandene Fragmente ->
     * je Aufruf ein Sample, das die Occlusion-Query passiert. */
    if (g_occl_counter) { (*g_occl_counter)++; }
    unsigned dst = (g_fs_color) ? g_fs_color[(long)py * g_fs_pitch + px] : 0;
    return fs_shade_dst(user, px, py, z, attr, dst);
}

/* renderArea ∩ Attachment als [x0,x1)×[y0,y1) berechnen. */
static void clip_area(const rt_cmd_t *c, unsigned w, unsigned h,
                      unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1)
{
    int ax0 = c->rx, ay0 = c->ry, ax1 = c->rx + c->rw, ay1 = c->ry + c->rh;
    if (ax0 < 0) { ax0 = 0; } if (ay0 < 0) { ay0 = 0; }
    if (ax1 > (int)w) { ax1 = (int)w; } if (ay1 > (int)h) { ay1 = (int)h; }
    if (ax1 < ax0) { ax1 = ax0; } if (ay1 < ay0) { ay1 = ay0; }
    *x0 = (unsigned)ax0; *y0 = (unsigned)ay0; *x1 = (unsigned)ax1; *y1 = (unsigned)ay1;
}

static void exec_clear(const rt_rp_t *rp, const rt_fb_t *fb, const rt_cmd_t *c)
{
    unsigned x0, y0, x1, y1;
    /* Alle Farb-Attachments (MRT) clearen, jeweils mit ihrer Clear-Farbe. */
    for (int a = 0; a < fb->n_color; a++) {
        if (a >= rp->n_color || !rp->color_clear[a]) { continue; }
        rt_image_t *ci = fb->color[a]->image;
        unsigned *px = (unsigned *)(void *)rt_image_pixels(ci);
        if (!px) { continue; }
        unsigned word = pack_rgba(c->cclear[a][0], c->cclear[a][1], c->cclear[a][2], c->cclear[a][3]);
        unsigned wpr = (unsigned)(ci->row_pitch / 4);
        clip_area(c, ci->extent.width, ci->extent.height, &x0, &y0, &x1, &y1);
        /* V1.6 MSAA: jede Sample-Ebene clearen (Sample-Major, Ebenen-Abstand height*wpr).
         * samples<1 defensiv als 1 behandeln (jedes Image hat >=1 Sample). */
        long long cplane = (long long)ci->extent.height * wpr;
        int csn = ci->samples < 1 ? 1 : ci->samples;
        for (int s = 0; s < csn; s++) {
            for (unsigned y = y0; y < y1; y++) {
                for (unsigned x = x0; x < x1; x++) { px[(long long)s * cplane + y * wpr + x] = word; }
            }
        }
    }
    if (fb->depth && rp->depth_clear) {
        rt_image_t *di = fb->depth->image;
        float *dp = (float *)(void *)rt_image_pixels(di);
        if (dp) {
            clip_area(c, di->extent.width, di->extent.height, &x0, &y0, &x1, &y1);
            long long dplane = (long long)di->extent.height * di->extent.width;
            int dsn = di->samples < 1 ? 1 : di->samples;
            for (int s = 0; s < dsn; s++) {   /* alle Depth-Sample-Ebenen */
                for (unsigned y = y0; y < y1; y++) {
                    for (unsigned x = x0; x < x1; x++) {
                        dp[(long long)s * dplane + y * di->extent.width + x] = c->dclear;
                    }
                }
            }
        }
    }
}

/* Effektiven Vertex-Index bestimmen: non-indexed -> first+i; indexed -> Index-Puffer[first+i]
 * + vertexOffset. ib==0 bei non-indexed. */
static unsigned fetch_index(const unsigned char *ibbase, unsigned itype, int indexed,
                            unsigned first, unsigned i, int vertex_offset)
{
    if (!indexed || !ibbase) { return first + i; }
    unsigned n = first + i;
    long idx;
    if (itype == 4)      { idx = (long)((const unsigned *)(const void *)ibbase)[n]; }
    else if (itype == 2) { idx = (long)((const unsigned short *)(const void *)ibbase)[n]; }
    else                 { idx = (long)ibbase[n]; }                       /* uint8 */
    return (unsigned)(idx + vertex_offset);
}

static void exec_draw(const rt_pipe_t *p, const rt_fb_t *fb, const rt_buffer_t *vb,
                      unsigned long long vboff, const unsigned char *push,
                      unsigned vcount, unsigned first,
                      const rt_buffer_t *ib, unsigned long long iboff, unsigned itype,
                      int indexed, int vertex_offset, unsigned inst_count, unsigned first_inst,
                      const rt_dset_t *dset)
{
    if (!p || !fb || !vb || !vb->mem || !p->vs || !p->fs || fb->n_color < 1) { return; }
    if (indexed && (!ib || !ib->mem)) { return; }
    rt_image_t *ci = fb->color[0]->image;            /* Primaer-Attachment 0 -> r3d */
    unsigned char *cpx = rt_image_pixels(ci);
    if (!cpx) { return; }

    r3d_target_t t;
    t.color = (unsigned *)(void *)cpx;
    t.pitch_px = (int)(ci->row_pitch / 4);
    t.width = (int)ci->extent.width;
    t.height = (int)ci->extent.height;
    t.samples = ci->samples;                         /* V1.6 MSAA: Sample-Zahl aus dem Attachment */
    g_fs_color = t.color; g_fs_pitch = t.pitch_px;   /* V1.1: Ziel-Pixel fuer Blending in fs_shade */
    t.depth = 0;
    if (fb->depth && fb->depth->image) {
        t.depth = (float *)(void *)rt_image_pixels(fb->depth->image);
    }
    if (g_dyn_discard == 1) { return; }              /* V3b: rasterizerDiscard -> keine Rasterisierung */
    if (g_dyn_vp_set) {                              /* V3b: dynamischer Viewport ueberschreibt Pipeline */
        t.vp_x = g_dyn_vp_x; t.vp_y = g_dyn_vp_y; t.vp_w = g_dyn_vp_w; t.vp_h = g_dyn_vp_h;
        t.vp_minz = g_dyn_vp_minz; t.vp_maxz = g_dyn_vp_maxz;
    } else {
        t.vp_x = p->vp_x; t.vp_y = p->vp_y; t.vp_w = p->vp_w; t.vp_h = p->vp_h;
        t.vp_minz = p->vp_minz; t.vp_maxz = p->vp_maxz;
    }
    if (g_dyn_sc_set) {                              /* V3b: dynamisches Scissor ueberschreibt Pipeline */
        t.sc_x = g_dyn_sc_x; t.sc_y = g_dyn_sc_y; t.sc_w = g_dyn_sc_w; t.sc_h = g_dyn_sc_h;
    } else {
        t.sc_x = p->sc_x; t.sc_y = p->sc_y; t.sc_w = p->sc_w; t.sc_h = p->sc_h;
    }
    t.cull_mode  = (g_dyn_cull  >= 0) ? g_dyn_cull  : p->cull_mode;    /* V3: Extended-Dynamic-State */
    t.front_ccw  = (g_dyn_front >= 0) ? g_dyn_front : p->front_ccw;
    t.depth_test = ((g_dyn_dtest >= 0) ? g_dyn_dtest : p->depth_test) && (t.depth != 0);
    t.depth_write = (g_dyn_dwrite >= 0) ? g_dyn_dwrite : p->depth_write;
    t.depth_compare = (g_dyn_dcompare >= 0) ? g_dyn_dcompare : p->depth_compare;   /* V3b */

    unsigned fnv = 0, fnf = 0;
    if (spv_find_entry(&p->vs->mod, SPV_MODEL_VERTEX, &fnv) != 0) { return; }
    if (spv_find_entry(&p->fs->mod, SPV_MODEL_FRAGMENT, &fnf) != 0) { return; }

    fs_ctx_t fc;
    fc.pipe = p; fc.fsmod = &p->fs->mod; fc.fs_fn = fnf; fc.push = push;
    /* V1.3: gebundene Descriptor (Set 0) in den Draw-Kontext ziehen -> VS + FS koennen
     * Uniform/Storage-Buffer je Binding lesen. */
    for (int d = 0; d < SPV_MAX_DESCRIPTOR; d++) {
        fc.ubo[d] = dset ? dset->base[d] : 0;
        fc.ubo_bytes[d] = dset ? dset->bytes[d] : 0;
        if (dset) { fc.tex[d] = dset->tex[d]; } else { fc.tex[d].pixels = 0; }
    }
    /* V1.5 MRT: Sekundaer-Attachment-Puffer (1..n-1) fuer fs_shade bereitstellen. */
    fc.n_color = fb->n_color;
    for (int a = 0; a < RT_MAX_COLOR_ATT; a++) { fc.mrt[a] = 0; fc.mrt_pitch[a] = 0; }
    for (int a = 1; a < fb->n_color && a < RT_MAX_COLOR_ATT; a++) {
        rt_image_t *ai = fb->color[a]->image;
        fc.mrt[a] = (unsigned *)(void *)rt_image_pixels(ai);
        fc.mrt_pitch[a] = (int)(ai->row_pitch / 4);
    }

    const unsigned char *base   = vb->mem->base + vb->off + vboff;
    const unsigned char *ibbase = (indexed && ib) ? (ib->mem->base + ib->off + iboff) : 0;

    /* V1.2: pro Instanz (gl_InstanceIndex) die Vertex-Liste durchlaufen; indiziert oder linear. */
    for (unsigned inst = 0; inst < inst_count; inst++) {
        r3d_vtx_t tri[3];
        int ti = 0;
        for (unsigned i = 0; i < vcount; i++) {
            unsigned vidx = fetch_index(ibbase, itype, indexed, first, i, vertex_offset);
            const unsigned char *v = base + (unsigned long long)vidx * p->stride;
            spv_io_t *io = &g_vs_io;
            memset(io->in_loc, 0, sizeof(io->in_loc));
            memset(io->out_loc, 0, sizeof(io->out_loc));
            for (unsigned a = 0; a < p->nattr; a++) {
                const float *src = (const float *)(const void *)(v + p->attr[a].off);
                for (unsigned k = 0; k < p->attr[a].ncomp; k++) { io->in_loc[p->attr[a].loc][k] = src[k]; }
            }
            io->vertex_index   = (int)vidx;                 /* gl_VertexIndex   */
            io->instance_index = (int)(first_inst + inst);  /* gl_InstanceIndex */
            io->push = push;
            io->push_bytes = RT_PUSH_MAX;
            for (int d = 0; d < SPV_MAX_DESCRIPTOR; d++) {
                io->ubo[d] = fc.ubo[d]; io->ubo_bytes[d] = fc.ubo_bytes[d]; io->tex[d] = fc.tex[d];
            }
            if (spv_exec(&p->vs->mod, fnv, io) != 0) { return; }
            r3d_vtx_t vv;
            vv.pos.x = io->builtin_pos[0]; vv.pos.y = io->builtin_pos[1];
            vv.pos.z = io->builtin_pos[2]; vv.pos.w = io->builtin_pos[3];
            for (int k = 0; k < 4; k++) {
                vv.attr[k]     = io->out_loc[0][k];         /* Varyings Location 0 + 1 */
                vv.attr[4 + k] = io->out_loc[1][k];
            }
            tri[ti++] = vv;
            if (ti == 3) {
                r3d_draw_tri(&t, &tri[0], &tri[1], &tri[2], 8, fs_shade, &fc);
                ti = 0;
            }
        }
    }
}

/* V1.7: Compute-Dispatch -- Compute-Shader je globaler Invokation (group*local) ausfuehren.
 * gl_GlobalInvocationID = (x,y,z); Descriptor Set 0 (Storage-Buffer schreibbar) im io. */
static spv_io_t g_cs_io;
static void exec_dispatch(const rt_pipe_t *p, const rt_dset_t *dset,
                          unsigned gx, unsigned gy, unsigned gz)
{
    if (!p || !p->is_compute || !p->cs) { return; }
    unsigned fn = 0;
    if (spv_find_entry(&p->cs->mod, SPV_MODEL_GLCOMPUTE, &fn) != 0) { return; }
    unsigned lsx = p->cs->mod.lsx ? p->cs->mod.lsx : 1;
    unsigned lsy = p->cs->mod.lsy ? p->cs->mod.lsy : 1;
    unsigned lsz = p->cs->mod.lsz ? p->cs->mod.lsz : 1;
    unsigned nx = gx * lsx, ny = gy * lsy, nz = gz * lsz;
    /* Subset-Grenze gegen exzessive Invokationen (Software-Interpreter). */
    if ((unsigned long long)nx * ny * nz > 65536ull) { return; }
    spv_io_t *io = &g_cs_io;
    memset(io, 0, sizeof(*io));
    for (int d = 0; d < SPV_MAX_DESCRIPTOR; d++) {
        io->ubo[d]       = dset ? dset->base[d]  : 0;
        io->ubo_bytes[d] = dset ? dset->bytes[d] : 0;
        if (dset) { io->tex[d] = dset->tex[d]; }
    }
    for (unsigned z = 0; z < nz; z++) {
        for (unsigned y = 0; y < ny; y++) {
            for (unsigned xg = 0; xg < nx; xg++) {
                io->global_id[0] = xg; io->global_id[1] = y; io->global_id[2] = z;
                if (spv_exec(&p->cs->mod, fn, io) != 0) { return; }  /* fail-loud: Abbruch */
            }
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
    (void)queue;
    for (uint32_t s = 0; s < submitCount; s++) {
        for (uint32_t b = 0; b < pSubmits[s].commandBufferCount; b++) {
            rt_cb_t *cb = (rt_cb_t *)(void *)pSubmits[s].pCommandBuffers[b];
            if (!cb) { continue; }
            const rt_rp_t *rp = 0;
            const rt_fb_t *fb = 0;
            rt_fb_t dyn_fb; rt_rp_t dyn_rp;    /* V3b: transientes FB/RP fuer Dynamic-Rendering-Draws */
            const rt_pipe_t *pipe = 0;
            const rt_pipe_t *cpipe = 0;        /* V1.7: separat gebundene Compute-Pipeline */
            const rt_buffer_t *vb = 0, *ib = 0;
            const rt_dset_t *dset = 0;
            unsigned long long vboff = 0, iboff = 0;
            unsigned itype = 2;
            g_dyn_cull = -1; g_dyn_front = -1; g_dyn_dtest = -1; g_dyn_dwrite = -1;  /* V3: Dynamic-State pro CB frisch */
            g_dyn_discard = -1; g_dyn_vp_set = 0; g_dyn_sc_set = 0;                 /* V3b */
            g_dyn_blend_en = -1; g_dyn_wmask = -1; g_dyn_dcompare = -1;
            for (int i = 0; i < cb->ncmd; i++) {
                const rt_cmd_t *c = &cb->cmd[i];
                switch (c->op) {
                case C_BEGINRP:
                    rp = (const rt_rp_t *)(unsigned long)c->a;
                    fb = (const rt_fb_t *)(unsigned long)c->b;
                    if (rp && fb) { exec_clear(rp, fb, c); }
                    break;
                case C_ENDRP:    rp = 0; fb = 0; break;
                case C_BEGINDYNR: {   /* V3b: Dynamic-Rendering -- transientes FB/RP aufbauen */
                    rt_view_t *cvv = (rt_view_t *)(unsigned long)c->a;
                    rt_view_t *dvv = (rt_view_t *)(unsigned long)c->b;
                    if (!cvv || !cvv->image) { break; }
                    memset(&dyn_fb, 0, sizeof(dyn_fb));
                    memset(&dyn_rp, 0, sizeof(dyn_rp));
                    dyn_fb.used = 1; dyn_fb.n_color = 1; dyn_fb.color[0] = cvv;
                    dyn_fb.depth = (dvv && dvv->image) ? dvv : 0;
                    dyn_fb.width = cvv->image->extent.width;
                    dyn_fb.height = cvv->image->extent.height;
                    dyn_rp.used = 1; dyn_rp.n_color = 1;
                    dyn_rp.color_clear[0] = (c->d & 1u) ? 1 : 0; dyn_rp.color_att[0] = 0;
                    dyn_rp.depth_fmt = dyn_fb.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
                    dyn_rp.depth_clear = (c->d & 2u) ? 1 : 0;
                    rp = &dyn_rp; fb = &dyn_fb;
                    exec_clear(rp, fb, c);            /* loadOp CLEAR auf die renderArea anwenden */
                    break;
                }
                case C_BINDPIPE:
                    if (c->b == 1) { cpipe = (const rt_pipe_t *)(unsigned long)c->a; }  /* V1.7 Compute */
                    else           { pipe  = (const rt_pipe_t *)(unsigned long)c->a; }
                    break;
                case C_DISPATCH:   /* V1.7 */
                    exec_dispatch(cpipe, dset, (unsigned)c->a, (unsigned)c->rx, (unsigned)c->ry);
                    break;
                case C_RESOLVEIMG: {   /* V1.6: MSAA-Resolve (Mittelung der Sample-Ebenen) */
                    rt_image_t *src = (rt_image_t *)(unsigned long)c->a;
                    rt_image_t *dst = (rt_image_t *)(unsigned long)c->b;
                    unsigned *sp = src ? (unsigned *)(void *)rt_image_pixels(src) : 0;
                    unsigned *dp = dst ? (unsigned *)(void *)rt_image_pixels(dst) : 0;
                    if (sp && dp && src->samples >= 1) {
                        unsigned swpr = (unsigned)(src->row_pitch / 4);
                        unsigned dwpr = (unsigned)(dst->row_pitch / 4);
                        unsigned W = src->extent.width, H = src->extent.height;
                        long long splane = (long long)H * swpr;
                        int n = src->samples;
                        for (unsigned y = 0; y < H; y++) {
                            for (unsigned x = 0; x < W; x++) {
                                unsigned aa = 0, ar = 0, ag = 0, ab = 0;
                                for (int s = 0; s < n; s++) {
                                    unsigned w = sp[(long long)s * splane + y * swpr + x];
                                    aa += (w >> 24) & 0xFF; ar += (w >> 16) & 0xFF;
                                    ag += (w >> 8) & 0xFF;  ab += w & 0xFF;
                                }
                                unsigned rr = (ar + n / 2) / n, gg = (ag + n / 2) / n;
                                unsigned bb = (ab + n / 2) / n, al = (aa + n / 2) / n;
                                dp[y * dwpr + x] = (al << 24) | (rr << 16) | (gg << 8) | bb;
                            }
                        }
                    }
                    break;
                }
                case C_BINDVB:
                    vb = (const rt_buffer_t *)(unsigned long)c->a;
                    vboff = c->b;
                    break;
                case C_BINDIB:
                    ib = (const rt_buffer_t *)(unsigned long)c->a;
                    iboff = c->b; itype = c->itype;
                    break;
                case C_BINDDESC:
                    dset = (const rt_dset_t *)(unsigned long)c->d;
                    break;
                case C_PUSH:     break;                /* Push-Zustand liegt im Draw-Snapshot dpush[] */
                case C_DRAW:
                    if (rp && fb) {
                        /* Push-Constants dieses Draws aus dem beim Aufzeichnen eingefrorenen
                         * Snapshot (nicht der globale letzte Push des CB). */
                        exec_draw(pipe, fb, vb, vboff, cb->dpush[i], (unsigned)c->a, (unsigned)c->b,
                                  ib, iboff, itype, c->indexed, c->vertex_offset,
                                  c->inst_count, c->first_inst, dset);
                    }
                    break;
                case C_DRAWINDIRECT:   /* V3: Draw-Parameter aus einem Buffer (+optionaler Count-Buffer) */
                    if (rp && fb) {
                        rt_buffer_t *idb = (rt_buffer_t *)(unsigned long)c->a;
                        if (idb && idb->mem) {
                            unsigned drawCount = c->inst_count;
                            rt_buffer_t *cbuf = (rt_buffer_t *)(unsigned long)c->d;
                            if (cbuf && cbuf->mem) {   /* Count-Variante: min(*countBuffer, maxDrawCount) */
                                unsigned actual = *(const unsigned *)(const void *)
                                    (cbuf->mem->base + cbuf->off + (unsigned)c->rx);
                                if (actual < drawCount) { drawCount = actual; }
                            }
                            unsigned stride = c->first_inst;
                            const unsigned char *base = idb->mem->base + idb->off + (unsigned)c->b;
                            for (unsigned dc = 0; dc < drawCount; dc++) {
                                const unsigned *p = (const unsigned *)(const void *)(base + dc * stride);
                                if (c->indexed) {   /* indexCount,instanceCount,firstIndex,vertexOffset,firstInstance */
                                    exec_draw(pipe, fb, vb, vboff, cb->dpush[i], p[0], p[2],
                                              ib, iboff, itype, 1, (int)p[3], p[1], p[4], dset);
                                } else {            /* vertexCount,instanceCount,firstVertex,firstInstance */
                                    exec_draw(pipe, fb, vb, vboff, cb->dpush[i], p[0], p[2],
                                              ib, iboff, itype, 0, 0, p[1], p[3], dset);
                                }
                            }
                        }
                    }
                    break;
                case C_SETCULL: {   /* V3: Extended-Dynamic-State cullMode */
                    unsigned cm = (unsigned)c->a;
                    g_dyn_cull = (cm == VK_CULL_MODE_BACK_BIT)  ? R3D_CULL_BACK :
                                 (cm == VK_CULL_MODE_FRONT_BIT) ? R3D_CULL_FRONT : R3D_CULL_NONE;
                    break;
                }
                case C_SETFRONT:
                    g_dyn_front = (c->a == VK_FRONT_FACE_COUNTER_CLOCKWISE) ? 1 : 0;
                    break;
                case C_SETDTEST:
                    g_dyn_dtest = (int)c->a;
                    break;
                case C_SETDWRITE:
                    g_dyn_dwrite = (int)c->a;
                    break;
                case C_SETDISCARD:                     /* V3b: rasterizerDiscardEnable */
                    g_dyn_discard = (int)c->a;
                    break;
                case C_SETVP:                          /* V3b: dynamischer Viewport 0 */
                    g_dyn_vp_set = 1;
                    g_dyn_vp_x = c->cclear[0][0]; g_dyn_vp_y = c->cclear[0][1];
                    g_dyn_vp_w = c->cclear[0][2]; g_dyn_vp_h = c->cclear[0][3];
                    g_dyn_vp_minz = c->cclear[1][0]; g_dyn_vp_maxz = c->cclear[1][1];
                    break;
                case C_SETSC:                          /* V3b: dynamisches Scissor 0 */
                    g_dyn_sc_set = 1;
                    g_dyn_sc_x = c->rx; g_dyn_sc_y = c->ry; g_dyn_sc_w = c->rw; g_dyn_sc_h = c->rh;
                    break;
                case C_SETBLENDEN:                     /* V3b: EDS3 colorBlendEnable[0] */
                    g_dyn_blend_en = (int)c->a;
                    break;
                case C_SETWMASK:                       /* V3b: EDS3 colorWriteMask[0] */
                    g_dyn_wmask = (int)c->a;
                    break;
                case C_SETDCOMPARE:                    /* V3b: dynamischer depthCompareOp */
                    g_dyn_dcompare = (int)c->a;
                    break;
                case C_FILLBUF: {
                    rt_buffer_t *bf = (rt_buffer_t *)(unsigned long)c->a;
                    if (bf && bf->mem) {
                        unsigned *p = (unsigned *)(void *)(bf->mem->base + bf->off + (unsigned)c->rx);
                        for (int w = 0; w < c->rw / 4; w++) { p[w] = (unsigned)c->d; }
                    }
                    break;
                }
                case C_COPYBUF: {
                    rt_buffer_t *sb = (rt_buffer_t *)(unsigned long)c->a;
                    rt_buffer_t *db = (rt_buffer_t *)(unsigned long)c->b;
                    if (sb && sb->mem && db && db->mem) {
                        const unsigned char *s = sb->mem->base + sb->off + (unsigned)c->rx;
                        unsigned char *d = db->mem->base + db->off + (unsigned)c->ry;
                        for (int k = 0; k < c->rw; k++) { d[k] = s[k]; }
                    }
                    break;
                }
                case C_CLEARCOLIMG: {
                    rt_image_t *im = (rt_image_t *)(unsigned long)c->a;
                    unsigned *p = (unsigned *)(void *)rt_image_pixels(im);
                    if (p) {
                        unsigned word = pack_rgba(c->cclear[0][0], c->cclear[0][1], c->cclear[0][2], c->cclear[0][3]);
                        unsigned wpr = (unsigned)(im->row_pitch / 4);
                        for (unsigned y = 0; y < im->extent.height; y++) {
                            for (unsigned x = 0; x < im->extent.width; x++) { p[y * wpr + x] = word; }
                        }
                    }
                    break;
                }
                case C_COPYBUF2IMG: {   /* V3: Buffer -> Image (dicht gepackt, B8G8R8A8) */
                    rt_buffer_t *sb = (rt_buffer_t *)(unsigned long)c->a;
                    rt_image_t  *di = (rt_image_t *)(unsigned long)c->b;
                    unsigned *dp = di ? (unsigned *)(void *)rt_image_pixels(di) : 0;
                    if (sb && sb->mem && dp) {
                        const unsigned char *src = sb->mem->base + sb->off + (unsigned)c->d;
                        unsigned wpr = (unsigned)(di->row_pitch / 4);
                        for (int y = 0; y < c->rh; y++) {
                            for (int x = 0; x < c->rw; x++) {
                                const unsigned char *sp = src + (unsigned)((y * c->rw + x) * 4);
                                unsigned val = (unsigned)sp[0] | ((unsigned)sp[1] << 8) |
                                               ((unsigned)sp[2] << 16) | ((unsigned)sp[3] << 24);
                                dp[(unsigned)(c->ry + y) * wpr + (unsigned)(c->rx + x)] = val;
                            }
                        }
                    }
                    break;
                }
                case C_IMG2BUF: {   /* V3: Image -> Buffer (dicht gepackt, B8G8R8A8) */
                    rt_image_t  *si = (rt_image_t *)(unsigned long)c->a;
                    rt_buffer_t *db = (rt_buffer_t *)(unsigned long)c->b;
                    const unsigned *sp = si ? (const unsigned *)(void *)rt_image_pixels(si) : 0;
                    if (sp && db && db->mem) {
                        unsigned char *dst = db->mem->base + db->off + (unsigned)c->d;
                        unsigned wpr = (unsigned)(si->row_pitch / 4);
                        for (int y = 0; y < c->rh; y++) {
                            for (int x = 0; x < c->rw; x++) {
                                unsigned val = sp[(unsigned)(c->ry + y) * wpr + (unsigned)(c->rx + x)];
                                unsigned char *dp = dst + (unsigned)((y * c->rw + x) * 4);
                                dp[0] = (unsigned char)(val & 0xFF);       dp[1] = (unsigned char)((val >> 8) & 0xFF);
                                dp[2] = (unsigned char)((val >> 16) & 0xFF); dp[3] = (unsigned char)((val >> 24) & 0xFF);
                            }
                        }
                    }
                    break;
                }
                case C_IMG2IMG: {   /* V3: Image -> Image (gleiche Groesse) */
                    rt_image_t *si = (rt_image_t *)(unsigned long)c->a;
                    rt_image_t *di = (rt_image_t *)(unsigned long)c->b;
                    const unsigned *sp = si ? (const unsigned *)(void *)rt_image_pixels(si) : 0;
                    unsigned *dp = di ? (unsigned *)(void *)rt_image_pixels(di) : 0;
                    if (sp && dp) {
                        unsigned swpr = (unsigned)(si->row_pitch / 4), dwpr = (unsigned)(di->row_pitch / 4);
                        for (int y = 0; y < c->rh; y++) {
                            for (int x = 0; x < c->rw; x++) {
                                dp[(c->first_inst + (unsigned)y) * dwpr + (c->inst_count + (unsigned)x)] =
                                    sp[(unsigned)(c->ry + y) * swpr + (unsigned)(c->rx + x)];
                            }
                        }
                    }
                    break;
                }
                case C_BLITIMG: {   /* V3: skalierender Blit (nearest) */
                    rt_image_t *si = (rt_image_t *)(unsigned long)c->a;
                    rt_image_t *di = (rt_image_t *)(unsigned long)c->b;
                    const unsigned *sp = si ? (const unsigned *)(void *)rt_image_pixels(si) : 0;
                    unsigned *dp = di ? (unsigned *)(void *)rt_image_pixels(di) : 0;
                    if (sp && dp && c->rw > 0 && c->rh > 0 && c->inst_count > 0 && c->first_inst > 0) {
                        unsigned swpr = (unsigned)(si->row_pitch / 4), dwpr = (unsigned)(di->row_pitch / 4);
                        for (unsigned dy = 0; dy < c->first_inst; dy++) {
                            for (unsigned dx = 0; dx < c->inst_count; dx++) {
                                unsigned sx = (unsigned)c->rx + (dx * (unsigned)c->rw) / c->inst_count;
                                unsigned sy = (unsigned)c->ry + (dy * (unsigned)c->rh) / c->first_inst;
                                dp[((unsigned)c->itype + dy) * dwpr + ((unsigned)c->vertex_offset + dx)] =
                                    sp[sy * swpr + sx];
                            }
                        }
                    }
                    break;
                }
                /* --- V1.8: Query-Pools + Events --- */
                case C_BEGINQUERY: {
                    rt_querypool_t *qp = (rt_querypool_t *)(unsigned long)c->a;
                    if (qp && qp->used && qp->type == VK_QUERY_TYPE_OCCLUSION && (unsigned)c->b < qp->count) {
                        qp->result[c->b] = 0;                 /* Occlusion-Zaehler frisch */
                        g_occl_counter = &qp->result[c->b];   /* Draws zaehlen jetzt hierhin */
                    }
                    break;
                }
                case C_ENDQUERY: {
                    rt_querypool_t *qp = (rt_querypool_t *)(unsigned long)c->a;
                    if (qp && qp->used && (unsigned)c->b < qp->count) { qp->avail[c->b] = 1; }
                    g_occl_counter = 0;                        /* Zaehlung beendet */
                    break;
                }
                case C_RESETQUERY: {
                    rt_querypool_t *qp = (rt_querypool_t *)(unsigned long)c->a;
                    if (qp && qp->used) {
                        for (int k = 0; k < c->rw && (unsigned)((int)c->b + k) < qp->count; k++) {
                            qp->result[(int)c->b + k] = 0; qp->avail[(int)c->b + k] = 0;
                        }
                    }
                    break;
                }
                case C_WRITETS: {
                    rt_querypool_t *qp = (rt_querypool_t *)(unsigned long)c->a;
                    if (qp && qp->used && qp->type == VK_QUERY_TYPE_TIMESTAMP && (unsigned)c->b < qp->count) {
                        qp->result[c->b] = ++g_ts_counter;    /* streng monoton -> t2 > t1 */
                        qp->avail[c->b] = 1;
                    }
                    break;
                }
                case C_SETEVENT: {
                    rt_event_t *e = (rt_event_t *)(unsigned long)c->a;
                    if (e && e->used) { e->signaled = 1; }
                    break;
                }
                case C_RESETEVENT: {
                    rt_event_t *e = (rt_event_t *)(unsigned long)c->a;
                    if (e && e->used) { e->signaled = 0; }
                    break;
                }
                default: break;
                }
            }
        }
    }
    if (fence) { ((rt_fence_t *)(void *)fence)->signaled = 1; }
    return VK_SUCCESS;
}

/* V3: Core-1.3 Synchronization2 -- vkQueueSubmit2 uebersetzt VkSubmitInfo2 (Command-Buffer-Infos
 * + Semaphore-Infos) auf den bestehenden, verifizierten Executor: jeder Command-Buffer wird als
 * 1-elementiges VkSubmitInfo synchron ausgefuehrt; Semaphore sind in dieser synchronen
 * In-Order-Implementierung No-ops (Wait ist trivial erfuellt). Fence einmal am Ende signalisiert. */
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence)
{
    for (uint32_t s = 0; s < submitCount; s++) {
        for (uint32_t b = 0; b < pSubmits[s].commandBufferInfoCount; b++) {
            VkCommandBuffer cb = pSubmits[s].pCommandBufferInfos[b].commandBuffer;
            VkSubmitInfo si; memset(&si, 0, sizeof(si));
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        }
    }
    if (fence) { ((rt_fence_t *)(void *)fence)->signaled = 1; }
    return VK_SUCCESS;
}
