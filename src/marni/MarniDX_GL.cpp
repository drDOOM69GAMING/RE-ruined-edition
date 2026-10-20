// MarniDX_GL.cpp - OpenGL implementation of the MarniDX backend interface.
//
// This is the Linux counterpart of MarniDX.cpp (Direct3D 11). It defines the
// same class, the same globals and the same opaque MarniHandle table, so every
// caller in src/game/ works unchanged. See docs/LINUX_PORT.md sections 3 and 6.
//
// Written against the GLES-3-compatible subset: core-profile VAOs/VBOs, no
// fixed-function state, no desktop-only entry points, so the body can be
// repointed at GLES for Android/Switch later.
//
// Phase 2 scope: context lifecycle, Clear/Present, texture management and the
// 2D quad/triangle path. DrawTrianglesPersp/DrawTriangles3D currently route to
// the affine 2D path; real depth and perspective-correct interpolation land in
// Phase 4 with the TMD renderer.
#include "MarniDX.h"
#include "MarniGLFuncs.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>

// DisplayModeInfo / D3DRendererInfo live with the rest of the shared types.
#include "../game/Types.h"

// plat_video_is_active: line-art mode is disabled while an FMV is playing.
// Other backends in this layer already include the platform header.
#include "../platform/platform.h"

// Line-art mode flag: defined in Globals.cpp, persisted as config.ini
// [Display] LineArt, toggled live with F7 (main.cpp). g_iLineArtDetail is the
// 1..5 edge-ramp level, persisted as [Display] LineArtDetail, stepped with
// F8/F9.
extern BOOL g_bLineArt;
extern int  g_iLineArtDetail;

// Stick-figure player (port extension): draw the main character's skeleton as
// ink over the composited page. Declared in game/TmdRenderer.h; drawn only when
// line-art mode is on, and only from Present, after the edge pass, so the
// figure is a pen stroke rather than something the shader re-detects.
extern void StickFigure_DrawOverlay(int depthTest);

#define MARNI_MAX_TEXTURES 2048

// ---------------------------------------------------------------------------
// Backend state
// ---------------------------------------------------------------------------

struct MarniDX::Impl {
    SDL_Window*   window = nullptr;
    SDL_GLContext ctx    = nullptr;
    int  width  = 0;
    int  height = 0;
    bool ready  = false;

    GLuint program = 0;
    GLuint vao     = 0;
    GLuint vbo     = 0;
    GLint  uMVP    = -1;
    GLint  uTex    = -1;
    GLint  uPersp  = -1;
    GLint  uForceAlpha = -1;   // 1 forces a solid alpha for room masks

    MarniHandle whiteHandle = MARNI_NULL_HANDLE;
    MarniHandle fontHandle  = MARNI_NULL_HANDLE;
    int fontW = 0, fontH = 0;

    // 4:3 letterbox (port extension): fbW/fbH is the full drawable; width/
    // height - what the game renders to and what GetBackBufferSize reports -
    // is the largest 4:3 rectangle centred inside it, with vpX/vpY its origin
    // in the drawable (GL's bottom-up coordinates). On a 16:9 screen the game
    // picture stays undistorted and the side columns are black.
    int fbW = 0, fbH = 0;
    int vpX = 0, vpY = 0;

    // Line-art composite (g_bLineArt, [Display] LineArt / F7): the scene is
    // rendered into laFbo at the content size and Present re-draws it through
    // a two-pass edge pipeline. Sampled once per frame in Clear.
    bool   laActive = false;
    GLuint laFbo    = 0;
    GLuint laTex    = 0;
    // The edge composite and the stick-figure ink land in laPageFbo, which
    // shares the depth image with laFbo: the figures are depth-tested against
    // the scene geometry (so a wall hides them) but never write depth. The
    // depth image is a TEXTURE, not a renderbuffer, so the edge-field pass
    // can sample it and draw the clean silhouettes (characters, objects,
    // furniture) that carry the hand-drawn outline look.
    GLuint laDepthTex = 0;
    GLuint laPageFbo = 0;
    GLuint laPageTex = 0;
    int    laW      = 0, laH = 0;
    // Two passes. Pass A (laEdgeProgram) blurs the scene and measures the
    // gradient magnitude AND direction from colour and depth into laEdgeFbo.
    // Pass B (laProgram) thins that field with non-maximum suppression into
    // contour strokes and composites the pen-and-ink drawing (contours plus
    // hatching) into laPageFbo.
    GLuint laProgram = 0;      // pass B: ink composite
    GLuint laEdgeProgram = 0;  // pass A: edge field
    GLuint laEdgeFbo  = 0;
    GLuint laEdgeTex  = 0;
    GLuint laVao     = 0;
    GLuint laVbo     = 0;
    GLint  laUScene  = -1;
    GLint  laUTexel  = -1;
    GLint  laUEdgeF  = -1;
    GLint  laUEdge   = -1;
    GLint  laUHatch  = -1;
    GLint  laUNoInk  = -1;
    GLint  laUDbg    = -1;
    GLint  laeUScene = -1;
    GLint  laeUDepth = -1;
    GLint  laeUTexel = -1;
    GLint  laeUBlur  = -1;
    GLint  laeUDepthOn = -1;

    struct TexSlot {
        GLuint tex = 0;
        int w = 0, h = 0;
    } slots[MARNI_MAX_TEXTURES];

    void ReleaseSlot(MarniHandle h) {
        if (h == MARNI_NULL_HANDLE || h >= MARNI_MAX_TEXTURES) return;
        if (slots[h].tex != 0) glDeleteTextures(1, &slots[h].tex);
        slots[h].tex = 0;
        slots[h].w = slots[h].h = 0;
    }

    void ReleaseAll() {
        for (MarniHandle h = 1; h < MARNI_MAX_TEXTURES; ++h) ReleaseSlot(h);
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        if (program) { glDeleteProgram(program); program = 0; }
        if (laFbo) { glDeleteFramebuffers(1, &laFbo); laFbo = 0; }
        if (laTex) { glDeleteTextures(1, &laTex); laTex = 0; }
        if (laPageFbo) { glDeleteFramebuffers(1, &laPageFbo); laPageFbo = 0; }
        if (laPageTex) { glDeleteTextures(1, &laPageTex); laPageTex = 0; }
        if (laDepthTex) { glDeleteTextures(1, &laDepthTex); laDepthTex = 0; }
        if (laEdgeFbo) { glDeleteFramebuffers(1, &laEdgeFbo); laEdgeFbo = 0; }
        if (laEdgeTex) { glDeleteTextures(1, &laEdgeTex); laEdgeTex = 0; }
        if (laProgram) { glDeleteProgram(laProgram); laProgram = 0; }
        if (laEdgeProgram) { glDeleteProgram(laEdgeProgram); laEdgeProgram = 0; }
        if (laVbo) { glDeleteBuffers(1, &laVbo); laVbo = 0; }
        if (laVao) { glDeleteVertexArrays(1, &laVao); laVao = 0; }
        laW = laH = 0;
        whiteHandle = MARNI_NULL_HANDLE;
        fontHandle  = MARNI_NULL_HANDLE;
    }
};

static MarniDX* g_pDX = nullptr;

MarniDX* Marni_DX() { return g_pDX; }

// ---------------------------------------------------------------------------
// Shader / pipeline setup
// ---------------------------------------------------------------------------

static const char* kVertexSrc =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "layout(location = 2) in vec4 aCol;\n"
    "layout(location = 3) in vec2 aZW;\n"      // x = depth [0,1], y = view-space w
    "uniform mat4 uMVP;\n"
    "uniform int  uPersp;\n"
    "out vec2 vUV;\n"
    "out vec4 vCol;\n"
    "void main() {\n"
    // GL's NDC z is [-1,1]; the callers pass normalised [0,1] depth.
    "    float z = aZW.x * 2.0 - 1.0;\n"
    "    vec4 p = uMVP * vec4(aPos, z, 1.0);\n"
    // Perspective-correct path: premultiply by w so the rasteriser's divide
    // restores the same screen position while interpolating UV/colour with 1/w.
    // Clamped like the Windows VS (max(input.pos.w, 1e-4)) so a degenerate
    // vertex cannot produce a NaN.
    "    if (uPersp != 0) {\n"
    "        float w = max(aZW.y, 1e-4);\n"
    "        p = vec4(p.xy * w, p.z * w, w);\n"
    "    }\n"
    "    gl_Position = p;\n"
    "    vUV = aUV;\n"
    "    vCol = aCol;\n"
    "}\n";

static const char* kFragmentSrc =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "in vec4 vCol;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uForceAlpha;   // 1: write alpha 1 regardless of the texture\n"
    "out vec4 oCol;\n"
    "void main() {\n"
    "    oCol = texture(uTex, vUV) * vCol;\n"
    "    if (uForceAlpha != 0) oCol.a = 1.0;\n"
    "}\n";

static GLuint CompileShader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
        log[n] = '\0';
        fprintf(stderr, "[GL] shader compile failed: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool BuildPipeline(MarniDX::Impl* p)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentSrc);
    if (!vs || !fs) return false;

    p->program = glCreateProgram();
    glAttachShader(p->program, vs);
    glAttachShader(p->program, fs);
    glLinkProgram(p->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(p->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetProgramInfoLog(p->program, sizeof(log) - 1, &n, log);
        log[n] = '\0';
        fprintf(stderr, "[GL] program link failed: %s\n", log);
        glDeleteProgram(p->program);
        p->program = 0;
        return false;
    }

    p->uMVP = glGetUniformLocation(p->program, "uMVP");
    p->uTex = glGetUniformLocation(p->program, "uTex");
    p->uPersp = glGetUniformLocation(p->program, "uPersp");
    p->uForceAlpha = glGetUniformLocation(p->program, "uForceAlpha");

    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);

    // Internal vertex layout: x, y, z, w, u, v, r, g, b, a (10 floats).
    const GLsizei stride = 10 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    return true;
}

// ---------------------------------------------------------------------------
// 4:3 letterbox + line-art composite (port extensions)
// ---------------------------------------------------------------------------

// Largest 4:3 rectangle centred in the drawable. width/height become the
// content size (what the game renders to) and vpX/vpY its origin, so the
// picture keeps its aspect on any display and the surround stays black.
static void UpdateViewportRect(MarniDX::Impl* p)
{
    if (p->fbW <= 0 || p->fbH <= 0) return;
    int w = p->fbW, h = (p->fbW * 3) / 4;
    if (h > p->fbH) {
        h = p->fbH;
        w = (p->fbH * 4) / 3;
    }
    p->vpX = (p->fbW - w) / 2;
    p->vpY = (p->fbH - h) / 2;
    p->width = w;
    p->height = h;
}

// Fullscreen NDC quad -> pen-and-ink drawing. The scene FBO is
// rendered at the content size, so the Sobel kernel steps one scene texel
// and the lines are hairline-true at any output resolution.
static const char* kLineArtVertexSrc =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

// ---------------------------------------------------------------------------
// Edge extraction, v2 (two passes).
//
// v1 ran a single 25-tap filter over the finished frame with five edge cues.
// Two of those cues - a 3x3 max-min "range" and a wide band-pass - were
// designed to fire on every single-texel feature, so every brick, carpet and
// wallpaper texel became a little square of ink and the page read as
// corrupted rather than drawn. It also had no line thinning, so strong
// responses smeared into blocks.
//
// v2 is the classic drawing pipeline instead:
//
//   Pass A (this shader, kEdgeFieldSrc): one 7x7 gather over the scene,
//   alpha-gated so stick-figure ink cannot halo itself, computes two
//   gaussian scales. Their difference (a Difference-of-Gaussians) is the
//   edge strength - flat in a featureless area, a smooth bump across an
//   edge however gradual. The derivative of the wide scale gives the edge
//   DIRECTION. A depth cue adds true silhouettes: a relative step between
//   neighbour depths outlines characters and objects without drawing the
//   perspective ramp of floors and walls. Output is magnitude + direction
//   into laEdgeTex.
//
//   Pass B (kLineArtFragmentSrc): non-maximum suppression along the stored
//   direction thins every response to a 1-texel ridge - that is what turns
//   smears into strokes - and a soft dual ramp anti-aliases them. Two
//   lighter shoulders across the line give the stroke weight.
//
// Texture noise dies in the pre-blur; the old range/band cues are gone. The
// detail ramp (kLineArtEdge) still chooses how soft an edge may be and still
// draw, but because every level is thinned, raising detail now adds finer
// lines instead of filling areas.
static const char* kEdgeFieldSrc =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 oCol;\n"
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uDepth;\n"
    "uniform vec2 uTexel;\n"
    "uniform float uBlur;\n"       // small gaussian sigma, in texels
    "uniform float uDepthOn;\n"    // 0 when no depth texture is attached
    "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "vec3 lift(vec3 c) { return sqrt(max(c, 0.0)); }\n"
    "void main() {\n"
    // The gather feeds everything at once: two gaussian scales and the
    // derivative of the wide one. Pixels with alpha < 0.5 are stick-figure
    // ink (DrawInkLine markers), gated out so a figure never edges itself.
    "    vec3 acc1 = vec3(0.0); float w1 = 0.0;\n"
    "    vec3 acc2 = vec3(0.0); float w2 = 0.0;\n"
    "    vec2 gLum = vec2(0.0);\n"
    "    vec3 gcx = vec3(0.0);\n"    // per-channel gradient, x part
    "    vec3 gcy = vec3(0.0);\n"    // per-channel gradient, y part
    "    float s1sq = max(uBlur * uBlur, 0.04);\n"
    "    float s2sq = s1sq * 4.0;\n"   // wide sigma is 2x the small one
    "    for (int j = -3; j <= 3; ++j) {\n"
    "        for (int i = -3; i <= 3; ++i) {\n"
    "            vec2 off = vec2(float(i), float(j));\n"
    "            vec4 s = texture(uScene, vUV + off * uTexel);\n"
    "            float gate = (s.a > 0.5) ? 1.0 : 0.0;\n"
    "            vec3 lc = lift(s.rgb);\n"
    "            float d2 = dot(off, off);\n"
    "            float wa = exp(-d2 / (2.0 * s1sq)) * gate;\n"
    "            float wb = exp(-d2 / (2.0 * s2sq)) * gate;\n"
    "            acc1 += lc * wa; w1 += wa;\n"
    "            acc2 += lc * wb; w2 += wb;\n"
    // d/dx of a gaussian is -x/sigma^2 times the weight; dividing by the
    // weight sum afterwards makes this the properly smoothed derivative.
    "            gLum += lum(lc) * wb * (-off / s2sq);\n"
    // Per-channel gradient needs its own x and y accumulators: the colour
    // cue wants the magnitude of each channel's gradient, so keep the two
    // components separate and combine after the loop.
    "            gcx += lc * (wb * -off.x / s2sq);\n"
    "            gcy += lc * (wb * -off.y / s2sq);\n"
    "        }\n"
    "    }\n"
    "    vec3 b1 = acc1 / max(w1, 1e-4);\n"
    "    vec3 b2 = acc2 / max(w2, 1e-4);\n"
    "    gLum /= max(w2, 1e-4);\n"
    "    gcx /= max(w2, 1e-4);\n"
    "    gcy /= max(w2, 1e-4);\n"
    "    float dog = abs(lum(b1) - lum(b2));\n"
    // Colour gradient on the smoothed colour: hue-only boundaries (equal
    // luminance) exist in the texture pages and the DoG misses them. But the
    // hue and raw luma-gradient cues also light up fine texture and dither,
    // which reads as scattered specks rather than contour, so they are kept
    // well under the Difference-of-Gaussians. The DoG plus the depth
    // silhouette below are what carry a real outline; the gradient cues only
    // fill in a genuine colour boundary the DoG cannot see.
    "    vec3 cmag = sqrt(gcx * gcx + gcy * gcy);\n"
    "    float hue = max(cmag.r, max(cmag.g, cmag.b));\n"
    "    float e = max(dog * 3.2, max(length(gLum) * 2.0, hue * 1.6));\n"
    // Depth silhouettes: a RELATIVE step between neighbour depths, so a
    // floor receding at a grazing angle (whose absolute depth slope can
    // match a silhouette's at range) stays clean while a character against
    // a wall draws a firm line. The 0.03 floor eats the perspective ramp of
    // flat surfaces; depth 1.0 is the cleared background, not geometry.
    "    float dC = texture(uDepth, vUV).r;\n"
    "    if (uDepthOn > 0.5 && dC < 0.9999) {\n"
    "        float md = 0.0; vec2 mdir = vec2(0.0);\n"
    "        for (int j = -1; j <= 1; ++j) {\n"
    "            for (int i = -1; i <= 1; ++i) {\n"
    "                if (i == 0 && j == 0) continue;\n"
    "                vec2 off = vec2(float(i), float(j));\n"
    "                float dN = texture(uDepth, vUV + off * uTexel).r;\n"
    "                float rel = abs(dN - dC) / max(min(dN, dC), 1e-4) - 0.03;\n"
    "                if (rel > md) { md = rel; mdir = off; }\n"
    "            }\n"
    "        }\n"
    "        e = max(e, min(md * 1.4, 1.0));\n"
    "        gLum += mdir * md * 0.6;\n"   // steer NMS across the silhouette
    "    }\n"
    "    vec2 dir = gLum;\n"
    "    float dl = length(dir);\n"
    "    dir = (dl > 1e-5) ? dir / dl : vec2(1.0, 0.0);\n"
    "    oCol = vec4(clamp(e, 0.0, 1.0), dir.x * 0.5 + 0.5, dir.y * 0.5 + 0.5, 1.0);\n"
    "}\n";

// Pass B: the pen-and-ink drawing. Non-maximum suppression over the edge
// field thins responses into contour strokes; hatching layers switch on as
// the scene darkens (single hatch -> crosshatch -> third pass) so tone is
// carried by drawn marks the way an ink illustration carries it, with lit
// areas left as clean paper. Contours and hatch composite onto a lightly
// washed paper base. This shader also keeps two contracts from v1: the
// stick-figure ink marker (alpha < 0.5) is copied straight through, and
// uNoInk (RE1_NOBGINK test hook) shows the plain scene for headless figure
// captures.
static const char* kLineArtFragmentSrc =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 oCol;\n"
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uEdgeField;\n"
    "uniform vec2 uTexel;\n"
    "uniform vec2 uEdge;\n"      // soft ramp: x = fade-in, y = full ink
    "uniform float uHatch;\n"    // hatch stroke pitch in texels (detail-scaled)
    "uniform float uNoInk;\n"
    // Test hook only. uDebug > 0.5 draws the raw edge magnitude as a grey
    // field instead of ink-on-paper, so a headless capture can be histogrammed
    // to calibrate the uEdge ramp against real scene data. Never set in play.
    "uniform float uDebug;\n"
    "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    // Cheap 2D value noise. Used to wobble contour sampling and to bend hatch
    // strokes, so the drawing never shows a machine-perfect straight line.
    "float vhash(vec2 p) {\n"
    "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
    "    p3 += dot(p3, p3.yzx + 33.33);\n"
    "    return fract((p3.x + p3.y) * p3.z);\n"
    "}\n"
    "float vnoise(vec2 p) {\n"
    "    vec2 i = floor(p); vec2 f = fract(p);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    float a = vhash(i);\n"
    "    float b = vhash(i + vec2(1.0, 0.0));\n"
    "    float c = vhash(i + vec2(0.0, 1.0));\n"
    "    float d = vhash(i + vec2(1.0, 1.0));\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    // One set of hatch strokes at angle `ang` (radians), `pitch` texels apart.
    // The strokes wander sideways like a hand line and vary in weight along
    // their length with pen pressure; `seed` offsets the noise so the layers
    // do not correlate into a weave.
    "float hatchLine(vec2 px, float ang, float pitch, float seed) {\n"
    "    vec2 dir = vec2(cos(ang), sin(ang));\n"
    "    float across = dot(px, vec2(-dir.y, dir.x));\n"
    "    float along = dot(px, dir);\n"
    "    across += (vnoise(vec2(along * 0.015, seed)) - 0.5) * pitch * 0.55;\n"
    "    float f = abs(fract(across / pitch) - 0.5) * pitch;\n"
    "    float w = pitch * 0.17 * (0.72 + 0.55 * vnoise(vec2(along * 0.04, seed + 3.7)));\n"
    "    return smoothstep(w, w * 0.45, f);\n"
    "}\n"
    // One ridge evaluation: compare the field against itself along the stored
    // gradient. The offset is 1.8 texels - wide enough that the neighbour
    // samples land past the pre-blurred plateau of a real edge, so its own
    // ridge reads clearly above the slope (e >> m) instead of e ~ m on a flat
    // top, which is what used to collapse the response. A ~2.2 gain then lifts
    // the surviving ridges back above the paper-noise floor. The soft
    // comparison keeps the ridge anti-aliased instead of breaking it at
    // quantisation steps.
    "float ridge(vec2 uv) {\n"
    "    vec4 f = texture(uEdgeField, uv);\n"
    "    float e = f.r;\n"
    "    if (e <= 0.004) return 0.0;\n"
    "    vec2 d = f.gb * 2.0 - 1.0;\n"
    "    float l = length(d);\n"
    "    d = (l > 1e-4) ? d / l : vec2(1.0, 0.0);\n"
    "    float m = max(texture(uEdgeField, uv + d * (1.8 * uTexel)).r,\n"
    "                  texture(uEdgeField, uv - d * (1.8 * uTexel)).r);\n"
    "    return 2.2 * e * smoothstep(-0.02, 0.025, e - m);\n"
    "}\n"
    "void main() {\n"
    "    vec4 sc = texture(uScene, vUV);\n"
    // Figure ink is a near-black page colour, copied straight through (the
    // contract): on paper it reads exactly like drawn linework, the same as
    // the contours below, so no adjustment is needed.
    "    if (sc.a < 0.5) { oCol = vec4(sc.rgb, 1.0); return; }\n"
    "    if (uNoInk > 0.5) { oCol = vec4(sc.rgb, 1.0); return; }\n"
    "    vec4 f = texture(uEdgeField, vUV);\n"
    // Calibration test hook: draw the post-NMS ridge response as a grey field
    // (uDebug==1), or the raw magnitude (uDebug==2), so a headless capture can
    // be histogrammed to place the uEdge ramp against real scene data. Never
    // set in play.
    "    if (uDebug > 1.5) { oCol = vec4(vec3(f.r), 1.0); return; }\n"
    "    if (uDebug > 0.5) { oCol = vec4(vec3(ridge(vUV)), 1.0); return; }\n"
    "    vec2 d = f.gb * 2.0 - 1.0;\n"
    "    float l = length(d);\n"
    "    d = (l > 1e-4) ? d / l : vec2(1.0, 0.0);\n"
    "    vec2 perp = vec2(-d.y, d.x);\n"
    // Hand jitter: displace the sampling point by a smooth sub-texel wobble
    // so contours wander the way a drawn line does instead of sitting
    // machine-perfect on the edge.
    "    vec2 juv = vUV + (vec2(vnoise(vUV * 420.0),\n"
    "                           vnoise(vUV * 420.0 + 19.7)) - 0.5) * (1.8 * uTexel);\n"
    // Three passes across the line: the core at full ink, the two shoulders
    // lighter - a stroke with weight and a soft edge instead of a hard slab.
    "    float r0 = ridge(juv);\n"
    "    float r1 = ridge(juv + perp * 0.9 * uTexel);\n"
    "    float r2 = ridge(juv - perp * 0.9 * uTexel);\n"
    "    float ink = smoothstep(uEdge.x, uEdge.y, r0);\n"
    "    ink = max(ink, 0.80 * smoothstep(uEdge.x, uEdge.y, r1));\n"
    "    ink = max(ink, 0.62 * smoothstep(uEdge.x, uEdge.y, r2));\n"
    // Connectivity gate. A real contour runs on along its own tangent, while
    // an isolated texture speck has nothing beside it. Sample the field a few
    // texels along the line; if it is weak in BOTH directions the pixel is a
    // speck and its ink drops out, which is what keeps fine dither from
    // peppering the page. A genuine line end keeps its one strong side, so
    // strokes are not truncated. ca/cb read the raw edge magnitude (the gained
    // ridge the ramp compares is ~2.2x this), so the gate sits just under the
    // ridge floor.
    "    float ca = texture(uEdgeField, vUV + perp * (3.5 * uTexel)).r;\n"
    "    float cb = texture(uEdgeField, vUV - perp * (3.5 * uTexel)).r;\n"
    "    ink *= smoothstep(uEdge.x * 0.28, uEdge.x * 0.65, max(ca, cb));\n"
    // Pen pressure: real ink varies slightly in darkness along a stroke.
    "    ink *= 0.86 + 0.28 * vnoise(juv * 950.0);\n"
    // Hatching carries the tone the way an ink drawing does: parallel strokes
    // that wander and vary in weight, layered one direction at a time as the
    // scene darkens - a single hatch in light shade, a cross in mid-darkness,
    // a third pass in deep shadow. The tone is measured in the same lifted
    // (sqrt) space the edge field works in, so RE1's dim rooms spread across
    // the layers - light pools stay clean paper, mid-tones get one hatch,
    // shadows cross - instead of the whole room landing in deep shadow.
    "    vec2 hpx = vUV / uTexel;\n"
    "    float dark = 1.0 - sqrt(lum(sc.rgb));\n"
    "    float s1 = smoothstep(0.28, 0.46, dark);\n"
    "    float s2 = smoothstep(0.46, 0.62, dark);\n"
    "    float s3 = smoothstep(0.70, 0.86, dark);\n"
    "    float h = 0.0;\n"
    "    if (s1 > 0.0) h += s1 * hatchLine(hpx, 0.62, uHatch, 1.0);\n"
    "    if (s2 > 0.0) h += s2 * hatchLine(hpx, -0.78, uHatch * 0.94, 5.0);\n"
    "    if (s3 > 0.0) h += s3 * hatchLine(hpx, 1.45, uHatch * 0.88, 9.0);\n"
    "    h = min(h, 0.70);\n"
    // Paper with a light wash of the scene's own luminance so large forms
    // read between the marks; contours and hatch lay ink on top of it.
    "    float lo = lum(sc.rgb * sc.rgb);\n"
    "    vec3 paper = vec3(0.985, 0.980, 0.960) - 0.22 * (1.0 - lo);\n"
    "    float draw = max(ink, h * 0.85);\n"
    "    oCol = vec4(mix(paper, vec3(0.05, 0.05, 0.08), draw), 1.0);\n"
    "}\n";

// Edge-ramp levels for [Display] LineArtDetail (1..5), stepped live with
// Shift+F7 (more detail) and Ctrl+Shift+F7 (less). The ramp picks how soft an
// edge may be and still draw; because every level is thinned by the
// suppression pass, the levels differ in how many FINE lines appear, not in
// how much of the page is covered. Level 3 is the default, level 1 the
// boldest outlines, level 5 the finest.
static const float kLineArtEdge[5][2] = {
    { 0.118f, 0.176f },   // 1  boldest outlines only
    { 0.100f, 0.145f },   // 2
    { 0.086f, 0.122f },   // 3  balanced (default)
    { 0.074f, 0.106f },   // 4  fine
    { 0.064f, 0.092f },   // 5  finest lines
};

// Hatch stroke pitch per detail level (texels between strokes). Higher
// detail draws finer, closer hatching; the tone bands that select the layers
// live in the shader.
static const float kLineArtHatch[5] = { 10.0f, 8.5f, 7.2f, 6.1f, 5.2f };

// Pre-blur sigma per detail level (texels). Lower detail blurs more, which
// suppresses texture and leaves only the big shapes; higher detail resolves
// finer and finer linework. This, plus the ramp above, is what the levels
// actually trade off now.
static const float kLineArtBlur[5] = { 2.80f, 2.50f, 2.20f, 1.80f, 1.45f };
static const int kLineArtDetailMin = 1;
static const int kLineArtDetailMax = 5;

static void LineArt_EdgeRamp(int detail, float* out)
{
    if (detail < kLineArtDetailMin) detail = kLineArtDetailMin;
    if (detail > kLineArtDetailMax) detail = kLineArtDetailMax;
    out[0] = kLineArtEdge[detail - 1][0];
    out[1] = kLineArtEdge[detail - 1][1];
}

// Lazily build (and on a content-size change, rebuild) the line-art targets.
// Returns false when the pipeline could not be created; the caller then keeps
// rendering the plain picture instead of a black screen.
static bool EnsureLineArt(MarniDX::Impl* p)
{
    if (p->laProgram == 0 || p->laEdgeProgram == 0) {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, kLineArtVertexSrc);
        if (!vs) return false;

        // Pass B: the ink composite.
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kLineArtFragmentSrc);
        if (!fs) { glDeleteShader(vs); return false; }
        p->laProgram = glCreateProgram();
        glAttachShader(p->laProgram, vs);
        glAttachShader(p->laProgram, fs);
        glLinkProgram(p->laProgram);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p->laProgram, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            GLsizei n = 0;
            glGetProgramInfoLog(p->laProgram, sizeof(log) - 1, &n, log);
            log[n] = '\0';
            fprintf(stderr, "[GL] line-art link failed: %s\n", log);
            glDeleteProgram(p->laProgram);
            p->laProgram = 0;
            glDeleteShader(vs);
            return false;
        }
        p->laUScene = glGetUniformLocation(p->laProgram, "uScene");
        p->laUTexel = glGetUniformLocation(p->laProgram, "uTexel");
        p->laUEdgeF = glGetUniformLocation(p->laProgram, "uEdgeField");
        p->laUEdge  = glGetUniformLocation(p->laProgram, "uEdge");
        p->laUHatch = glGetUniformLocation(p->laProgram, "uHatch");
        p->laUNoInk = glGetUniformLocation(p->laProgram, "uNoInk");
        p->laUDbg   = glGetUniformLocation(p->laProgram, "uDebug");

        // Pass A: the edge field. Shares the vertex shader and the quad.
        fs = CompileShader(GL_FRAGMENT_SHADER, kEdgeFieldSrc);
        if (!fs) { glDeleteShader(vs); return false; }
        p->laEdgeProgram = glCreateProgram();
        glAttachShader(p->laEdgeProgram, vs);
        glAttachShader(p->laEdgeProgram, fs);
        glLinkProgram(p->laEdgeProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
        ok = 0;
        glGetProgramiv(p->laEdgeProgram, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            GLsizei n = 0;
            glGetProgramInfoLog(p->laEdgeProgram, sizeof(log) - 1, &n, log);
            log[n] = '\0';
            fprintf(stderr, "[GL] edge-field link failed: %s\n", log);
            glDeleteProgram(p->laEdgeProgram);
            p->laEdgeProgram = 0;
            return false;
        }
        p->laeUScene   = glGetUniformLocation(p->laEdgeProgram, "uScene");
        p->laeUDepth   = glGetUniformLocation(p->laEdgeProgram, "uDepth");
        p->laeUTexel   = glGetUniformLocation(p->laEdgeProgram, "uTexel");
        p->laeUBlur    = glGetUniformLocation(p->laEdgeProgram, "uBlur");
        p->laeUDepthOn = glGetUniformLocation(p->laEdgeProgram, "uDepthOn");

        // x, y (NDC), u, v - two triangles.
        static const float kQuad[6][4] = {
            { -1.0f, -1.0f, 0.0f, 0.0f },
            {  1.0f, -1.0f, 1.0f, 0.0f },
            { -1.0f,  1.0f, 0.0f, 1.0f },
            { -1.0f,  1.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 1.0f, 0.0f },
            {  1.0f,  1.0f, 1.0f, 1.0f },
        };
        glGenVertexArrays(1, &p->laVao);
        glBindVertexArray(p->laVao);
        glGenBuffers(1, &p->laVbo);
        glBindBuffer(GL_ARRAY_BUFFER, p->laVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
    }

    if (p->laFbo != 0 && p->laPageFbo != 0 && p->laEdgeFbo != 0 &&
        p->laW == p->width && p->laH == p->height) return true;
    if (p->laFbo != 0 || p->laPageFbo != 0 || p->laEdgeFbo != 0) {
        if (p->laFbo) glDeleteFramebuffers(1, &p->laFbo);
        if (p->laPageFbo) glDeleteFramebuffers(1, &p->laPageFbo);
        if (p->laEdgeFbo) glDeleteFramebuffers(1, &p->laEdgeFbo);
        if (p->laTex) glDeleteTextures(1, &p->laTex);
        if (p->laPageTex) glDeleteTextures(1, &p->laPageTex);
        if (p->laEdgeTex) glDeleteTextures(1, &p->laEdgeTex);
        if (p->laDepthTex) glDeleteTextures(1, &p->laDepthTex);
        p->laFbo = 0;
        p->laPageFbo = 0;
        p->laEdgeFbo = 0;
        p->laTex = 0;
        p->laPageTex = 0;
        p->laEdgeTex = 0;
        p->laDepthTex = 0;
    }

    glGenTextures(1, &p->laTex);
    glBindTexture(GL_TEXTURE_2D, p->laTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->width, p->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Depth as a TEXTURE, not a renderbuffer: the same image is attached to
    // laFbo and laPageFbo as before (scene geometry writes it, the stick
    // figures test against it), but the edge-field pass can now also SAMPLE
    // it for the silhouette outlines. Compare mode must be NONE so the
    // shader reads raw depth.
    glGenTextures(1, &p->laDepthTex);
    glBindTexture(GL_TEXTURE_2D, p->laDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, p->width, p->height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    glGenFramebuffers(1, &p->laFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, p->laFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           p->laTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           p->laDepthTex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    // Page target: the edge composite and the ink overlay share the scene's
    // depth image. Drawing into a second FBO with the same depth image is
    // legal as long as the two FBOs are never bound together, and it is
    // what lets the stick figures be clipped by the room geometry.
    if (ok) {
        glGenTextures(1, &p->laPageTex);
        glBindTexture(GL_TEXTURE_2D, p->laPageTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->width, p->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &p->laPageFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, p->laPageFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, p->laPageTex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, p->laDepthTex, 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    // Edge-field target (pass A output). Deliberately NO depth attachment:
    // the fullscreen quad needs no depth test, and keeping the scene depth
    // off this FBO is what lets the pass sample it as a texture without a
    // rendering feedback loop.
    if (ok) {
        glGenTextures(1, &p->laEdgeTex);
        glBindTexture(GL_TEXTURE_2D, p->laEdgeTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->width, p->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &p->laEdgeFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, p->laEdgeFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, p->laEdgeTex, 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        fprintf(stderr, "[GL] line-art FBO incomplete\n");
        if (p->laFbo) glDeleteFramebuffers(1, &p->laFbo);
        if (p->laPageFbo) glDeleteFramebuffers(1, &p->laPageFbo);
        if (p->laEdgeFbo) glDeleteFramebuffers(1, &p->laEdgeFbo);
        if (p->laTex) glDeleteTextures(1, &p->laTex);
        if (p->laPageTex) glDeleteTextures(1, &p->laPageTex);
        if (p->laEdgeTex) glDeleteTextures(1, &p->laEdgeTex);
        if (p->laDepthTex) glDeleteTextures(1, &p->laDepthTex);
        p->laFbo = 0;
        p->laPageFbo = 0;
        p->laEdgeFbo = 0;
        p->laTex = 0;
        p->laPageTex = 0;
        p->laEdgeTex = 0;
        p->laDepthTex = 0;
        return false;
    }
    p->laW = p->width;
    p->laH = p->height;
    return true;
}

// ---------------------------------------------------------------------------
// Internal draw
// ---------------------------------------------------------------------------

static void ApplyBlend(MarniBlend blend)
{
    switch (blend) {
    case MARNI_BLEND_ADD:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        break;
    case MARNI_BLEND_DISABLE:
        glDisable(GL_BLEND);
        break;
    case MARNI_BLEND_ALPHA:
    default:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

static void ApplySampler(MarniSampler sampler)
{
    GLint filter = (sampler == MARNI_SAMPLER_POINT) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

static void DrawBatch(MarniDX::Impl* p, const float* verts, int vertexCount,
                      MarniHandle tex, MarniSampler sampler, MarniBlend blend,
                      bool depthTest, bool depthWrite, bool persp,
                      bool noBlend = false, bool forceAlpha = false)
{
    if (p == nullptr || !p->ready || p->program == 0 || vertexCount <= 0) return;

    // Y-down ortho: (0,0) top-left -> NDC (-1,+1).
    float mvp[16] = {0};
    mvp[0]  = 2.0f / (float)p->width;
    mvp[5]  = -2.0f / (float)p->height;
    mvp[10] = 1.0f;
    mvp[12] = -1.0f;
    mvp[13] = 1.0f;
    mvp[15] = 1.0f;

    GLuint texture = p->whiteHandle != MARNI_NULL_HANDLE
                   ? p->slots[p->whiteHandle].tex : 0;
    if (tex != MARNI_NULL_HANDLE && tex < MARNI_MAX_TEXTURES && p->slots[tex].tex != 0) {
        texture = p->slots[tex].tex;
    }

    glUseProgram(p->program);
    glUniformMatrix4fv(p->uMVP, 1, GL_FALSE, mvp);
    glUniform1i(p->uTex, 0);
    glUniform1i(p->uPersp, persp ? 1 : 0);
    if (p->uForceAlpha >= 0) glUniform1i(p->uForceAlpha, forceAlpha ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    ApplySampler(sampler);

    ApplyBlend(blend);
    if (noBlend) glDisable(GL_BLEND);

    if (depthTest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }

    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertexCount * 10 * sizeof(float)),
                 verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    glBindVertexArray(0);
}

// Expand a caller vertex into the internal 10-float layout
// (x, y, z, w, u, v, r, g, b, a). `src` points at one vertex in the caller's
// layout; `hasZ`/`hasW` say which of z/w the caller already provides.
static inline void ExpandVertex(float* dst, const float* src, bool hasZ, bool hasW)
{
    if (hasZ && hasW) {
        // Already { x, y, z, w, u, v, r, g, b, a } - DrawTrianglesPersp.
        for (int i = 0; i < 10; ++i) dst[i] = src[i];
        return;
    }

    dst[0] = src[0];   // x
    dst[1] = src[1];   // y
    dst[2] = hasZ ? src[2] : 0.0f;   // z
    dst[3] = 1.0f;                   // w

    const float* rest = src + (hasZ ? 3 : 2);   // u, v, r, g, b, a
    for (int i = 0; i < 6; ++i) dst[4 + i] = rest[i];
}

// Scratch buffer for the expansion, reused across calls (single-threaded).
static float* ExpandBatch(const float* src, int vertexCount, bool hasZ, bool hasW)
{
    static float* s_buf = nullptr;
    static int s_cap = 0;
    int need = vertexCount * 10;
    if (need > s_cap) {
        float* grown = (float*)realloc(s_buf, (size_t)need * sizeof(float));
        if (grown == nullptr) return nullptr;
        s_buf = grown;
        s_cap = need;
    }
    int inStride = hasZ && hasW ? 10 : (hasZ ? 9 : 8);
    for (int v = 0; v < vertexCount; ++v) {
        ExpandVertex(s_buf + v * 10, src + v * inStride, hasZ, hasW);
    }
    return s_buf;
}

// ---------------------------------------------------------------------------
// MarniDX lifecycle
// ---------------------------------------------------------------------------

MarniDX::MarniDX() : m_pImpl(new Impl()) {}
MarniDX::~MarniDX() { Destroy(); delete m_pImpl; m_pImpl = nullptr; }

BOOL MarniDX::Create(HWND hWnd, int width, int height, BOOL fullScreen,
                     int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr) return FALSE;

    // The window and its GL context are created by the platform entry point
    // (main.cpp); this binds the backend to them.
    p->window = (SDL_Window*)hWnd;
    p->ctx = SDL_GL_GetCurrentContext();
    if (p->window == nullptr || p->ctx == nullptr) {
        fprintf(stderr, "[GL] Create: no window or GL context\n");
        return FALSE;
    }

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(p->window, &dw, &dh);
    p->fbW = (dw > 0) ? dw : width;
    p->fbH = (dh > 0) ? dh : height;
    UpdateViewportRect(p);

    if (!BuildPipeline(p)) return FALSE;

    // 1x1 opaque white, the fallback for solid-colour quads.
    const DWORD white = 0xFFFFFFFFu;
    p->whiteHandle = CreateTexture(1, 1, 32, &white, nullptr, nullptr);
    if (p->whiteHandle == MARNI_NULL_HANDLE) return FALSE;

    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    p->ready = true;
    if (outWidth)  *outWidth  = p->width;
    if (outHeight) *outHeight = p->height;
    return TRUE;
}

void MarniDX::Destroy()
{
    Impl* p = m_pImpl;
    if (p == nullptr) return;
    p->ReleaseAll();
    p->ready = false;
    p->window = nullptr;
    p->ctx = nullptr;
}

BOOL MarniDX::IsReady() const
{
    return (m_pImpl != nullptr && m_pImpl->ready) ? TRUE : FALSE;
}

void MarniDX::GetBackBufferSize(DWORD* outWidth, DWORD* outHeight) const
{
    Impl* p = m_pImpl;
    if (outWidth)  *outWidth  = (p != nullptr) ? (DWORD)p->width : 0;
    if (outHeight) *outHeight = (p != nullptr) ? (DWORD)p->height : 0;
}

int MarniDX::ChangeDisplayMode(DWORD newWidth, DWORD newHeight, BOOL fullScreen)
{
    Impl* p = m_pImpl;
    if (p == nullptr || p->window == nullptr) return 0;

    SDL_SetWindowSize(p->window, (int)newWidth, (int)newHeight);
    if (fullScreen) {
        SDL_SetWindowFullscreen(p->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(p->window, 0);
    }

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(p->window, &dw, &dh);
    if (dw > 0) p->fbW = dw;
    if (dh > 0) p->fbH = dh;
    UpdateViewportRect(p);
    return 1;
}

int MarniDX::HandleWindowMessage(HWND, UINT msg, WPARAM, LPARAM)
{
    // SDL owns the event loop on this platform; the only message that matters
    // to the backend is a resize.
    if (msg == WM_SIZE && m_pImpl != nullptr && m_pImpl->window != nullptr) {
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(m_pImpl->window, &dw, &dh);
        if (dw > 0) m_pImpl->fbW = dw;
        if (dh > 0) m_pImpl->fbH = dh;
        UpdateViewportRect(m_pImpl);
    }
    return 1;
}

unsigned int MarniDX::QueryVideoMemory(BOOL) const
{
    return 128u * 1024u * 1024u;
}

void MarniDX::EnumerateDisplayModes(DisplayModeInfo* outModes, int maxModes,
                                    int* outCount)
{
    int count = 0;
    int display = 0;
    int numModes = SDL_GetNumDisplayModes(display);

    for (int i = 0; i < numModes && count < maxModes; ++i) {
        SDL_DisplayMode mode;
        if (SDL_GetDisplayMode(display, i, &mode) != 0) continue;
        if (outModes != nullptr) {
            outModes[count].dwWidth       = (DWORD)mode.w;
            outModes[count].dwHeight      = (DWORD)mode.h;
            outModes[count].dwBPP         = 32;
            outModes[count].dwRefreshRate = (DWORD)mode.refresh_rate;
            outModes[count].dwFlags       = 0;
        }
        ++count;
    }
    if (outCount) *outCount = count;
}

void MarniDX::EnumerateAdapters(D3DRendererInfo* outRenderers, int max,
                                int* outCount)
{
    int count = 0;
    if (outRenderers != nullptr && max > 0) {
        const char* name = (const char*)glGetString(GL_RENDERER);
        strncpy(outRenderers[0].name, name ? name : "OpenGL", sizeof(outRenderers[0].name) - 1);
        outRenderers[0].name[sizeof(outRenderers[0].name) - 1] = '\0';
        outRenderers[0].flags = 0;
        count = 1;
    }
    if (outCount) *outCount = count;
}

void MarniDX::Clear(float r, float g, float b, float a)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready) return;

    // Line-art mode is sampled once per frame here (Clear runs before any
    // drawing, Present after all of it), so a mid-frame F7 toggle cannot
    // split a frame across the two paths.
    //
    // It is also switched off for the duration of an FMV. A movie is full of
    // fine detail and motion, so running the edge detector over it produces a
    // crawling mess that does not follow the picture. The FMV path calls Clear
    // itself right before it blits the frame, which is where this is caught.
    if (plat_video_is_active()) {
        p->laActive = false;
    } else {
        p->laActive = (g_bLineArt != 0) && EnsureLineArt(p);
    }

    if (p->laActive) {
        // Paint the letterbox bars black on the window; the content rect is
        // filled later by the page blit in Present.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, p->fbW, p->fbH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Scene goes into the offscreen target at the content size. This also
        // clears the depth renderbuffer the page FBO shares.
        glBindFramebuffer(GL_FRAMEBUFFER, p->laFbo);
        glViewport(0, 0, p->width, p->height);
        glClearColor(r, g, b, a);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // glClear ignores the viewport (only the scissor clips it), so paint the
    // whole drawable black first - that is what leaves the letterbox bars -
    // then lay the game's clear colour over the 4:3 content rectangle only.
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, p->fbW, p->fbH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(p->vpX, p->vpY, p->width, p->height);
    glViewport(p->vpX, p->vpY, p->width, p->height);
    glClearColor(r, g, b, a);
    // glClear is masked by the depth write mask. The 2D path leaves it GL_FALSE
    // (DrawBatch disables it for every non-depth draw), so without forcing it
    // back on the depth buffer was never cleared at all - each frame's 3D pass
    // tested against the PREVIOUS frame's depths. Static geometry survived
    // (same depths every frame), which is why it went unnoticed; the item
    // examine screen, whose model rotates, lost everything except the few
    // fragments that happened to be no farther than last frame's surface.
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

void MarniDX::Present()
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready || p->window == nullptr) return;

    if (p->laActive) {
        // Composite the edge-detected page into laPageFbo. The stick figures
        // are no longer drawn here: FlushTmdObjects wrote their ink into the
        // scene colour before the composite, so the shader above can copy it
        // straight through and room masks can cover it in painter order.
        glDisable(GL_SCISSOR_TEST);

        // Pass A: the edge field (blur + gradient magnitude/direction from
        // colour and depth) into laEdgeFbo. No depth test or attachment; the
        // scene depth is sampled as a texture.
        glBindFramebuffer(GL_FRAMEBUFFER, p->laEdgeFbo);
        glViewport(0, 0, p->width, p->height);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glUseProgram(p->laEdgeProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p->laTex);
        glUniform1i(p->laeUScene, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, p->laDepthTex);
        glUniform1i(p->laeUDepth, 1);
        glActiveTexture(GL_TEXTURE0);
        glUniform2f(p->laeUTexel, 1.0f / (float)p->laW, 1.0f / (float)p->laH);
        {
            int d = g_iLineArtDetail;
            if (d < kLineArtDetailMin) d = kLineArtDetailMin;
            if (d > kLineArtDetailMax) d = kLineArtDetailMax;
            glUniform1f(p->laeUBlur, kLineArtBlur[d - 1]);
        }
        glUniform1f(p->laeUDepthOn, p->laDepthTex != 0 ? 1.0f : 0.0f);
        glBindVertexArray(p->laVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);

        // Pass B: non-maximum suppression over the field, then the pen-and-ink
        // composite (contours + hatching on paper), into laPageFbo. The stick
        // figures are not drawn here: FlushTmdObjects wrote their ink into the
        // scene colour before the composite, so the shader copies it straight
        // through and room masks can cover it in painter order.
        glBindFramebuffer(GL_FRAMEBUFFER, p->laPageFbo);
        glViewport(0, 0, p->width, p->height);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glUseProgram(p->laProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p->laTex);
        glUniform1i(p->laUScene, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, p->laEdgeTex);
        glUniform1i(p->laUEdgeF, 1);
        glActiveTexture(GL_TEXTURE0);
        glUniform2f(p->laUTexel, 1.0f / (float)p->laW, 1.0f / (float)p->laH);
        {
            float ramp[2];
            int d = g_iLineArtDetail;
            if (d < kLineArtDetailMin) d = kLineArtDetailMin;
            if (d > kLineArtDetailMax) d = kLineArtDetailMax;
            LineArt_EdgeRamp(g_iLineArtDetail, ramp);
            glUniform2f(p->laUEdge, ramp[0], ramp[1]);
            glUniform1f(p->laUHatch, kLineArtHatch[d - 1]);
        }
        {
            // RE1_NOBGINK=1 hides the background edge ink for headless figure
            // captures. Read once; never set in normal play.
            static int s_noBgInk = -1;
            if (s_noBgInk < 0) {
                const char* v = getenv("RE1_NOBGINK");
                s_noBgInk = (v != NULL && v[0] == '1') ? 1 : 0;
            }
            glUniform1f(p->laUNoInk, s_noBgInk ? 1.0f : 0.0f);
        }
        {
            // RE1_LA_DEBUG=1|2 draws the post-NMS ridge / raw edge magnitude as
            // a grey field for threshold calibration. Test hook only; never set
            // in play.
            static int s_laDebug = -1;
            if (s_laDebug < 0) {
                const char* v = getenv("RE1_LA_DEBUG");
                s_laDebug = (v != NULL && v[0] >= '1' && v[0] <= '9') ? (v[0] - '0') : 0;
            }
            glUniform1f(p->laUDbg, (float)s_laDebug);
        }
        glBindVertexArray(p->laVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Ink overlay: the stick figures were written into the scene colour by
        // FlushTmdObjects (alpha-0 marker, respected by the shader above), so
        // this only consumes the collected poses.
        StickFigure_DrawOverlay(0);

        // Straight copy of the completed page into the window content rect.
        // The letterbox bars were painted black in Clear.
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, p->laPageFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, p->laW, p->laH,
                          p->vpX, p->vpY, p->vpX + p->width, p->vpY + p->height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    else {
        // Stick-figure player (port extension). Outside line-art mode this
        // only consumes the pending figure, so a toggle cannot leave one on
        // screen.
        StickFigure_DrawOverlay(0);
    }

    SDL_GL_SwapWindow(p->window);
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

MarniHandle MarniDX::CreateTexture(int width, int height, int bpp,
                                   const void* pixelData,
                                   int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr || width <= 0 || height <= 0 || pixelData == nullptr) {
        return MARNI_NULL_HANDLE;
    }

    // Only the layouts the game actually uploads are supported here: 32-bit
    // RGBA (r in the lowest byte) and 24-bit BGR. Everything else is converted
    // by the caller before reaching this point.
    const unsigned char* src = (const unsigned char*)pixelData;
    const unsigned char* upload = src;
    unsigned char* converted = nullptr;

    if (bpp == 24) {
        converted = (unsigned char*)malloc((size_t)width * height * 4);
        if (converted == nullptr) return MARNI_NULL_HANDLE;
        for (int i = 0; i < width * height; ++i) {
            converted[i * 4 + 0] = src[i * 3 + 2];  // R
            converted[i * 4 + 1] = src[i * 3 + 1];  // G
            converted[i * 4 + 2] = src[i * 3 + 0];  // B
            converted[i * 4 + 3] = 0xFF;
        }
        upload = converted;
    } else if (bpp != 32) {
        return MARNI_NULL_HANDLE;
    }

    MarniHandle handle = MARNI_NULL_HANDLE;
    for (MarniHandle h = 1; h < MARNI_MAX_TEXTURES; ++h) {
        if (p->slots[h].tex == 0) { handle = h; break; }
    }
    if (handle == MARNI_NULL_HANDLE) {
        free(converted);
        return MARNI_NULL_HANDLE;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, upload);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(converted);

    p->slots[handle].tex = tex;
    p->slots[handle].w = width;
    p->slots[handle].h = height;

    if (outWidth)  *outWidth  = width;
    if (outHeight) *outHeight = height;
    return handle;
}

MarniHandle MarniDX::CreateTextureFromBits(CMarniBits* bits)
{
    if (bits == nullptr || bits->m_pPixelData == nullptr) return MARNI_NULL_HANDLE;
    return CreateTexture((int)bits->m_width, (int)bits->m_height,
                         (int)bits->m_bitDepth, bits->m_pPixelData, nullptr, nullptr);
}

BOOL MarniDX::UpdateTexturePixels(MarniHandle tex, const void* pixelData,
                                  int width, int height, int bpp)
{
    Impl* p = m_pImpl;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return FALSE;
    if (p->slots[tex].tex == 0 || pixelData == nullptr) return FALSE;
    if (width != p->slots[tex].w || height != p->slots[tex].h) return FALSE;
    if (bpp != 32) return FALSE;

    glBindTexture(GL_TEXTURE_2D, p->slots[tex].tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixelData);
    glBindTexture(GL_TEXTURE_2D, 0);
    return TRUE;
}

void MarniDX::GetTextureSize(MarniHandle tex, int* outWidth, int* outHeight) const
{
    Impl* p = m_pImpl;
    if (outWidth)  *outWidth  = 0;
    if (outHeight) *outHeight = 0;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return;
    if (outWidth)  *outWidth  = p->slots[tex].w;
    if (outHeight) *outHeight = p->slots[tex].h;
}

void MarniDX::DestroyTexture(MarniHandle tex)
{
    if (m_pImpl != nullptr) m_pImpl->ReleaseSlot(tex);
}

MarniHandle MarniDX::WhiteTexture() const
{
    return (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void MarniDX::DrawSprite(float x, float y, float w, float h,
                         float u0, float v0, float u1, float v1,
                         DWORD color, MarniHandle tex,
                         MarniSampler sampler, MarniBlend blend)
{
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float x1 = x + w, y1 = y + h;
    const float verts[6 * 8] = {
        x,  y,  u0, v0, r, g, b, a,
        x1, y,  u1, v0, r, g, b, a,
        x,  y1, u0, v1, r, g, b, a,
        x,  y1, u0, v1, r, g, b, a,
        x1, y,  u1, v0, r, g, b, a,
        x1, y1, u1, v1, r, g, b, a,
    };
    const float* expanded = ExpandBatch(verts, 6, false, false);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, 6, tex, sampler, blend, false, false, false);
    }
}

// Room-mask sprite. Colour blends normally, but the alpha channel is forced to
// 1 so a wall reclaims a pixel the stick figure's ink marker had claimed (the
// line-art composite copies any pixel below alpha 0.5 straight through).
void MarniDX::DrawSpriteSolid(float x, float y, float w, float h,
                              float u0, float v0, float u1, float v1,
                              DWORD color, MarniHandle tex,
                              MarniSampler sampler)
{
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float x1 = x + w, y1 = y + h;
    const float verts[6 * 8] = {
        x,  y,  u0, v0, r, g, b, a,
        x1, y,  u1, v0, r, g, b, a,
        x,  y1, u0, v1, r, g, b, a,
        x,  y1, u0, v1, r, g, b, a,
        x1, y,  u1, v0, r, g, b, a,
        x1, y1, u1, v1, r, g, b, a,
    };
    const float* expanded = ExpandBatch(verts, 6, false, false);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, 6, tex, sampler, MARNI_BLEND_ALPHA,
                  false, false, false, false, true);
    }
}

void MarniDX::DrawRect(int x, int y, int w, int h, DWORD color){
    DrawSprite((float)x, (float)y, (float)w, (float)h,
               0.0f, 0.0f, 1.0f, 1.0f, color,
               (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE,
               MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA);
}

void MarniDX::DrawLine(float x0, float y0, float x1, float y1,
                       float thickness, DWORD color)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) return;

    // Perpendicular offset of half the thickness.
    float nx = -dy / len * (thickness * 0.5f);
    float ny =  dx / len * (thickness * 0.5f);

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float ax = x0 + nx, ay = y0 + ny;
    float bx = x1 + nx, by = y1 + ny;
    float cx = x1 - nx, cy = y1 - ny;
    float ex = x0 - nx, ey = y0 - ny;

    const float verts[6 * 8] = {
        ax, ay, 0.0f, 0.0f, r, g, b, a,
        bx, by, 1.0f, 0.0f, r, g, b, a,
        ex, ey, 0.0f, 1.0f, r, g, b, a,
        ex, ey, 0.0f, 1.0f, r, g, b, a,
        bx, by, 1.0f, 0.0f, r, g, b, a,
        cx, cy, 1.0f, 1.0f, r, g, b, a,
    };
    const float* expanded = ExpandBatch(verts, 6, false, false);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, 6,
                  (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE,
                  MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA,
                  false, false, false);
    }
}

void MarniDX::DrawLineDepth(float x0, float y0, float z0, float w0,
                            float x1, float y1, float z1, float w1,
                            float thickness, DWORD color)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) return;

    float nx = -dy / len * (thickness * 0.5f);
    float ny =  dx / len * (thickness * 0.5f);

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float ax = x0 + nx, ay = y0 + ny;
    float bx = x1 + nx, by = y1 + ny;
    float cx = x1 - nx, cy = y1 - ny;
    float ex = x0 - nx, ey = y0 - ny;

    // { x, y, z (NDC [0,1]), w (view-space Z), u, v, r, g, b, a }
    const float verts[6 * 10] = {
        ax, ay, z0, w0, 0.0f, 0.0f, r, g, b, a,
        bx, by, z1, w1, 1.0f, 0.0f, r, g, b, a,
        ex, ey, z0, w0, 0.0f, 1.0f, r, g, b, a,
        ex, ey, z0, w0, 0.0f, 1.0f, r, g, b, a,
        bx, by, z1, w1, 1.0f, 0.0f, r, g, b, a,
        cx, cy, z1, w1, 1.0f, 1.0f, r, g, b, a,
    };
    DrawTrianglesPersp(verts, 2,
                       (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE,
                       MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA, true);
}

void MarniDX::DrawRectDepth(float x, float y, float w, float h,
                            float z, float viewZ, DWORD color)
{
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float x1 = x + w, y1 = y + h;
    const float verts[6 * 10] = {
        x,  y,  z, viewZ, 0.0f, 0.0f, r, g, b, a,
        x1, y,  z, viewZ, 1.0f, 0.0f, r, g, b, a,
        x,  y1, z, viewZ, 0.0f, 1.0f, r, g, b, a,
        x,  y1, z, viewZ, 0.0f, 1.0f, r, g, b, a,
        x1, y,  z, viewZ, 1.0f, 0.0f, r, g, b, a,
        x1, y1, z, viewZ, 1.0f, 1.0f, r, g, b, a,
    };
    DrawTrianglesPersp(verts, 2,
                       (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE,
                       MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA, true);
}

void MarniDX::DrawInkLine(float x0, float y0, float z0, float w0,
                          float x1, float y1, float z1, float w1,
                          float thickness, DWORD color)
{
    DrawInkLineEx(x0, y0, z0, w0, x1, y1, z1, w1, thickness, color, false);
}

// Depth-tested variant of the ink line. The scene ink needs this: without it
// the line is a flat 2D splat that paints straight over walls, so a character
// standing behind a wall is fully visible through it. depthTest leaves the ink
// marker (alpha 0) and the no-blend composite intact, but the fragment is
// rejected where the depth buffer already holds something nearer.
void MarniDX::DrawInkLineEx(float x0, float y0, float z0, float w0,
                            float x1, float y1, float z1, float w1,
                            float thickness, DWORD color, bool depthTest)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) return;

    float nx = -dy / len * (thickness * 0.5f);
    float ny =  dx / len * (thickness * 0.5f);

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    // Alpha 0 is the line-art shader's ink marker, not opacity.
    const float a = 0.0f;

    float ax = x0 + nx, ay = y0 + ny;
    float bx = x1 + nx, by = y1 + ny;
    float cx = x1 - nx, cy = y1 - ny;
    float ex = x0 - nx, ey = y0 - ny;

    const float verts[6 * 10] = {
        ax, ay, z0, w0, 0.0f, 0.0f, r, g, b, a,
        bx, by, z1, w1, 1.0f, 0.0f, r, g, b, a,
        ex, ey, z0, w0, 0.0f, 1.0f, r, g, b, a,
        ex, ey, z0, w0, 0.0f, 1.0f, r, g, b, a,
        bx, by, z1, w1, 1.0f, 0.0f, r, g, b, a,
        cx, cy, z1, w1, 1.0f, 1.0f, r, g, b, a,
    };

    const float* expanded = ExpandBatch(verts, 6, true, true);
    if (expanded != nullptr) {
        // depthWrite stays off: the ink is composited into the page, and a
        // write would stamp the depth buffer with the figure and hide scene
        // geometry drawn after it.
        DrawBatch(m_pImpl, expanded, 6, MARNI_NULL_HANDLE,
                  MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA,
                  depthTest, false, true, true);
    }
}

void MarniDX::DrawInkRect(float x, float y, float w, float h,
                          float z, float viewZ, DWORD color)
{
    DrawInkRectEx(x, y, w, h, z, viewZ, color, false);
}

// Depth-tested ink rect: the joint dots, eyes and head dots of a scene stick
// figure go through here, so a wall nearer than the dot rejects it instead of
// the dot showing through the wall.
void MarniDX::DrawInkRectEx(float x, float y, float w, float h,
                            float z, float viewZ, DWORD color, bool depthTest)
{
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    const float a = 0.0f;

    float x1 = x + w, y1 = y + h;
    const float verts[6 * 10] = {
        x,  y,  z, viewZ, 0.0f, 0.0f, r, g, b, a,
        x1, y,  z, viewZ, 1.0f, 0.0f, r, g, b, a,
        x,  y1, z, viewZ, 0.0f, 1.0f, r, g, b, a,
        x,  y1, z, viewZ, 0.0f, 1.0f, r, g, b, a,
        x1, y,  z, viewZ, 1.0f, 0.0f, r, g, b, a,
        x1, y1, z, viewZ, 1.0f, 1.0f, r, g, b, a,
    };

    const float* expanded = ExpandBatch(verts, 6, true, true);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, 6, MARNI_NULL_HANDLE,
                  MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA,
                  depthTest, false, true, true);
    }
}

// Diagnostic only. Reads the depth buffer back at one screen pixel so a
// stick-figure leak can be traced to its cause. Called from inside
// FlushTmdObjects, which already has the scene framebuffer bound, so it reads
// from whatever is current. The ink coordinates are Y-down screen pixels; GL's
// readback origin is bottom-left, so Y is flipped here. Returns 1.0 when
// nothing wrote depth at that pixel.
float MarniDX::ProbeDepth(int x, int y)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready) return -1.0f;
    if (x < 0 || y < 0 || x >= p->width || y >= p->height) return -1.0f;

    float d = 1.0f;
    glReadPixels(x, p->height - 1 - y, 1, 1,
                 GL_DEPTH_COMPONENT, GL_FLOAT, &d);
    return d;
}

void MarniDX::DrawTriangles(const float* verts, int triCount, MarniHandle tex,
                            MarniSampler sampler, MarniBlend blend)
{
    if (verts == nullptr || triCount <= 0) return;
    const float* expanded = ExpandBatch(verts, triCount * 3, false, false);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, triCount * 3, tex, sampler, blend,
                  false, false, false);
    }
}

void MarniDX::DrawTrianglesPersp(const float* verts, int triCount, MarniHandle tex,
                                 MarniSampler sampler, MarniBlend blend,
                                 bool depthTest)
{
    // Input is already { x, y, z, w, u, v, r, g, b, a }.
    if (verts == nullptr || triCount <= 0) return;
    const float* expanded = ExpandBatch(verts, triCount * 3, true, true);
    if (expanded != nullptr) {
        // Test only, never write: translucent geometry must not hide the
        // depth-tested fade polys behind it.
        DrawBatch(m_pImpl, expanded, triCount * 3, tex, sampler, blend,
                  depthTest, false, true);
    }
}

void MarniDX::DrawTriangles3D(const float* verts, int triCount, MarniHandle tex,
                              MarniSampler sampler, MarniBlend blend,
                              bool depthWrite)
{
    // Input is { x, y, z (NDC [0,1]), w (view-space Z), u, v, r, g, b, a } -
    // same layout as DrawTrianglesPersp. The header's "9 floats" comment is
    // stale: TmdRenderer's TMD_VERT_FLOATS is 10 and the Windows backend
    // memcpy's sizeof(Model3DVertex) (10 floats) per vertex. Both 3D paths
    // run the perspective vertex shader; only the depth state differs.
    if (verts == nullptr || triCount <= 0) return;
    const float* expanded = ExpandBatch(verts, triCount * 3, true, true);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, triCount * 3, tex, sampler, blend,
                  true, depthWrite, true);
    }
}

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------

BOOL MarniDX::CaptureBackbufferToRGBA(void** outPixels, DWORD* outWidth, DWORD* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready || outPixels == nullptr) return FALSE;

    size_t size = (size_t)p->width * p->height * 4;
    unsigned char* buf = (unsigned char*)malloc(size);
    if (buf == nullptr) return FALSE;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(p->vpX, p->vpY, p->width, p->height, GL_RGBA, GL_UNSIGNED_BYTE, buf);

    // GL reads bottom-up; the callers expect top-down.
    unsigned char* row = (unsigned char*)malloc((size_t)p->width * 4);
    if (row != nullptr) {
        for (int y = 0; y < p->height / 2; ++y) {
            unsigned char* a = buf + (size_t)y * p->width * 4;
            unsigned char* b = buf + (size_t)(p->height - 1 - y) * p->width * 4;
            memcpy(row, a, (size_t)p->width * 4);
            memcpy(a, b, (size_t)p->width * 4);
            memcpy(b, row, (size_t)p->width * 4);
        }
        free(row);
    }

    *outPixels = buf;
    if (outWidth)  *outWidth  = (DWORD)p->width;
    if (outHeight) *outHeight = (DWORD)p->height;
    return TRUE;
}

BOOL MarniDX::ReadTextureRGBA(MarniHandle, void**, int*, int*)
{
    // Phase 8: needs a framebuffer-object round trip (glGetTexImage is not in
    // the GLES subset). Only the character-select card mask uses this.
    return FALSE;
}

// ---------------------------------------------------------------------------
// Font texture
// ---------------------------------------------------------------------------

void MarniDX::SetFontTexture(MarniHandle tex, int width, int height)
{
    if (m_pImpl == nullptr) return;
    m_pImpl->fontHandle = tex;
    m_pImpl->fontW = width;
    m_pImpl->fontH = height;
}

MarniHandle MarniDX::FontTexture() const
{
    return (m_pImpl != nullptr) ? m_pImpl->fontHandle : MARNI_NULL_HANDLE;
}

int MarniDX::FontTextureWidth() const
{
    return (m_pImpl != nullptr) ? m_pImpl->fontW : 0;
}

int MarniDX::FontTextureHeight() const
{
    return (m_pImpl != nullptr) ? m_pImpl->fontH : 0;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

MarniDX* MarniDX_Create()
{
    if (g_pDX != nullptr) return g_pDX;
    g_pDX = new MarniDX();
    return g_pDX;
}

void MarniDX_DestroyGlobal()
{
    if (g_pDX != nullptr) {
        g_pDX->Destroy();
        delete g_pDX;
        g_pDX = nullptr;
    }
}
