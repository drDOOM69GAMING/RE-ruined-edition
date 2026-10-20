// TmdRenderer.cpp - 3D TMD object render queue (DX11 replacement for the
// original DX5 ordering-table 3D path). See TmdRenderer.h for the data flow.
//
// Per-object data layout inside a CMarniDirect3DTMD slot (0x84-byte entries,
// base at slot+0x4D0 for m_objectData / slot+0xD10 for m_objectDataCopy):
//   +0x00  DWORD  primitive type (4 = TMD mesh object)
//   +0x08  float[16] model->view transform, written by
//          CMarniDirect3DTMD::Transform from the FUN_00483080 matrix. The GTE
//          rotation is stored so that row r of g_gteRotTransMatrix lands in
//          M[r], M[r+4], M[r+8] (matching the original store order at
//          0x004830ef), translation in M[12..14], and the FUN_00486190 view is
//          already folded in. A transformed position is therefore
//          vx = M[0]*x + M[4]*y + M[8]*z + M[12], i.e. the D3D row-vector
//          convention.
//   +0x54  DWORD  D3D object handle (unused in the DX11 port)
//   +0x58  DWORD  texture handle (MarniHandle via VTable_CreateTextureHandle)
//   +0x80  DWORD  render flags. Bit 2 = UNLIT: the original's renderer tests
//          it at 0x00446e99 (skip transforming the light directions) and
//          0x00447043 (skip accumulating the lights - write the vertex colour
//          straight into the primitive). Only the door animation sets it, in
//          DoorAsyncCreateTmd; the door is meant to be full-bright rather than
//          shaded by whatever room lighting was last in the globals.
//
// Geometry lives in the slot's embedded CMarniViewport2 elements
// (slot + objIndex*0x4C): m_pVertexBuffer holds 11-float vertices
// {x,y,z, nx,ny,nz, r,g,b, u,v}, m_pIndexBuffer holds WORD indices
// (3 per triangle, 4 per quad in PS1 order 0,1,3,2). Both position and normal
// have Y negated by PSXObject_Store (PS1 +Y is down, D3D +Y is up).
#include "TmdRenderer.h"
#include "SpriteRenderer.h"
#include "../Globals.h"
#include "../marni/MarniDX.h"
#include "../marni/MarniSystem.h"
#include "../marni/Marni3DObject.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

// Forward declarations for dependencies defined elsewhere
extern unsigned int AsyncCreateTmdObject(unsigned int param1, unsigned int param2, unsigned int param3);
extern void FUN_004896c0(void* joint, short p1, short p2, int p3); // 0x004896c0 (GteMatrix.cpp)
extern void FUN_0048a210(void* joint);                      // 0x0048a210 (PathTrail.cpp)
extern int  is_entity_in_switch_zone(VECTOR* pos, void* zoneData); // 0x00462d90 (Room.cpp)
extern void FUN_00483580(int* joint, MATRIX* out);          // 0x00483580 item_viewer_compose_matrix (MainMenu.cpp)
#include <algorithm>
#include <cmath>
#include "../DebugPrint.h"

// ============================================================================
// Tuning constants for the projection. The original DX5 renderer projected
// with a perspective viewport; the GTE-equivalent pinhole model uses
//   sx = cx + vx * f / vz
//   sy = cy - vy * f / vz
// where (vx,vy,vz) is the objData-matrix view position, (cx,cy) = subpixel
// offset (screen centre) and f = g_sceneRenderParam (the RDT camera fov /
// title render param).
//
// vz is POSITIVE in front of the camera: FUN_00483080 stores the GTE depth
// (g_gteRotTransMatrix.t[2], positive in front) in the matrix translation and
// FUN_00486190 only rotates it. The Y term is subtracted because
// SetRotAndTransMatrix negates the GTE Y translation while screen Y grows
// downwards.
// ============================================================================
// TMD_NEAR_Z is only the depth-buffer range's lower edge and a floor under the
// real near plane; the near CLIP itself is 2 * g_sceneRenderParam, computed per
// frame in FlushTmdObjects because the fov changes with the camera.
#define TMD_NEAR_Z          (1.0f)
#define TMD_FAR_Z      (131072.0f)    // depth-buffer range; every scene z fits
#define TMD_MAX_QUEUE       2048
#define TMD_MAX_TRIS_FLUSH  1024      // MarniDX::DrawTriangles3D per-call cap
#define TMD_MAX_TRIS_COLLECT 8192     // per-frame triangle pool for the depth sort

// View-space Z -> normalised [0,1] depth for the depth buffer. Linear: a D24
// buffer over TMD_FAR_Z still resolves better than a hundredth of a world unit,
// and a linear ramp keeps distant room geometry from collapsing into one value
// the way a 1/z ramp would.
static inline float TmdDepthNdc(float vz)
{
    float d = (vz - TMD_NEAR_Z) * (1.0f / (TMD_FAR_Z - TMD_NEAR_Z));
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return d;
}

// Shared with the fade-poly (ground shadow / blood pool) path in
// SpriteRenderer.cpp: its type-12 quads carry the same per-corner view-space
// Z and must land on the SAME [0,1] ramp the model triangles write, or the
// depth comparison between the two is meaningless.
float TmdViewZToNdc(float vz)
{
    return TmdDepthNdc(vz);
}

struct TmdDrawEntry {
    BYTE* slot;        // owning CMarniDirect3DTMD slot in g_tmdObjectBuffer,
                       // NULL for the complex-object pool (see FUN_00486df0)
    BYTE* objData;     // the queued 0x84-byte object entry (matrix at +0x08)
    BYTE* elem;        // geometry element (CMarniViewport2 / CDirect3DObject).
                       // Held explicitly because the complex-object pool keeps
                       // its elements in a SEPARATE 0x38-stride array rather
                       // than embedded in the owning slot at stride 0x4C.
    int   objIndex;    // embedded CMarniViewport2 index (slot-owned entries)
    int   depth;       // original OT depth (gte t[2] >> shift); kept for
                       // reference - ordering is per-triangle, see FlushTmdObjects
};

// Lighting state AS OF QUEUE TIME, one record per queue slot. Unlike the
// transform (which the caller writes after insertion, see TmdQueueObject) the
// lights are already set when an object is queued: render_entity calls
// update_entity_lighting for the entity, then SetLightMatrix per joint, and only
// then queues. On real hardware the driver latched the light state per
// DrawPrimitive, so each entity kept its own lighting; reading the globals at
// flush time instead shaded EVERY entity with whatever was set last - the
// player, since the render loop draws enemies first and the player after. A
// corpse lying still then appeared to be lit by a lamp the player was carrying.
//
// This lives on the heap rather than beside g_tmdQueue on purpose: 152KB of
// extra .bss shifts every static that follows it, and this port has globals
// whose addresses other code derives arithmetically. Keeping the fix out of
// .bss keeps the layout byte-identical to before it.
struct TmdLightState {
    float dir[3][3];   // g_d3dLightData[i*12 + 3..5]
    float col[3][3];   // g_d3dLightData[i*12 + 6..8]
    DWORD ambient;     // g_d3dAmbientColor
};

// Pinned so a change here cannot silently grow the .bss footprint unnoticed.
// 20 rather than 16 since the geometry element is now carried explicitly: the
// complex-object pool (FUN_00486df0) keeps its elements outside any TMD slot,
// so `slot + objIndex * 0x4C` can no longer stand in for it.
static_assert(sizeof(TmdDrawEntry) == 20, "TmdDrawEntry must stay 20 bytes");

static TmdDrawEntry  g_tmdQueue[TMD_MAX_QUEUE];
static int           g_tmdQueueCount = 0;
static TmdLightState* g_tmdLight = NULL;   // TMD_MAX_QUEUE records, heap, never freed

// Per-frame triangle pool. Triangles from every queued object are gathered here
// and submitted only after a global depth sort (see FlushTmdObjects).
// 3 vertices x {x, y, z(ndc), w, u, v, r, g, b, a}. `w` is the vertex's
// view-space Z: DrawTriangles3D's vertex shader divides by it so the UVs and
// colours interpolate perspective-correctly instead of affinely (see
// g_Model3DVS_Source). Without it, large near polygons - the door panel in the
// room transition above all - swim their texture as they turn.
#define TMD_VERT_FLOATS  10
#define TMD_TRI_FLOATS   (TMD_VERT_FLOATS * 3)
struct TmdTri {
    float v[TMD_TRI_FLOATS];
    float depth;   // mean view-space Z (larger = farther)
    DWORD tex;
    float alpha;   // opacity this triangle was emitted with (1 - the record's
                   // +0x68 background weight); < 1 marks translucent geometry
                   // (water, glass), which must NOT write depth or it hides
                   // the fade polys (ground shadows / blood pools) behind it -
                   // see room40E0 flooded, where the water plane silenced
                   // every shadow once they became depth-tested.
    int   otDepth; // the ordering-table depth its object was queued at. Only
                   // used to break exact `depth` ties - see the sort below.
};
static TmdTri g_tmdTris[TMD_MAX_TRIS_COLLECT];
static int    g_tmdTriOrder[TMD_MAX_TRIS_COLLECT];

// ============================================================================
// TmdQueue_Reset
// ============================================================================
void TmdQueue_Reset(void)
{
    g_tmdQueueCount = 0;
}

// ============================================================================
// TmdQueueObject (called from CMarniDirect3D vtable[10], original 0x00448300)
// Recovers the owning TMD slot + object index from the objData pointer and
// records a reference to it for this frame.
//
// Only the POINTER may be recorded here, never a copy of the object's render
// state: CMarniDirect3DTMD::Transform (0x00415520) inserts every object into
// the ordering table FIRST and writes the model->view matrix to objData + 0x08
// afterwards, exactly as the original does. Snapshotting the matrix at queue
// time therefore yields the previous frame's transform - or an all-zero matrix
// the first time a slot is used, which collapses every vertex onto the near
// plane and draws nothing at all.
// ============================================================================
void TmdQueueObject(void* objData, int depth)
{
    if (objData == NULL || g_tmdQueueCount >= TMD_MAX_QUEUE) return;

    if (g_tmdLight == NULL) {
        g_tmdLight = (TmdLightState*)calloc(TMD_MAX_QUEUE, sizeof(TmdLightState));
        if (g_tmdLight == NULL) return;
    }

    BYTE* p = (BYTE*)objData;
    const int slotStride = TMD_SLOT_STRIDE;

    // Resolve the pointer to (owning slot, offset within it). Every region that
    // can own a CMarniDirect3DTMD slot is listed here; they are disjoint, so
    // order does not matter. Each bound is the region's own size - notably the
    // main buffer's is its capacity, NOT TMD_CLEANUP_SLOT_COUNT, which is a
    // fact about what the cleanup destroys rather than about what is
    // addressable. Tying those two together is what broke the door animation.
    static const struct { BYTE* base; size_t size; } kSlotRegions[] = {
        { g_renderStateTMD,     sizeof(g_renderStateTMD)     },  // item examine / render state
        { g_doorTmdSlotBuffer,  sizeof(g_doorTmdSlotBuffer)  },  // door animation
        { g_itemTmdSlotBuffer,  sizeof(g_itemTmdSlotBuffer)  },  // item viewer
        { g_itemSharedTmdSlot,  sizeof(g_itemSharedTmdSlot)  },  // item viewer, shared transparent
        { g_tmdObjectBuffer,    sizeof(g_tmdObjectBuffer)    },  // entities, room objects
    };

    BYTE* slotBase = NULL;
    ptrdiff_t within = 0;
    for (size_t r = 0; r < sizeof(kSlotRegions) / sizeof(kSlotRegions[0]); r++) {
        BYTE* base = kSlotRegions[r].base;
        if (p < base || p >= base + kSlotRegions[r].size) continue;
        ptrdiff_t diff = p - base;
        ptrdiff_t off  = (diff / slotStride) * slotStride;
        slotBase = base + off;
        within   = diff - off;
        break;
    }
    if (slotBase == NULL) return;

    int objIndex;
    if (within >= 0x4D0 && within < 0x4D0 + 16 * 0x84) {
        objIndex = (int)(within - 0x4D0) / 0x84;
    }
    else if (within >= 0xD10 && within < 0xD10 + 16 * 0x84) {
        objIndex = (int)(within - 0xD10) / 0x84;
    }
    else {
        return;
    }

    int idx = g_tmdQueueCount++;
    TmdDrawEntry* e = &g_tmdQueue[idx];
    e->slot     = slotBase;
    e->objData  = p;
    e->elem     = slotBase + objIndex * 0x4C;
    e->objIndex = objIndex;
    e->depth    = depth;

    // Latch the light state for this object (see the note on TmdLightState).
    TmdLightState* ls = &g_tmdLight[idx];
    const float* lights = (const float*)g_d3dLightData;
    for (int i = 0; i < 3; i++) {
        const float* L = lights + i * 12;
        ls->dir[i][0] = L[3]; ls->dir[i][1] = L[4]; ls->dir[i][2] = L[5];
        ls->col[i][0] = L[6]; ls->col[i][1] = L[7]; ls->col[i][2] = L[8];
    }
    ls->ambient = g_d3dAmbientColor;
}

// ============================================================================
// Queue one entry from the complex-object pool (FUN_00486df0). Same contract as
// TmdQueueObject, except the geometry element is passed in: those elements live
// in g_objectListPtrArray at stride 0x38, not embedded in a TMD slot at 0x4C,
// so there is no slot to resolve the pointer against.
// ============================================================================
void TmdQueueComplexObject(void* objData, void* elem, int depth)
{
    if (objData == NULL || elem == NULL || g_tmdQueueCount >= TMD_MAX_QUEUE) return;

    if (g_tmdLight == NULL) {
        g_tmdLight = (TmdLightState*)calloc(TMD_MAX_QUEUE, sizeof(TmdLightState));
        if (g_tmdLight == NULL) return;
    }

    int idx = g_tmdQueueCount++;
    TmdDrawEntry* e = &g_tmdQueue[idx];
    e->slot     = NULL;
    e->objData  = (BYTE*)objData;
    e->elem     = (BYTE*)elem;
    e->objIndex = 0;
    e->depth    = depth;

    TmdLightState* ls = &g_tmdLight[idx];
    const float* lights = (const float*)g_d3dLightData;
    for (int i = 0; i < 3; i++) {
        const float* L = lights + i * 12;
        ls->dir[i][0] = L[3]; ls->dir[i][1] = L[4]; ls->dir[i][2] = L[5];
        ls->col[i][0] = L[6]; ls->col[i][1] = L[7]; ls->col[i][2] = L[8];
    }
    ls->ambient = g_d3dAmbientColor;
}

// ============================================================================
// Lighting. g_d3dLightData holds 3 lights x 12 DWORDs:
//   [3..5] = direction float3 (GTE camera space, written by SetLightMatrix)
//   [6..8] = color float3 0..1 (written by FUN_0040ac80)
// Normals are rotated by the objData matrix, which is the GTE joint rotation
// composed with the FUN_00486190 view rotation. That view is a small tilt away
// from identity (it only leans by the subpixel offset), so the light directions
// are used in GTE camera space as-is: both the normals and the light vectors go
// through the same camera rotation, and a rotation preserves the dot product.
// A previous revision negated X and Z here to cancel out an incorrect 180 degree
// view flip in FUN_00486190.
// ============================================================================
static void TmdComputeLight(const TmdLightState* ls, const float* n, const float* rot,
                            float* outR, float* outG, float* outB)
{
    // Normal into view space (rotation part of the objData matrix)
    float nx = rot[0] * n[0] + rot[4] * n[1] + rot[8]  * n[2];
    float ny = rot[1] * n[0] + rot[5] * n[1] + rot[9]  * n[2];
    float nz = rot[2] * n[0] + rot[6] * n[1] + rot[10] * n[2];

    // Ambient from the latched g_d3dAmbientColor (packed r<<16|g<<8|b, 0..255)
    // - NOT the live global, see TmdLightState.
    float r = (float)((ls->ambient >> 16) & 0xFF) / 255.0f;
    float g = (float)((ls->ambient >> 8)  & 0xFF) / 255.0f;
    float b = (float)( ls->ambient        & 0xFF) / 255.0f;

    for (int i = 0; i < 3; i++) {
        // L[3..5] = direction, L[6..8] = colour, from the latched copy.
        const float L[9] = {
            0.0f, 0.0f, 0.0f,
            ls->dir[i][0], ls->dir[i][1], ls->dir[i][2],
            ls->col[i][0], ls->col[i][1], ls->col[i][2],
        };
        // SetLightMatrix writes pLight[0] = 2 (D3DLIGHT_DIRECTIONAL) and stores
        // the normalised light POSITION in the direction field, so [3..5] is a
        // D3D dvDirection: the direction the light travels. D3D's diffuse term
        // is dot(N, -dvDirection), hence the negation. Dotting without it lit
        // only the faces pointing away from the camera - the options-menu lights
        // all sit just behind the model (z = -780 with the model at z = -700),
        // so the visible side came out at ambient level (~10%).
        float d = -(nx * L[3] + ny * L[4] + nz * L[5]);
        if (d > 0.0f) {
            r += d * L[6];
            g += d * L[7];
            b += d * L[8];
        }
    }

    *outR = (r > 1.0f) ? 1.0f : r;
    *outG = (g > 1.0f) ? 1.0f : g;
    *outB = (b > 1.0f) ? 1.0f : b;
}

// ============================================================================
// FlushTmdObjects - transform, light and draw every queued TMD object.
//
// The queue entry's `depth` (the original OT depth) is not used for ordering:
// triangles from all objects go into one pool and are sorted individually by
// view-space Z, which is what the original's depth buffer did within an object.
//
// The scene sprites - the room background masks and the entities' ground
// shadows - are merged into that same far-to-near walk. The original put them
// and the entity primitives in one ordering table, and their keys are directly
// comparable with a triangle's view-space Z, because this port stores every
// scene primitive at depthSort = OT index * 16 while an entity enters the table
// at t[2] >> 4 (FUN_00483250 passes depthShift 4). Drawing the whole TMD pass
// first and the sprites afterwards - which is what this did - put every mask in
// front of the player unconditionally, whichever side of the wall she was
// standing on, and every shadow in front of every mask.
//
// Scene sprites do not write depth, so the interleave is a pure painter's
// order: every triangle behind one is already down before it paints over them,
// and every triangle in front of it is submitted after. Triangle-vs-triangle
// ordering still comes from the depth buffer, untouched.
// ============================================================================
// 255 room sprites (the count is a byte) + shadows + the 2D billboard effects.
// Above MAX_SPRITE_COMMANDS (300) on purpose: the whole queue is the ceiling on
// how many scene depths can exist, so this bound can never actually be reached.
#define TMD_MAX_SCENE_DEPTHS  320

// Scene-order helpers defined with the stick-figure section at the bottom of
// this file. FlushTmdObjects (above that section) calls them while walking the
// painter order.
void  StickFigure_SortSceneFigures(void);
int   StickFigure_SceneCount(void);
float StickFigure_SceneDepth(int index);
void  StickFigure_DrawSceneInk(int index);
void  StickFigure_ResetScene(void);

// Convert a raw view-space Z into the room-mask ordering key.
//
// This is exactly what the stick-figure sort now does, and for the same reason.
// A mask is submitted at depthSort = posData * 16 where posData is the PS1
// ordering-table bucket (min(posData >> 2, 0x3ff)), and an entity enters that
// table at viewZ >> 4. So one bucket is 16 view-Z units and a mask's key equals
// (viewZ >> 4) * 16. TMD triangles carry raw view Z, 16x smaller, and comparing
// them straight against the mask keys made every wall sort as farther than the
// geometry and paint before it - so a mask never covered a model.
static inline float TmdSceneKeyFromViewZ(float viewZ)
{
    if (viewZ < 0.0f) return 0.0f;
    return (float)((int)(viewZ * 0.0625f)) * 16.0f;   // (viewZ >> 4) * 16
}

void FlushTmdObjects(void)
{
    int queued = g_tmdQueueCount;

    unsigned int maskDepths[TMD_MAX_SCENE_DEPTHS];
    const int    maskCount  = SpriteQueue_CollectSceneDepths(maskDepths, TMD_MAX_SCENE_DEPTHS);
    int          maskIdx    = 0;
    unsigned int maskCursor = 0xFFFFFFFFu;

    // Stick figures take part in the same painter walk as the TMD triangles
    // and room-mask sprites. Sort them far to near by their farthest joint;
    // the event loop below then draws each one exactly where a model at that
    // depth would have been, so a mask drawn later covers it and a triangle
    // drawn later (nearer) paints over it.
    StickFigure_SortSceneFigures();
    int figCount = StickFigure_SceneCount();
    int figIdx   = 0;

    if (queued > 0 && g_tmdLight != NULL && Marni_DX() != NULL) {
        float scaleX, scaleY;
        MarniGetRenderScale(&scaleX, &scaleY);
        float cx = (float)g_SubpixelOffsetX * scaleX;
        float cy = (float)g_SubpixelOffsetY * scaleY;
        // Project in GAME-SPACE (320x240) exactly like the original GTE
        // (GteMatrix.cpp: sx = x*param/z + 160, sy = ... + 120) and only then
        // apply the render scales - X and Y SEPARATELY. The old code folded
        // scaleX into f and applied it to both axes, which was fine while the
        // backbuffer shared the logical 4:3 (640x480 of 320x240) but inflates
        // the models by scaleX/scaleY on the 16:9 native-resolution
        // fullscreen backbuffer (models render ~1.33x too tall, i.e. "closer
        // to the camera"). The pre-rendered backgrounds stretch anisotropically,
        // so the models must too.
        float fg = (float)g_sceneRenderParam;

        // Near plane. The original marks a vertex clipped when its view-space Z
        // is under TWICE the projection distance (0x00447023-0x0044703c):
        //     MOV EAX, [EBP + 0x44]   ; the projection distance (g_primParam,
        //                             ; which FUN_0040a8f0 loads from
        //                             ; g_sceneRenderParam - the RDT camera fov)
        //     ADD EAX, EAX            ; 2 * f
        //     CMP EAX, EBX            ; EBX = (int)vz
        //     JLE  -> [vtx+0x18] = 0  ; keep
        //             [vtx+0x18] = 1  ; clip
        // and drops a primitive whose vertices' clip flags do not sum to zero
        // (the `... + ... + ... == 0` test guarding every AddPrim in
        // FUN_00446e40). This port used 1.0, i.e. "behind the eye" only, so
        // anything that came within a few hundred units of the camera still
        // projected - blown up until it filled the frame. That is the boulder
        // pinning itself against the wall the room-30F0 camera looks out of,
        // and the crank-driven bridge wall as it swings through the eye point.
        //
        // vz is in world units, so this must use the UNSCALED parameter: f
        // above carries the render-resolution scale purely to turn view space
        // into pixels.
        float nearZ = 2.0f * (float)g_sceneRenderParam;
        if (nearZ < TMD_NEAR_Z) nearZ = TMD_NEAR_Z;   // fov never read / zero

        // Is the original's OWN per-texel path aimed at something this frame?
        // FUN_00484c40 knocks the STP texels out of a COPY of the page and
        // FUN_00484dc0 draws the model through g_renderStateTMD unblended - a
        // whole mechanism with exactly ONE call site in the game, the
        // aquarium's tank shell (cmd_omodel_set, MANSION_2F / ROOM_STUDY_2F
        // slot 0).
        //
        // What the original gives that object is an UNBLENDED draw, so the
        // shell's authored look is full-bright (its USA counterpart, which has
        // no ABE prims at all, renders exactly that way in Mode=OG). Relighting
        // the shell through the generic twin therefore fights the original:
        // measured, it came out dark brown against the PS1's cream.
        //
        // The TWIN itself must stay, though, and this is the important half: in
        // this port that render-state pass is not covering the shell at all
        // (suppressing the twin left the tank fully see-through), so the twin's
        // opaque copy is the only thing making the DC shell solid. Without it,
        // the shell's own ABE blend at +0x68 = 0.5 is all there is.
        bool renderStateModelActive = false;
        for (int i = 0; i < queued; i++) {
            if (g_tmdQueue[i].slot == g_renderStateTMD) {
                renderStateModelActive = true;
                break;
            }
        }
        bool uniformBlend = (g_stageId % 5 == STAGE_MANSION_2F &&
                             g_roomId == ROOM_FRONT_LESSON_ROOM);

        // Triangles are collected across every queued object and submitted only
        // after a per-triangle depth sort. The original inserts each TMD object
        // into the ordering table at a single depth and lets the D3D depth
        // buffer resolve the triangles inside it; MarniDX::DrawTriangles carries
        // no Z, so without this the far side of each limb paints over its own
        // near side and the model renders as a dark shell.
        int collected = 0;

        for (int i = 0; i < queued; i++) {
            TmdDrawEntry* e = &g_tmdQueue[i];

            // TmdQueueObject only ever stores a slot inside g_tmdObjectBuffer,
            // so a null slot means this record was never filled - i.e. the
            // count outran the writes. Dereferencing it read address 4 and
            // faulted; skip it and say so instead.
            if (e->elem == NULL) {
                dbg_printf("FlushTmdObjects: unfilled queue entry %d of %d "
                           "(objData=%p objIndex=%d)\n",
                           i, queued, (void*)e->objData, e->objIndex);
                continue;
            }

            CMarniViewport2* elem = (CMarniViewport2*)e->elem;

            const float* vbuf = (const float*)elem->m_pVertexBuffer;
            const WORD*  ibuf = (const WORD*)elem->m_pIndexBuffer;
            int vtxCount = (int)elem->m_vertexCount;
            int listCount = (int)elem->m_listCount;
            int primType = (int)elem->m_primitiveType;
            if (vbuf == NULL || ibuf == NULL || vtxCount <= 0 || listCount <= 0)
                continue;
            if (primType != 3 && primType != 4)
                continue;

            // Read the transform and texture handle now (see TmdQueueObject):
            // both are written to the object entry after it was queued.
            const float* M = (const float*)(e->objData + 0x08);
            // A NULL slot marks a complex-pool entry. Those are only ever built
            // from textured triangle primitives (ComplexTmdObjectSetup filters
            // on flags == 0x34000609) and their 0x38-byte element has no +0x48
            // flag to read, so the texture is unconditional there.
            DWORD tex = (e->slot == NULL || *(DWORD*)(e->elem + 0x48) != 0)
                        ? *(DWORD*)(e->objData + 0x58) : 0;
            // Render flags at +0x80: bit 2 = unlit, take the vertex colour as
            // it stands (see the layout note at the top of this file).
            bool unlit = (*(DWORD*)(e->objData + 0x80) & 2) != 0;

            // Per-object colour scale. Marni3DObject's store loop writes 1.0f
            // to all three at load (Marni3DObject.cpp, `puVar4[0..2]`), and
            // scd_model_tint_apply -> TmdObjectTintSet (0x00485fa0) overwrites
            // them with `1.0 + delta` to tint a whole model at runtime.
            const float objScaleR = *(float*)(e->objData + 0x5C);
            const float objScaleG = *(float*)(e->objData + 0x60);
            const float objScaleB = *(float*)(e->objData + 0x64);

            // Blend weight at +0x68, INDEPENDENT of the unlit flag.
            // CMarniDirect3DTMD::Create zeroes the field (puVar4[3] = 0 at
            // 0x00415650), so an object nobody marked semi-transparent reads 0
            // here. Three writers set it:
            //   CreateTmdObjectInternal (0x00483910) - room/entity TMDs; it
            //     happens to set the unlit bit as well, which is what made
            //     gating on that bit look right,
            //   FUN_00483270 (0x004834d2), which re-stamps the anim object's
            //     +0x14 into +0x68/+0x78 of all 31 records every frame, and
            //   the item viewer (0x0048467f) - it stamps DAT_004d2c10 into
            //     +0x68/+0x78 of every record and sets NO flag at all.
            // Gating on the unlit bit therefore dropped the examine screen's
            // 50% pass entirely: the glass bottles rendered solid.
            //
            // The value is the weight of the BACKGROUND, not the opacity of
            // the model - the two are inverted. The proof is the per-texel
            // semi-transparency pass (FUN_00484dc0): its whole job is to draw
            // the opaque part of a model back over the translucent copy, and
            // `Create` leaves its +0x68 at 0. Read as opacity, 0 makes that
            // pass invisible; read as a background weight, 0 is exactly "let
            // none of the background through". The same reading is what makes
            // `blendMode == 0 -> +0x14 = 0` mean opaque in
            // CreateTmdObjectInternal, and it puts abr 0 - the common case,
            // table value 0x80 - on 0.5*src + 0.5*dst, the PS1 B/2 + F/2 rule
            // exactly.
            //
            // 0.5 maps to 0.5 either way, so this only moves objects that are
            // NOT half-transparent. In practice that is room 20A0's tank water:
            // cmd_omodel_set writes 0x004d2be0 = 0x30 for that one model
            // (0x00461df2, the only site in the game), which becomes 0.1875
            // background = 81% opaque. It had been rendering at 19% opacity - a
            // dark wash you could read the wallpaper through instead of a solid
            // green tank.
            float triAlpha = 1.0f;
            {
                float bgWeight = *(float*)(e->objData + 0x68);
                if (bgWeight < 0.0f)
                    triAlpha = 1.0f + bgWeight;
                else if (bgWeight > 0.0f && bgWeight <= 1.0f)
                    triAlpha = 1.0f - bgWeight;
                if (triAlpha < 0.0f) triAlpha = 0.0f;
                if (triAlpha > 1.0f) triAlpha = 1.0f;
            }

            // Per-texel semi-transparency. The PS1 blends a texel only when its
            // palette entry carries the STP bit, so a model whose primitives are
            // all ABE-flagged still has opaque parts - room 107's display case
            // is 80 ABE prims over a palette with just two STP entries, so its
            // glass top blends and its wooden frame must not. Blending the whole
            // record at `triAlpha` (what this did) washes the frame out.
            //
            // MarniStpKnockoutTwin hands back a copy of the same texture holding
            // only the opaque texels, so the geometry can be emitted a second
            // time at full alpha: that pass covers the blended one wherever the
            // texture is opaque, leaving the blend visible only where the PS1
            // would have blended it. Both passes carry the same vertices and so
            // a bit-identical depth; the LESS_EQUAL depth test lets the opaque
            // copy through because the blended one wrote no depth, and the sort
            // keeps each opaque copy immediately after the blended copy it
            // covers - the ordering matters where a translucent part of the
            // model sits in front of an opaque one, as the case's glass top
            // does. Room 107's display case is the model this exists for.
            MarniHandle knockTex = MARNI_NULL_HANDLE;
            if (triAlpha < 0.999f && !uniformBlend)
                knockTex = MarniStpKnockoutTwin((MarniHandle)tex);

            // The same ABE path also forces the model full-bright - bit 2 IS the
            // renderer's unlit path (0x00446e99 skips the light transform,
            // 0x00447043 skips the accumulation), and CreateTmdObjectInternal ORs
            // it into all 32 records of a transparent TMD at 0x00483c43. The PS1
            // shades those polygons normally.
            //
            // Where the knock-out pass exists the model is a lit object with a
            // glass insert - the case in rooms 107/108, whose TMD carries 41
            // gouraud normals that nothing was using - so follow the PS1 and
            // shade it. Doors and the uniformly-blended washes are unaffected:
            // a door's +0x68 is 0 and an all-STP palette gets no twin, so
            // neither reaches here, and bit 2 keeps its original meaning for
            // them - as it does for the tank shell, whose unblended
            // render-state draw means full-bright is the authored look (see the
            // note on renderStateModelActive above).
            if (knockTex != MARNI_NULL_HANDLE && !renderStateModelActive)
                unlit = false;

            // Transform + project + light every vertex
            // (heap-allocate per object; vertex counts are small)
            float* sx = (float*)malloc(sizeof(float) * vtxCount * 4);
            float* sy = sx + vtxCount;
            float* vzArr = sx + vtxCount * 2;   // view-space Z, for the depth sort
            float* cr = (float*)malloc(sizeof(float) * vtxCount * 3);
            float* cg = cr + vtxCount;
            float* cb = cg + vtxCount;
            char* clipped = (char*)malloc(vtxCount);
            if (!sx || !cr || !clipped) {
                free(sx); free(cr); free(clipped);
                continue;
            }

            // First normal (flat-shaded primitives only fill vertex 0)
            float flatN[3] = { vbuf[3], vbuf[4], vbuf[5] };

            for (int v = 0; v < vtxCount; v++) {
                const float* vtx = vbuf + v * 11;
                float x = vtx[0], y = vtx[1], z = vtx[2];
                float vx = M[0] * x + M[4] * y + M[8]  * z + M[12];
                float vy = M[1] * x + M[5] * y + M[9]  * z + M[13];
                float vz = M[2] * x + M[6] * y + M[10] * z + M[14];

                vzArr[v] = vz;
                if (vz < nearZ) {
                    clipped[v] = 1;
                    sx[v] = sy[v] = 0.0f;
                }
                else {
                    clipped[v] = 0;
                    float iz = fg / vz;
                    sx[v] = cx + vx * iz * scaleX;
                    sy[v] = cy - vy * iz * scaleY;
                }

                if (unlit) {
                    // The original's else-branch at 0x00447043: no ambient, no
                    // lights, the vertex colour becomes the primitive colour.
                    // PSXObject_Store writes 1.0 for textured primitives, so a
                    // door renders at full texture brightness every time.
                    //
                    // The death-wound tint (JointSetColorTint) writes the
                    // object entry's colour floats at +0x5C and sets the unlit
                    // bit at +0x80 at runtime; the vertex buffer is built once
                    // at load and never re-synced, so an unlit entry with a
                    // tinted colour reads it here (the wounds on the corpse).
                    // Untinted entries keep 0 there and fall back to the
                    // buffered colour.
                    float tr = *(float*)(e->objData + 0x5C);
                    float tg = *(float*)(e->objData + 0x60);
                    float tb = *(float*)(e->objData + 0x64);
                    if (tr != 0.0f || tg != 0.0f || tb != 0.0f) {
                        cr[v] = tr; cg[v] = tg; cb[v] = tb;
                    } else {
                        cr[v] = vtx[6]; cg[v] = vtx[7]; cb[v] = vtx[8];
                    }
                }
                else {
                    // Lighting: vertices without a normal (flat prims) reuse
                    // the first vertex's normal.
                    const float* n = vtx + 3;
                    if (n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f) n = flatN;
                    TmdComputeLight(&g_tmdLight[i], n, M, &cr[v], &cg[v], &cb[v]);
                    cr[v] *= vtx[6]; cg[v] *= vtx[7]; cb[v] *= vtx[8];
                    // The original multiplies the lit colour by the object's
                    // colour scale before the 0..255 clamp: 0x00447169
                    // (`FLD [EDI+0x5c]` then `FIMUL`), 0x0044719d for +0x60 and
                    // 0x004471b7 for +0x64. Only TWO of this function's SIX
                    // reads of +0x5C sit behind the `TEST [EDI+0x80],2` unlit
                    // gate at 0x00446e99 / 0x00447043 - this one does not.
                    //
                    // Without it a LIT model can never be tinted, because the
                    // only other reader is the unlit branch below. That is why
                    // the monster plant's poison pulse accumulated a correct
                    // (2,-3,0) into all 15 joint objects of all six plants and
                    // nothing changed on screen. Untinted objects hold 1.0f
                    // here, so this is a no-op for them.
                    cr[v] *= objScaleR; cg[v] *= objScaleG; cb[v] *= objScaleB;
                }
            }

            // Emit triangles
            for (int pIdx = 0; pIdx < listCount; pIdx++) {
                WORD idx[4];
                int vertsInPrim = (primType == 3) ? 3 : 4;
                const WORD* ip = ibuf + pIdx * vertsInPrim;
                idx[0] = ip[0]; idx[1] = ip[1]; idx[2] = ip[2];
                if (vertsInPrim == 4) idx[3] = ip[3];

                bool oob = false;
                for (int v = 0; v < vertsInPrim; v++)
                    if (idx[v] >= vtxCount) { oob = true; break; }
                if (oob) continue;

                // Both the near-clip and the backface test are decided ONCE per
                // primitive and gate all of its triangles, exactly as the
                // original does - a quad never loses only one of its halves.
                //
                // Near clip: the original's guard is
                // `clip[a] + clip[b] + clip[c] (+ clip[d]) == 0`, i.e. one
                // clipped vertex drops the whole packet.
                int clipSum = 0;
                for (int v = 0; v < vertsInPrim; v++) clipSum += clipped[idx[v]];
                if (clipSum != 0) continue;

                // Backface cull. The original tests
                //     0 < (ya - yb) * (xc - xb) + (yc - yb) * (xb - xa)
                // over the packet's FIRST THREE screen vertices (the condition
                // in front of every AddPrim in FUN_00446e40), which expands
                // term for term to the signed area below - so there is no
                // winding convention left to choose. This port previously drew
                // both sides and leaned on the depth buffer; that is what let
                // an object the camera sits inside paint its far shell over the
                // whole frame, since every surviving triangle there is a back
                // face. The index buffer feeding this is byte-identical to the
                // original's (PSXObject_Store writes quads as base+0,+1,+3,+2),
                // so the vertex order the test sees is the original's too.
                float x0 = sx[idx[0]], y0 = sy[idx[0]];
                float x1 = sx[idx[1]], y1 = sy[idx[1]];
                float x2 = sx[idx[2]], y2 = sy[idx[2]];
                if ((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) <= 0.0f)
                    continue;

                // Triangulate: (0,1,2) + (0,2,3) for quads
                for (int t = 0; t < vertsInPrim - 2; t++) {
                    WORD i0 = idx[0];
                    WORD i1 = idx[t + 1];
                    WORD i2 = idx[t + 2];

                    x0 = sx[i0]; y0 = sy[i0];
                    x1 = sx[i1]; y1 = sy[i1];
                    x2 = sx[i2]; y2 = sy[i2];

                    if (collected >= TMD_MAX_TRIS_COLLECT) continue;

                    TmdTri* t3 = &g_tmdTris[collected];
                    float* o = t3->v;
                    const float* v0 = vbuf + i0 * 11;
                    const float* v1 = vbuf + i1 * 11;
                    const float* v2 = vbuf + i2 * 11;
                    o[0]  = x0; o[1]  = y0; o[2]  = TmdDepthNdc(vzArr[i0]);
                    o[3]  = vzArr[i0];
                    o[4]  = v0[9];  o[5]  = v0[10];
                    o[6]  = cr[i0]; o[7]  = cg[i0]; o[8]  = cb[i0]; o[9]  = triAlpha;
                    o[10] = x1; o[11] = y1; o[12] = TmdDepthNdc(vzArr[i1]);
                    o[13] = vzArr[i1];
                    o[14] = v1[9];  o[15] = v1[10];
                    o[16] = cr[i1]; o[17] = cg[i1]; o[18] = cb[i1]; o[19] = triAlpha;
                    o[20] = x2; o[21] = y2; o[22] = TmdDepthNdc(vzArr[i2]);
                    o[23] = vzArr[i2];
                    o[24] = v2[9];  o[25] = v2[10];
                    o[26] = cr[i2]; o[27] = cg[i2]; o[28] = cb[i2]; o[29] = triAlpha;
                    t3->depth = (vzArr[i0] + vzArr[i1] + vzArr[i2]) * (1.0f / 3.0f);
                    t3->tex   = tex;
                    t3->alpha = triAlpha;
                    t3->otDepth = e->depth;
                    g_tmdTriOrder[collected] = collected;
                    collected++;

                    // Second, opaque pass over the same geometry with the STP
                    // texels knocked out (see knockTex above). It carries the
                    // same depth and the same otDepth as its blended copy, so
                    // the sort's `a < b` tail places it immediately after it -
                    // nothing else can fall between two adjacent collection
                    // indices.
                    //
                    // `alpha` is the DEPTH-WRITE class, not the opacity: the VS
                    // takes the opacity from each vertex's own alpha, and the
                    // two are deliberately split here. This copy must inherit
                    // its blended partner's class (`triAlpha`, always < 1 on
                    // this path) and write NO depth. It is a colour-only overlay
                    // of geometry that is already in the buffer at the same Z -
                    // giving it a depth footprint would let a model that never
                    // wrote depth start occluding whatever the painter walk
                    // reaches later, and the interior of a case this size is
                    // exactly where a mis-ordered triangle would then punch a
                    // hole instead of just painting in the wrong order.
                    if (knockTex != MARNI_NULL_HANDLE &&
                        collected < TMD_MAX_TRIS_COLLECT) {
                        TmdTri* k3 = &g_tmdTris[collected];
                        memcpy(k3->v, t3->v, sizeof(k3->v));
                        for (int v = 0; v < 3; v++)
                            k3->v[v * TMD_VERT_FLOATS + 9] = 1.0f;
                        k3->depth   = t3->depth;
                        k3->tex     = (DWORD)knockTex;
                        k3->alpha   = triAlpha;
                        k3->otDepth = e->depth;
                        g_tmdTriOrder[collected] = collected;
                        collected++;
                    }
                }
            }

            free(sx); free(cr); free(clipped);
        }

        // The depth buffer resolves which face wins, so the sort is no longer
        // load-bearing for opaque geometry - it stays because it keeps the
        // alpha-blended triangles blending back-to-front and it groups runs of
        // one texture together, which halves the draw calls.
        // Ties on `depth` are not a curiosity - they are how the original draws
        // per-texel semi-transparency. It cannot blend individual texels, so it
        // queues the SAME geometry twice at two ordering-table depths: the whole
        // model at its blend weight, and a second, opaque copy whose texture has
        // every STP-flagged palette entry punched out to index 0. Room 20A0's
        // water tank is the room-object case (FUN_00484d90 / FUN_00484e40 /
        // FUN_00485000, the only site in the game); the item examine screen is
        // the other. Both passes project through the same matrix, so every
        // triangle's mean view Z is bit-identical and the primary key cannot
        // separate them - which left introsort's partitioning to decide which of
        // the two blended first.
        //
        // The ordering table is what decides it in the original: it is walked
        // from the highest index down to 0, so the LARGER depth draws first. The
        // tank's translucent pass sits at OT 469 and its opaque pass at OT 117 -
        // wash first, opaque over the top, punched texels keeping the wash. The
        // collection index is the last resort, so the whole order is
        // deterministic frame to frame.
        std::sort(g_tmdTriOrder, g_tmdTriOrder + collected, [](int a, int b) {
            const TmdTri& ta = g_tmdTris[a];
            const TmdTri& tb = g_tmdTris[b];
            if (ta.depth != tb.depth)     return ta.depth > tb.depth;
            if (ta.otDepth != tb.otDepth) return ta.otDepth > tb.otDepth;
            return a < b;
        });

        static float triVerts[TMD_MAX_TRIS_FLUSH * TMD_TRI_FLOATS];
        int   triCount = 0;
        DWORD triTex   = 0;
        bool  triWrite = true;   // depth-WRITE mode of the batch in flight

        auto flushTriBatch = [&]() {
            if (triCount > 0) {
                Marni_DX()->DrawTriangles3D(triVerts, triCount, (MarniHandle)triTex,
                                            MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA,
                                            triWrite);
                triCount = 0;
            }
        };

        // Draw every mask and stick figure farther than `depth`, far to near.
        // The triangle batch is flushed first: its triangles are all farther
        // than the figure being inserted (the caller only reaches this point
        // when the next triangle is nearer than the event), so they belong
        // behind it. A tie draws the figure first, then the mask over it.
        auto drawEventsAbove = [&](float depth) {
            for (;;) {
                bool haveMask = maskIdx < maskCount &&
                                (float)maskDepths[maskIdx] > depth;
                bool haveFig  = figIdx < figCount &&
                                StickFigure_SceneDepth(figIdx) > depth;
                if (!haveMask && !haveFig) break;

                float md = haveMask ? (float)maskDepths[maskIdx] : -3.0e38f;
                float fd = haveFig  ? StickFigure_SceneDepth(figIdx) : -3.0e38f;

                if (haveMask && (!haveFig || md > fd)) {
                    flushTriBatch();
                    FlushSpriteCommandsRange(maskDepths[maskIdx], maskCursor,
                                             SPRITE_CLASS_SCENE);
                    maskCursor = maskDepths[maskIdx];
                    ++maskIdx;
                } else {
                    flushTriBatch();
                    StickFigure_DrawSceneInk(figIdx);
                    ++figIdx;
                }
            }
        };

        for (int k = 0; k < collected; k++) {
            const TmdTri* t3 = &g_tmdTris[g_tmdTriOrder[k]];
            const bool triWriteThis = t3->alpha >= 0.999f;

            // Every scene sprite and stick figure farther than this triangle
            // has to be on screen before it, far to near. The triangle's raw
            // view Z is converted into the mask key scale first (see
            // TmdSceneKeyFromViewZ): the two are otherwise 16x apart.
            drawEventsAbove(TmdSceneKeyFromViewZ(t3->depth));


            if ((triCount > 0 && (t3->tex != triTex || triWriteThis != triWrite)) ||
                triCount >= TMD_MAX_TRIS_FLUSH) {
                Marni_DX()->DrawTriangles3D(triVerts, triCount, (MarniHandle)triTex,
                                            MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA,
                                            triWrite);
                triCount = 0;
            }
            triTex = t3->tex;
            triWrite = triWriteThis;
            memcpy(triVerts + triCount * TMD_TRI_FLOATS, t3->v, sizeof(t3->v));
            triCount++;
        }
        if (triCount > 0) {
            Marni_DX()->DrawTriangles3D(triVerts, triCount, (MarniHandle)triTex,
                                        MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA,
                                        triWrite);
        }

        // Everything nearer than the last triangle (or every event, when the
        // queue collected no triangles at all).
        drawEventsAbove(-3.0e38f);
    }

    // The scene sprites and figures nearer than the last triangle - and, when
    // no entity was queued at all, every one of them. Nothing else draws
    // SPRITE_CLASS_SCENE, so this drain is what guarantees they are not
    // silently dropped.
    while (maskIdx < maskCount || figIdx < figCount) {
        bool haveMask = maskIdx < maskCount;
        bool haveFig  = figIdx < figCount;
        float md = haveMask ? (float)maskDepths[maskIdx] : -3.0e38f;
        float fd = haveFig  ? StickFigure_SceneDepth(figIdx) : -3.0e38f;

        if (haveMask && (!haveFig || md > fd)) {
            FlushSpriteCommandsRange(maskDepths[maskIdx], maskCursor,
                                     SPRITE_CLASS_SCENE);
            maskCursor = maskDepths[maskIdx];
            ++maskIdx;
        } else {
            StickFigure_DrawSceneInk(figIdx);
            ++figIdx;
        }
    }

    // The figures are already in the scene colour; a stale pose must not be
    // reused if this pass does not run next frame.
    StickFigure_ResetScene();
    g_tmdQueueCount = 0;
}
// (0x00481660) - Update entity lighting from RDT point lights
// Recomputes the 3 D3D lights based on the entity's distance to each RDT
// light. Lights with lightType == 0 are point lights with radial falloff
// (direction = light->entity, color attenuated by distance); the others are
// used as-is (directional).
void update_entity_lighting(VECTOR* entityPos)
{
    if (g_RdtPointer == NULL) return;

    for (int i = 0; i < 3; i++) {
        RDT_Light* light = &g_RdtPointer->lights[i];
        if (light->lightType == 0) {
            struct { int x, y, z; unsigned char r, g, b; } pointLight;
            pointLight.x = entityPos->x - light->pos_x;
            pointLight.y = entityPos->y - light->pos_y;
            pointLight.z = entityPos->z - light->pos_z;

            int atten = (int)(unsigned short)light->radius -
                        SquareRoot0(pointLight.z * pointLight.z +
                                    pointLight.x * pointLight.x);
            if (atten < 0) atten = 0;

            if ((unsigned short)light->radius == 0) {
                pointLight.r = pointLight.g = pointLight.b = 0;
            }
            else {
                pointLight.r = (unsigned char)((light->red   * atten) / (int)(unsigned short)light->radius);
                pointLight.g = (unsigned char)((light->green * atten) / (int)(unsigned short)light->radius);
                pointLight.b = (unsigned char)((light->blue  * atten) / (int)(unsigned short)light->radius);
            }
            FUN_0040ac80(i, &pointLight);
        }
        else {
            FUN_0040ac80(i, light);
        }
    }
}

// ============================================================================
// ============================================================================
// Stick-figure cast (port extension, no original address)
//
// config.ini [Display] StickFigure / F1 draws every character and monster as
// a joint skeleton instead of its model. It is a line-art-mode effect: the
// figure is literal ink on the composited page, so with LineArt=0 the frame is
// untouched.
//
// render_entity measures the pose, because that is where the joint world
// matrices are current - rotate_entity has just composed joint->world for the
// whole hierarchy. Walking the animation header's own child lists (child count
// at animBase + idx*4, child-list offset at +2) gives every joint's real
// parent, so an ordinary render yields a correctly-connected skeleton that
// follows the character's real animation - there is no hand-authored figure to
// keep in sync. While the figure is on, the character's TMD objects are not
// queued at all, so the model really is replaced rather than hidden behind.
//
// Joint origins are projected from the very matrix render_entity renders the
// joint with, so the figure lands on the model's own pixels. Note the Y flip:
// SetRotAndTransMatrix negates t[1] before the vertex path reads it, and this
// mirrors that.
//
// The ink is written into the line-art page FBO while the scene's depth buffer
// is still attached (MarniDX::Present), so room geometry clips the figure
// exactly where it used to clip the model. The ink tests depth but never
// writes it, so one bone never hides another.
// ============================================================================

#define STICK_MAX_JOINTS   32
#define STICK_MAX_SEGMENTS 48
#define STICK_MAX_FIGURES  16

struct StickSegment {
    float x0, y0, x1, y1;
    float vz0, vz1;                 // camera-space Z at each end
    // Which limb this bone is, from the two joints it joins (-1 = not
    // classified). A costume needs to colour the torso differently from the
    // trousers and leave the boots and gloves dark, so the draw pass has to
    // know which bone it is holding. See StickFigure_SegName.
    signed char part;
};

// Pose scratch, one entry per joint index, filled inside render_entity's joint
// loop.
static bool  g_stickOk[STICK_MAX_JOINTS];
static float g_stickPx[STICK_MAX_JOINTS];
static float g_stickPy[STICK_MAX_JOINTS];
static float g_stickPz[STICK_MAX_JOINTS];

// What the skeleton belongs to. The joint tree is not the same for every
// character, so the style selects both the head joint and the ink details.
enum StickStyle {
    STYLE_NONE = -1,
    STYLE_HUMAN = 0,
    STYLE_JILL,
    STYLE_REBECCA,
    STYLE_CHRIS,
    STYLE_BARRY,
    STYLE_WESKER,
    STYLE_RICHARD,
    STYLE_ENRICO,
    STYLE_CORPSE,
    // Brad and Joseph only turn up in the prologue and the odd cutscene, and
    // they were sharing STYLE_HUMAN - one cold blue eye colour, no hair, no
    // vest. They have the same 15-joint skeleton as the rest of the cast, so
    // they only need a style of their own.
    STYLE_BRAD,
    STYLE_JOSEPH,
    STYLE_ZOMBIE,
    STYLE_DOG,
    STYLE_HUNTER,
    STYLE_TYRANT,
    STYLE_CHIMERA,
    STYLE_CROW,
    STYLE_WASP,
    STYLE_SPIDER,
    STYLE_SNAKE,
    STYLE_SHARK,
    STYLE_VINE,
    STYLE_COUNT
};

struct StickFigureData {
    StickSegment segments[STICK_MAX_SEGMENTS];
    int          segmentCount;
    int          jointCount;
    float        jx[STICK_MAX_JOINTS];
    float        jy[STICK_MAX_JOINTS];
    float        jz[STICK_MAX_JOINTS];
    bool         jok[STICK_MAX_JOINTS];
    // Joint flags, copied from JointStruct.flags when the pose was recorded.
    // Bit 4 (0x04) is render_entity's "blown off" marker: the joint has been
    // launched by the severed-limb ballistic step (FUN_004896c0) and its world
    // matrix is no longer being composed from its parent. Drawing a bone from
    // the parent to such a joint is what stretched a zombie's arm to the floor
    // and made it longer when it was shot. A flagged joint is drawn as its own
    // detached stub instead, and nothing links it to the body.
    unsigned char jflags[STICK_MAX_JOINTS];
    // Parent joint of each index, from the animation's child lists. Kept so the
    // draw pass can find limb tips (a joint with no child is a hand, a foot, a
    // paw, a wingtip) and so the neck can be drawn between the head and the
    // joint that carries it.
    signed char  jparent[STICK_MAX_JOINTS];
    bool         headless;              // no head in this pose (see StickFigure_Finish)
    float        headX, headY, headR, headZ;
    float        footX, footY, footZ;   // farthest joint from the head
    float        rootX, rootY, rootZ;   // joint 0
    float        downX, downY;          // unit head -> root (down the body)
    float        torsoLen;              // head -> root distance
    float        bodyLen;               // head -> farthest joint
    float        maxZ;                  // sorting key: the farthest camera-space
                                        // Z of the pose, converted to the same
                                        // OT-key scale the room masks use. See
                                        // StickFigure_SceneDepth.
    int          style;
    bool         isPlayer;
};

static StickFigureData g_stickFigures[STICK_MAX_FIGURES];
static int   g_stickFigureCount = 0;

// True while characters are drawn as stick figures. Line art is the gate: the
// figure is ink on the page, so there has to be a page. g_bLineArtRealChars
// (seven F7 presses, main.cpp) wins over both flags: the models then render
// as ordinary scene geometry and the line-art pass inks them like the level.
static bool StickFigure_Active(void)
{
    return g_bLineArt && g_bStickFigure && !g_bLineArtRealChars;
}

// Project one joint origin exactly the way FlushTmdObjects projects a vertex.
// `cam` is the camera-space matrix render_entity just built for the joint.
static void StickFigure_ProjectJoint(int idx, const MATRIX* cam)
{
    if (idx < 0 || idx >= STICK_MAX_JOINTS) return;
    g_stickOk[idx] = false;

    float scaleX, scaleY;
    MarniGetRenderScale(&scaleX, &scaleY);
    float cx = (float)g_SubpixelOffsetX * scaleX;
    float cy = (float)g_SubpixelOffsetY * scaleY;
    float fg = (float)g_sceneRenderParam;
    float nearZ = 2.0f * (float)g_sceneRenderParam;
    if (nearZ < TMD_NEAR_Z) nearZ = TMD_NEAR_Z;

    float vz = (float)cam->t[2];        // view-space Z, world units
    if (vz < nearZ) return;             // behind / inside the near plane

    float iz = fg / vz;
    float vx =  (float)cam->t[0];
    float vy = -(float)cam->t[1];       // SetRotAndTransMatrix's Y flip
    g_stickPx[idx] = cx + vx * iz * scaleX;
    g_stickPy[idx] = cy - vy * iz * scaleY;
    g_stickPz[idx] = vz;
    g_stickOk[idx] = true;
}

// Entity::id is the update-handler table id. Ids 0-21 are the monsters; 32-47
// are the human cutscene actors/corpses (model index = id + 4, see RoomInit).
// The player is matched by pointer because its id (0 or 1) collides with the
// zombie ids.
static int StickFigure_StyleOf(const Entity* ent)
{
    if (ent == (const Entity*)&g_playerEntity) {
        // Which S.T.A.R.S. member the player is. The live authority is the
        // player entity's OWN id, exactly as the rest of the engine reads it
        // (EndingScreen, ComputerLab, WeaponDamage, EntityModelLoader all test
        // `g_playerEntity.id & 1`). g_SelectedCharactedId is only the value the
        // character-select screen wrote and restored from the BioCard, and it
        // can disagree with the entity after a load or a room change - reading
        // it here is what dressed Jill as Chris. Bit 3 is the alternative
        // outfit (added to the id), so mask it off with & 1 rather than & 3.
        int id = (int)g_playerEntity.id;
        // If the live id has not been established yet (0 with a Jill card),
        // fall back to the card the select screen filled in.
        if (id == 0 && (g_SelectedCharactedId & 1) != 0)
            id = (int)g_SelectedCharactedId;
        return (id & 1) ? STYLE_JILL : STYLE_CHRIS;
    }
    switch (ent->id) {
        case 0: case 1: case 17: return STYLE_ZOMBIE;
        case 2:                  return STYLE_DOG;
        case 3: case 4:          return STYLE_SPIDER;
        case 5:                  return STYLE_CROW;
        case 6:                  return STYLE_HUNTER;
        case 7:                  return STYLE_WASP;
        case 8: case 15:         return STYLE_VINE;
        case 9:                  return STYLE_CHIMERA;
        case 10: case 13: case 18: return STYLE_SNAKE;
        case 11:                 return STYLE_SHARK;
        case 12: case 16:        return STYLE_TYRANT;
        case 22: case 23: case 24: case 25: case 26: case 27:
        case 28: case 29: case 30: case 31:
                                 return STYLE_HUMAN;   // placeholder NPC models
        // The named cast each get their own style so they get their own eye
        // colour. Sharing STYLE_HUMAN gave Barry, Wesker, Chris, Richard and
        // Enrico all the same cold blue, which is why every character used to
        // look identical. Only the genuinely unnamed placeholders above still
        // share STYLE_HUMAN.
        case 32:                 return STYLE_CHRIS;   // Chris (cutscene actor)
        case 33:                 return STYLE_JILL;    // Jill (cutscene actor)
        case 34: case 43: case 45: return STYLE_BARRY;
        case 35: case 44:        return STYLE_REBECCA;
        case 36: case 46:        return STYLE_WESKER;
        case 37: case 38:        return STYLE_CORPSE;  // Kenneth / Forest
        case 39:                 return STYLE_RICHARD;
        case 40:                 return STYLE_ENRICO;
        case 41: case 42:        return STYLE_CORPSE;  // devoured Kenneth / dying Barry
        case 47:                 return STYLE_CHRIS;   // Chris, alternative outfit
        // Joseph Frost, the last of the Alpha team to appear (the dogs'
        // prologue kill). Id 48 was falling through to STYLE_NONE, so he was
        // not drawn at all rather than drawn as a placeholder. The emd table
        // would name index 48 "Chris alternative outfit 2", so this id is not
        // certain - but Joseph and Brad are both dead by the time the player
        // has a model on screen, so the stakes are a prologue cameo and the
        // mapping is left as the best reading rather than chased further.
        case 48:                 return STYLE_JOSEPH;
        // Brad Vickers, the Alpha pilot, with his own style so the yellow
        // flight vest and the afro have somewhere to come from.
        case 49:                 return STYLE_BRAD;
    }
    return STYLE_NONE;   // plant roots (1 joint), spider web, computer arms
}

// ---------------------------------------------------------------------------
// Which bone is which. Every model the stick figures draw - the player, the
// named cast, the zombies and the tyrant - has the same 15-joint humanoid
// skeleton, confirmed against the shipped models with
// tools/dump_emd_skeleton.py:
//
//     parent:  0<--1 1<-0 2<-0 3<-2 4<-3 5<-4 6<-2 7<-6 8<-7
//              9<-0 10<-9 11<-10 12<-0 13<-12 14<-13
//     leaves:  [1, 5, 8, 11, 14]
//
// so, read off that tree and cross-checked against StickFigure_LimbTips:
//
//     0        root / pelvis      (the spine runs 0 -> 1, into the head)
//     ├ 1      head
//     ├ 9 -> 10 -> 11      arm:  shoulder, elbow, hand   (upper arm 9->10)
//     ├ 12 -> 13 -> 14     arm:  shoulder, elbow, hand   (upper arm 12->13)
//     ├ 2 -> 3 -> 4 -> 5   leg:  hip, knee, ankle, foot  (thigh 2->3)
//     └ 6 -> 7 -> 8 -> 9*  (the other leg ends at 8; 9 is the arm above)
//
// The two chains swap sides from pose to pose, so a "left arm" bone is not
// reliably the character's left. What matters for a costume is only which
// bone is a shoulder, which is a thigh and which is a foot.
// ---------------------------------------------------------------------------
enum StickPart {
    PART_BONE = 0,     // unclassified: plain ink
    PART_HEAD,         // joint 1's ring
    PART_NECK,         // the head bone (0 -> 1)
    PART_TORSO,        // the body up to the shoulders
    PART_ARM,          // upper arm: a sleeve
    PART_HAND,         // forearm to hand: bare, or a glove
    PART_HIP,          // the thigh root's own bone
    PART_LEG,          // thigh and shin: trousers
    PART_FOOT          // ankle to foot: boots
};

// The child end of a bone decides what it is: the bone 9->10 is the upper arm
// because 10 is the elbow, and 10->11 is the hand. Joints the humanoid tree
// below does not reach return PART_BONE.
static int StickFigure_PartOfJoint(int style, int joint)
{
    // Only the humanoid skeletons are classified. A hunter or a dog shares the
    // low joint numbers with all the meaning stripped out, and colouring a
    // hunter's forearm as a "glove" would be wrong, so restrict to the styles
    // that actually have a costume.
    switch (style) {
    case STYLE_HUMAN: case STYLE_JILL: case STYLE_CHRIS: case STYLE_BARRY:
    case STYLE_WESKER: case STYLE_RICHARD: case STYLE_ENRICO:
    case STYLE_REBECCA: case STYLE_BRAD: case STYLE_JOSEPH:
    // The bodies on the floor are Bravo members and use the same humanoid
    // skeleton, so they have to be classified too. Without CORPSE here the
    // bone parts all fall back to PART_BONE, `dressed` goes false, and a corpse
    // renders as a bare ink skeleton with no uniform - which is exactly what a
    // body lying in the corridor looked like.
    case STYLE_CORPSE:
    // The humanoid monsters share the same 15-joint tree, so their limbs can be
    // classified the same way and then filled with monster flesh instead of a
    // costume (see StickFigure_MonsterFlesh). They are not `dressed`, so they
    // never take a S.T.A.R.S. colour; the parts only pick which flesh tone a
    // bone gets. The dog is a quadruped and is handled by the spine fill.
    case STYLE_ZOMBIE: case STYLE_TYRANT:
    case STYLE_HUNTER: case STYLE_CHIMERA:
        break;
    default:
        return PART_BONE;
    }

    switch (joint) {
    case 1:             return PART_HEAD;
    case 0:             return PART_TORSO;   // the bone 0->1 is the spine
    case 9: case 12:    return PART_TORSO;   // shoulder: torso, so it takes the vest
    case 10: case 13:   return PART_ARM;     // elbow
    case 11: case 14:   return PART_HAND;    // hand
    case 2: case 6:     return PART_HIP;     // thigh root
    case 3: case 7:     return PART_LEG;     // thigh/knee
    case 4: case 8:     return PART_FOOT;    // ankle/foot
    default:            return PART_BONE;
    }
}

// ---------------------------------------------------------------------------
// The costume. Every entry is a page colour: the figure's ink is copied to the
// line-art page byte for byte, so it is not lit and not hatched, and each
// colour has to read on white paper without being so dark that the strokes
// merge into the black outline. They are deliberately taken from the character
// bible rather than from the models' textures, one flat mid tone each, so the
// cast is still a drawing rather than a photograph.
//
// `vest` fills the torso, `sleeve` the upper arms, `trouser` the hips and legs,
// `boot` the feet, `skin` the hands and the head, and `hair` the cap and the
// hanging pieces. PART_BONE bones (a blown-off stump, an unclassified joint)
// keep the plain ink.
// ---------------------------------------------------------------------------
struct StickCostume {
    DWORD vest;
    DWORD sleeve;
    DWORD trouser;
    DWORD boot;
    DWORD glove;       // hands, when the character is not wearing fingerless ones
    DWORD skin;
    DWORD hair;
    DWORD accent;      // shoulder pads, belts, medical cross, bandanna
    bool  shoulderPads;
    bool  fingerlessGloves;
};

// Which page colour a bone is drawn in. The figure keeps its own tonal
// relationships - a boot is darker than the trouser above it - so it still
// reads as a drawing rather than as a colouring book, and the plain ink stays
// on everything a costume does not cover.
static DWORD StickFigure_InkForPart(const StickCostume* c, int part)
{
    switch (part) {
    case PART_TORSO:  return c->vest;
    case PART_NECK:   return c->vest;
    case PART_ARM:    return c->sleeve;
    case PART_HAND:   return c->fingerlessGloves ? c->skin : c->glove;
    case PART_HIP:    return c->trouser;
    case PART_LEG:    return c->trouser;
    case PART_FOOT:   return c->boot;
    default:          return 0;   // caller keeps the plain ink
    }
}

// True for the styles that wear a S.T.A.R.S. costume. Kept separate from
// StickFigure_PartOfJoint because the humanoid monsters are classified by the
// same joint map (so their flesh can be filled) but must never be handed a
// uniform colour.
static bool StickFigure_CostumedHuman(int style)
{
    switch (style) {
    case STYLE_HUMAN: case STYLE_JILL: case STYLE_CHRIS: case STYLE_BARRY:
    case STYLE_WESKER: case STYLE_RICHARD: case STYLE_ENRICO:
    case STYLE_REBECCA: case STYLE_BRAD: case STYLE_JOSEPH:
    case STYLE_CORPSE:
        return true;
    default:
        return false;
    }
}

// The flesh fill for a monster's body. A monster wears no clothes, but it is
// still a body on the page, so its torso and limbs are filled with a skin tone
// instead of being left as an empty outline. Each species gets its own page
// colour so they stay distinguishable, and the tone is kept pale enough to sit
// under the ink contour without swallowing it. Returns 0 for a style that has
// no body fill (the non-humanoid creatures fill along their own spine).
static DWORD StickFigure_MonsterFlesh(int style)
{
    switch (style) {
    case STYLE_ZOMBIE:  return 0xFF9AA085u;   // rotten grey-green flesh
    case STYLE_TYRANT:  return 0xFFC49A8Cu;   // pale exposed muscle
    case STYLE_HUNTER:  return 0xFF6E8A54u;   // mottled green hide
    case STYLE_CHIMERA: return 0xFF8A7E62u;   // fly-blown tan
    case STYLE_DOG:     return 0xFF7A6E60u;   // dark hide, mangy
    case STYLE_CROW:    return 0xFF3A3E4Au;   // black plumage
    case STYLE_SPIDER:  return 0xFF5A4A3Au;   // bristled brown abdomen
    case STYLE_SNAKE:   return 0xFF6E7A4Cu;   // olive scale
    case STYLE_SHARK:   return 0xFF6C7A86u;   // grey shark skin
    case STYLE_VINE:    return 0xFF4E6A3Cu;   // green vine
    case STYLE_WASP:    return 0xFFB0A03Cu;   // banded yellow
    default:            return 0;
    }
}

static StickCostume StickFigure_CostumeOf(int style)
{
    const DWORD vest     = 0xFF3A4452u;   // the S.T.A.R.S. uniform blue
    const DWORD sleeve   = 0xFF56606Eu;
    const DWORD trouser  = 0xFF2C3038u;
    const DWORD boot     = 0xFF23262Bu;
    const DWORD glove    = 0xFF2A2622u;
    const DWORD tan      = 0xFFD2A87Cu;
    const DWORD auburn   = 0xFF6B563Cu;

    StickCostume c;
    c.vest = vest;
    c.sleeve = sleeve;
    c.trouser = trouser;
    c.boot = boot;
    c.glove = glove;
    c.skin = tan;
    c.hair = auburn;
    c.accent = vest;
    c.shoulderPads = false;
    c.fingerlessGloves = false;

    switch (style) {
    case STYLE_JILL:
        // Navy beret, chestnut chin-length bob, dark teal padded vest with the
        // thick horizontal shoulder guards, fingerless gloves, blue S.T.A.R.S.
        // arm patch, thigh holster on the right leg. Her vest is the darkest
        // in the cast, so the beret and the patch get a brighter blue to stay
        // readable against it.
        //
        // The blue top is what the user sees first, so it has to be an honest
        // blue rather than the near-black teal the vest started as: the vest
        // (torso fill) is the S.T.A.R.S. blue, and the sleeve - the shirt on
        // the upper arms - is a lighter blue of the same family, so the top
        // reads as blue clothing instead of as a dark silhouette.
        c.vest = 0xFF2E5A86u;   // the blue top / padded vest
        c.sleeve = 0xFF3E6E9Eu; // lighter blue shirt on the arms
        c.trouser = 0xFF556E92u; // slate-blue trousers, light enough to read
                                 // as painted cloth against the ink outline
        c.boot = 0xFF3A3A42u;   // boots, a shade off black but not ink
        c.skin = 0xFFE4B489u;   // she is the fair one of the two leads
        c.hair = 0xFF8A5A38u;   // chestnut
        c.accent = 0xFF3C6E8Cu; // beret, patch, holster
        c.shoulderPads = true;
        c.fingerlessGloves = true;
        break;

    case STYLE_CHRIS:
        // Spiky brown hair, olive vest over a light sage shirt, dark straps,
        // khaki cargos with knee seams, oversized rectangular shoulder pads.
        // His arms are drawn thicker (see the ink pass).
        c.vest = 0xFF4A5232u;   // olive drab
        c.sleeve = 0xFF7C8A66u; // sage tee, still a light tone
        c.trouser = 0xFFA99A73u; // khaki
        c.boot = 0xFF3A3E44u;
        c.skin = 0xFFD8AC80u;
        c.hair = 0xFF6B4A2Cu;   // brown
        c.accent = 0xFF39404Cu; // straps, pads
        c.shoulderPads = true;
        break;

    case STYLE_WESKER:
        // Slicked blonde hair, black rectangular sunglasses, navy-black vest
        // with silver buckles over a midnight-blue shirt with rolled sleeves,
        // brown gun belt with dual hip holsters, polished black dress boots.
        c.vest = 0xFF242A36u;
        c.sleeve = 0xFF2E394Cu; // rolled sleeves: darker than a bare arm
        c.trouser = 0xFF232A36u;
        c.boot = 0xFF1C1E22u;
        c.skin = 0xFFD8AC80u;
        c.hair = 0xFFC2A45Cu;   // blond
        c.accent = 0xFF9AA6B4u; // buckles and the sunglasses bar
        break;

    case STYLE_BARRY:
        // Reddish-brown spikes and a thick rounded beard, high-visibility
        // crimson vest over a denim-blue shirt, heavy brown utility belt with
        // oversized pouches.
        c.vest = 0xFFA03434u;   // crimson. Not fire-engine red: the red-means-
                                // infection rule is about eyes and wounds, and
                                // Barry's vest is his own colour, but it is
                                // still kept deepest of the bright vests.
        c.sleeve = 0xFF4E6E96u; // denim blue
        c.trouser = 0xFF2E3A4Cu;
        c.boot = 0xFF23262Bu;
        c.skin = 0xFFD2A87Cu;
        c.hair = 0xFF8C4E34u;   // reddish brown
        c.accent = 0xFF6A4A2Cu; // the utility belt
        break;

    case STYLE_REBECCA:
        // Short dark pixie cut with long bangs, white medic vest with a red
        // cross and green shoulder trim over an olive undershirt, red bandanna
        // at the neck, white socks over green-trimmed boots.
        c.vest = 0xFFE2E4DEu;   // white medic vest
        c.sleeve = 0xFF4E5A3Cu; // olive undershirt
        c.trouser = 0xFF4E5A3Cu;
        c.boot = 0xFF3C4434u;
        c.skin = 0xFFE8C09Cu;
        c.hair = 0xFF4A3826u;   // dark brown
        c.accent = 0xFFB23530u; // the red cross and the bandanna
        break;

    case STYLE_BRAD:
        // Brown afro, yellow flight headset, bright yellow vest over a
        // powder-blue shirt.
        c.vest = 0xFFE0BC32u;
        c.sleeve = 0xFFA8C2DAu;
        c.trouser = 0xFF304868u;
        c.boot = 0xFF23262Bu;
        c.skin = 0xFFC89468u;
        c.hair = 0xFF3E2E20u;
        c.accent = 0xFFE8C84Eu; // headset
        break;

    case STYLE_JOSEPH:
        // Dark red bandanna, olive vest over dark gray, a diagonal bandolier.
        c.vest = 0xFF4A5232u;
        c.sleeve = 0xFF44484Eu;
        c.trouser = 0xFFA99A73u; // khaki cargo
        c.boot = 0xFF2E3238u;
        c.skin = 0xFFD2A87Cu;
        c.hair = 0xFF3E3226u;
        c.accent = 0xFF8C2A2Au;  // bandanna
        break;

    case STYLE_RICHARD:
        // Medium brown hair, the S.T.A.R.S. vest, bandaged across the
        // midsection. The bandage is drawn, not coloured.
        c.hair = 0xFF5E4630u;
        c.skin = 0xFFD8AC80u;
        break;

    case STYLE_ENRICO:
        // Receding graying hair, mustache, bloodied dark blue vest.
        c.vest = 0xFF333E52u;
        c.hair = 0xFF9A948Cu;   // graying
        c.skin = 0xFFC89C74u;
        break;

    case STYLE_CORPSE:
    case STYLE_HUMAN:
    default:
        // The two corpses are Bravo members and the placeholder actors are
        // unnamed, so they keep the plain uniform: dark blue vest, no colour
        // coding, so a body on the floor is not dressed as a survivor.
        break;
    }
    return c;
}

// The head joint for each skeleton. Humans put the head on the root's only
// leaf child (joint 1 - the game's own code names joint 1 the head).
// Zombies/tyrants hang the head off the neck hub (joint 2); the hunter's skull
// is also joint 2 (joint 3 is the snout). The cerberus and chimera skulls are
// joint 3. Spiders/crows/wasps put it on joint 1. Snakes and the shark carry
// the head at the root of the chain; a vine's head is the far tip.
static int StickFigure_HeadJoint(int style, const unsigned char* animBase,
                                 int jointCount, int topJoint)
{
    int head = -1;
    switch (style) {
    case STYLE_ZOMBIE:
    case STYLE_TYRANT:  head = 2; break;
    case STYLE_HUNTER:  head = 3; break;   // .emd: 1->2->3, the skull is leaf 3
    case STYLE_DOG:     head = 4; break;   // cerberus .emd: 1->2->3->4, skull leaf 4
    case STYLE_CHIMERA: head = 3; break;   // chimera .emd: 1->2->3, skull leaf 3
    case STYLE_SPIDER:
    case STYLE_WASP:    head = 1; break;   // body hub 0; joint 1 is the head stub
    case STYLE_CROW:    head = 2; break;   // crow .emd: 0<-1<-2, the skull is leaf 2
    case STYLE_SNAKE:
    case STYLE_SHARK:   head = 0; break;
    case STYLE_VINE:    head = jointCount - 1; break;
    default: {
        int leafChild = -1, leafCount = 0;
        int rootChildren = (signed char)animBase[0];
        const unsigned char* rootChildList =
            animBase + *(const unsigned short*)(animBase + 2);
        for (int c = 0; c < rootChildren; ++c) {
            int ch = rootChildList[c];
            if (ch <= 0 || ch >= jointCount) continue;
            if ((signed char)animBase[ch * 4] == 0) { leafChild = ch; ++leafCount; }
        }
        if (leafCount == 1) head = leafChild;
        break;
    }
    }
    if (head < 0 || head >= jointCount || !g_stickOk[head]) head = topJoint;
    return head;
}

// Head ring radius as a fraction of body length. Snakes, the shark and vines
// have no ring: their skull is drawn as a wedge/maw at the head joint.
static float StickFigure_HeadRadiusFactor(int style)
{
    switch (style) {
    case STYLE_HUMAN: case STYLE_JILL: case STYLE_REBECCA:
    case STYLE_CHRIS: case STYLE_BARRY: case STYLE_WESKER:
    case STYLE_RICHARD: case STYLE_ENRICO: case STYLE_BRAD:
    case STYLE_JOSEPH:
    case STYLE_ZOMBIE: return 0.115f;    case STYLE_CORPSE: return 0.110f;
    case STYLE_HUNTER: return 0.110f;
    case STYLE_CHIMERA: return 0.100f;
    case STYLE_TYRANT: return 0.095f;
    case STYLE_DOG:    return 0.095f;
    case STYLE_CROW:   return 0.075f;
    case STYLE_WASP:   return 0.060f;
    case STYLE_SPIDER: return 0.030f;
    default:           return 0.0f;
    }
}

// Turn the recorded joint origins into segments, using the animation header's
// child lists for the parent links. Called at the end of render_entity.
static void StickFigure_Finish(Entity* ent, int style)
{
    if (ent == NULL || ent->jointsStructs == NULL || ent->animHeader == 0) return;
    if (g_stickFigureCount >= STICK_MAX_FIGURES) return;

    StickFigureData* fig = &g_stickFigures[g_stickFigureCount];
    memset(fig, 0, sizeof(*fig));
    fig->style = style;
    fig->isPlayer = (ent == (Entity*)&g_playerEntity);

    int jointCount = ent->jointCount;
    if (jointCount > STICK_MAX_JOINTS) jointCount = STICK_MAX_JOINTS;
    if (jointCount < 2) return;

    signed char parent[STICK_MAX_JOINTS];
    for (int i = 0; i < STICK_MAX_JOINTS; ++i) parent[i] = -1;

    const unsigned short* animPtr = (const unsigned short*)ent->animHeader;
    const unsigned char* animBase =
        (const unsigned char*)animPtr + (*animPtr & 0xFFFFFFFCu);

    // Breadth-first from the root joint (index 0), so every joint is reached
    // through its parent and its parent index is known.
    signed char queue[STICK_MAX_JOINTS];
    int qHead = 0, qTail = 0;
    queue[qTail++] = 0;
    while (qHead < qTail) {
        int idx = queue[qHead++];
        int childCount = (signed char)animBase[idx * 4];
        const unsigned char* childList =
            animBase + *(const unsigned short*)(animBase + 2 + idx * 4);
        for (int c = 0; c < childCount; ++c) {
            int child = childList[c];
            if (child <= 0 || child >= jointCount) continue;
            if (parent[child] >= 0) continue;   // already linked, or a cycle
            parent[child] = (signed char)idx;
            if (qTail < STICK_MAX_JOINTS) queue[qTail++] = (signed char)child;
        }
    }

    int   topJoint = -1;
    float topY = 0.0f;

    fig->jointCount = jointCount;
    float farthestZ = -3.0e38f;
    const JointStruct* jointArr = ent->jointsStructs;
    for (int i = 0; i < jointCount; ++i) {
        fig->jok[i] = g_stickOk[i];
        fig->jparent[i] = parent[i];
        // flags bit 0x08 means joint_enable_special_effect has been run on this
        // joint: it was sprayed by joint_setup_attack_effect. That happens when
        // the joint is blown off (the zombie head at joint 2 on a headshot,
        // the two legs on explode_leg_and_drop), so it is the one signal that
        // says "this part is no longer attached to the body".
        fig->jflags[i] = jointArr ? jointArr[i].flags : 0;
        if (!g_stickOk[i]) continue;
        fig->jx[i] = g_stickPx[i];
        fig->jy[i] = g_stickPy[i];
        fig->jz[i] = g_stickPz[i];
        if (g_stickPz[i] > farthestZ) farthestZ = g_stickPz[i];
        if (topJoint < 0 || g_stickPy[i] < topY) {
            topY = g_stickPy[i];
            topJoint = i;
        }
    }
    // Sorting key, in the SAME units the room masks carry.
    //
    // A room mask is submitted as depthSort = posData * 16 (AddSprite, 0x0046ddc0)
    // and its posData is an ordering-table BUCKET, not a raw view Z: the PS1
    // DrawRoomSpr sorts each overlay with min(posData >> 2, 0x3ff). An entity
    // enters that same table at viewZ >> 4 (FUN_00483250 passes depthShift 4), so
    // one bucket is 16 view-Z units and the mask key equals (viewZ >> 4) * 16.
    // Measured in room 6: the player's view Z is 15777, bucket 986, and the masks
    // bracket it at posData 998/1002 - the two scales line up one for one.
    //
    // The key is the ROOT joint (joint 0), because that is the point the original
    // enters the model into the ordering table at: one entity, one OT depth, at
    // t[2] >> 4. Keying on the farthest joint instead put the figure at the depth
    // of whichever limb happened to point away from the camera, so a wall that
    // sat between the camera and the body fell on the far side of the figure and
    // was painted before it - the figure then showed through the wall. Raw view Z
    // (what this stored before) is also 16x too small for the mask scale.
    float sortZ = g_stickOk[0] ? g_stickPz[0] : farthestZ;
    if (sortZ > -3.0e37f) {
        float bucket = (sortZ < 0.0f) ? 0.0f : (float)((int)(sortZ * 0.0625f)); // >> 4
        fig->maxZ = bucket * 16.0f;
    } else {
        fig->maxZ = sortZ;
    }

    for (int i = 0; i < jointCount; ++i) {
        if (!g_stickOk[i]) continue;
        if (fig->segmentCount >= STICK_MAX_SEGMENTS) continue;
        // A blown-off joint is detached: its world matrix is no longer composed
        // from its parent (FUN_004896c0 flies it under gravity on its own), so
        // a bone to the parent would be a rubber band stretching from the body
        // to wherever the limb landed. Skip the link; the stub is drawn at the
        // joint's own position below.
        if ((fig->jflags[i] & 0x04) != 0) continue;
        int p = parent[i];
        if (p < 0 || p >= jointCount || !g_stickOk[p]) continue;

        StickSegment* s = &fig->segments[fig->segmentCount++];
        s->x0 = g_stickPx[p]; s->y0 = g_stickPy[p]; s->vz0 = g_stickPz[p];
        s->x1 = g_stickPx[i]; s->y1 = g_stickPy[i]; s->vz1 = g_stickPz[i];
        // Which limb this bone is. The far end names it: the bone into joint 5
        // is the forearm/hand, the bone into joint 3 is the chest.
        int part = StickFigure_PartOfJoint(style, i);
        if (part == PART_BONE && (fig->jflags[i] & 0x04) == 0 &&
            fig->jflags[p] == 0) {
            part = StickFigure_PartOfJoint(style, p);
        }
        s->part = (signed char)part;
    }

    int headJoint = StickFigure_HeadJoint(style, animBase, jointCount, topJoint);

    // A blown-off head must not be drawn: the original's model has no head
    // geometry there, so the skull ring and the face would be a head floating
    // over a headless corpse.
    //
    // The head joint is marked with 0x88 when it is destroyed (Zombie.cpp,
    // "|0x88" on joint +0xF8 during the head-bite/headshot keyframe, and the
    // game's own "already blown apart" test is the same byte & 0xCC).
    //
    // Read bit 0x80, not bit 0x08: rotate_entity rewrites the flags byte every
    // frame with `(f & 0xF5) | 0x50`, and 0xF5 clears bit 0x08 while keeping
    // 0x80. Testing 0x08 here was why the head never went away.
    if (headJoint >= 0 && headJoint < jointCount &&
        (fig->jflags[headJoint] & 0x80) != 0) {
        fig->headless = true;
    }

    if (g_stickOk[0]) {
        fig->rootX = g_stickPx[0];
        fig->rootY = g_stickPy[0];
        fig->rootZ = g_stickPz[0];
    }

    if (headJoint >= 0 && g_stickOk[headJoint]) {
        fig->headX = g_stickPx[headJoint];
        fig->headY = g_stickPy[headJoint];
        fig->headZ = g_stickPz[headJoint];

        // Only fall back to the head when joint 0 is off-camera; otherwise the
        // root is the pelvis/rear anchor the dog's tail hangs from.
        if (!g_stickOk[0]) {
            fig->rootX = fig->headX;
            fig->rootY = fig->headY;
            fig->rootZ = fig->headZ;
        }

        // Body length: the farthest joint from the head - a foot, paw, wing or
        // leg tip depending on the species. The head ring and the details are
        // sized from it, so the figure keeps its proportions with distance.
        float bodyLen = 0.0f;
        int farJoint = headJoint;
        for (int i = 0; i < jointCount; ++i) {
            if (!g_stickOk[i]) continue;
            float dx = g_stickPx[i] - fig->headX;
            float dy = g_stickPy[i] - fig->headY;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > bodyLen) { bodyLen = d; farJoint = i; }
        }
        fig->footX = g_stickPx[farJoint];
        fig->footY = g_stickPy[farJoint];
        fig->footZ = g_stickPz[farJoint];
        fig->bodyLen = bodyLen;

        // Head -> root is the body's "down" axis (humanoid faces, Jill's
        // costume). It is stable even when an arm is the farthest joint.
        float dxr = fig->rootX - fig->headX;
        float dyr = fig->rootY - fig->headY;
        float tl = sqrtf(dxr * dxr + dyr * dyr);
        if (tl < 1.0f) {
            dxr = fig->footX - fig->headX;
            dyr = fig->footY - fig->headY;
            tl = sqrtf(dxr * dxr + dyr * dyr);
        }
        if (tl >= 1.0f) {
            fig->downX = dxr / tl;
            fig->downY = dyr / tl;
            fig->torsoLen = tl;
        } else {
            fig->downX = 0.0f;
            fig->downY = 1.0f;
            fig->torsoLen = 1.0f;
        }

        float scaleX;
        MarniGetRenderScale(&scaleX, NULL);
        float f = StickFigure_HeadRadiusFactor(style);
        fig->headR = f * bodyLen;
        if (f > 0.0f) {
            if (fig->headR > 8.0f * scaleX) fig->headR = 8.0f * scaleX;
            if (fig->headR < 1.5f) fig->headR = 1.5f;
        } else {
            fig->headR = 0.0f;
        }
    } else {
        fig->headR = 0.0f;
        fig->bodyLen = 0.0f;
        fig->footX = fig->headX;
        fig->footY = fig->headY;
        fig->footZ = fig->headZ;
    }

    ++g_stickFigureCount;
}

// ---------------------------------------------------------------------------
// Ink primitives. `vz` is the camera-space Z of the stroke; the depth-tested
// path converts it with the same linear ramp the model vertices use, so the
// comparison is against the identical depth buffer value.
// ---------------------------------------------------------------------------

// When set, the ink primitives write the scene-colour alpha marker instead of
// drawing into the line-art page. The figure is then part of the scene's
// painter order (see FlushTmdObjects), and every primitive is also depth-tested
// against the room, so a wall nearer than the figure hides it. Both are needed:
// painter order alone let the ink paint over a wall it should have been behind.
static int g_stickSceneInk = 0;

static void StickFigure_Line(float x0, float y0, float vz0,
                             float x1, float y1, float vz1,
                             float thick, DWORD ink, int depthTest)
{
    if (g_stickSceneInk) {
        // Scene ink is depth-tested. Without this the ink is a flat composite
        // that paints over walls, so a character behind a wall stays fully
        // visible. The line already carries each end's camera-space Z, so the
        // { x, y, ndcZ, viewZ } quad tests correctly at both ends and is
        // interpolated between them.
        MarniDrawInkLineDepth(x0, y0, TmdDepthNdc(vz0), vz0,
                              x1, y1, TmdDepthNdc(vz1), vz1, thick, ink);
    } else if (depthTest) {
        MarniDrawLineDepth(x0, y0, TmdDepthNdc(vz0), vz0,
                           x1, y1, TmdDepthNdc(vz1), vz1, thick, ink);
    } else {
        MarniDrawLine(x0, y0, x1, y1, thick, ink);
    }
}

static void StickFigure_Dot(float cx, float cy, int size, DWORD ink,
                            float vz, int depthTest)
{
    if (size < 1) size = 1;
    if (g_stickSceneInk) {
        // Depth-tested: a joint dot, an eye or the head dot must not show
        // through a wall any more than the bone lines do.
        MarniDrawInkRectDepth(cx - size * 0.5f, cy - size * 0.5f,
                              (float)size, (float)size,
                              TmdDepthNdc(vz), vz, ink);
    } else if (depthTest) {
        MarniDrawRectDepth(cx - size * 0.5f, cy - size * 0.5f,
                           (float)size, (float)size,
                           TmdDepthNdc(vz), vz, ink);
    } else {
        MarniDrawRect((int)(cx - size * 0.5f), (int)(cy - size * 0.5f),
                      size, size, ink);
    }
}

// Closed ring of `segs` lines - the head, and the hunter's frog eyes.
static void StickFigure_Ring(float cx, float cy, float r, int segs,
                             float thick, DWORD ink, float vz, int depthTest)
{
    for (int k = 0; k < segs; ++k) {
        float a0 = (float)k * 6.2831853f / (float)segs;
        float a1 = (float)(k + 1) * 6.2831853f / (float)segs;
        StickFigure_Line(cx + r * cosf(a0), cy + r * sinf(a0), vz,
                         cx + r * cosf(a1), cy + r * sinf(a1), vz,
                         thick, ink, depthTest);
    }
}

// Polyline through pts[] (x, y pairs), n points.
static void StickFigure_Polyline(const float* pts, int n, float thick,
                                 DWORD ink, float vz, int depthTest)
{
    for (int i = 0; i + 1 < n; ++i) {
        StickFigure_Line(pts[i * 2], pts[i * 2 + 1], vz,
                         pts[i * 2 + 2], pts[i * 2 + 3], vz,
                         thick, ink, depthTest);
    }
}

// The same polyline with the last point joined back to the first. Needed for
// any shape that has to read as a closed outline - Jill's chest cones are
// triangles, and an open triangle looks like a single line with a kink.
static void StickFigure_Polygon(const float* pts, int n, float thick,
                                DWORD ink, float vz, int depthTest)
{
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        StickFigure_Line(pts[i * 2], pts[i * 2 + 1], vz,
                         pts[j * 2], pts[j * 2 + 1], vz,
                         thick, ink, depthTest);
    }
}

// Fan of strokes from a joint along base->tip - claws, wing feathers, webbed
// fingers. Endpoints share the tip's depth (the details are only a few pixels).
static void StickFigure_Fan(const StickFigureData* fig, int tip, int base,
                            float length, float spread, int count,
                            float thick, DWORD ink, int depthTest)
{
    if (tip < 0 || tip >= fig->jointCount || base < 0 || base >= fig->jointCount)
        return;
    if (!fig->jok[tip] || !fig->jok[base]) return;

    float fx = fig->jx[tip] - fig->jx[base];
    float fy = fig->jy[tip] - fig->jy[base];
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl < 0.5f) return;
    fx /= fl; fy /= fl;
    float px = -fy, py = fx;
    float vz = fig->jz[tip];

    for (int k = 0; k < count; ++k) {
        float t = (count <= 1) ? 0.0f
                : (-0.5f + (float)k / (float)(count - 1));
        StickFigure_Line(fig->jx[tip], fig->jy[tip], vz,
                         fig->jx[tip] + fx * length + px * t * spread,
                         fig->jy[tip] + fy * length + py * t * spread, vz,
                         thick, ink, depthTest);
    }
}

// ---------------------------------------------------------------------------
// Faces. Every style gets eyes and a mouth; the human ones are deliberately
// plain so the monster faces read as different.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The details a character bible calls out that are not a colour: Wesker's
// sunglasses, Barry's beard, Rebecca's medic cross and bandanna, Brad's
// headset, Joseph's bandanna, Chris's knife. Each one is anchored to the head
// ring and the body axis, so it follows the pose.
// ---------------------------------------------------------------------------
static void StickFigure_DrawFaceAccessory(const StickFigureData* fig, float thick,
                                          DWORD ink, int which, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;
    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    switch (which) {
    case 1: {
        // Wesker's sunglasses: one solid bar right across the eyes, which is
        // the single trait that has to survive at this size. The eyes are then
        // not drawn at all (the caller draws no face), so nothing shows behind
        // the lenses.
        float ey = -0.06f * hr;         // the same eye line DrawHumanFace uses
        float halfW = 0.44f * hr;
        float rise = 0.30f * hr;
        // A filled lens: three strokes across the same bar, because a stick
        // figure has no fill primitive.
        for (int k = -1; k <= 1; ++k) {
            float t = (float)k * 0.5f;
            StickFigure_Line(hx - px * halfW + ux * (ey + t * rise),
                             hy - py * halfW + uy * (ey + t * rise), vz,
                             hx + px * halfW + ux * (ey + t * rise),
                             hy + py * halfW + uy * (ey + t * rise), vz,
                             thick * 1.5f, ink, depthTest);
        }
        // Temples: a short arm back toward each ear.
        for (int s = -1; s <= 1; s += 2) {
            StickFigure_Line(hx + px * (float)s * halfW + ux * ey,
                             hy + py * (float)s * halfW + uy * ey, vz,
                             hx + px * (float)s * hr * 0.88f - ux * hr * 0.10f,
                             hy + py * (float)s * hr * 0.88f - uy * hr * 0.10f,
                             vz, thick * 0.8f, ink, depthTest);
        }
        break;
    }
    case 2: {
        // Barry's beard: a thick rounded mass covering the lower face from ear
        // to ear, meeting under the chin.
        float back = 0.10f * hr;
        float pts[6] = {
            hx - px * hr * 0.70f + ux * hr * 0.52f,
            hy - py * hr * 0.70f + uy * hr * 0.52f,
            hx + ux * hr * 1.04f, hy + uy * hr * 1.04f,
            hx + px * hr * 0.70f + ux * hr * 0.52f,
            hy + py * hr * 0.70f + uy * hr * 0.52f,
        };
        // Three nested arcs, so the beard reads as a full mass rather than a
        // single line, then the chin.
        for (int k = 0; k < 3; ++k) {
            float o = (float)k * 0.10f * hr;
            float arc[6] = {
                pts[0] - ux * o * 0.5f, pts[1] - uy * o * 0.5f,
                pts[2] + ux * o,        pts[3] + uy * o,
                pts[4] - ux * o * 0.5f, pts[5] - uy * o * 0.5f,
            };
            StickFigure_Polyline(arc, 3, thick * 1.1f, ink, vz, depthTest);
        }
        StickFigure_Line(hx - px * hr * 0.70f - ux * back,
                         hy - py * hr * 0.70f - uy * back, vz,
                         hx + px * hr * 0.70f - ux * back,
                         hy + py * hr * 0.70f - uy * back, vz,
                         thick * 0.8f, ink, depthTest);
        break;
    }
    case 3: {
        // Rebecca: a red cross low on the chest, and a red bandanna knotted at
        // the throat.
        StickFigure_Line(hx - px * hr * 0.24f + ux * hr * 2.05f,
                         hy - py * hr * 0.24f + uy * hr * 2.05f, vz,
                         hx + px * hr * 0.24f + ux * hr * 2.05f,
                         hy + py * hr * 0.24f + uy * hr * 2.05f, vz,
                         thick * 1.5f, ink, depthTest);
        StickFigure_Line(hx + ux * hr * 1.80f, hy + uy * hr * 1.80f, vz,
                         hx + ux * hr * 2.30f, hy + uy * hr * 2.30f, vz,
                         thick * 1.5f, ink, depthTest);
        // Bandanna: a band around the neck with a tail.
        StickFigure_Line(hx - px * hr * 0.52f + ux * hr * 1.10f,
                         hy - py * hr * 0.52f + uy * hr * 1.10f, vz,
                         hx + px * hr * 0.52f + ux * hr * 1.10f,
                         hy + py * hr * 0.52f + uy * hr * 1.10f, vz,
                         thick * 1.3f, ink, depthTest);
        StickFigure_Line(hx + px * hr * 0.30f + ux * hr * 1.10f,
                         hy + py * hr * 0.30f + uy * hr * 1.10f, vz,
                         hx + px * hr * 0.70f + ux * hr * 1.62f,
                         hy + py * hr * 0.70f + uy * hr * 1.62f, vz,
                         thick * 0.8f, ink, depthTest);
        break;
    }
    case 4: {
        // Brad: a headset band over the crown with an earpiece, because the
        // afro alone reads as any of the brown-haired cast.
        StickFigure_Line(hx - px * hr * 0.80f + ux * hr * 0.10f,
                         hy - py * hr * 0.80f + uy * hr * 0.10f, vz,
                         hx + px * hr * 0.80f + ux * hr * 0.10f,
                         hy + py * hr * 0.80f + uy * hr * 0.10f, vz,
                         thick * 0.9f, ink, depthTest);
        StickFigure_Dot(hx + px * hr * 0.82f + ux * hr * 0.10f,
                        hy + py * hr * 0.82f + uy * hr * 0.10f,
                        (int)(hr * 0.24f) + 2, ink, vz, depthTest);
        break;
    }
    case 5: {
        // Joseph: a bandanna wound round the head, a tail at the back.
        StickFigure_Line(hx - px * hr * 0.86f - ux * hr * 0.06f,
                         hy - py * hr * 0.86f - uy * hr * 0.06f, vz,
                         hx + px * hr * 0.86f - ux * hr * 0.06f,
                         hy + py * hr * 0.86f - uy * hr * 0.06f, vz,
                         thick * 1.4f, ink, depthTest);
        StickFigure_Line(hx - px * hr * 0.70f - ux * hr * 0.02f,
                         hy - py * hr * 0.70f - uy * hr * 0.02f, vz,
                         hx - px * hr * 1.10f + ux * hr * 0.55f,
                         hy - py * hr * 1.10f + uy * hr * 0.55f, vz,
                         thick * 0.8f, ink, depthTest);
        break;
    }
    case 6: {
        // Chris/Joseph: a diagonal strap across the chest harness - the combat
        // knife for Chris, the bandolier for Joseph. One stroke, coloured by
        // the caller.
        StickFigure_Line(hx + px * hr * 0.30f + ux * hr * 1.35f,
                         hy + py * hr * 0.30f + uy * hr * 1.35f, vz,
                         hx - px * hr * 0.30f + ux * hr * 2.45f,
                         hy - py * hr * 0.30f + uy * hr * 2.45f, vz,
                         thick * 1.3f, ink, depthTest);
        break;
    }
    default:
        break;
    }
}


static void StickFigure_DrawHumanFace(const StickFigureData* fig, float thick,
                                      DWORD ink, DWORD eyeInk, DWORD mouthInk,
                                      int depthTest, int smile)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    int eye = (int)(hr * 0.26f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx - ux * hr * 0.08f + px * hr * 0.38f,
                    hy - uy * hr * 0.08f + py * hr * 0.38f,
                    eye, eyeInk, vz, depthTest);
    StickFigure_Dot(hx - ux * hr * 0.08f - px * hr * 0.38f,
                    hy - uy * hr * 0.08f - py * hr * 0.38f,
                    eye, eyeInk, vz, depthTest);

    float d = smile ? 0.60f : 0.50f;
    float pts[6] = {
        hx - px * hr * 0.38f + ux * hr * 0.50f,
        hy - py * hr * 0.38f + uy * hr * 0.50f,
        hx + ux * hr * d,
        hy + uy * hr * d,
        hx + px * hr * 0.38f + ux * hr * 0.50f,
        hy + py * hr * 0.38f + uy * hr * 0.50f,
    };
    StickFigure_Polyline(pts, 3, thick * 1.05f, mouthInk, vz, depthTest);
    (void)ink;
}

// Hair for the named cast. It has to HUG the head: the old version drew a
// semicircle at 1.3 to 1.45 times the head radius, which is a dome floating
// around the skull, and at stick-figure size a dome on a round head just reads
// as an afro on everyone. This draws a hairline that follows the top of the
// head at about the head radius, then a side and back shape that is what
// actually tells the hairstyles apart.
//
//   capScale  how far the hairline sits above the skull (1.02 = just touching)
//   capArc    how much of the top it covers, as a fraction of a half circle
//             (1.0 = ear to ear, 0.7 = a smaller crown)
//   hangR     how far the back of the hair falls down the body axis, in head
//             radii (0 = a close crop, 0.6 = a bob to the jaw)
//   flareR    how far the sides stick out, in head radii (0 = flat to the head)
//   side      which sides get the hanging pieces: -1 left, +1 right, 0 both
static void StickFigure_DrawHair(const StickFigureData* fig, float thick,
                                 DWORD ink, float capScale, float capArc,
                                 float hangR, float flareR, int side,
                                 int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    // The cap: an arc over the top of the head only, sitting just off the
    // skull. `capArc` limits how far down the sides it reaches, so it is a
    // hairline rather than a full circle around the face.
    float hairR = hr * capScale;
    int segs = 20;
    float span = 3.1415927f * capArc;
    float start = -1.5707963f - span * 0.5f;
    for (int k = 0; k < segs; ++k) {
        float a0 = start + span * (float)k / (float)segs;
        float a1 = start + span * (float)(k + 1) / (float)segs;
        float c0 = cosf(a0), s0 = sinf(a0);
        float c1 = cosf(a1), s1 = sinf(a1);
        StickFigure_Line(hx - ux * c0 * hairR + px * s0 * hairR,
                         hy - uy * c0 * hairR + py * s0 * hairR, vz,
                         hx - ux * c1 * hairR + px * s1 * hairR,
                         hy - uy * c1 * hairR + py * s1 * hairR, vz,
                         thick, ink, depthTest);
    }

    if (hangR <= 0.0f) return;

    // The hanging pieces: where the hair falls past the cap on the sides or the
    // back. A bob drops on both sides, a side parting on one, a long back only
    // behind.
    for (int sd = -1; sd <= 1; sd += 2) {
        if (side != 0 && sd != side) continue;
        float s = (float)sd;
        float ax = hx + px * s * hr * capScale * 0.92f;
        float ay = hy + py * s * hr * capScale * 0.92f;
        float pts[8] = {
            ax, ay,
            ax + px * s * flareR * hr + ux * hangR * 0.45f * hr,
            ay + py * s * flareR * hr + uy * hangR * 0.45f * hr,
            ax + px * s * flareR * 0.70f * hr + ux * hangR * 0.80f * hr,
            ay + py * s * flareR * 0.70f * hr + uy * hangR * 0.80f * hr,
            ax + ux * hangR * hr, ay + uy * hangR * hr,
        };
        StickFigure_Polyline(pts, 4, thick, ink, vz, depthTest);
    }
}

// Spiky hair, for the two heads the bible calls spiky (Chris and Barry). Five
// short strokes radiating from the crown of the cap, drawn on top of the
// hairline, so at stick-figure size the head has a broken, bristling edge
// instead of the smooth dome that reads as the default crop.
static void StickFigure_DrawHairSpikes(const StickFigureData* fig, float thick,
                                       DWORD ink, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    // Spread the spikes over the top third of the skull, fanning out from the
    // crown. Each one leans outwards from the head's own up axis, so the spikes
    // follow a head that is looking off to one side.
    for (int k = -2; k <= 2; ++k) {
        float t = (float)k / 2.0f;              // -1 .. 1 across the crown
        float baseR = hr * 1.04f;
        float bx = hx - ux * baseR + px * t * hr * 0.72f;
        float by = hy - uy * baseR + py * t * hr * 0.72f;
        // Lean: outward along the side direction, and up the body axis.
        float lean = 0.34f + 0.16f * fabsf(t);
        float tx = bx + px * t * hr * lean * 0.9f - ux * hr * lean;
        float ty = by + py * t * hr * lean * 0.9f - uy * hr * lean;
        StickFigure_Line(bx, by, vz, tx, ty, vz, thick * 0.85f, ink, depthTest);
    }
}

static void StickFigure_DrawShoulderPads(const StickFigureData* fig, float thick,
                                         DWORD ink, int wide, int depthTest);
static void StickFigure_DrawBeret(const StickFigureData* fig, float thick,
                                  DWORD ink, int depthTest);

// Jill's costume details (port extension, for the laughs). A stick figure has
// no silhouette, so the two things that read as Jill are drawn explicitly:
// short hair, and two pointed breasts. Everything is
// placed off the head->root axis, which is the figure's own body axis, and
// measured in head-ring radii, so it follows the character around the room and
// keeps its proportions at any camera distance.
static void StickFigure_DrawJillCostume(const StickFigureData* fig, float thick,
                                        const StickCostume& c, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    // Hair and beret are drawn by the caller (see STYLE_JILL) so it uses the
    // same hairline as the rest of the cast; this function adds the vest
    // furniture: the padded chest, the shoulder guards, the blue arm patch, the
    // fingerless gloves and the thigh holster.
    StickFigure_DrawShoulderPads(fig, thick, c.accent, 1, depthTest);

    // S.T.A.R.S. shoulder patch on the upper arm, a small bright block. The
    // two arm chains are 9->10->11 and 12->13->14, so the upper arm is the bone
    // 9->10 / 12->13 and the patch is a dot across its middle.
    for (int a = 0; a < 2; ++a) {
        int up = (a == 0) ? 9 : 12, lo = (a == 0) ? 10 : 13;
        if (!fig->jok[up] || !fig->jok[lo]) continue;
        float mx = (fig->jx[up] + fig->jx[lo]) * 0.5f;
        float my = (fig->jy[up] + fig->jy[lo]) * 0.5f;
        float mxz = (fig->jz[up] + fig->jz[lo]) * 0.5f;
        StickFigure_Dot(mx, my, (int)(hr * 0.42f) + 2, c.accent, mxz,
                        depthTest);
    }

    // Fingerless gloves: the hands are drawn as bare skin by the colour pass
    // (see fingerlessGloves), with a dark knuckle bar over the fingers so they
    // still read as gloved.
    if (c.fingerlessGloves) {
        static const int hand[2] = { 11, 14 };
        static const int wrist[2] = { 10, 13 };
        for (int a = 0; a < 2; ++a) {
            if (!fig->jok[hand[a]] || !fig->jok[wrist[a]]) continue;
            float hxx = fig->jx[hand[a]], hyy = fig->jy[hand[a]];
            float dx = hxx - fig->jx[wrist[a]], dy = hyy - fig->jy[wrist[a]];
            float dl = sqrtf(dx * dx + dy * dy);
            if (dl < 0.5f) { continue; }
            dx /= dl; dy /= dl;
            float qx = -dy, qy = dx;
            float off = hr * 0.55f;
            StickFigure_Line(hxx - dx * off, hyy - dy * off,
                             fig->jz[hand[a]],
                             hxx - dx * off * 0.2f, hyy - dy * off * 0.2f,
                             fig->jz[hand[a]], thick * 0.9f, c.boot, depthTest);
            StickFigure_Dot(hxx - qx * hr * 0.28f, hyy - qy * hr * 0.28f,
                            (int)(thick * 1.4f) + 1, c.boot, fig->jz[hand[a]],
                            depthTest);
        }
    }

    // Thigh holster on one leg: a dark block on the thigh bone 2->3.
    if (fig->jok[2] && fig->jok[3]) {
        float mx = (fig->jx[2] + fig->jx[3]) * 0.5f;
        float my = (fig->jy[2] + fig->jy[3]) * 0.5f;
        float mxz = (fig->jz[2] + fig->jz[3]) * 0.5f;
        StickFigure_Dot(mx, my, (int)(hr * 0.52f) + 2, c.boot, mxz, depthTest);
    }

    // Chest: two long thin cones, high on the chest, starting apart at the top
    // and angling IN toward a point near the middle of the torso, so the tips
    // point down and inward. Drawn as CLOSED triangles with StickFigure_Polygon
    // (a _Polyline would leave the last point unjoined and the triangle would
    // read as a slash).
    //
    // A stick figure has only a screen-space body axis, so the cone is laid out
    // the way simple front-view line art draws it: a wide base across the top of
    // the chest, then sides converging down the body to a point. The previous
    // versions got this wrong twice - first as two horns splayed sideways on the
    // screen-perpendicular, then as short triangles leaning outward. The pair is
    // long (about 1.6 head radii), thin, and leans inward.
    for (int side = -1; side <= 1; side += 2) {
        float s = (float)side;
        float baseY    = 1.10f * hr;    // base sits high, just below the chin
        float baseOut  = 0.42f * hr;    // outer corner of the base, away from the axis
        float baseIn   = 0.04f * hr;    // inner corner, nearly on the axis
        float apexY    = 2.70f * hr;    // the tip reaches about halfway down the torso
        float apexSide = 0.10f * hr;    // tip meets its pair near the centre line
        float pts[6] = {
            // base, outer corner (high, wide)
            hx + px * s * baseOut, hy + py * s * baseOut,
            // apex (low, drawn in toward the centre line)
            hx + px * s * apexSide + ux * apexY,
            hy + py * s * apexSide + uy * apexY,
            // base, inner corner (high, near the axis)
            hx + px * s * baseIn + ux * (baseY - 0.30f * hr),
            hy + py * s * baseIn + uy * (baseY - 0.30f * hr),
        };
        // The outer corner sits a touch lower than the inner one, which is what
        // gives the pair its downward, inward lean.
        pts[0] += ux * (baseY + 0.10f * hr);
        pts[1] += uy * (baseY + 0.10f * hr);
        StickFigure_Polygon(pts, 3, thick * 1.3f, c.vest, vz, depthTest);
    }
}

// Rebecca: a short dark pixie cut with long bangs, a white medic vest with a
// red cross and a red bandanna, over an olive undershirt.
static void StickFigure_DrawRebeccaDetails(const StickFigureData* fig,
                                           float thick, const StickCostume& c,
                                           int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    // The bob HUGS the head. This used to be a semicircle at 1.3 times the head
    // radius, which floated around the skull and read as an afro; it is now a
    // cap just off the head with the length hanging down the sides.
    StickFigure_DrawHair(fig, thick, c.hair, 1.03f, 0.98f, 0.62f, 0.10f, 0,
                         depthTest);

    // Side ponytail.
    float tail[6] = {
        hx + px * hr * 1.02f - ux * hr * 0.42f,
        hy + py * hr * 1.02f - uy * hr * 0.42f,
        hx + px * hr * 1.62f + ux * hr * 0.10f,
        hy + py * hr * 1.62f + uy * hr * 0.10f,
        hx + px * hr * 1.48f + ux * hr * 0.92f,
        hy + py * hr * 1.48f + uy * hr * 0.92f,
    };
    StickFigure_Polyline(tail, 3, thick, c.hair, vz, depthTest);

    StickFigure_DrawHumanFace(fig, thick, c.skin, 0xFF7FB06Eu, c.skin,
                              depthTest, 1);

    // White medic vest furniture: the red cross, the green shoulder trim and
    // the bandanna at the neck.
    StickFigure_DrawFaceAccessory(fig, thick, c.accent, 3, depthTest);
    StickFigure_DrawShoulderPads(fig, thick, 0xFF5E7A44u, 0, depthTest);
}

// Corpse: crossed-out eyes and a flat mouth. The X is blood red, so a body on
// the floor is unmistakably dead rather than asleep.
static void StickFigure_DrawCorpseFace(const StickFigureData* fig, float thick,
                                       DWORD ink, DWORD blood, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;
    float e = hr * 0.28f;

    for (int side = -1; side <= 1; side += 2) {
        float cx = hx + px * (float)side * hr * 0.38f;
        float cy = hy + py * (float)side * hr * 0.38f;
        StickFigure_Line(cx - e, cy - e, vz, cx + e, cy + e, vz,
                         thick * 0.9f, blood, depthTest);
        StickFigure_Line(cx - e, cy + e, vz, cx + e, cy - e, vz,
                         thick * 0.9f, blood, depthTest);
    }

    float pts[4] = {
        hx - px * hr * 0.45f + ux * hr * 0.52f,
        hy - py * hr * 0.45f + uy * hr * 0.52f,
        hx + px * hr * 0.45f + ux * hr * 0.52f,
        hy + py * hr * 0.45f + uy * hr * 0.52f,
    };
    StickFigure_Polyline(pts, 2, thick * 0.8f, ink, vz, depthTest);
}

// Zombie face: mismatched red eyes and a jagged mouth across the lower skull.
static void StickFigure_DrawZombieFace(const StickFigureData* fig, float thick,
                                       DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    float ex = hx - ux * hr * 0.10f;
    float ey = hy - uy * hr * 0.10f;
    int eye = (int)(hr * 0.30f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(ex + px * hr * 0.40f, ey + py * hr * 0.40f, eye + 1, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(ex - px * hr * 0.40f, ey - py * hr * 0.40f, eye, eyeInk,
                    vz, depthTest);

    float mouth[10];
    for (int k = 0; k < 5; ++k) {
        float t = -0.55f + 1.10f * (float)k / 4.0f;
        float dip = (k & 1) ? 0.10f : -0.05f;
        mouth[k * 2]     = hx + px * t * hr + ux * (0.52f + dip) * hr;
        mouth[k * 2 + 1] = hy + py * t * hr + uy * (0.52f + dip) * hr;
    }
    StickFigure_Polyline(mouth, 5, thick, ink, vz, depthTest);
}

// Cerberus details: pointed ears, red eyes, a mouth along the snout and a tail
// curling up from the body root.
static void StickFigure_DrawDogDetails(const StickFigureData* fig, float thick,
                                       DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float rx = fig->rootX, ry = fig->rootY, rz = fig->rootZ;

    // Direction from the root to the head is "forward" (toward the snout).
    float fx = hx - rx, fy = hy - ry;
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl < 0.5f) { fx = 0.0f; fy = -1.0f; fl = 1.0f; }
    fx /= fl; fy /= fl;

    float px = -fy, py = fx;                // across the spine
    for (int side = -1; side <= 1; side += 2) {
        float s = (float)side;
        float bx = hx + px * s * hr * 0.45f - fx * hr * 0.10f;
        float by = hy + py * s * hr * 0.45f - fy * hr * 0.10f;
        float ear[6] = {
            bx, by,
            bx + px * s * hr * 0.30f, by + py * s * hr * 0.30f - 1.15f * hr,
            bx + px * s * hr * 0.55f - fx * hr * 0.25f,
            by + py * s * hr * 0.55f - fy * hr * 0.25f,
        };
        // Closed: a dog's ear has to read as a triangular flap, and an open
        // polyline leaves the base edge missing so it looks like a bent line.
        StickFigure_Polygon(ear, 3, thick, ink, vz, depthTest);
    }

    float ex = hx + fx * hr * 0.32f;
    float ey = hy + fy * hr * 0.32f;
    int eye = (int)(hr * 0.26f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(ex + px * hr * 0.32f, ey + py * hr * 0.32f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(ex - px * hr * 0.32f, ey - py * hr * 0.32f, eye, eyeInk,
                    vz, depthTest);

    // Mouth: a short line along the snout, offset to the lower side.
    float dpx = -fy, dpy = fx;
    if (dpy < 0.0f) { dpx = -dpx; dpy = -dpy; }
    float m0x = hx + fx * hr * 0.75f + dpx * hr * 0.22f;
    float m0y = hy + fy * hr * 0.75f + dpy * hr * 0.22f;
    float m1x = hx + fx * hr * 1.55f + dpx * hr * 0.10f;
    float m1y = hy + fy * hr * 1.55f + dpy * hr * 0.10f;
    StickFigure_Line(m0x, m0y, vz, m1x, m1y, vz, thick * 0.8f, ink, depthTest);

    // Tail: from the body root, away from the head, curling up.
    float tx = rx, ty = ry;
    float pts[8] = {
        tx, ty,
        tx - fx * hr * 0.75f, ty - fy * hr * 0.75f - hr * 0.15f,
        tx - fx * hr * 1.35f, ty - fy * hr * 1.35f - hr * 0.65f,
        tx - fx * hr * 1.55f, ty - fy * hr * 1.55f - hr * 1.35f,
    };
    StickFigure_Polyline(pts, 4, thick, ink, rz, depthTest);

    // Exposed ribs: the bible's cerberus is a "low-slung skeletal dog with
    // exposed ribs". Short arcs on both flanks of the spine, between the
    // shoulder and the hip, so the torso reads as skin over bone.
    {
        float sx = hx + fx * hr * 0.85f;     // shoulder, just behind the head
        float sy = hy + fy * hr * 0.85f;
        for (int k = 0; k < 4; ++k) {
            float t = 0.16f + 0.24f * (float)k;
            float cx = sx + (rx - hx) * t;
            float cy = sy + (ry - hy) * t;
            for (int side = -1; side <= 1; side += 2) {
                float s = (float)side;
                StickFigure_Line(cx - px * s * hr * 0.10f,
                                 cy - py * s * hr * 0.10f, rz,
                                 cx + px * s * hr * 0.52f - fx * hr * 0.18f,
                                 cy + py * s * hr * 0.52f - fy * hr * 0.18f, rz,
                                 thick * 0.7f, ink, depthTest);
            }
        }
    }

    // Paws: each leg ends in a short stub with a toe, so the dog reads as
    // standing on four feet instead of trailing four bare sticks. The cerberus
    // .emd leaves are [4=head, 6, 8, 11, 14], so 11/14 are the front pair and
    // 6/8 the back pair.
    {
        int legs[4] = { 11, 14, 6, 8 };
        for (int i = 0; i < 4; ++i) {
            int tip = legs[i];
            if (tip < 0 || tip >= fig->jointCount || !fig->jok[tip]) continue;
            if ((fig->jflags[tip] & 0x04) != 0) continue;   // blown-off leg
            int par = fig->jparent[tip];
            float legX = fig->jx[tip], legY = fig->jy[tip], legZ = fig->jz[tip];
            float dx = 0.0f, dy = 1.0f;
            if (par >= 0 && par < fig->jointCount && fig->jok[par]) {
                dx = legX - fig->jx[par];
                dy = legY - fig->jy[par];
                float dl = sqrtf(dx * dx + dy * dy);
                if (dl < 0.5f) { dx = 0.0f; dy = 1.0f; }
                else { dx /= dl; dy /= dl; }
            }
            float qx = -dy, qy = dx;
            float paw = hr * 0.55f;
            if (paw < 2.5f) paw = 2.5f;
            StickFigure_Line(legX, legY, legZ,
                             legX + dx * paw + qx * paw * 0.45f,
                             legY + dy * paw + qy * paw * 0.45f, legZ,
                             thick * 0.9f, ink, depthTest);
        }
    }
}

// ---------------------------------------------------------------------------
// Monster faces and silhouettes. Each one is anchored to joints that actually
// exist in that model's skeleton (see the EMD tree survey), so the details stay
// attached to the animation instead of floating near a screen-space guess.
// ---------------------------------------------------------------------------

// Hunter: frog eye bulges, a wide grin, a red forked tongue, and webbed hands
// and feet. The skull is joint 2 and the snout joint 3.
static void StickFigure_DrawHunterDetails(const StickFigureData* fig, float thick,
                                          DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    float fx = 0.0f, fy = -1.0f;
    if (fig->jok[3]) {                       // snout: points the face forward
        fx = fig->jx[3] - hx; fy = fig->jy[3] - hy;
        float fl = sqrtf(fx * fx + fy * fy);
        if (fl > 0.5f) { fx /= fl; fy /= fl; }
        else { fx = 0.0f; fy = -1.0f; }
    }

    // Bulging frog eyes sit high on the skull, one each side of the snout.
    float ex = hx - ux * hr * 0.30f + fx * hr * 0.20f;
    float ey = hy - uy * hr * 0.30f + fy * hr * 0.20f;
    for (int side = -1; side <= 1; side += 2) {
        float s = (float)side;
        float cx = ex + px * s * hr * 0.50f;
        float cy = ey + py * s * hr * 0.50f;
        StickFigure_Ring(cx, cy, hr * 0.30f, 8, thick * 0.9f, ink, vz, depthTest);
        StickFigure_Dot(cx, cy, (int)(hr * 0.20f) + 1, eyeInk, vz, depthTest);
    }

    // Wide grin across the front of the skull, with a red forked tongue.
    float mx = hx + fx * hr * 0.45f + ux * hr * 0.55f;
    float my = hy + fy * hr * 0.45f + uy * hr * 0.55f;
    float grin[8] = {
        mx - px * hr * 0.85f, my - py * hr * 0.85f,
        mx - px * hr * 0.35f, my - py * hr * 0.35f + hr * 0.18f,
        mx + px * hr * 0.35f, my + py * hr * 0.35f + hr * 0.18f,
        mx + px * hr * 0.85f, my + py * hr * 0.85f,
    };
    StickFigure_Polyline(grin, 4, thick, ink, vz, depthTest);

    float tx = mx, ty = my + hr * 0.05f;
    float tongue[8] = {
        tx, ty,
        tx + ux * hr * 0.55f, ty + uy * hr * 0.55f,
        tx + ux * hr * 0.70f - px * hr * 0.25f,
        ty + uy * hr * 0.70f - py * hr * 0.25f,
    };
    StickFigure_Polyline(tongue, 3, thick * 0.8f, eyeInk, vz, depthTest);
    float fork[4] = {
        tx + ux * hr * 0.50f, ty + uy * hr * 0.50f,
        tx + ux * hr * 0.70f + px * hr * 0.25f,
        ty + uy * hr * 0.70f + py * hr * 0.25f,
    };
    StickFigure_Polyline(fork, 2, thick * 0.8f, eyeInk, vz, depthTest);

    // Webbed hands and feet: three short strokes on each limb tip.
    static const int webTip[4]  = { 6, 9, 12, 15 };
    static const int webBase[4] = { 5, 8, 11, 14 };
    for (int i = 0; i < 4; ++i) {
        StickFigure_Fan(fig, webTip[i], webBase[i], hr * 0.55f, hr * 0.45f,
                        3, thick * 0.8f, ink, depthTest);
    }
}

// Tyrant: heavy brow, red eyes, a flat mouth and a giant four-pronged claw on
// each hand. The tyrant's skeleton is the zombie family tree
// (0<--1 1<-0 2<-1 3<-1 4<-3 5<-4 6<-1 7<-6 8<-7 9<-0 10<-9 11<-10
//  12<-0 13<-12 14<-13, leaves [2, 5, 8, 11, 14]): head 2, hub 1, legs
// 3->4->5 and 6->7->8, arms 9->10->11 and 12->13->14. So the hands are 11/14 and
// their forearms 10/13 - NOT 5/8, which are the feet.
static void StickFigure_DrawTyrantDetails(const StickFigureData* fig, float thick,
                                          DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    // The bible's tyrant is "a white blank head with exposed red muscle": the
    // skull is left as the plain ring, and the flesh is the only red on it.
    float brow = hr * 0.62f;
    StickFigure_Line(hx - px * brow - ux * hr * 0.18f,
                     hy - py * brow - uy * hr * 0.18f, vz,
                     hx + px * brow - ux * hr * 0.18f,
                     hy + py * brow - uy * hr * 0.18f, vz,
                     thick * 1.2f, ink, depthTest);
    // Exposed red muscle: a jagged band down one side of the face.
    for (int k = 0; k < 4; ++k) {
        float t0 = (float)k / 4.0f;
        float t1 = (float)(k + 1) / 4.0f;
        float j = ((k & 1) ? 0.16f : -0.10f);
        StickFigure_Line(hx + px * (0.55f + j) * hr - ux * (0.10f + t0 * 1.05f) * hr,
                         hy + py * (0.55f + j) * hr - uy * (0.10f + t0 * 1.05f) * hr,
                         vz,
                         hx + px * (0.55f - j) * hr - ux * (0.10f + t1 * 1.05f) * hr,
                         hy + py * (0.55f - j) * hr - uy * (0.10f + t1 * 1.05f) * hr,
                         vz, thick * 0.9f, eyeInk, depthTest);
    }

    int eye = (int)(hr * 0.22f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx + px * hr * 0.36f + ux * hr * 0.04f,
                    hy + py * hr * 0.36f + uy * hr * 0.04f,
                    eye, eyeInk, vz, depthTest);
    StickFigure_Dot(hx - px * hr * 0.36f + ux * hr * 0.04f,
                    hy - py * hr * 0.36f + uy * hr * 0.04f,
                    eye, eyeInk, vz, depthTest);

    float mouth[4] = {
        hx - px * hr * 0.42f + ux * hr * 0.55f,
        hy - py * hr * 0.42f + uy * hr * 0.55f,
        hx + px * hr * 0.42f + ux * hr * 0.55f,
        hy + py * hr * 0.42f + uy * hr * 0.55f,
    };
    StickFigure_Polyline(mouth, 2, thick, ink, vz, depthTest);

    // The giant four-pronged claw on each hand (hands 11 and 14, forearms
    // 10 and 13), splayed wide and long - the tyrant's tell.
    StickFigure_Fan(fig, 11, 10, hr * 0.95f, hr * 0.60f, 4, thick * 1.1f, ink,
                    depthTest);
    StickFigure_Fan(fig, 14, 13, hr * 0.95f, hr * 0.60f, 4, thick * 1.1f, ink,
                    depthTest);
}

// Chimera: antennae, three red compound eyes and mandibles, plus claws.
static void StickFigure_DrawChimeraDetails(const StickFigureData* fig, float thick,
                                           DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    // Antennae: two jointed stalks off the top of the skull.
    for (int side = -1; side <= 1; side += 2) {
        float s = (float)side;
        float pts[6] = {
            hx + px * s * hr * 0.18f - ux * hr * 0.60f,
            hy + py * s * hr * 0.18f - uy * hr * 0.60f,
            hx + px * s * hr * 0.62f - ux * hr * 1.15f,
            hy + py * s * hr * 0.62f - uy * hr * 1.15f,
            hx + px * s * hr * 0.95f - ux * hr * 1.60f,
            hy + py * s * hr * 0.95f - uy * hr * 1.60f,
        };
        StickFigure_Polyline(pts, 3, thick * 0.9f, ink, vz, depthTest);
        StickFigure_Dot(pts[4], pts[5], (int)(hr * 0.16f) + 1, eyeInk, vz,
                        depthTest);
    }

    // Three compound eyes: one high centre, two lower sides.
    int eye = (int)(hr * 0.20f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx - ux * hr * 0.02f, hy - uy * hr * 0.02f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(hx - ux * hr * 0.20f + px * hr * 0.42f,
                    hy - uy * hr * 0.20f + py * hr * 0.42f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(hx - ux * hr * 0.20f - px * hr * 0.42f,
                    hy - uy * hr * 0.20f - py * hr * 0.42f, eye, eyeInk,
                    vz, depthTest);

    // Mandibles: two short pincers converging below the face.
    float mand[4] = {
        hx - px * hr * 0.45f + ux * hr * 0.45f,
        hy - py * hr * 0.45f + uy * hr * 0.45f,
        hx - px * hr * 0.12f + ux * hr * 0.85f,
        hy - py * hr * 0.12f + uy * hr * 0.85f,
    };
    StickFigure_Polyline(mand, 2, thick, ink, vz, depthTest);
    float mand2[4] = {
        hx + px * hr * 0.45f + ux * hr * 0.45f,
        hy + py * hr * 0.45f + uy * hr * 0.45f,
        hx + px * hr * 0.12f + ux * hr * 0.85f,
        hy + py * hr * 0.12f + uy * hr * 0.85f,
    };
    StickFigure_Polyline(mand2, 2, thick, ink, vz, depthTest);

    StickFigure_Fan(fig, 6, 5, hr * 0.50f, hr * 0.35f, 3, thick * 0.8f, ink,
                    depthTest);
    StickFigure_Fan(fig, 10, 9, hr * 0.50f, hr * 0.35f, 3, thick * 0.8f, ink,
                    depthTest);
}

// Crow: a pointed beak, red eyes and feather fans at the wing tips.
static void StickFigure_DrawCrowDetails(const StickFigureData* fig, float thick,
                                        DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    float fx = 1.0f, fy = 0.0f;
    if (fig->jok[2]) {
        fx = fig->jx[2] - hx; fy = fig->jy[2] - hy;
        float fl = sqrtf(fx * fx + fy * fy);
        if (fl > 0.5f) { fx /= fl; fy /= fl; }
    }

    float tipx = hx + fx * hr * 1.45f;
    float tipy = hy + fy * hr * 1.45f;
    StickFigure_Line(hx - px * hr * 0.28f, hy - py * hr * 0.28f, vz,
                     tipx, tipy, vz, thick, ink, depthTest);
    StickFigure_Line(hx + px * hr * 0.28f, hy + py * hr * 0.28f, vz,
                     tipx, tipy, vz, thick, ink, depthTest);

    int eye = (int)(hr * 0.16f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx + px * hr * 0.32f + fx * hr * 0.10f,
                    hy + py * hr * 0.32f + fy * hr * 0.10f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(hx - px * hr * 0.32f + fx * hr * 0.10f,
                    hy - py * hr * 0.32f + fy * hr * 0.10f, eye, eyeInk,
                    vz, depthTest);

    StickFigure_Fan(fig, 5, 4, hr * 1.10f, hr * 0.75f, 4, thick * 0.8f, ink,
                    depthTest);
    StickFigure_Fan(fig, 8, 7, hr * 1.10f, hr * 0.75f, 4, thick * 0.8f, ink,
                    depthTest);
}

// Wasp: antennae, red eyes and a stinger opposite the head.
static void StickFigure_DrawWaspDetails(const StickFigureData* fig, float thick,
                                        DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float rx = fig->rootX, ry = fig->rootY;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    float fx = hx - rx, fy = hy - ry;
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl < 0.5f) { fx = 0.0f; fy = -1.0f; fl = 1.0f; }
    fx /= fl; fy /= fl;

    for (int side = -1; side <= 1; side += 2) {
        float s = (float)side;
        float pts[6] = {
            hx + px * s * hr * 0.25f - ux * hr * 0.30f,
            hy + py * s * hr * 0.25f - uy * hr * 0.30f,
            hx + px * s * hr * 0.65f - ux * hr * 1.05f,
            hy + py * s * hr * 0.65f - uy * hr * 1.05f,
            hx + px * s * hr * 0.90f - ux * hr * 1.55f,
            hy + py * s * hr * 0.90f - uy * hr * 1.55f,
        };
        StickFigure_Polyline(pts, 3, thick * 0.8f, ink, vz, depthTest);
    }

    int eye = (int)(hr * 0.22f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx + px * hr * 0.38f + fx * hr * 0.10f,
                    hy + py * hr * 0.38f + fy * hr * 0.10f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(hx - px * hr * 0.38f + fx * hr * 0.10f,
                    hy - py * hr * 0.38f + fy * hr * 0.10f, eye, eyeInk,
                    vz, depthTest);

    // Stinger: a tapered spike pointing away from the head.
    float sx = rx - fx * hr * 0.25f, sy = ry - fy * hr * 0.25f;
    float tx = rx - fx * hr * 1.55f, ty = ry - fy * hr * 1.55f;
    StickFigure_Line(sx - px * hr * 0.22f, sy - py * hr * 0.22f, vz,
                     tx, ty, vz, thick * 0.9f, ink, depthTest);
    StickFigure_Line(sx + px * hr * 0.22f, sy + py * hr * 0.22f, vz,
                     tx, ty, vz, thick * 0.9f, ink, depthTest);
}

// Spider: a six-eye cluster, fangs and a big abdomen behind the root.
static void StickFigure_DrawSpiderDetails(const StickFigureData* fig, float thick,
                                          DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float rx = fig->rootX, ry = fig->rootY, rz = fig->rootZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    // The head is at the front; the abdomen sits behind the root.
    float bx = rx - hx, by = ry - hy;
    float bl = sqrtf(bx * bx + by * by);
    if (bl > 0.5f) {
        bx /= bl; by /= bl;
        float cx = rx + bx * bl * 0.38f;
        float cy = ry + by * bl * 0.38f;
        float ar = bl * 0.30f;
        StickFigure_Ring(cx, cy, ar, 10, thick, ink, rz, depthTest);
        StickFigure_Line(cx - ar * 0.55f, cy, rz, cx + ar * 0.55f, cy, rz,
                         thick * 0.7f, ink, depthTest);
        // The web spinner is the fuzzy one of the two spiders: a bristle halo
        // around the abdomen, so its silhouette is broken the way fur breaks a
        // pen outline. Skip every other angle so it reads as hair, not a sun.
        for (int k = 0; k < 12; ++k) {
            float a = (float)k * 0.5236f;        // 30 degrees
            float c = cosf(a), s = sinf(a);
            float ia = ar * 1.02f;
            float oa = ar * (1.10f + 0.34f * (float)(k & 1));
            StickFigure_Line(cx + c * ia, cy + s * ia, rz,
                             cx + c * oa, cy + s * oa, rz,
                             thick * 0.55f, ink, depthTest);
        }
    }

    // Fangs point forward from the head.
    float fx = hx - rx, fy = hy - ry;
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl < 0.5f) { fx = 1.0f; fy = 0.0f; }
    else { fx /= fl; fy /= fl; }

    float f0x = hx + fx * hr * 0.15f;
    float f0y = hy + fy * hr * 0.15f;
    StickFigure_Line(f0x + px * hr * 0.35f, f0y + py * hr * 0.35f, vz,
                     hx + fx * hr * 1.20f + px * hr * 0.12f,
                     hy + fy * hr * 1.20f + py * hr * 0.12f, vz,
                     thick * 0.9f, ink, depthTest);
    StickFigure_Line(f0x - px * hr * 0.35f, f0y - py * hr * 0.35f, vz,
                     hx + fx * hr * 1.20f - px * hr * 0.12f,
                     hy + fy * hr * 1.20f - py * hr * 0.12f, vz,
                     thick * 0.9f, ink, depthTest);

    // Six eyes in three pairs across the top of the head.
    int eye = (int)(hr * 0.16f);
    if (eye < 2) eye = 2;
    for (int row = 0; row < 3; ++row) {
        float o = (float)(row - 1) * hr * 0.30f;
        StickFigure_Dot(hx - fx * hr * 0.15f + px * hr * 0.24f + fx * o,
                        hy - fy * hr * 0.15f + py * hr * 0.24f + fy * o,
                        eye, eyeInk, vz, depthTest);
        StickFigure_Dot(hx - fx * hr * 0.15f - px * hr * 0.24f + fx * o,
                        hy - fy * hr * 0.15f - py * hr * 0.24f + fy * o,
                        eye, eyeInk, vz, depthTest);
    }
}

// Snake / Yawn: a wedge skull, red eyes, a forked tongue, and a zigzag of
// belly scales down the chain. `forward` points from the tail to the head.
static void StickFigure_DrawSnakeDetails(const StickFigureData* fig, float thick,
                                         DWORD ink, DWORD eyeInk, int depthTest)
{
    float hr = fig->headR;
    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float px = 0.0f, py = 1.0f;

    float fx = hx - fig->footX, fy = hy - fig->footY;
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl > 0.5f) { fx /= fl; fy /= fl; }
    else { fx = 1.0f; fy = 0.0f; }
    px = -fy; py = fx;

    // The skull is a wedge even when headR is 0: derive its size from a
    // nominal fraction of the snake's projected length.
    float size = (fig->bodyLen > 2.0f) ? fig->bodyLen * 0.055f : 3.0f;
    if (size < 2.0f) size = 2.0f;

    float tipx = hx + fx * size * 0.80f;
    float tipy = hy + fy * size * 0.80f;
    float backx = hx - fx * size * 0.95f;
    float backy = hy - fy * size * 0.95f;
    StickFigure_Line(tipx, tipy, vz, backx - px * size * 0.62f,
                     backy - py * size * 0.62f, vz, thick, ink, depthTest);
    StickFigure_Line(tipx, tipy, vz, backx + px * size * 0.62f,
                     backy + py * size * 0.62f, vz, thick, ink, depthTest);
    StickFigure_Line(backx - px * size * 0.62f, backy - py * size * 0.62f, vz,
                     backx + px * size * 0.62f, backy + py * size * 0.62f, vz,
                     thick * 0.8f, ink, depthTest);

    int eye = (int)(size * 0.30f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx + fx * size * 0.10f + px * size * 0.38f,
                    hy + fy * size * 0.10f + py * size * 0.38f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(hx + fx * size * 0.10f - px * size * 0.38f,
                    hy + fy * size * 0.10f - py * size * 0.38f, eye, eyeInk,
                    vz, depthTest);

    // Forked tongue.
    float tx = tipx, ty = tipy;
    StickFigure_Line(tx, ty, vz, tx + fx * size * 0.85f,
                     ty + fy * size * 0.85f, vz, thick * 0.8f, eyeInk,
                     depthTest);
    StickFigure_Line(tx + fx * size * 0.70f, ty + fy * size * 0.70f, vz,
                     tx + fx * size * 1.00f + px * size * 0.22f,
                     ty + fy * size * 1.00f + py * size * 0.22f, vz,
                     thick * 0.7f, eyeInk, depthTest);
    StickFigure_Line(tx + fx * size * 0.70f, ty + fy * size * 0.70f, vz,
                     tx + fx * size * 1.00f - px * size * 0.22f,
                     ty + fy * size * 1.00f - py * size * 0.22f, vz,
                     thick * 0.7f, eyeInk, depthTest);

    // The bible calls Yawn (and the adder) out for a body of X-hatching: the
    // scales are drawn as an X per chain link rather than a single belly bar,
    // so the length of the snake carries its own hatching instead of relying on
    // the shader's tone pass. Two diagonals per step, offset along the chain so
    // the X's tile rather than pile up on one spot.
    for (int i = 0; i < fig->segmentCount; ++i) {
        const StickSegment* s = &fig->segments[i];
        float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
        float dl = sqrtf(dx * dx + dy * dy);
        if (dl < 1.0f) continue;
        dx /= dl; dy /= dl;
        float qx = -dy, qy = dx;
        float q = (dl < size * 0.8f) ? dl * 0.40f : size * 0.30f;
        float step = dl * 0.5f;
        for (int k = 0; k < 2; ++k) {
            float t = 0.30f + 0.40f * (float)k;
            float mx = s->x0 + dx * dl * t;
            float my = s->y0 + dy * dl * t;
            // One arm of the X leans forward, the other back.
            StickFigure_Line(mx - qx * q - dx * step * 0.5f,
                             my - qy * q - dy * step * 0.5f, s->vz1,
                             mx + qx * q + dx * step * 0.5f,
                             my + qy * q + dy * step * 0.5f, s->vz1,
                             thick * 0.6f, ink, depthTest);
            StickFigure_Line(mx - qx * q + dx * step * 0.5f,
                             my - qy * q + dy * step * 0.5f, s->vz1,
                             mx + qx * q - dx * step * 0.5f,
                             my + qy * q - dy * step * 0.5f, s->vz1,
                             thick * 0.6f, ink, depthTest);
        }
    }
}

// Neptune / shark: a broad wedge skull, zigzag teeth, red eyes and gill slits.
static void StickFigure_DrawSharkDetails(const StickFigureData* fig, float thick,
                                         DWORD ink, DWORD eyeInk, int depthTest)
{
    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;

    float fx = hx - fig->footX, fy = hy - fig->footY;
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl > 0.5f) { fx /= fl; fy /= fl; }
    else { fx = 1.0f; fy = 0.0f; }
    float px = -fy, py = fx;

    float size = (fig->bodyLen > 2.0f) ? fig->bodyLen * 0.075f : 4.0f;
    if (size < 3.0f) size = 3.0f;

    float tipx = hx + fx * size * 0.55f;
    float tipy = hy + fy * size * 0.55f;
    float backx = hx - fx * size * 1.00f;
    float backy = hy - fy * size * 1.00f;
    float halfw = size * 1.05f;
    StickFigure_Line(tipx, tipy, vz, backx - px * halfw,
                     backy - py * halfw, vz, thick, ink, depthTest);
    StickFigure_Line(tipx, tipy, vz, backx + px * halfw,
                     backy + py * halfw, vz, thick, ink, depthTest);
    StickFigure_Line(backx - px * halfw, backy - py * halfw, vz,
                     backx + px * halfw, backy + py * halfw, vz,
                     thick * 0.8f, ink, depthTest);

    // Teeth: a zigzag across the mouth line.
    float mx = hx + fx * size * 0.10f;
    float my = hy + fy * size * 0.10f;
    float zx = mx - px * halfw * 0.82f, zy = my - py * halfw * 0.82f;
    for (int k = 0; k < 6; ++k) {
        float t0 = (float)k / 6.0f;
        float t1 = ((float)k + 0.5f) / 6.0f;
        float t2 = ((float)k + 1.0f) / 6.0f;
        float ax = zx + px * halfw * 1.64f * t0;
        float ay = zy + py * halfw * 1.64f * t0;
        float bx = zx + px * halfw * 1.64f * t1 + fx * size * 0.18f;
        float by = zy + py * halfw * 1.64f * t1 + fy * size * 0.18f;
        float cx = zx + px * halfw * 1.64f * t2;
        float cy = zy + py * halfw * 1.64f * t2;
        StickFigure_Line(ax, ay, vz, bx, by, vz, thick * 0.7f, ink, depthTest);
        StickFigure_Line(bx, by, vz, cx, cy, vz, thick * 0.7f, ink, depthTest);
    }

    int eye = (int)(size * 0.20f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx + fx * size * 0.05f + px * size * 0.45f,
                    hy + fy * size * 0.05f + py * size * 0.45f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(hx + fx * size * 0.05f - px * size * 0.45f,
                    hy + fy * size * 0.05f - py * size * 0.45f, eye, eyeInk,
                    vz, depthTest);

    // Gill slits behind the head.
    for (int k = 0; k < 3; ++k) {
        float t = 0.35f + 0.20f * (float)k;
        for (int side = -1; side <= 1; side += 2) {
            float s = (float)side;
            StickFigure_Line(hx - fx * size * t + px * s * size * 0.55f,
                             hy - fy * size * t + py * s * size * 0.55f, vz,
                             hx - fx * size * t + px * s * size * 0.92f,
                             hy - fy * size * t + py * s * size * 0.92f, vz,
                             thick * 0.7f, ink, depthTest);
        }
    }
}

// Plant 42 / monster plant: thorns down the chain and a maw with red eyes at
// the tip. The tip is the head joint, so `forward` points out of the plant.
static void StickFigure_DrawVineDetails(const StickFigureData* fig, float thick,
                                        DWORD ink, DWORD eyeInk, int depthTest)
{
    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;

    float fx = hx - fig->footX, fy = hy - fig->footY;
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl > 0.5f) { fx /= fl; fy /= fl; }
    else { fx = 0.0f; fy = -1.0f; }
    float px = -fy, py = fx;

    float maw = (fig->bodyLen > 2.0f)
              ? fig->bodyLen / (float)(fig->jointCount > 1 ? fig->jointCount - 1 : 1)
              : 3.0f;
    maw *= 0.75f;
    if (maw < 2.5f) maw = 2.5f;
    if (maw > 10.0f) maw = 10.0f;

    // Open maw: a leaf-shaped outline with jagged lip teeth.
    float frontx = hx + fx * maw * 0.70f;
    float fronty = hy + fy * maw * 0.70f;
    float backx = hx - fx * maw * 1.10f;
    float backy = hy - fy * maw * 1.10f;
    StickFigure_Line(frontx, fronty, vz, backx - px * maw * 0.85f,
                     backy - py * maw * 0.85f, vz, thick, ink, depthTest);
    StickFigure_Line(frontx, fronty, vz, backx + px * maw * 0.85f,
                     backy + py * maw * 0.85f, vz, thick, ink, depthTest);
    StickFigure_Line(backx - px * maw * 0.85f, backy - py * maw * 0.85f, vz,
                     backx + px * maw * 0.85f, backy + py * maw * 0.85f, vz,
                     thick * 0.8f, ink, depthTest);

    for (int side = -1; side <= 1; side += 2) {
        float s = (float)side;
        float ax = backx + px * s * maw * 0.45f;
        float ay = backy + py * s * maw * 0.45f;
        float bx = hx + fx * maw * 0.10f + px * s * maw * 0.16f;
        float by = hy + fy * maw * 0.10f + py * s * maw * 0.16f;
        StickFigure_Line(ax, ay, vz, bx, by, vz, thick * 0.8f, ink, depthTest);
    }

    int eye = (int)(maw * 0.25f);
    if (eye < 2) eye = 2;
    StickFigure_Dot(hx - fx * maw * 0.35f + px * maw * 0.32f,
                    hy - fy * maw * 0.35f + py * maw * 0.32f, eye, eyeInk,
                    vz, depthTest);
    StickFigure_Dot(hx - fx * maw * 0.35f - px * maw * 0.32f,
                    hy - fy * maw * 0.35f - py * maw * 0.32f, eye, eyeInk,
                    vz, depthTest);

    // Thorns: one per chain segment, alternating sides.
    for (int i = 0; i < fig->segmentCount; ++i) {
        const StickSegment* s = &fig->segments[i];
        float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
        float dl = sqrtf(dx * dx + dy * dy);
        if (dl < 1.5f) continue;
        dx /= dl; dy /= dl;
        float qx = -dy, qy = dx;
        float side = (i & 1) ? 1.0f : -1.0f;
        float mx = (s->x0 + s->x1) * 0.5f;
        float my = (s->y0 + s->y1) * 0.5f;
        float thorn = dl * 0.22f;
        StickFigure_Line(mx, my, s->vz1,
                         mx + qx * side * thorn, my + qy * side * thorn,
                         s->vz1, thick * 0.8f, ink, depthTest);
    }

    // Plant 42 (port extension): the bible's version is a bulbous body with
    // lashing tentacles, not just a maw on a stalk. Behind the maw the root
    // bulges into a heavy bulb, and a few tendrils curl out of the bulb in
    // pen strokes so the plant reads as rooted, not as another animal head.
    float rr = maw * 0.9f;
    StickFigure_Ring(fig->rootX, fig->rootY, rr * 0.85f, 12, thick * 1.1f, ink,
                     fig->rootZ, depthTest);
    for (int side = -1; side <= 1; side += 2) {
        float s = (float)side;
        for (int k = 0; k < 3; ++k) {
            float t = 0.25f + 0.32f * (float)k;
            float ox = fig->rootX + px * s * rr * 0.85f;
            float oy = fig->rootY + py * s * rr * 0.85f;
            float e1x = ox + px * s * rr * (0.9f + t) - fx * rr * (0.3f + t);
            float e1y = oy + py * s * rr * (0.9f + t) - fy * rr * (0.3f + t);
            float e2x = e1x + px * s * rr * 0.5f + fx * rr * 0.6f;
            float e2y = e1y + py * s * rr * 0.5f + fy * rr * 0.6f;
            float pts[6] = { ox, oy, e1x, e1y, e2x, e2y };
            StickFigure_Polyline(pts, 3, thick * 0.75f, ink, fig->rootZ,
                                 depthTest);
        }
    }
}

// The weapon the player has equipped, held in the right hand. The hand is
// joint 14 in every player/NPC human skeleton, and the forearm (13 -> 14) is
// the natural "point it where the arm points" axis.
static void StickFigure_DrawWeapon(const StickFigureData* fig, float thick,
                                   DWORD ink, DWORD metal, int depthTest)
{
    const int hand = 14, elbow = 13;
    if (!fig->jok[hand] || !fig->jok[elbow]) return;

    // Which weapon is in this figure's hand.
    //
    // The player uses whatever is actually equipped. Everyone else has no
    // weapon data at all - the game only ever stores the player's three slots -
    // so each named character gets the weapon they carry in RE1. This used to
    // `return` immediately for anyone who was not the player, which is why
    // Barry and the rest stood there empty handed.
    unsigned char w = 0;
    if (fig->isPlayer) {
        w = (unsigned char)g_playerEntity.equippedWeaponId;
        if (w == 0) return;                 // nothing equipped, nothing drawn
    } else {
        switch (fig->style) {
        case STYLE_BARRY:   w = 5;  break;  // .44 Magnum
        case STYLE_RICHARD: w = 3;  break;  // shotgun
        case STYLE_WESKER:  w = 12; break;  // handgun
        case STYLE_ENRICO:  w = 12; break;  // handgun
        case STYLE_REBECCA: w = 12; break;  // handgun
        case STYLE_CHRIS:   w = 3;  break;  // pump shotgun (the bible's weapon)
        case STYLE_BRAD:    w = 12; break;  // handgun
        case STYLE_JOSEPH:  w = 3;  break;  // assault rifle, drawn as the long gun
        case STYLE_HUMAN:   w = 12; break;  // unnamed actors: a handgun
        default:            return;         // monsters and bodies carry nothing
        }
    }

    float hx = fig->jx[hand], hy = fig->jy[hand], hz = fig->jz[hand];
    float fx = hx - fig->jx[elbow];
    float fy = hy - fig->jy[elbow];
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl < 0.5f) { fx = 0.0f; fy = 1.0f; }
    else { fx /= fl; fy /= fl; }
    float px = -fy, py = fx;

    float u = (fig->headR > 0.0f) ? fig->headR : 6.0f;
    float len, wid;
    int kind;
    switch (w) {
    case 1:  kind = 1; len = 1.55f * u; wid = thick * 0.8f;  break; // knife
    case 3:  kind = 2; len = 4.20f * u; wid = thick * 1.3f;  break; // shotgun
    case 5:  kind = 3; len = 2.70f * u; wid = thick * 1.1f;  break; // magnum
    case 6:  kind = 4; len = 3.60f * u; wid = thick * 1.6f;  break; // flamethrower
    case 7:  case 8: case 9:
             kind = 2; len = 2.80f * u; wid = thick * 1.8f;  break; // launcher
    case 10: kind = 2; len = 4.60f * u; wid = thick * 1.9f;  break; // rocket
    default: kind = 0; len = ((w > 0x6e) ? 2.90f : 1.75f) * u;
             wid = thick * 1.1f; break;                            // handgun
    }
    if (len < 3.0f) len = 3.0f;

    float tipx = hx + fx * len, tipy = hy + fy * len;

    switch (kind) {
    case 1: {
        // Knife: short dark handle, long bright-ish blade, small guard.
        StickFigure_Line(hx - fx * u * 0.55f, hy - fy * u * 0.55f, hz,
                         hx, hy, hz, wid * 1.6f, metal, depthTest);
        StickFigure_Line(hx, hy, hz, tipx, tipy, hz, wid * 0.75f, ink,
                         depthTest);
        StickFigure_Line(hx - px * u * 0.35f, hy - py * u * 0.35f, hz,
                         hx + px * u * 0.35f, hy + py * u * 0.35f, hz,
                         wid, metal, depthTest);
        break;
    }
    case 2: {
        // Long gun: barrel, stock and a muzzle band.
        StickFigure_Line(hx - fx * u * 0.90f, hy - fy * u * 0.90f, hz,
                         tipx, tipy, hz, wid, metal, depthTest);
        StickFigure_Line(hx - fx * u * 0.20f + px * u * 0.22f,
                         hy - fy * u * 0.20f + py * u * 0.22f, hz,
                         hx + fx * len * 0.72f + px * u * 0.22f,
                         hy + fy * len * 0.72f + py * u * 0.22f, hz,
                         wid * 0.55f, ink, depthTest);
        StickFigure_Line(tipx - px * u * 0.28f, tipy - py * u * 0.28f, hz,
                         tipx + px * u * 0.28f, tipy + py * u * 0.28f, hz,
                         wid * 0.8f, ink, depthTest);
        break;
    }
    case 3: {
        // Magnum: barrel, cylinder and grip.
        StickFigure_Line(hx, hy, hz, tipx, tipy, hz, wid, metal, depthTest);
        StickFigure_Ring(hx + fx * u * 0.55f, hy + fy * u * 0.55f,
                         u * 0.30f, 8, wid * 0.7f, metal, hz, depthTest);
        StickFigure_Line(hx + fx * u * 0.10f + fig->downX * u * 0.15f,
                         hy + fy * u * 0.10f + fig->downY * u * 0.15f, hz,
                         hx + fig->downX * u * 0.95f,
                         hy + fig->downY * u * 0.95f, hz,
                         wid, metal, depthTest);
        break;
    }
    case 4: {
        // Flamethrower: thick tube, tank and flared nozzle.
        StickFigure_Line(hx, hy, hz, tipx, tipy, hz, wid, metal, depthTest);
        StickFigure_Ring(hx - fx * u * 0.45f, hy - fy * u * 0.45f,
                         u * 0.55f, 10, wid * 0.8f, metal, hz, depthTest);
        StickFigure_Line(tipx - px * u * 0.42f, tipy - py * u * 0.42f, hz,
                         tipx + px * u * 0.42f, tipy + py * u * 0.42f, hz,
                         wid * 1.0f, metal, depthTest);
        break;
    }
    default: {
        // Handgun: slide, short barrel and a grip down the body axis.
        StickFigure_Line(hx, hy, hz, tipx, tipy, hz, wid, metal, depthTest);
        StickFigure_Line(hx + fx * u * 0.35f + px * u * 0.10f,
                         hy + fy * u * 0.35f + py * u * 0.10f, hz,
                         tipx + px * u * 0.06f, tipy + py * u * 0.06f, hz,
                         wid * 0.45f, ink, depthTest);
        StickFigure_Line(hx + fx * u * 0.12f + fig->downX * u * 0.10f,
                         hy + fy * u * 0.12f + fig->downY * u * 0.10f, hz,
                         hx + fx * u * 0.02f + fig->downX * u * 0.85f,
                         hy + fy * u * 0.02f + fig->downY * u * 0.85f, hz,
                         wid * 0.85f, metal, depthTest);
        break;
    }
    }
}

// Joint indices of the two arm and two leg tips, per skeleton. These are read
// from the shipped .emd models with tools/dump_emd_skeleton.py, not guessed.
//
// Every humanoid skeleton (chris/jill/barry/rebecca and the zombie/tyrant
// family) is the same shape:
//
//   human:             zombie:            tips
//     0 root             0 root
//     ├ 1 head           ├ 1 hub
//     ├ 2 ─ 3 ─ 4 ─ 5    │  ├ 2 head      leg  5
//     │   └ 6 ─ 7 ─ 8    │  ├ 3─4─5       leg  8
//     ├ 9 ─10 ─11        │  └ 6─7─8       arm 11
//     └12 ─13 ─14        ├ 9─10─11        arm 14
//                        └12─13─14
//
// The arm and leg tips are the SAME numbered joints in both families; only the
// head differs (joint 1 for the humans, joint 2 for the zombie family, because
// the zombie hangs the head off a neck hub).
static bool StickFigure_LimbTips(int style, int* rArm, int* lArm,
                                 int* rLeg, int* lLeg)
{
    switch (style) {
    case STYLE_ZOMBIE:
    case STYLE_TYRANT:
        *rArm = 11; *lArm = 14; *rLeg = 5; *lLeg = 8; return true;
    case STYLE_HUMAN: case STYLE_JILL: case STYLE_REBECCA:
    case STYLE_CHRIS: case STYLE_BARRY: case STYLE_WESKER:
    case STYLE_RICHARD: case STYLE_ENRICO: case STYLE_CORPSE:
        *rArm = 11; *lArm = 14; *rLeg = 5; *lLeg = 8; return true;
    case STYLE_HUNTER:
        *rArm = 12; *lArm = 15; *rLeg = 6; *lLeg = 9; return true;
    case STYLE_CHIMERA:
        *rArm = 13; *lArm = 16; *rLeg = 6; *lLeg = 10; return true;
    default:
        *rArm = -1; *lArm = -1; *rLeg = -1; *lLeg = -1; return false;
    }
}

static bool StickFigure_HumanoidStyle(int style)
{
    switch (style) {
    case STYLE_HUMAN: case STYLE_JILL: case STYLE_REBECCA:
    case STYLE_CHRIS: case STYLE_BARRY: case STYLE_WESKER:
    case STYLE_RICHARD: case STYLE_ENRICO: case STYLE_CORPSE:
    case STYLE_ZOMBIE: case STYLE_TYRANT:
    case STYLE_HUNTER: case STYLE_CHIMERA:
        return true;
    default:
        return false;
    }
}

// Necks, hands and feet. A bare stick figure reads as a bundle of sticks, so
// this pass gives each humanoid the three joins the eye looks for: a short neck
// under the skull, a hand with fingers at the end of each arm, and a foot at the
// end of each leg. Everything is measured in head-ring radii in the figure's own
// screen frame, so it holds up at any camera distance.
static void StickFigure_DrawExtremities(const StickFigureData* fig, float thick,
                                        DWORD ink, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;
    int style = fig->style;

    // Hands, feet and the neck carry the costume too, so the extremities need
    // the same palette the bones got. `handInk` is bare skin (or a glove), and
    // `bootInk` is the boot colour the feet already get from the bone pass.
    StickCostume extCostume = StickFigure_CostumeOf(style);
    bool extDressed = StickFigure_CostumedHuman(style);
    DWORD handInk = extDressed ? StickFigure_InkForPart(&extCostume, PART_HAND)
                               : ink;
    DWORD footInk = extDressed ? extCostume.boot : ink;
    if (handInk == 0) handInk = ink;
    if (footInk == 0) footInk = ink;

    // ---- Neck ----
    // A collar under the chin: two strokes meeting on the body axis, so the
    // skull does not sit straight on the shoulders. Skipped when headless,
    // because there is no head to carry.
    if (!fig->headless) {
        float ny = 1.02f * hr;
        float halfW = 0.42f * hr;
        StickFigure_Line(fig->headX + px * halfW, fig->headY + py * halfW,
                         fig->headZ,
                         fig->headX + ux * ny, fig->headY + uy * ny,
                         fig->headZ, thick * 0.8f, ink, depthTest);
        StickFigure_Line(fig->headX - px * halfW, fig->headY - py * halfW,
                         fig->headZ,
                         fig->headX + ux * ny, fig->headY + uy * ny,
                         fig->headZ, thick * 0.8f, ink, depthTest);

        // Monsters and NPCs hang the head off a neck joint, so also bridge from
        // that joint to the skull for a real neck. The player's head hangs off
        // the root, so the collar alone is his neck.
        if (!fig->isPlayer) {
            int hp = -1;
            for (int i = 0; i < fig->jointCount; ++i) {
                if (!fig->jok[i]) continue;
                if (fabsf(fig->jx[i] - fig->headX) < 0.5f &&
                    fabsf(fig->jy[i] - fig->headY) < 0.5f) { hp = i; break; }
            }
            if (hp >= 0) {
                int par = fig->jparent[hp];
                if (par >= 0 && par < fig->jointCount && fig->jok[par]) {
                    StickFigure_Line(fig->jx[par], fig->jy[par], fig->jz[par],
                                     fig->headX - ux * hr * 0.72f,
                                     fig->headY - uy * hr * 0.72f, fig->headZ,
                                     thick * 0.85f, ink, depthTest);
                }
            }
        }
    }

    if (!StickFigure_HumanoidStyle(style)) return;

    int rArm, lArm, rLeg, lLeg;
    if (!StickFigure_LimbTips(style, &rArm, &lArm, &rLeg, &lLeg)) return;

    // ---- Hands ----
    // Three short finger strokes fanned off each arm tip, plus a palm bar. This
    // is what tells the viewer which end of the arm is the business end.
    {
        int tips[2] = { rArm, lArm };
        for (int a = 0; a < 2; ++a) {
            int tip = tips[a];
            if (tip < 0 || tip >= fig->jointCount || !fig->jok[tip]) continue;
            int par = fig->jparent[tip];
            float hx = fig->jx[tip], hy = fig->jy[tip], hz = fig->jz[tip];
            float dx = ux, dy = uy;
            if (par >= 0 && par < fig->jointCount && fig->jok[par]) {
                dx = hx - fig->jx[par];
                dy = hy - fig->jy[par];
                float dl = sqrtf(dx * dx + dy * dy);
                if (dl < 0.5f) { dx = ux; dy = uy; }
                else { dx /= dl; dy /= dl; }
            }
            float qx = -dy, qy = dx;
            float flen = hr * 0.55f;
            if (flen < 2.0f) flen = 2.0f;
            for (int k = -1; k <= 1; ++k) {
                float t = (float)k * 0.5f;
                StickFigure_Line(hx, hy, hz,
                                 hx + dx * flen + qx * t * flen * 0.9f,
                                 hy + dy * flen + qy * t * flen * 0.9f, hz,
                                 thick * 0.7f, handInk, depthTest);
            }
            StickFigure_Line(hx - qx * hr * 0.30f, hy - qy * hr * 0.30f, hz,
                             hx + qx * hr * 0.30f, hy + qy * hr * 0.30f, hz,
                             thick * 0.7f, handInk, depthTest);
        }
    }

    // ---- Feet ----
    // A foot continues the leg: the toe runs on past the ankle in the same
    // direction the shin was going, then flares a little. The heel is a short
    // stub back the other way. Pointing the toe perpendicular to the shin (what
    // this used to do) stuck it out sideways, which read as a backwards foot.
    {
        int tips[2] = { rLeg, lLeg };
        for (int a = 0; a < 2; ++a) {
            int tip = tips[a];
            if (tip < 0 || tip >= fig->jointCount || !fig->jok[tip]) continue;
            int par = fig->jparent[tip];
            float tx = fig->jx[tip], ty = fig->jy[tip], tz = fig->jz[tip];
            float dx = ux, dy = uy;   // leg direction: shin, or down the body
            if (par >= 0 && par < fig->jointCount && fig->jok[par]) {
                dx = tx - fig->jx[par];
                dy = ty - fig->jy[par];
                float dl = sqrtf(dx * dx + dy * dy);
                if (dl < 0.5f) { dx = ux; dy = uy; }
                else { dx /= dl; dy /= dl; }
            }
            // The toe runs on in the shin direction, angled slightly outward so
            // the two feet splay apart instead of pointing at each other.
            float outward = (a == 0) ? 1.0f : -1.0f;
            float qx = -dy, qy = dx;
            float footLen = hr * 0.95f;
            if (footLen < 3.0f) footLen = 3.0f;
            float toeX = tx + dx * footLen + qx * outward * footLen * 0.30f;
            float toeY = ty + dy * footLen + qy * outward * footLen * 0.30f;
            StickFigure_Line(tx, ty, tz, toeX, toeY, tz,
                             thick * 0.9f, footInk, depthTest);
            // Heel: a short nub back up the shin, so the foot has two ends.
            StickFigure_Dot(tx - dx * thick * 0.8f, ty - dy * thick * 0.8f,
                            (int)(thick * 0.8f) + 1, footInk, tz, depthTest);
        }
    }
}

// ---------------------------------------------------------------------------
// Shoulder pads. They are drawn across the top of each upper arm,
// perpendicular to it, so they stick out horizontally the way the costume
// bible describes them, and they are sized off the head radius so they hold
// their proportions at any distance.
// ---------------------------------------------------------------------------
static void StickFigure_DrawShoulderPads(const StickFigureData* fig, float thick,
                                         DWORD ink, int wide, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    // The shoulder is the upper end of each upper-arm bone: shoulder joint 9
    // (or 12) to elbow 10 (or 13). See StickFigure_PartOfJoint.
    static const int shoulder[2] = { 9, 12 };
    static const int elbow[2]    = { 10, 13 };

    for (int a = 0; a < 2; ++a) {
        int sj = shoulder[a], ej = elbow[a];
        if (!fig->jok[sj] || !fig->jok[ej]) continue;
        float sx = fig->jx[sj], sy = fig->jy[sj], sz = fig->jz[sj];
        float dx = fig->jx[ej] - sx, dy = fig->jy[ej] - sy;
        float dl = sqrtf(dx * dx + dy * dy);
        if (dl < 0.5f) { dx = fig->downX; dy = fig->downY; }
        else { dx /= dl; dy /= dl; }
        float px = -dy, py = dx;

        // Outward from the body centre, which for this chain is the head axis.
        float outSign = ((sx - fig->headX) * px + (sy - fig->headY) * py) < 0.0f
                      ? -1.0f : 1.0f;
        float out = (wide ? 0.95f : 0.70f) * hr;

        float bx = sx - px * outSign * hr * 0.25f;
        float by = sy - py * outSign * hr * 0.25f;
        float tx = sx + px * outSign * out;
        float ty = sy + py * outSign * out;
        StickFigure_Line(bx, by, sz, tx, ty, sz, thick * 2.6f, ink, depthTest);
        // A short second bar just below it, so the pad reads as a plate rather
        // than as one more bone.
        StickFigure_Line(bx + dx * hr * 0.34f, by + dy * hr * 0.34f, sz,
                         tx + dx * hr * 0.20f, ty + dy * hr * 0.20f, sz,
                         thick * 1.5f, ink, depthTest);
    }
}

// Jill's beret: a flat disc of colour tipped over the top of the head, with a
// short tab. A beret sits off-centre, which is what stops it reading as hair.
static void StickFigure_DrawBeret(const StickFigureData* fig, float thick,
                                  DWORD ink, int depthTest)
{
    float hr = fig->headR;
    if (hr <= 0.0f) return;

    float hx = fig->headX, hy = fig->headY, vz = fig->headZ;
    float ux = fig->downX, uy = fig->downY;
    float px = -uy, py = ux;

    float cx = hx + px * hr * 0.26f - ux * hr * 0.42f;
    float cy = hy + py * hr * 0.26f - uy * hr * 0.42f;
    StickFigure_Ring(cx, cy, hr * 0.82f, 10, thick * 2.0f, ink, vz, depthTest);
    // The tab, sticking out to the side the beret leans towards.
    StickFigure_Line(cx + px * hr * 0.70f, cy + py * hr * 0.70f, vz,
                     cx + px * hr * 1.05f - ux * hr * 0.10f,
                     cy + py * hr * 1.05f - uy * hr * 0.10f, vz,
                     thick * 1.4f, ink, depthTest);
}

// ---------------------------------------------------------------------------
// Draw one figure as ink. With g_stickSceneInk == 0 this writes into the
// line-art page (the old Present-time overlay). FlushTmdObjects sets the flag
// and calls it inside the scene's painter order instead, and the ink primitives
// are depth-tested against the room, so a wall nearer than the figure hides it.
static void StickFigure_DrawOneFigure(const StickFigureData* fig, int depthTest)
{
    if (fig == NULL || fig->segmentCount <= 0) return;

    float scaleX;
    MarniGetRenderScale(&scaleX, NULL);

    // Near-black ink, per-character eye colours, and a light gunmetal so a held
    // weapon reads as an object rather than another bone. The ink goes through
    // the scene alpha marker and is copied to the page verbatim, so these are
    // page colours, not scene-lit colours.
    //
    // Colour rule: red means the infection, so it belongs to the monsters and
    // the dead only. No living character ever gets red eyes. Every style has its
    // own eye colour so the cast reads apart at a glance even before the shape
    // does:
    //
        //   player / human NPCs  cold blue       Jill        bright blue
        //   Chris                grey green      Barry       tawny brown
        //   Wesker               pale steel      Richard     olive green
        //   Enrico               dark brown      Rebecca     warm hazel
        //   zombie               infected red (mismatched)
        //   dog                  sickly red      hunter      acid green
        //   tyrant               molten red      chimera     three red compound eyes    //   crow                 beady red       wasp        red
        //   spider               red cluster     snake       red slit
        //   shark                black           vine        red maw
        //   corpse               red X (dead)
    const DWORD ink       = 0xFF23202Au;
    const DWORD humanEye  = 0xFFC0882Cu;   // unnamed placeholder NPCs: cold blue
    // Every named human gets their own eye colour, all non-red (red is the
    // infection, so living people never get it). Cold neutrals for the men, a
    // bright blue for Jill, hazel for Rebecca.
    const DWORD jillEye    = 0xFF3FA8FFu;  // Jill: bright blue
    const DWORD chrisEye   = 0xFF6E8C4Au;  // Chris: grey green
    const DWORD barryEye   = 0xFF8A7A5Cu;  // Barry: tawny brown
    const DWORD weskerEye  = 0xFF9AA6B4u;  // Wesker: pale steel blue
    const DWORD richardEye = 0xFF7C8C3Cu;  // Richard: olive green
    const DWORD enricoEye  = 0xFF6E5A3Cu;  // Enrico: dark brown
    const DWORD rebeccaEye = 0xFF7FB06Eu;  // Rebecca: warm hazel green
    const DWORD blood     = 0xFF8E1210u;

    // Monsters and the dead. All red family, but each a different shade so the
    // species stay distinguishable from one another.
    const DWORD zombieEye   = 0xFF9E1C10u;  // dark, clotted
    const DWORD dogEye      = 0xFFD02A12u;  // sickly bright
    const DWORD hunterEye   = 0xFF2FB43Cu;  // acid green, so it reads apart from the zombie red
    const DWORD tyrantEye   = 0xFFF0400Cu;  // molten
    const DWORD chimeraEye  = 0xFFE01810u;  // compound
    const DWORD crowEye     = 0xFFD4281Cu;  // beady
    const DWORD waspEye     = 0xFFE62C10u;
    const DWORD spiderEye   = 0xFFC0201Cu;
    const DWORD snakeEye    = 0xFFF03818u;
    const DWORD vineEye     = 0xFFA81410u;
    const DWORD metal     = 0xFFB9C2CEu;

    // The plain body ink, for a mouth on a character whose face is otherwise
    // drawn in its own colours, and for details the costume does not cover.
    // Kept named so a reader can see at a glance that a call which passes ink is
    // deliberately not colour-coded.
    const DWORD inert = ink;

    {
        int style = fig->style;
        bool chainStyle = (style == STYLE_SNAKE || style == STYLE_SHARK ||
                           style == STYLE_VINE);
        float thick = (chainStyle ? 0.018f : 0.035f) * fig->bodyLen;
        if (thick > 1.8f * scaleX) thick = 1.8f * scaleX;
        if (thick < 1.1f) thick = 1.1f;

        int dot = (int)(thick * 1.8f);
        if (dot < 2) dot = 2;

        // Bones first, with each end carrying its own camera-space depth. The
        // humanoid cast draws each bone in its costume colour (see
        // StickFigure_CostumeOf) so the vest, the trousers and the boots read
        // as clothing rather than as one black body; monsters have no costume
        // and every bone is the plain ink.
        StickCostume costume = StickFigure_CostumeOf(style);
        // A costumed human: one of the named cast, the placeholder human, or a
        // corpse in uniform. This is the ONLY thing that takes S.T.A.R.S.
        // clothing colours. The humanoid monsters share the same joint map (so
        // their bones can be filled) but must not be dressed in a uniform, so
        // the two are tested separately.
        bool dressed = StickFigure_CostumedHuman(style);
        DWORD monsterFlesh = StickFigure_MonsterFlesh(style);
        bool monster = !dressed && monsterFlesh != 0;
        for (int i = 0; i < fig->segmentCount; ++i) {
            const StickSegment* s = &fig->segments[i];
            DWORD boneInk = ink;
            if (dressed || monster) {
                int part = (int)s->part;
                if (dressed) {
                    DWORD c = StickFigure_InkForPart(&costume, part);
                    if (c != 0) boneInk = c;
                } else {
                    // A monster's bones carry the flesh outline; the flat flesh
                    // fill is laid under them in the pass below. Only the head
                    // is left as plain ink so it does not merge with the fill.
                    if (part != PART_HEAD) boneInk = monsterFlesh;
                }
            }
            StickFigure_Line(s->x0, s->y0, s->vz0, s->x1, s->y1, s->vz1,
                             thick, boneInk, depthTest);
        }

        // The vest. The humanoid tree has no torso bone - the root connects
        // straight to the two shoulder joints (9, 12) and the two hip joints
        // (2, 6) - so the body is a filled quad between the shoulder line and
        // the hip line. Without it the cast is a spine with four sticks and the
        // costume colours have nowhere to show. Drawn after the bones so the
        // ink limbs sit on top of the wash, and before the joint dots so the
        // dots read as buttons on the vest.
        if (dressed && fig->jok[9] && fig->jok[12] &&
            fig->jok[2] && fig->jok[6]) {
            // Which shoulder pairs with which hip is not fixed by the tree:
            // the two arm chains swap sides from pose to pose. Order both
            // pairs by the same side test - the sign of the joint's offset
            // across the body axis - so the left edge always joins the two
            // joints on the figure's left and the quad cannot cross itself
            // into a bowtie when the figure turns.
            float ax = fig->downX, ay = fig->downY;   // the body axis
            int sh[2] = { 9, 12 };
            int hp[2] = { 2, 6 };
            float sd0 = (fig->jx[9] - fig->rootX) * ax +
                        (fig->jy[9] - fig->rootY) * ay;
            float sd1 = (fig->jx[12] - fig->rootX) * ax +
                        (fig->jy[12] - fig->rootY) * ay;
            if (sd0 > sd1) { int t = sh[0]; sh[0] = sh[1]; sh[1] = t; }
            float hd0 = (fig->jx[2] - fig->rootX) * ax +
                        (fig->jy[2] - fig->rootY) * ay;
            float hd1 = (fig->jx[6] - fig->rootX) * ax +
                        (fig->jy[6] - fig->rootY) * ay;
            if (hd0 > hd1) { int t = hp[0]; hp[0] = hp[1]; hp[1] = t; }
            for (int k = 0; k <= 6; ++k) {
                float t = (float)k / 6.0f;
                float lx = fig->jx[sh[0]] + (fig->jx[hp[0]] - fig->jx[sh[0]]) * t;
                float ly = fig->jy[sh[0]] + (fig->jy[hp[0]] - fig->jy[sh[0]]) * t;
                float rx = fig->jx[sh[1]] + (fig->jx[hp[1]] - fig->jx[sh[1]]) * t;
                float ry = fig->jy[sh[1]] + (fig->jy[hp[1]] - fig->jy[sh[1]]) * t;
                float z = fig->jz[sh[0]] + (fig->jz[hp[0]] - fig->jz[sh[0]]) * t;
                // The vest widens a touch over the chest and tucks in at the
                // waist, so the two sides do not read as a flat plank.
                float bloat = 0.14f * fig->headR * (1.0f - t) - 0.05f * fig->headR * t;
                float dx = rx - lx, dy = ry - ly;
                float dl = sqrtf(dx * dx + dy * dy);
                if (dl < 0.5f) continue;
                float qx = -dy / dl, qy = dx / dl;
                StickFigure_Line(lx - qx * bloat, ly - qy * bloat, z,
                                 rx + qx * bloat, ry + qy * bloat, z,
                                 fig->headR * 0.75f, costume.vest, depthTest);
            }
        }

        // Painted limbs. The bones above are drawn as thin ink outlines, so
        // without this pass the arms and legs are bare sticks with no clothing
        // on them - the vest is the only thing with a fill. Every limb bone is
        // therefore re-drawn fat enough to fill as a solid stroke in its own
        // costume colour (trousers for the thigh and shin, boots for the foot,
        // the sleeve for the upper arm, skin or glove for the forearm and hand)
        // and the thin ink outline goes back on top of it. Only bones the
        // costume classifies are painted, so a monster keeps its plain ink.
        if (dressed) {
            for (int i = 0; i < fig->segmentCount; ++i) {
                const StickSegment* s = &fig->segments[i];
                int part = (int)s->part;
                DWORD c = StickFigure_InkForPart(&costume, part);
                if (c == 0) continue;   // unclassified bone: leave it as ink
                // Limbs are filled one step slimmer than the vest so the ink
                // outline still reads as a contour rather than a thick smear.
                StickFigure_Line(s->x0, s->y0, s->vz0, s->x1, s->y1, s->vz1,
                                 fig->headR * 0.46f, c, depthTest);
            }
        } else if (monster) {
            // A monster wears no clothes, but it is still a body: fill its
            // torso and limbs with its own flesh tone so a zombie or a cerberus
            // reads as a solid creature instead of an empty wire frame. Every
            // bone is filled along its own length in the same tone, so the fill
            // follows the pose rather than a fixed box. The dog and the chain
            // creatures have no joint map, but filling every segment works for
            // them too - the tone just runs along the spine.
            {
                // A flatter, wider stroke than a clothed limb: bare flesh has
                // no cuff to break it up, so it fills as one mass.
                float fillW = fig->headR * 0.52f;
                for (int i = 0; i < fig->segmentCount; ++i) {
                    const StickSegment* s = &fig->segments[i];
                    StickFigure_Line(s->x0, s->y0, s->vz0, s->x1, s->y1, s->vz1,
                                     fillW, monsterFlesh, depthTest);
                }
            }
        }

        // Joints. The head joint is left to the ring/face, otherwise every
        // skull would have a dot in the middle of it. When the head is gone
        // there is no ring, so the stump joint does get its dot.
        for (int i = 0; i < fig->jointCount; ++i) {
            if (!fig->jok[i]) continue;
            if (!fig->headless &&
                fabsf(fig->jx[i] - fig->headX) < 0.5f &&
                fabsf(fig->jy[i] - fig->headY) < 0.5f) continue;
            StickFigure_Dot(fig->jx[i], fig->jy[i], dot, ink, fig->jz[i],
                            depthTest);
        }

        if (fig->headR > 0.0f && !fig->headless) {
            // A head with a costume gets a filled skull: a flat skin tone inside
            // the ink ring, so the face reads as a face instead of as a hole in
            // the page. Monsters keep the hollow ring. The sweep runs along the
            // figure's own down axis, so a head that is lying on its side still
            // fills the right way up.
            if (dressed) {
                float hr = fig->headR;
                float ux = fig->downX, uy = fig->downY;
                float qx = -uy, qy = ux;
                for (int k = -3; k <= 3; ++k) {
                    float t  = (float)k / 3.0f;
                    float cx = fig->headX + ux * t * hr * 0.90f;
                    float cy = fig->headY + uy * t * hr * 0.90f;
                    float halfw = hr * sqrtf(1.0f - t * t) * 0.90f;
                    StickFigure_Line(cx - qx * halfw, cy - qy * halfw,
                                     fig->headZ,
                                     cx + qx * halfw, cy + qy * halfw,
                                     fig->headZ, hr * 0.60f, costume.skin,
                                     depthTest);
                }
            }
            StickFigure_Ring(fig->headX, fig->headY, fig->headR, 12, thick,
                             ink, fig->headZ, depthTest);
        }

        // Necks, hands and feet, so a humanoid reads as a body rather than a
        // bundle of sticks.
        StickFigure_DrawExtremities(fig, thick, ink, depthTest);

        // A blown-off limb: draw a short stub at the joint's own position so it
        // reads as a detached piece on the ground, not a gap in the figure.
        for (int i = 0; i < fig->jointCount; ++i) {
            if (!fig->jok[i]) continue;
            if ((fig->jflags[i] & 0x04) == 0) continue;
            float sx = fig->jx[i], sy = fig->jy[i];
            StickFigure_Line(sx - thick * 1.6f, sy - thick * 0.9f, fig->jz[i],
                             sx + thick * 1.6f, sy - thick * 0.9f, fig->jz[i],
                             thick * 0.9f, ink, depthTest);
            StickFigure_Dot(sx, sy, dot, ink, fig->jz[i], depthTest);
        }

        if (!fig->headless) {
        switch (style) {
        // Living people get a plain ink mouth, same colour as the rest of the
        // figure. The mouth used to be drawn in the blood colour, which put a
        // red slash on every friendly face. Red is for infection and death, so
        // only the zombie and the corpse below keep a bloody mouth.
        case STYLE_HUMAN:
            StickFigure_DrawHair(fig, thick, costume.hair, 1.03f, 0.95f,
                                 0.0f, 0.04f, 0, depthTest);
            StickFigure_DrawHumanFace(fig, thick, inert, humanEye, inert,
                                      depthTest, 0);
            break;
        case STYLE_CHRIS:
            // Chris: short, slightly messy. A crown cap with a broken spiky
            // edge, so he does not read as the same crop as every other man.
            StickFigure_DrawHair(fig, thick, costume.hair, 1.04f, 1.0f,
                                 0.0f, 0.06f, 0, depthTest);
            StickFigure_DrawHairSpikes(fig, thick, costume.hair, depthTest);
            StickFigure_DrawHumanFace(fig, thick, inert, chrisEye, inert,
                                      depthTest, 0);
            // Oversized rectangular shoulder pads, and the knife on the chest
            // harness drawn as a diagonal.
            StickFigure_DrawShoulderPads(fig, thick, costume.accent, 0,
                                         depthTest);
            StickFigure_DrawFaceAccessory(fig, thick, metal, 6, depthTest);
            break;
        case STYLE_BARRY:
            // Barry: a full head of spiky reddish hair - the spikes are what
            // separates him at a glance from the other men, so the cap is drawn
            // slightly oversized. The beard covers the mouth, so the face is
            // drawn with the mouth colour set to the hair colour and then
            // painted over by the beard.
            StickFigure_DrawHair(fig, thick, costume.hair, 1.09f, 1.02f,
                                 0.0f, 0.10f, 0, depthTest);
            StickFigure_DrawHairSpikes(fig, thick, costume.hair, depthTest);
            StickFigure_DrawHumanFace(fig, thick, inert, barryEye, inert,
                                      depthTest, 0);
            StickFigure_DrawFaceAccessory(fig, thick, costume.hair, 2,
                                          depthTest);
            break;
        case STYLE_WESKER:
            // Wesker: slicked straight back. The cap is tight and the hair
            // falls only behind the head, never past the ears. The sunglasses
            // are the face: drawn instead of the eyes, in the bar colour.
            StickFigure_DrawHair(fig, thick, costume.hair, 1.02f, 0.95f,
                                 0.30f, -0.04f, 0, depthTest);
            StickFigure_DrawFaceAccessory(fig, thick, weskerEye, 1, depthTest);
            break;
        case STYLE_RICHARD:
            // Richard: short and a little full on top.
            StickFigure_DrawHair(fig, thick, costume.hair, 1.06f, 1.0f,
                                 0.10f, 0.07f, 0, depthTest);
            StickFigure_DrawHumanFace(fig, thick, inert, richardEye, inert,
                                      depthTest, 0);
            break;
        case STYLE_ENRICO:
            // Enrico: a balding crown, so the cap covers less of the top.
            StickFigure_DrawHair(fig, thick, costume.hair, 1.02f, 0.78f,
                                 0.0f, 0.0f, 0, depthTest);
            StickFigure_DrawHumanFace(fig, thick, inert, enricoEye, inert,
                                      depthTest, 0);
            break;
        case STYLE_BRAD:
            // Brad: the afro is the silhouette, so the hair is drawn as a big
            // round mass on top of the crown, not the usual hugging cap.
            for (int k = 0; k < 3; ++k) {
                float rr = 1.18f + 0.16f * (float)k;
                StickFigure_DrawHair(fig, thick * 0.9f, costume.hair, rr,
                                     0.95f - 0.10f * (float)k, 0.0f, 0.0f, 0,
                                     depthTest);
            }
            StickFigure_DrawHumanFace(fig, thick, inert, humanEye, inert,
                                      depthTest, 0);
            StickFigure_DrawFaceAccessory(fig, thick, costume.accent, 4,
                                          depthTest);
            break;
        case STYLE_JOSEPH:
            // Joseph: the bandanna over the crown, the vest, the diagonal
            // bandolier across it.
            StickFigure_DrawHair(fig, thick, costume.hair, 1.03f, 0.92f,
                                 0.0f, 0.02f, 0, depthTest);
            StickFigure_DrawHumanFace(fig, thick, inert, humanEye, inert,
                                      depthTest, 0);
            StickFigure_DrawFaceAccessory(fig, thick, costume.accent, 5,
                                          depthTest);
            StickFigure_DrawFaceAccessory(fig, thick, costume.boot, 6,
                                          depthTest);
            break;
        case STYLE_JILL:
            // Jill: a short chin-length bob, hanging on both sides, with the
            // beret sitting over the top of it.
            StickFigure_DrawHair(fig, thick, costume.hair, 1.03f, 0.98f,
                                 0.58f, 0.10f, 0, depthTest);
            StickFigure_DrawHumanFace(fig, thick, inert, jillEye, inert,
                                      depthTest, 1);
            StickFigure_DrawBeret(fig, thick, costume.accent, depthTest);
            StickFigure_DrawJillCostume(fig, thick, costume, depthTest);
            break;
        case STYLE_REBECCA:
            StickFigure_DrawRebeccaDetails(fig, thick, costume, depthTest);
            break;
        case STYLE_CORPSE:
            StickFigure_DrawCorpseFace(fig, thick, ink, blood, depthTest);
            break;
        case STYLE_ZOMBIE:
            StickFigure_DrawZombieFace(fig, thick, ink, zombieEye, depthTest);
            break;
        case STYLE_DOG:
            StickFigure_DrawDogDetails(fig, thick, ink, dogEye, depthTest);
            break;
        case STYLE_HUNTER:
            StickFigure_DrawHunterDetails(fig, thick, ink, hunterEye, depthTest);
            break;
        case STYLE_TYRANT:
            StickFigure_DrawTyrantDetails(fig, thick, ink, tyrantEye, depthTest);
            break;
        case STYLE_CHIMERA:
            StickFigure_DrawChimeraDetails(fig, thick, ink, chimeraEye, depthTest);
            break;
        case STYLE_CROW:
            StickFigure_DrawCrowDetails(fig, thick, ink, crowEye, depthTest);
            break;
        case STYLE_WASP:
            StickFigure_DrawWaspDetails(fig, thick, ink, waspEye, depthTest);
            break;
        case STYLE_SPIDER:
            StickFigure_DrawSpiderDetails(fig, thick, ink, spiderEye, depthTest);
            break;
        case STYLE_SNAKE:
            StickFigure_DrawSnakeDetails(fig, thick, ink, snakeEye, depthTest);
            break;
        case STYLE_SHARK:
            StickFigure_DrawSharkDetails(fig, thick, ink, ink, depthTest);
            break;
        case STYLE_VINE:
            StickFigure_DrawVineDetails(fig, thick, ink, vineEye, depthTest);
            break;
        }
        }   // !fig->headless

        StickFigure_DrawWeapon(fig, thick, ink, metal, depthTest);
    }
}

// Called by MarniDX::Present to clear the list. The figures have already been
// written into the scene colour by FlushTmdObjects, so this only prevents a
// stale pose being reused if no 3D pass runs next frame.
void StickFigure_DrawOverlay(int depthTest)
{
    (void)depthTest;
    g_stickFigureCount = 0;
}

// Scene-order interface used by FlushTmdObjects. Figures are sorted far to
// near by their farthest joint; the painter walk then draws each one where a
// model at that depth would have been.
void StickFigure_SortSceneFigures(void)
{
    for (int i = 1; i < g_stickFigureCount; ++i) {
        StickFigureData tmp = g_stickFigures[i];
        int j = i - 1;
        while (j >= 0 && g_stickFigures[j].maxZ < tmp.maxZ) {
            g_stickFigures[j + 1] = g_stickFigures[j];
            --j;
        }
        g_stickFigures[j + 1] = tmp;
    }
}

int StickFigure_SceneCount(void)
{
    return g_stickFigureCount;
}

float StickFigure_SceneDepth(int index)
{
    if (index < 0 || index >= g_stickFigureCount) return 0.0f;
    return g_stickFigures[index].maxZ;
}

void StickFigure_DrawSceneInk(int index)
{
    if (index < 0 || index >= g_stickFigureCount) return;
    g_stickSceneInk = 1;
    StickFigure_DrawOneFigure(&g_stickFigures[index], 0);
    g_stickSceneInk = 0;
}
void StickFigure_ResetScene(void)
{
    g_stickFigureCount = 0;
}
// (0x0048c350) - render_entity: the in-game character renderer.
// Ghidra called this calc_entity_lighting, but the lighting maths lives in
// update_entity_lighting (0x00481660), which this calls once for the entity
// (and again per joint for types 0x0D/0x12) before drawing. The body is a
// per-joint loop: composes the camera matrix with the joint's world matrix,
// sets the light/rot matrices, and queues the joint's TMD object. Joints
// flagged 0x20 go to the path-trail step instead, flag 4 to the severed-limb
// ballistic step, and flags 0x74 render only inside the camera switch zone.
// Skipped for entity types 0x0D/0x12 with sub-type 1 (they render elsewhere).
// The menu-side twin is options_render_entity (0x004775b0).
void render_entity(Entity* ent)
{
    int param_1 = (int)ent;
    unsigned char* entBytes = (unsigned char*)ENTITY;

    if (((entBytes[1] == 0x0D) || (entBytes[1] == 0x12)) && (entBytes[2] == 1)) {
        return;
    }

    // Stick-figure characters (port extension): the figure replaces the model,
    // so the joint objects are not queued below, and the pose is measured here
    // because rotate_entity has just made every joint->world current.
    int  stickStyle  = StickFigure_StyleOf(ent);
    bool stickFigure = StickFigure_Active() && stickStyle >= 0 &&
                       g_stickFigureCount < STICK_MAX_FIGURES;
    if (stickFigure) {
        for (int i = 0; i < STICK_MAX_JOINTS; ++i) g_stickOk[i] = false;
    }

    g_animFrameIdSave = (unsigned int)((*(unsigned char*)(param_1 + 3) & 0x7f) == 0);

    unsigned char jointIdx = *(char*)(param_1 + 0x8d) - 1;
    MATRIX* pJoint = (MATRIX*)((unsigned int)jointIdx * 0x7c + *(int*)(param_1 + 0x98));

    update_entity_lighting((VECTOR*)(param_1 + 0x34));

    do {
        short jointFlags = pJoint->m[0][0];

        if ((entBytes[1] == 0x0D) || (entBytes[1] == 0x12)) {
            update_entity_lighting((VECTOR*)(pJoint[2].t + 1));
        }

        if ((jointFlags & 4) != 0) {
            g_svecScratch.x = 0;
            g_svecScratch.z = 0;
            g_svecScratch.y = 0x1e;
            pJoint->m[0][2] = -0x14;
            pJoint->m[1][1] = 0;
            pJoint->m[1][0] = 200;
            FUN_004896c0(pJoint, (short)0xffdd, (short)0xff9c, 1);
        }

        if ((jointFlags & 1) == 0) {
            if ((jointFlags & 0x20) != 0) {
                FUN_0048a210(pJoint);
            }
        }
        else {
            MATRIX localMatrix;
            ApplyLVAndMul0Matrix(&g_RoomCameraData, pJoint[2].m[0] + 2, &localMatrix);
            // NOTE: the stick-figure projection is NOT done here. The original
            // only reaches doRender after the jointFlags & 0x74 cull below (the
            // checkSwitchZone test). Projecting at this point recorded joints
            // the original would have skipped, so a figure standing in a switch
            // zone kept those joints and drew through the wall. The projection
            // now happens inside the doRender block, immediately before the TMD
            // object is queued for the same joint - the same place the original
            // queues it.

            // Copy g_lightMatrix to g_matrixScratch
            MATRIX* src = &g_lightMatrix;
            MATRIX* dst = &g_matrixScratch;
            for (int i = 8; i != 0; i--) {
                *(unsigned int*)dst->m[0] = *(unsigned int*)src->m[0];
                src = (MATRIX*)(src->m[0] + 2);
                dst = (MATRIX*)(dst->m[0] + 2);
            }

            if (g_animFrameIdSave == 0) {
                if ((jointFlags & 0x74) != 0) goto checkSwitchZone;
doRender:
                if (((entBytes[1] != 18) || (get_stage_id() != STAGE_MANSION_RETURN_2F)) ||
                    ((g_roomId != ROOM_LESSON_ROOM) || (g_roomCameraId != 3))) {
                    g_entityJointPosX = pJoint->t[0];
                    SetLightMatrix(&g_matrixScratch);
                    SetRotAndTransMatrix(&localMatrix);
                    if (stickFigure) {
                        // Projected only on the paths that actually draw the
                        // joint, so the figure inherits the original's
                        // jointFlags & 0x74 / switch-zone cull.
                        StickFigure_ProjectJoint(jointIdx, &localMatrix);
                    } else {
                        FUN_00483250(0, 0, 0, pJoint->t[1], 0, 4,
                            (BYTE*)&g_spriteAnimSlots[2] + (unsigned int)g_spriteAnimActive * 0x14);
                    }
                }
            }
            else if ((jointFlags & 0x74) != 0) {
checkSwitchZone:
                if (is_entity_in_switch_zone((VECTOR*)(pJoint[2].t + 1), g_CurrentRdtDataTypePtr) != 0) {
                    goto doRender;
                }
            }
        }

        pJoint = (MATRIX*)(pJoint[-4].m[0] + 2);
        bool done = (jointIdx == 0);
        jointIdx--;
        if (!done) continue;
        if (stickFigure) StickFigure_Finish(ent, stickStyle);
        return;
    } while (true);
}


// (0x00483250) - Entity sprite rendering helper
// Forwards joint sprite data and depth shift to the TMD renderer.
// NOTE: 0x00483230 (tmd_render_object_cb) is a byte-identical twin of this
// thunk used by tyrant_draw_heart / FUN_00429d50 / FUN_00469d20 call sites;
// the port serves both from this single implementation.
void FUN_00483250(int p0, int p1, int p2, int p3, int p4, int p5, void* p6)
{
    // Assembly: MOV EAX,[ESP+0x18]; MOV ECX,[ESP+0x10]; PUSH EAX; PUSH ECX; CALL FUN_00483080
    // (also covers 0x00483230)
    FUN_00483080((void*)p3, p5);
}

// (0x0048cc50) - Build view matrix from eye/target positions
static unsigned int FUN_0048cc50(float* eyeTarget, float* eyePos, float* outMatrix)
{
    float dx = eyePos[0] - eyeTarget[0];
    float dy = eyePos[1] - eyeTarget[1];
    float dz = eyePos[2] - eyeTarget[2];
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len == 0.0f) len = 1.0f;
    float invLen = 1.0f / len;
    float ny = -(dy * invLen);
    float horiz = sqrtf(1.0f - ny * ny);
    float nx, nz;
    if (horiz == 0.0f) {
        nx = 0.0f;
        nz = 1.0f;
    } else {
        nx = -((dx * invLen) / horiz);
        nz = (dz * invLen) / horiz;
    }
    outMatrix[0] = nz;      outMatrix[4] = 0.0f;  outMatrix[8]  = nx;
    outMatrix[1] = -(nx * ny); outMatrix[5] = horiz; outMatrix[9]  = nz * ny;
    outMatrix[2] = -(nx * horiz); outMatrix[6] = -ny; outMatrix[10] = nz * horiz;
    outMatrix[12] = outMatrix[0] * dx + outMatrix[8] * dz;
    outMatrix[13] = outMatrix[1] * dx + outMatrix[5] * dy + outMatrix[9] * dz;
    outMatrix[14] = outMatrix[2] * dx + outMatrix[6] * dy + outMatrix[10] * dz;
    outMatrix[3] = 0.0f; outMatrix[7] = 0.0f; outMatrix[11] = 0.0f; outMatrix[15] = 1.0f;
    return 1;
}

// (0x0048c730) - 4x4 matrix multiply (rotation part only, 3x3)
static void FUN_0048c730(float* a, float* b, float* out)
{
    out[0]  = a[0]*b[0] + a[1]*b[4] + a[2]*b[8];
    out[1]  = a[0]*b[1] + a[1]*b[5] + a[2]*b[9];
    out[2]  = a[0]*b[2] + a[1]*b[6] + a[2]*b[10];
    out[4]  = a[4]*b[0] + a[5]*b[4] + a[6]*b[8];
    out[5]  = a[4]*b[1] + a[5]*b[5] + a[6]*b[9];
    out[6]  = a[4]*b[2] + a[5]*b[6] + a[6]*b[10];
    out[8]  = a[8]*b[0] + a[9]*b[4] + a[10]*b[8];
    out[9]  = a[8]*b[1] + a[9]*b[5] + a[10]*b[9];
    out[10] = a[8]*b[2] + a[9]*b[6] + a[10]*b[10];
}

// (0x0048c820) - Transform translation vector by rotation matrix
static void FUN_0048c820(float* translation, float* rotMatrix)
{
    float x = translation[0], y = translation[1], z = translation[2];
    translation[0] = rotMatrix[0]*x + rotMatrix[4]*y + rotMatrix[8]*z;
    translation[1] = rotMatrix[1]*x + rotMatrix[5]*y + rotMatrix[9]*z;
    translation[2] = rotMatrix[2]*x + rotMatrix[6]*y + rotMatrix[10]*z;
}

// (0x00486190) - Camera/projection matrix setup
// Builds view matrix from camera parameters and composites with the model matrix.
// The original builds the two input vectors as
//   from = (0, 0, -g_sceneRenderParam)
//   to   = (0xA0 - subpixelX, subpixelY - 0x78, 0)
// so the direction handed to FUN_0048cc50 (to - from) has a POSITIVE Z of
// g_sceneRenderParam. An earlier revision folded -g_sceneRenderParam into `to`
// and left `from` at the origin, which negated the Z axis (an extra 180 degree
// yaw) and only happened to look right while the subpixel offset was exactly
// the screen centre.
static void FUN_00486190(float* modelMatrix)
{
    float from[3];
    from[0] = 0.0f;
    from[1] = 0.0f;
    from[2] = (float)-g_sceneRenderParam;

    float to[3];
    to[0] = (float)(0xA0 - g_SubpixelOffsetX);
    to[1] = (float)(g_SubpixelOffsetY + (-0x78));
    to[2] = 0.0f;

    float viewMatrix[16];
    FUN_0048cc50(from, to, viewMatrix);
    FUN_0048c730(modelMatrix, viewMatrix, modelMatrix);
    FUN_0048c820(modelMatrix + 12, viewMatrix);
}

// ============================================================================
// FUN_00486df0 (0x00486df0) - draw the "complex" object pool.
//
// ComplexTmdObjectSetup (0x00486990) explodes one TMD into up to 256 standalone
// textured-triangle objects: geometry into the 0x38-stride element array at
// g_objectListPtrArray, per-object render state into the 0x84-stride array at
// g_complexTmdObjectData. FUN_00483080 routes any animation object it has
// processed (spriteData[4] == 1) here instead of through the normal
// AsyncCreateTmdObject path.
//
// This was an empty stub in EngineStubs.cpp, so everything built through that
// path drew nothing at all - visibly, Plant 42's curtain of hanging tendrils in
// room 40C0 (the plant's 3D limbs come through the ordinary path and did show).
//
// Unlike FUN_00483080 the original does NOT fold the FUN_00486190 view rotation
// into these matrices; the raw GTE rotation/translation is written straight into
// each object entry. That is deliberate and is preserved here.
// ============================================================================
void FUN_00486df0(void* spriteData)
{
    if (g_objectListCleanupFlag != 1) return;
    if (spriteData == NULL || ((int*)spriteData)[4] != 1) return;

    // Base ordering-table depth: GTE t[2] / 4, clamped to 3000.
    int t2 = g_gteRotTransMatrix.t[2];
    int baseDepth = (t2 + (t2 >> 31 & 3)) >> 2;
    if (baseDepth >= 3000) baseDepth = 3000;

    const int count = g_objectListCleanupCount;
    if (count <= 0) return;

    // Depth-key every object from the face centroid ComplexTmdObjectSetup
    // stashed for it (0x008fb8b0, one SVECTOR each).
    const int maxFaces = (int)(sizeof(g_faceNormalBuffer) / 8);
    const int keyed = (count < maxFaces) ? count : maxFaces;
    for (int i = 0; i < keyed; i++) {
        SVECTOR out;
        ApplyMatrixSV(&g_gteRotTransMatrix, (SVECTOR*)(g_faceNormalBuffer + i * 8), &out);
        g_complexTmdObjectIds[i]   = i;
        g_complexTmdObjectArray[i] = (int)out.z;
    }
    for (int i = keyed; i < count; i++) {
        g_complexTmdObjectIds[i]   = i;
        g_complexTmdObjectArray[i] = 0;
    }

    // Selection sort, near to far (the original's nested loop, verbatim).
    for (int i = 0; i < count - 1; i++) {
        for (int j = i; j < count; j++) {
            if (g_complexTmdObjectArray[j] < g_complexTmdObjectArray[i]) {
                int d = g_complexTmdObjectArray[i];
                g_complexTmdObjectArray[i] = g_complexTmdObjectArray[j];
                g_complexTmdObjectArray[j] = d;
                int id = g_complexTmdObjectIds[i];
                g_complexTmdObjectIds[i] = g_complexTmdObjectIds[j];
                g_complexTmdObjectIds[j] = id;
            }
        }
    }

    const float scale = 0.00024414063f;  // 1/4096
    for (int i = 0; i < count; i++) {
        int id = g_complexTmdObjectIds[i];
        if (id < 0 || id >= 256) continue;

        BYTE*  entry = g_complexTmdObjectData + id * 0x84;
        float* M     = (float*)(entry + 0x08);

        M[0]  = (float)g_gteRotTransMatrix.m[0][0] * scale;
        M[4]  = (float)g_gteRotTransMatrix.m[0][1] * scale;
        M[8]  = (float)g_gteRotTransMatrix.m[0][2] * scale;
        M[1]  = (float)g_gteRotTransMatrix.m[1][0] * scale;
        M[5]  = (float)g_gteRotTransMatrix.m[1][1] * scale;
        M[9]  = (float)g_gteRotTransMatrix.m[1][2] * scale;
        M[2]  = (float)g_gteRotTransMatrix.m[2][0] * scale;
        M[6]  = (float)g_gteRotTransMatrix.m[2][1] * scale;
        M[10] = (float)g_gteRotTransMatrix.m[2][2] * scale;
        M[12] = (float)g_gteRotTransMatrix.t[0];
        M[13] = (float)g_gteRotTransMatrix.t[1];
        // Each object is nudged one unit further back than the previous one so
        // the sorted order survives into the ordering table.
        M[14] = (float)i + (float)g_gteRotTransMatrix.t[2];
        M[3]  = 0.0f;
        M[7]  = 0.0f;
        M[11] = 0.0f;
        M[15] = 1.0f;

        // +0x54 is the object's D3D handle; zero means it was never created.
        if (*(DWORD*)(entry + 0x54) == 0) continue;

        TmdQueueComplexObject(entry, &g_objectListPtrArray[id * 0x0E], baseDepth + i);
    }
}

// (0x00482fa0) - Copy light data to TMD render object and insert into ordering table
static void FUN_00482fa0(void* spriteData, int depthShift)
{
    if (spriteData == NULL || g_gteRotTransMatrix.t[2] < 0) return;

    int depth = g_gteRotTransMatrix.t[2] >> (depthShift & 0x1F);
    int* data = (int*)spriteData;
    float* lightDst = (float*)((unsigned char*)data + 0x24);
    DWORD* pLight = g_d3dLightData;

    for (int i = 0; i < 3; i++) {
        memcpy(lightDst, pLight, 12 * sizeof(DWORD));
        if (data[6] != 0) {
            lightDst[6] = (float)((data[6] & 0xFF0000) >> 16);
            lightDst[7] = (float)((data[6] >> 8) & 0xFF);
            lightDst[8] = (float)(data[6] & 0xFF);
        }
        OT_InsertPrimitive(lightDst, depth);
        pLight += 12;
        lightDst += 12;
    }
}

// (0x00483080) - Main TMD entity render function
// Reads GTE state buffers, creates TMD object, builds transform matrix, renders
void FUN_00483080(void* spriteData, int depthShift)
{
    int depthField = g_gteRotTransMatrix.t[2];
    if (spriteData == NULL || depthField < 0) {
        return;
    }

    int depth = depthField >> (depthShift & 0x1F);
    int* data = (int*)spriteData;

    FUN_00482fa0(spriteData, depthShift);

    // data[1] is the minimum CLUT depth of the animation slot (FindMinClutDepth)
    // and doubles as the texture bank id; zero means the object carries no
    // textured primitives and is not rendered.
    if (data[1] == 0) {
        return;
    }

    if (data[4] == 1) {
        FUN_00486df0(spriteData);
        return;
    }

    unsigned int tmdObj = AsyncCreateTmdObject(data[1], data[0], (unsigned int)spriteData);
    data[8] = tmdObj;
    if (tmdObj == 0) {
        return;
    }

    // Build 4x4 transform matrix from GTE rotation/translation buffer.
    // Column layout, matching the original store order at 0x004830ef:
    // GTE row 0 (m[0][0..2]) lands in M[0], M[4], M[8], so the consumer reads
    // a transformed X as M[0]*x + M[4]*y + M[8]*z. An earlier revision wrote
    // m[0][1] to M[1] etc., i.e. the transposed (inverse) rotation.
    float transformMatrix[16];
    float scale = 0.00024414063f; // 1/4096

    transformMatrix[0]  = (float)g_gteRotTransMatrix.m[0][0] * scale;
    transformMatrix[4]  = (float)g_gteRotTransMatrix.m[0][1] * scale;
    transformMatrix[8]  = (float)g_gteRotTransMatrix.m[0][2] * scale;
    transformMatrix[1]  = (float)g_gteRotTransMatrix.m[1][0] * scale;
    transformMatrix[5]  = (float)g_gteRotTransMatrix.m[1][1] * scale;
    transformMatrix[9]  = (float)g_gteRotTransMatrix.m[1][2] * scale;
    transformMatrix[2]  = (float)g_gteRotTransMatrix.m[2][0] * scale;
    transformMatrix[6]  = (float)g_gteRotTransMatrix.m[2][1] * scale;
    transformMatrix[10] = (float)g_gteRotTransMatrix.m[2][2] * scale;
    transformMatrix[12] = (float)g_gteRotTransMatrix.t[0];
    transformMatrix[13] = (float)g_gteRotTransMatrix.t[1];
    transformMatrix[14] = (float)depthField;
    transformMatrix[3]  = 0.0f;
    transformMatrix[7]  = 0.0f;
    transformMatrix[11] = 0.0f;
    transformMatrix[15] = 1.0f;

    FUN_00486190(transformMatrix);

    // Call CMarniDirect3DTMD::Transform(ctx, depth, matrix, doubleBuffer=0)
    // Original: Direct3DTMD_Transform(g_pMarniDirect3D, iVar2, &local_40, 0)
    // (ECX = [spriteData+0x20] = the TMD object handle; depth is the OT depth,
    // NOT the matrix — an earlier revision passed the matrix as arg 2, which
    // left every object with a garbage depth and no stored transform).
    CMarniDirect3DTMD* tmd = (CMarniDirect3DTMD*)(void*)tmdObj;
    tmd->Transform(g_pMarniDirect3D, (void*)(size_t)depth, transformMatrix, 0);
}

// ============================================================================
// Room item / 3D object per-frame rendering (0x00473ff0 -> 0x004745f0 ->
// 0x00483270). This is the per-frame pass the original runs between the player
// update and the entity render in game_loop; the port had it as an empty stub
// in EngineStubs.cpp, so the RDT's item models (g_omodel_table) and
// obstacle models (g_item_model_table) were never queued and FlushTmdObjects
// only ever drew entities.
//
// Item/model record layout (0xA4 bytes, one per RDT model slot):
//   +0x00  flags byte (bit 0 = visible, bit 7 = coarse depth shift 10)
//   +0x01  model type id (low 6 bits)
//   +0x04  pointer to the +0x88 sub-record
//   +0x0C  anim field: [0] = 0x40000000 flags, [1] = ScaMatrixData ptr,
//          [2] = AnimSlot ptr (written by SetAnimSlot via FUN_00473ea0),
//          [3] = spriteData ptr (written by CreateAnimObject)
//   +0x1C  ScaMatrixData (field_00 = "world recomputed" dirty flag)
//   +0x20  rotation MATRIX (rebuilt from the +0x72 SVECTOR every frame)
//   +0x34  position VECTOR (doubles as the matrix translation)
//   +0x54  position used for the camera switch-zone cull
//   +0x72  rotation SVECTOR (RotMatrix input)
// ============================================================================

// (0x00483270) - Render one room object into the TMD queue
// objPtr points at the record's anim field (+0x0C); the spriteData block hangs
// off its +0x0C (record +0x18) where CreateAnimObject stored it. Reads the GTE
// rotation/translation buffer (set by SetRotAndTransMatrix in the caller) for
// the transform, exactly like the entity render FUN_00483080. Depth is normally
// GTE t[2] >> shift; DAT_00ae9ef8 / DAT_00ae9ee4 pin it to the fixed 0x32/0x33
// ordering-table slot for the rooms that need it.
static void FUN_00483270(unsigned char* objPtr, int depthShift)
{
    // spriteData = *(record + 0x18): the animation object CreateAnimObject
    // built when the SCD command bound the TMD (FUN_00473ea0).
    int* spriteData = *(int**)(objPtr + 0xc);

    if (spriteData == NULL) return;

    int depth = (DAT_00ae9ee4 != 0) ? 0x33 : 0x32;
    if (DAT_00ae9ef8 == 0) {
        int depthField = g_gteRotTransMatrix.t[2];
        if (depthField < 0) return;
        depth = depthField >> (depthShift & 0x1F);

    }

    // 0x004832cc-0x00483358: per-light colour override records copied into
    // spriteData+0x24 and OT_InsertPrimitive'd at `depth`. The DX11 port's
    // ordering table only consumes depth 0xFFF (the background) and the flush
    // latches the live g_d3dLightData at queue time (TmdQueueObject), so the
    // copy is a no-op here — same call as FUN_00482fa0 in the entity path.

    // data[1] is the minimum CLUT depth (texture bank id); zero means the
    // object carries no textured primitives and is not rendered.
    if (spriteData[1] == 0) return;

    if (spriteData[4] == 1) {
        FUN_00486df0(spriteData);
        return;
    }

    unsigned int tmdObj = AsyncCreateTmdObject(spriteData[1], spriteData[0], (unsigned int)spriteData);
    spriteData[8] = (int)tmdObj;
    if (tmdObj == 0) return;

    // Build the transform from the GTE buffer — same layout as FUN_00483080.
    float m[16];
    float scale = 0.00024414063f; // 1/4096
    m[0]  = (float)g_gteRotTransMatrix.m[0][0] * scale;
    m[4]  = (float)g_gteRotTransMatrix.m[0][1] * scale;
    m[8]  = (float)g_gteRotTransMatrix.m[0][2] * scale;
    m[1]  = (float)g_gteRotTransMatrix.m[1][0] * scale;
    m[5]  = (float)g_gteRotTransMatrix.m[1][1] * scale;
    m[9]  = (float)g_gteRotTransMatrix.m[1][2] * scale;
    m[2]  = (float)g_gteRotTransMatrix.m[2][0] * scale;
    m[6]  = (float)g_gteRotTransMatrix.m[2][1] * scale;
    m[10] = (float)g_gteRotTransMatrix.m[2][2] * scale;
    m[12] = (float)g_gteRotTransMatrix.t[0];
    m[13] = (float)g_gteRotTransMatrix.t[1];
    m[14] = (float)g_gteRotTransMatrix.t[2];
    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;

    FUN_00486190(m);

    CMarniDirect3DTMD* tmd = (CMarniDirect3DTMD*)(void*)tmdObj;
    tmd->Transform(g_pMarniDirect3D, (void*)(size_t)depth, m, 0);

    // 0x004834d2: copy the anim object's live blend weight (spriteData[5] ==
    // spriteData + 0x14, what scd_model_tint_apply -> TmdObjectSetLightScale
    // (0x004870a0) writes) into the +0x68/+0x78 blend-weight fields of all 31
    // records of the first object buffer, every frame. Without this the flush's
    // `bgWeight` read (e->objData + 0x68) keeps the value CreateTmdObjectInternal
    // stamped at creation (0 for an opaque model), so any runtime alpha change -
    // room 20B's lighter-lit map fade over the 2F map - never left the object
    // solid. The loop runs i = 0x84..0xFF8 (ESI += 0x84, ESI < 0x1080), which
    // lands on record k's +0x68/+0x78 for k = 0..30 of m_objectData.
    for (int rec = 0; rec < 31; rec++) {
        *(int*)((unsigned char*)tmdObj + 0x4D0 + rec * 0x84 + 0x68) = spriteData[5];
        *(int*)((unsigned char*)tmdObj + 0x4D0 + rec * 0x84 + 0x78) = spriteData[5];
    }
}

// (0x00484eb0) 
// FUN_00486190 view rotation — the original transforms the raw GTE matrix.
static void FUN_00484eb0(void)
{
    int depth = g_gteRotTransMatrix.t[2] >> 6;
    if (depth < 0) depth = 0;
    if (depth > 0xffa) depth = 0xffa;

    float m[16];
    float scale = 0.00024414063f; // 1/4096
    m[0]  = (float)g_gteRotTransMatrix.m[0][0] * scale;
    m[4]  = (float)g_gteRotTransMatrix.m[0][1] * scale;
    m[8]  = (float)g_gteRotTransMatrix.m[0][2] * scale;
    m[1]  = (float)g_gteRotTransMatrix.m[1][0] * scale;
    m[5]  = (float)g_gteRotTransMatrix.m[1][1] * scale;
    m[9]  = (float)g_gteRotTransMatrix.m[1][2] * scale;
    m[2]  = (float)g_gteRotTransMatrix.m[2][0] * scale;
    m[6]  = (float)g_gteRotTransMatrix.m[2][1] * scale;
    m[10] = (float)g_gteRotTransMatrix.m[2][2] * scale;
    m[12] = (float)g_gteRotTransMatrix.t[0];
    m[13] = (float)g_gteRotTransMatrix.t[1];
    m[14] = (float)g_gteRotTransMatrix.t[2];
    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;

    CMarniDirect3DTMD* tmd = (CMarniDirect3DTMD*)g_renderStateTMD;
    tmd->Transform(g_pMarniDirect3D, (void*)(size_t)depth, m, 0);
}

// (0x00485000) - schedule the dining-hall table render asynchronously
static void FUN_00485000(void)
{
    ExecAsync((void*)FUN_00484eb0);
}

// (0x004745f0) - Render one item/model record: compose matrices, set lights,
// cull against the camera switch zones, and queue the object's TMD.
static void RoomObjectRender(unsigned char* obj)
{
    MATRIX localMatrix;

    // 0x004745fd: per-object lighting from its world position (record +0x34,
    // which doubles as the +0x20 rotation matrix's translation).
    update_entity_lighting((VECTOR*)(obj + 0x34));

    // 0x0047460e: compose the ScaMatrixData chain (rooted at record +0x10)
    // into localMatrix, then fold in the camera.
    FUN_00483580(*(int**)(obj + 0x10), &localMatrix);

    // 0x00474624: object light matrix = g_lightMatrix * obj rotation matrix
    MulMatrix0(&g_lightMatrix, (MATRIX*)(obj + 0x20), &g_matrixScratch);
    SetLightMatrix(&g_matrixScratch);

    // 0x0047465f: heliport item models 1-4 (pass 0 only) sit 1000 units
    // further along the view axis — the room's shelf displays.
    if ((g_stageId == STAGE_COURTYARD) && (g_roomId == ROOM_HELIPORT) && (DAT_008f8688 == 0)) {
        int t = obj[1] & 0x3f;
        if (t != 0 && t < 5) {
            localMatrix.t[2] += 1000;
        }
    }

    // 0x0047466e: DAT_00ae9ee4 — the mansion-2F study / front lesson room
    // open-lid model renders at the fixed depth 0x33 instead of 0x32.
    DAT_00ae9ee4 = 0;
    int stageMod = g_stageId % 5; // get mansion absolute index
    if (stageMod == STAGE_MANSION_2F) {
        if ((g_roomId == ROOM_STUDY_2F) && (DAT_008f8688 == 0) && ((obj[1] & 0x3f) == 1)) DAT_00ae9ee4 = 1;
        if ((g_roomId == ROOM_FRONT_LESSON_ROOM) && (DAT_008f8688 == 0) && ((obj[1] & 0x3f) == 1)) DAT_00ae9ee4 = 1;
    }

    // 0x004746d2-0x004747b7: room-specific objects that must not render
    // (mirror/door-frame stand-ins the SCD keeps for interaction but that
    // have their own model elsewhere, e.g. the guardhouse room 002 mirror).
    bool skip = false;
    if ((g_stageId == STAGE_GUARDHOUSE) && (g_roomId == ROOM_002) && (g_roomCameraId == 4) &&
        (DAT_008f8688 == 0) && ((obj[1] & 0x3f) == 0)) {
        skip = true;
    } else if ((g_stageId == STAGE_LABORATORY) && (g_roomId == ROOM_LAB_B3_PRIVATE_ROOM_B) &&
               ((g_roomCameraId == 0) || (g_roomCameraId == 4)) &&
               (DAT_008f8688 == 0) && ((obj[1] & 0x3f) == 0)) {
        skip = true;
    } else if ((stageMod == STAGE_MANSION_1F) && (g_roomId == ROOM_TRAP_ROOM) && (g_roomCameraId == 0) &&
               (DAT_008f8688 == 0) && ((obj[1] & 0x3f) == 0) &&
               (*(int*)(obj + 0x34) == 0x12fc) &&
               (*(int*)(obj + 0x38) == -0x2828) &&
               (*(int*)(obj + 0x3c) == 0x12fc)) {
        skip = true;
    } else if ((stageMod == STAGE_COURTYARD) && (g_roomId == ROOM_BOULDER_2_PASSAGE) && (g_roomCameraId == 3) &&
               (DAT_008f8688 == 0) && ((obj[1] & 0x3f) == 0) &&
               (*(int*)(obj + 0x34) > 0x7274)) {
        // 0x004747ab: CMP dword ptr [EDI], 0x7274 / JG <return>, with EDI at
        // record + 0x34 - the model's world X. The sense is "skip once X passes
        // the threshold", and this port had it inverted, so the object was
        // hidden exactly when it should have drawn and drawn exactly when it
        // should have been hidden.
        //
        // This is the boulder in room 30F0 (g_stageId is 0-based, so the stage-3
        // file name is id 2). Camera 3 sits at x = 32940 looking back down the
        // corridor; 0x7275 = 29301, so the original drops the model as soon as
        // the boulder rolls within ~3600 units of the eye - which is the whole
        // point of the rule, and why it filled the screen here instead.
        //
        // The type is compared against DAT_004c3694 in the original, but that
        // global has exactly one xref (this read) and holds 0, so it is a
        // build-time constant and the literal below is faithful.
        skip = true;
    }
    if (skip) return;

    // 0x004747b7-0x0047488b: DAT_00ae9ef8 — keep the fixed ordering-table
    // depth for flooded rooms instead of sorting by GTE t[2].
    DAT_00ae9ef8 = 0;
    int stageModP1 = (g_stageId + 1) % 5; // get 1-indexed stage id
    if (stageModP1 == GUARDHOUSE) {
        if ((g_roomId == ROOM_WATER_TANK_ENTRY) || (g_roomId == ROOM_SECURITY_ROOM) ||
            (g_roomId == ROOM_WATER_TANK) || (g_roomId == ROOM_ARMS_STOREHOUSE) ||
            (g_roomId == ROOM_CONTROL_ROOM)) {
            DAT_00ae9ef8 = 1;
        }
    }
    if ((stageModP1 == COURTYARD) && (g_roomId == ROOM_WATER_GATE)) DAT_00ae9ef8 = 1;
    if ((stageMod == MANSION_1F) && (g_roomId == ROOM_1F_RIGHT_STAIRS) && (DAT_008f8688 == 0) && ((obj[1] & 0x3f) < 2)) DAT_00ae9ef8 = 1;

    // 0x00474890: push the composed matrix into the GTE rotation/translation
    // buffer — FUN_00483270 reads the transform from there.
    SetRotAndTransMatrix(&localMatrix);

    // 0x004748a2: cull objects outside the current camera's switch-zone group.
    if (is_entity_in_switch_zone((VECTOR*)(obj + 0x54), g_CurrentRdtDataTypePtr) == 0) return;

    // 0x004748d2
    // path (FUN_00485000) with a clamped depth.
    if ((stageModP1 == MANSION_2F) && (g_roomId == ROOM_STUDY_2F) && ((obj[1] & 0x3f) == 0)) {
        FUN_00485000();
    }

    // 0x004748d7: records with bit 7 set use the coarse depth shift (10);
    // everything else uses 4.
    if ((obj[0] & 0x80) != 0) {
        FUN_00483270(obj + 0xc, 10);
    } else {
        FUN_00483270(obj + 0xc, 4);
    }
}

// (0x00473ff0) - render_room_objects
// Per-frame draw of the room's own 3D content - despite the Ghidra name
// (room_camera_and_lighting_update) it touches neither the camera nor the
// lights. Pass 0 walks the omodel records (g_omodel_table, count = RDT
// omodel_slot_count), pass 1 the item models (g_item_model_table, count = RDT
// item_count). Each visible record with a bound model gets its rotation matrix
// rebuilt from its +0x72 SVECTOR, its ScaMatrixData marked dirty for the
// compose, and is handed to RoomObjectRender. Called from game_loop while
// g_dwRoomObjectRenderEnabled is set; was an empty stub, which is why room
// items and 3D objects never appeared.
void render_room_objects(void)
{
    for (int pass = 0; pass < 2; pass++) {
        int count = (pass == 0)
            ? g_RdtPointer->omodel_slot_count
            : g_RdtPointer->item_count;
        DAT_008f8688 = pass;

        for (int i = 0; i < count; i++) {
            unsigned char* obj = (pass == 0)
                ? (unsigned char*)g_omodel_table[i]
                : (unsigned char*)g_item_model_table[i];

            // Visible (bit 0) and with a bound model (AnimSlot ptr at +0x14).
            if ((obj != NULL) && ((*obj & 1) != 0) && (*(int*)(obj + 0x14) != 0)) {
                // 0x0047405b: rebuild the rotation matrix from the record's
                // SVECTOR; 0x00474063: mark the ScaMatrixData dirty so
                // FUN_00483580 recomposes it.
                RotMatrix((SVECTOR*)(obj + 0x72), (MATRIX*)(obj + 0x20));
                *(int*)(obj + 0x1c) = 0;
                RoomObjectRender(obj);
            }
        }
    }
}

// ============================================================================
// Float vector / matrix helpers (original cluster 0x0048c5b0..0x0048cad0).
// Self-contained float math used by the viewport normal recalculation in
// CMarniViewport2::CopyFrom/Convert0 and by entity camera/web code. Angles
// are expressed in "turns" (1/360 of a circle), matching the original's
// atan * (180/pi) / 360 pipeline.
// ============================================================================

// (0x0048c5b0) - Normalize a 3-float vector in place.
void vec3_normalize(float* v)
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

// (0x0048c690) - atan-based angle in turns: atan(tangent), negated when
// signRef is negative (the original tests the raw sign bit of the first
// stack argument), scaled by 180/pi then 1/360.
static float angle_atan_turns(float signRef, float tangent)
{
    float deg = atanf(tangent) * 57.29577951f;
    if (signRef < 0.0f) deg = -deg;
    return deg * (1.0f / 360.0f);
}

// (0x0048c6d0) - Angle between two 2D points (x1,y1)-(x2,y2) in turns,
// measured from the Y axis (atan(dx/r) with the dy sign as quadrant fix).
float angle_between_points_turns(int x1, int y1, int x2, int y2)
{
    float dx = (float)(y2 - y1);
    float dz = (float)(x2 - x1);
    float r = sqrtf(dx * dx + dz * dz);
    return angle_atan_turns(dx / r, dz / r);
}

// (0x0048c8f0) - Rotate one matrix row's [1]/[2] float pair by `turns`
// (fraction of a full circle). Row is addressed as {float a; float y; float z;}
// at base+0/+4/+8.
static void rot_row_yz(float turns, float* row)
{
    float y = row[1];
    float angle = turns * 6.2831855f;
    float s = sinf(angle);
    float c = cosf(angle);
    row[1] = s * row[2] + c * y;
    row[2] = c * row[2] - s * y;
}

// (0x0048c950) - Same rotation for the [0]/[2] pair of a float triple.
static void rot_row_xz(float turns, float* row)
{
    float x = row[0];
    float angle = turns * 6.2831855f;
    float s = sinf(angle);
    float c = cosf(angle);
    row[0] = s * row[2] + c * x;
    row[2] = c * row[2] - s * x;
}

// (0x0048ca10) - Rotate the three rows of a 3x4 float matrix about the axis
// handled by rot_row_yz (each row's components 1 and 2).
void matrix_rotate_rows_yz(float turns, float* m)
{
    rot_row_yz(turns, m + 0);   // row 0: elements 0,1,2
    rot_row_yz(turns, m + 4);   // row 1: elements 4,5,6
    rot_row_yz(turns, m + 8);   // row 2: elements 8,9,10
}

// (0x0048cad0) - Rotate the three rows of a 3x4 float matrix about the axis
// handled by rot_row_xz (each row's components 0 and 2).
void matrix_rotate_rows_xz(float turns, float* m)
{
    rot_row_xz(turns, m + 0);
    rot_row_xz(turns, m + 4);
    rot_row_xz(turns, m + 8);
}

