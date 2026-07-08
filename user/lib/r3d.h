/*
 * user/lib/r3d.h  --  Software-3D-Rasterizer.
 */
#ifndef RPI_RTOS_R3D_H
#define RPI_RTOS_R3D_H

typedef struct { float x, y, z, w; } r3d_vec4;
typedef struct { float m[16]; } r3d_mat4;      /* column-major: m[spalte*4 + zeile] */

#define R3D_MAX_ATTR 8

/* Ein Vertex im CLIP-SPACE (Ausgabe des "Vertex-Shaders") + bis zu R3D_MAX_ATTR
 * float-Attribute (z.B. Farbe r,g,b in [0,1]), die zum Fragment interpoliert werden. */
typedef struct {
    r3d_vec4 pos;
    float    attr[R3D_MAX_ATTR];
} r3d_vtx_t;

#define R3D_CULL_NONE  0
#define R3D_CULL_BACK  1
#define R3D_CULL_FRONT 2

/* Render-Target + Pipeline-Zustand (entspricht Viewport/Scissor/Raster/DepthState). */
typedef struct {
    unsigned *color;         /* 0x00RRGGBB je Pixel; Zeilenabstand pitch_px WOERTER */
    float    *depth;         /* width*height floats (Zeilenabstand width) oder 0 = kein Depth */
    int width, height;       /* Groesse des Targets in Pixeln */
    int pitch_px;            /* Woerter je Zeile des color-Puffers */
    /* Viewport (Vulkan: x,y Offset; w,h Ausdehnung; minz/maxz Tiefenfenster). */
    float vp_x, vp_y, vp_w, vp_h, vp_minz, vp_maxz;
    /* Scissor-Rechteck (Pixel). */
    int sc_x, sc_y, sc_w, sc_h;
    int cull_mode;            /* R3D_CULL_* */
    int front_ccw;            /* 1: counter-clockwise (Framebuffer) ist Front, 0: clockwise */
    int depth_test;           /* 1: Tiefen-Vergleich gegen *depth aktiv */
    int depth_compare;        /* VkCompareOp-Wert: 0=NEVER,1=LESS,2=EQUAL,3=LEQUAL,4=GREATER,
                               * 5=NOTEQUAL,6=GEQUAL,7=ALWAYS. 0-init (=NEVER) waere degeneriert;
                               * jeder Erzeuger setzt es explizit (Pipeline default LESS). */
    int depth_write;          /* 1: bestandene Fragmente schreiben z */
    /* V1.6 MSAA: >1 (nur 4) -> color/depth sind SAMPLE-MAJOR (Ebene s bei Offset
     * s*height*pitch_px bzw. s*height*width). Coverage+Tiefe je Sample, FS 1x je Pixel. */
    int samples;
} r3d_target_t;

/* Fragment-Shader-Callback: liefert 0x00RRGGBB fuer Pixel (px,py) mit screen-linearer
 * Tiefe z und perspektivisch korrekt interpolierten Attributen. 0 = Standard-Gouraud
 * (attr[0..2] als r,g,b in [0,1] gepackt). 'user' wird durchgereicht. */
typedef unsigned (*r3d_fs_fn)(void *user, int px, int py, float z, const float *attr);

/* Ein Dreieck (Clip-Space-Vertices) durch die komplette Pipeline zeichnen.
 * nattr = Anzahl genutzter Attribute (0..R3D_MAX_ATTR). */
void r3d_draw_tri(const r3d_target_t *t,
                  const r3d_vtx_t *a, const r3d_vtx_t *b, const r3d_vtx_t *c,
                  int nattr, r3d_fs_fn fs, void *user);

/* Depth-Buffer auf 'value' (typisch 1.0) setzen. */
void r3d_depth_clear(float *depth, int count, float value);

/* --- Vektor/Matrix (Column-Major, wie GLSL/SPIR-V) --- */
void r3d_mat4_identity(r3d_mat4 *out);
void r3d_mat4_mul(r3d_mat4 *out, const r3d_mat4 *a, const r3d_mat4 *b);   /* out = a*b */
void r3d_mat4_mul_vec4(r3d_vec4 *out, const r3d_mat4 *m, const r3d_vec4 *v);
void r3d_mat4_rot_x(r3d_mat4 *out, float rad);
void r3d_mat4_rot_y(r3d_mat4 *out, float rad);
void r3d_mat4_translate(r3d_mat4 *out, float x, float y, float z);
/* Perspektivische Projektion mit VULKAN-Tiefenbereich z_ndc in [0,1] und NEGIERTEM
 * Y (m[1][1] = -f): Welt-+Y erscheint auf dem y-down-Framebuffer OBEN. */
void r3d_mat4_perspective(r3d_mat4 *out, float fovy_rad, float aspect, float znear, float zfar);

/* Trigonometrie ohne libm (Bereichsreduktion + Taylor; |err| < 1e-5). */
float r3d_sinf(float x);
float r3d_cosf(float x);

#endif /* RPI_RTOS_R3D_H */
