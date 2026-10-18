// Static batching of the full-map extra sections (Vanilla profile only - merged meshes change the RTX Remix hashes).
// Spec: docs/BATCHING_SPEC.md. Measured 2026-10-17: up to 35 000 draws per frame, ~1.6 us CPU each - the frame time
// is the draw submission, the GPU idles. The extras are static world geometry, so it is merged once, in the loading
// screen, into a few thousand meshes of our own:
//   - group = (512 m cell of the section centre, size class of the instance radius, effect WORLD / WORLDREFLECT),
//     one mesh per group (split at 65 534 vertices / 256 textures), vertices pre-transformed to world space;
//   - one part per run of consecutive sections that are extras in the current zone and per TextureInfo;
//   - drawn by the game itself: a normal 12-byte entry {model | clip state, 0, &fake instance} in the player view's
//     visibility list - SceneryRender -> Render (identity matrix, per-part frustum test) -> render lists -> flush,
//     with the game's own effect, texture, blend and fog handling.
// Never batched (per instance, as before): flags 0x02/0x04/0x08/0x10, every instance of the override table (groups,
// smokeables, respawn toggle 0x10 at run time), marker flares, solid 0x40 with matrix, CAR/WORLDBONE meshes, part flag
// bit 0, textures still the default stand-in. Those run through AddInstance like any instance.
// Swaying trees (BatchTrees): own groups; a tree group nearer than kTreeNear draws its trees per instance (swaying),
// farther away the merged mesh without sway - nobody sees the sway at that distance.
// Safety: our model/solid/mesh live in our memory only (never in a game list, so the game can neither rebind nor
// free them); a texture pack being unloaded kills every mesh using one of its textures, its sections fall back to the
// per-instance walk; a section whose header changed falls back too. Rebuilt in every loading screen.
#include "core.h"
#include <d3d9.h>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <cmath>
#include <new>

namespace {
constexpr int   kMaxSection = 0xA8C;
constexpr DWORD kMaxVerts   = 65534;        // FlushRenderLists 0x5CF655: drawn only if mesh+0x34 < 0xFFFF
constexpr int   kStride     = 0x18;         // WORLD / WORLDREFLECT: float3 pos, D3DCOLOR, float2 uv
constexpr float kCell       = 512.0f;
const float     kClassHi[5] = { 8.f, 16.f, 32.f, 64.f, 0.f };   // r = radius + 6 (upper edges, last class open)
constexpr float kTreeNear   = 200.0f;       // tree groups nearer than this (nearest point of the group box): per instance, swaying

struct eModelS { void* next; void* prev; DWORD hash; void* solid; void* repl; int replCount; };
struct eMeshS {
    void* next; void* prev; DWORD ten; DWORD meshFlags; int partCount; void* parts; WORD* indexBase; DWORD indexBytes;
    IDirect3DIndexBuffer9* ib; int triCount; void* vertexData; IDirect3DVertexBuffer9* vb; DWORD vertexBytes; DWORD vertexCount;
    const int* effectRemap; void* effect; DWORD dirty;
};
static_assert(sizeof(eMeshS) == 0x44, "mesh");
struct ePartS {
    float mn[3]; DWORD indexCount; float mx[3]; BYTE texSlot; BYTE pad1D[3]; BYTE lmSlot; BYTE pad21[0x13]; WORD* firstIndex; DWORD partFlags;
};
static_assert(sizeof(ePartS) == 0x3C, "part");
struct eSolidTexS { DWORD hash; void* ti; };
struct eSolidS {
    eMeshS* mesh; void* next; void* prev; BYTE version, loaded; WORD flags; DWORD nameHash; short triCount; BYTE pad16[3];
    BYTE numTex, numLm, numMarkers; BYTE pad1C[4]; float bmin[3]; eSolidTexS* texTable; float bmax[3]; void* lmTable;
    float pivot[16]; void* markers; void* smoothing; void* boundNext; void* boundPrev; void* c13401B; float vol, dens; BYTE pad9C[8]; char name[0x1C];
};
static_assert(sizeof(eSolidS) == 0xC0, "solid");

struct Contrib { short section; BYTE slot; DWORD first, count; float mn[3], mx[3]; };

struct Batch {                       // allocated 16-aligned (eSolid is 16-aligned in the game)
    eSolidS solid;
    eMeshS  mesh;
    eModelS model;
    eSolidTexS tex[256];
    int   numTex = 0, effectId = 0, group = 0;
    float rHi = 0;                   // class distance edge, 0 = unlimited
    float mn[3], mx[3];              // AABB of the active parts
    bool  dead = false;
    DWORD bytes = 0;
    std::vector<Contrib> contribs;   // sorted by slot, then section; consecutive = contiguous in the index buffer
    std::vector<ePartS>  parts;      // reserved to contribs.size(): data() never moves
};

struct SwayInst { WORD inst; int group; };
struct SecInfo { BYTE* hdr = nullptr; bool batched = false, fallback = false; std::vector<WORD> leftover; std::vector<SwayInst> sway; };

// per group of the build: box over all its instances, tree flag, near flag of the current frame
struct GroupRt { float mn[3], mx[3]; bool sway, isNear; };
std::vector<GroupRt> g_groups;

std::vector<Batch*> g_batches;
SecInfo g_sec[kMaxSection];
BYTE    g_active[kMaxSection];
bool    g_built = false;
DWORD   g_sig = 0, g_lastSig = 0xFFFFFFFF;
BatchStats g_st = {};

typedef void  (__thiscall* AddInstance_t)(void* hdr, int idx, BYTE* ci, int state);
typedef int   (__thiscall* VisState_t)(void* view, const float* mn, const float* mx, void* m);
typedef void* (__cdecl*    GetView_t)(int i);
const AddInstance_t kAddInstance = reinterpret_cast<AddInstance_t>(0x005D6570);
const VisState_t    kVisState    = reinterpret_cast<VisState_t>(0x0048AC60);
const GetView_t     kGetView     = reinterpret_cast<GetView_t>(0x0048B1E0);

BYTE g_fakeInst[0x40];               // SceneryRender reads +0x1A only (0x0100 = world piece) and clears bit 0x8000

void Grow(float* mn, float* mx, const float* a, const float* b)
{
    for (int k = 0; k < 3; ++k) { if (a[k] < mn[k]) mn[k] = a[k]; if (b[k] > mx[k]) mx[k] = b[k]; }
}

void FreeBatch(Batch* b)
{
    if (b->mesh.vb) b->mesh.vb->Release();
    if (b->mesh.ib) b->mesh.ib->Release();
    b->~Batch();
    _aligned_free(b);
}

void KillBatch(Batch* b)
{
    if (b->dead) return;
    b->dead = true;
    b->mesh.partCount = 0;
    if (b->mesh.vb) { b->mesh.vb->Release(); b->mesh.vb = nullptr; }
    if (b->mesh.ib) { b->mesh.ib->Release(); b->mesh.ib = nullptr; }
    for (const Contrib& c : b->contribs) if (!g_sec[c.section].fallback) { g_sec[c.section].fallback = true; ++g_st.fallbackSections; }
    ++g_st.deadMeshes;
    g_lastSig = 0xFFFFFFFF;
}

void FreeAll()
{
    for (Batch* b : g_batches) FreeBatch(b);
    g_batches.clear();
    for (SecInfo& s : g_sec) { s.hdr = nullptr; s.batched = s.fallback = false; std::vector<WORD>().swap(s.leftover); std::vector<SwayInst>().swap(s.sway); }
    std::vector<GroupRt>().swap(g_groups);
    g_built = false; g_lastSig = 0xFFFFFFFF;
    g_st = BatchStats{};
}

// --- build -----------------------------------------------------------------------------------------
struct Item {
    int group; void* ti; short section; WORD inst; WORD part;
    const BYTE* instPtr; const BYTE* mesh; const BYTE* partPtr; DWORD nv; float mn[3], mx[3];
};

struct Group { DWORD key; int items = 0, distinctTi = 0; double bytes = 0; bool accepted = false, sway = false; int cls = 0, eff = 0; };

// instance -> world: p' = x*row0 + y*row1 + z*row2 + T (SceneryInstance_BuildRotation 0x5D6240 / SetPosition 0x5D6210)
struct Xform { float r[3][3]; float t[3]; bool ident; };
void MakeXform(const BYTE* inst, Xform& x)
{
    x.ident = (inst[0x1B] & 1) != 0;
    const short* s = reinterpret_cast<const short*>(inst + 0x2C);
    for (int k = 0; k < 3; ++k) for (int j = 0; j < 3; ++j) x.r[k][j] = s[3 * k + j] * (1.0f / 8192.0f);
    const float* t = reinterpret_cast<const float*>(inst + 0x20);
    x.t[0] = t[0]; x.t[1] = t[1]; x.t[2] = t[2];
}
void Apply(const Xform& x, const float* p, float* o)
{
    if (x.ident) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; return; }
    for (int j = 0; j < 3; ++j) o[j] = p[0] * x.r[0][j] + p[1] * x.r[1][j] + p[2] * x.r[2][j] + x.t[j];
}

// sysmem vertex data of a game mesh: +0x28, else a read-only lock of its (managed) VB
const BYTE* MeshVerts(const BYTE* mesh, bool& locked)
{
    locked = false;
    const BYTE* v = *reinterpret_cast<const BYTE* const*>(mesh + 0x28);
    if (v) return v;
    IDirect3DVertexBuffer9* vb = *reinterpret_cast<IDirect3DVertexBuffer9* const*>(mesh + 0x2C);
    void* p = nullptr;
    if (vb && SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_READONLY))) { locked = true; return static_cast<const BYTE*>(p); }
    return nullptr;
}
void MeshUnlock(const BYTE* mesh, bool locked)
{
    if (locked) (*reinterpret_cast<IDirect3DVertexBuffer9* const*>(mesh + 0x2C))->Unlock();
}

WORD  g_stamp[65536];
WORD  g_remap[65536];
WORD  g_gen = 0;
void NextGen() { if (++g_gen == 0) { memset(g_stamp, 0, sizeof(g_stamp)); g_gen = 1; } }

// unique vertices of one part (all indices < vertexCount, else -1)
int CountVerts(const BYTE* mesh, const BYTE* part)
{
    const DWORD vc = *reinterpret_cast<const DWORD*>(mesh + 0x34);
    const DWORD ic = *reinterpret_cast<const DWORD*>(part + 0x0C);
    const WORD* idx = *reinterpret_cast<const WORD* const*>(part + 0x34);
    if (!idx) return -1;
    NextGen();
    int n = 0;
    for (DWORD i = 0; i < ic; ++i) {
        const WORD v = idx[i];
        if (v >= vc) return -1;
        if (g_stamp[v] != g_gen) { g_stamp[v] = g_gen; ++n; }
    }
    return n;
}

struct Builder {
    IDirect3DDevice9* dev;
    Batch* cur = nullptr;
    std::vector<BYTE> vtx;
    std::vector<WORD> idx;
    double totalBytes = 0;

    void Start(const Group& g, int gi)
    {
        void* mem = _aligned_malloc(sizeof(Batch), 16);
        cur = new (mem) Batch();
        memset(&cur->solid, 0, sizeof(cur->solid)); memset(&cur->mesh, 0, sizeof(cur->mesh)); memset(&cur->model, 0, sizeof(cur->model));
        cur->effectId = g.eff; cur->rHi = kClassHi[g.cls]; cur->group = gi;
        vtx.clear(); idx.clear();
    }
    bool Finish()
    {
        Batch* b = cur; cur = nullptr;
        if (!b) return true;
        const DWORD nv = static_cast<DWORD>(vtx.size() / kStride), ni = static_cast<DWORD>(idx.size());
        bool ok = nv && ni;
        IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; void* p = nullptr;
        if (ok) ok = SUCCEEDED(dev->CreateVertexBuffer(nv * kStride, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr)) &&
                     SUCCEEDED(vb->Lock(0, 0, &p, 0));
        if (ok) { memcpy(p, vtx.data(), vtx.size()); vb->Unlock(); }
        if (ok) ok = SUCCEEDED(dev->CreateIndexBuffer(ni * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr)) &&
                     SUCCEEDED(ib->Lock(0, 0, &p, 0));
        if (ok) { memcpy(p, idx.data(), ni * 2); ib->Unlock(); }
        b->mesh.vb = vb; b->mesh.ib = ib;          // released by FreeBatch / KillBatch whatever happened
        // our objects: self-linked, never in a game list (unbound models 0x82DBC0, all meshes 0x872ED0, loaded solids 0x835794)
        b->mesh.next = b->mesh.prev = &b->mesh;
        b->mesh.ten = 0x10; b->mesh.meshFlags = b->effectId == 2 ? 0x40 : 0;
        b->mesh.indexBase = nullptr; b->mesh.indexBytes = ni * 2; b->mesh.triCount = static_cast<int>(ni / 3);
        b->mesh.vertexData = nullptr; b->mesh.vertexBytes = nv * kStride; b->mesh.vertexCount = nv;
        b->solid.mesh = &b->mesh; b->solid.next = b->solid.prev = &b->solid;
        b->solid.version = 0x16; b->solid.loaded = 1; b->solid.numTex = static_cast<BYTE>(b->numTex);
        b->solid.texTable = b->tex; b->solid.boundNext = b->solid.boundPrev = &b->solid.boundNext;
        b->model.next = b->model.prev = &b->model; b->model.solid = &b->solid;
        b->bytes = nv * kStride + ni * 2;
        b->parts.reserve(b->contribs.size());
        g_batches.push_back(b);
        if (!ok) { KillBatch(b); return false; }
        totalBytes += b->bytes;
        return true;
    }
    void Add(const Item& it, const Group& g)
    {
        const DWORD ic = *reinterpret_cast<const DWORD*>(it.partPtr + 0x0C);
        if (!cur || vtx.size() / kStride + it.nv > kMaxVerts || (cur->numTex && cur->tex[cur->numTex - 1].ti != it.ti && cur->numTex == 256)) {
            Finish(); Start(g, it.group);
        }
        if (!cur->numTex || cur->tex[cur->numTex - 1].ti != it.ti) { cur->tex[cur->numTex].hash = 0; cur->tex[cur->numTex].ti = it.ti; ++cur->numTex; }
        const BYTE slot = static_cast<BYTE>(cur->numTex - 1);
        const DWORD first = static_cast<DWORD>(idx.size());
        if (!cur->contribs.empty() && cur->contribs.back().slot == slot && cur->contribs.back().section == it.section) {
            Contrib& c = cur->contribs.back(); c.count += ic; Grow(c.mn, c.mx, it.mn, it.mx);
        } else {
            Contrib c; c.section = it.section; c.slot = slot; c.first = first; c.count = ic;
            memcpy(c.mn, it.mn, 12); memcpy(c.mx, it.mx, 12);
            cur->contribs.push_back(c);
        }
        Xform x; MakeXform(it.instPtr, x);
        bool locked; const BYTE* src = MeshVerts(it.mesh, locked);
        const WORD* sidx = *reinterpret_cast<const WORD* const*>(it.partPtr + 0x34);
        const WORD base = static_cast<WORD>(vtx.size() / kStride);
        NextGen();
        WORD n = 0;
        for (DWORD i = 0; i < ic; ++i) {
            const WORD v = sidx[i];
            if (g_stamp[v] != g_gen) {
                g_stamp[v] = g_gen; g_remap[v] = static_cast<WORD>(base + n++);
                const size_t o = vtx.size(); vtx.resize(o + kStride);
                BYTE* d = vtx.data() + o;
                memcpy(d, src + v * kStride, kStride);
                Apply(x, reinterpret_cast<const float*>(src + v * kStride), reinterpret_cast<float*>(d));
            }
            idx.push_back(g_remap[v]);
        }
        MeshUnlock(it.mesh, locked);
    }
};

bool IsBatchable(const BYTE* inst, const BYTE* model, std::vector<const BYTE*>& partsOut, const BYTE*& meshOut, bool& sway)
{
    partsOut.clear();
    sway = false;
    const WORD fl = *reinterpret_cast<const WORD*>(inst + 0x1A);
    if (fl & 0x1E) return false;                                            // hidden / never in the player view / direction
    const bool ident = (fl & 0x100) != 0;
    if (*reinterpret_cast<void* const*>(model + 0x10)) return false;        // texture replacement table
    const DWORD mh = *reinterpret_cast<const DWORD*>(model + 0x08);
    if (mh == 0xBE43EDBB || mh == 0x72923C81) return false;                 // SKYDOME / no-model hashes
    const BYTE* solid = *reinterpret_cast<const BYTE* const*>(model + 0x0C);
    if (!solid) return false;
    const BYTE sflags = solid[0x0E];
    if (!ident && ((sflags & 0x40) || static_cast<signed char>(solid[0x1B]) > 0)) return false;   // light context / marker flares
    if (!ident && (sflags & 0x80)) { if (!g_cfg.batchTrees) return false; sway = true; }       // swaying tree: own groups, near = per instance
    const BYTE* mesh = *reinterpret_cast<const BYTE* const*>(solid);
    if (!mesh) return false;
    if (*reinterpret_cast<const DWORD*>(mesh + 0x0C) & 0x81) return false;  // WORLDBONE / CAR
    const BYTE* eff = *reinterpret_cast<const BYTE* const*>(mesh + 0x3C);
    if (!eff) return false;
    const int effId = *reinterpret_cast<const int*>(eff + 4);
    if (effId != 0 && effId != 2) return false;
    const DWORD vc = *reinterpret_cast<const DWORD*>(mesh + 0x34);
    if (!vc || vc >= 0xFFFF || *reinterpret_cast<const DWORD*>(mesh + 0x30) < vc * kStride) return false;
    const eSolidTexS* tt = *reinterpret_cast<const eSolidTexS* const*>(solid + 0x2C);
    const BYTE numTex = solid[0x19];
    const int pc = *reinterpret_cast<const int*>(mesh + 0x10);
    const BYTE* parts = *reinterpret_cast<const BYTE* const*>(mesh + 0x14);
    if (!tt || pc <= 0 || !parts) return false;
    for (int i = 0; i < pc; ++i) {
        const BYTE* p = parts + i * 0x3C;
        if (*reinterpret_cast<const DWORD*>(p + 0x38) & 1) return false;    // second matrix / bones
        if (!*reinterpret_cast<const DWORD*>(p + 0x0C)) continue;           // draws nothing
        const BYTE slot = p[0x1C];
        if (slot >= numTex) return false;
        const void* ti = tt[slot].ti;
        if (!ti || ti == reinterpret_cast<const void*>(0x0082DB08)) return false;   // default stand-in, patched later
        partsOut.push_back(p);
    }
    meshOut = mesh;
    return !partsOut.empty();
}

int ClassOf(float r) { for (int c = 0; c < 4; ++c) if (r <= kClassHi[c]) return c; return 4; }

void Build(const BYTE* isPan, const BYTE* isNever)
{
    const DWORD t0 = GetTickCount();
    IDirect3DDevice9* dev = *reinterpret_cast<IDirect3DDevice9* const*>(0x00870974);
    if (!dev) { Log("[batch] no device - off"); return; }

    std::unordered_set<DWORD> overrides;                                    // SceneryOverrideInfo {s16 section, s16 instance, u16 flags}
    if (const BYTE* ot = *reinterpret_cast<const BYTE* const*>(0x00883DF0)) {
        const int on = *reinterpret_cast<const int*>(0x00883DF4);
        for (int i = 0; i < on && i < 65536; ++i) {
            const short* e = reinterpret_cast<const short*>(ot + i * 8);
            overrides.insert((static_cast<DWORD>(static_cast<WORD>(e[0])) << 16) | static_cast<WORD>(e[1]));
        }
    }

    std::vector<Item> items; items.reserve(131072);
    std::vector<Group> groups;
    std::unordered_map<DWORD, int> groupOf;
    std::vector<const BYTE*> parts;
    std::vector<int> instItems;                                              // per section: first item of each instance or -1
    BYTE seen[kMaxSection] = {};
    int sections = 0, instTotal = 0;
    const BYTE* head = reinterpret_cast<const BYTE*>(0x0088EAD4);
    int guard = 0;
    for (const BYTE* node = *reinterpret_cast<const BYTE* const*>(head); node && node != head && guard < 8192; node = *reinterpret_cast<const BYTE* const*>(node), ++guard) {
        const int s = *reinterpret_cast<const int*>(node + 0x0C);
        if (s < 0 || s >= kMaxSection || s / 100 == 26 || seen[s] || isPan[s] || isNever[s]) continue;
        seen[s] = 1;
        BYTE* slotp = reinterpret_cast<BYTE* const*>(0x00883EC0)[s];
        BYTE* hdr = slotp ? *reinterpret_cast<BYTE* const*>(slotp + 4) : nullptr;
        if (!hdr) continue;
        const BYTE* tree = *reinterpret_cast<const BYTE* const*>(hdr + 0x28);
        if (!tree) continue;
        SecInfo& si = g_sec[s];
        si.hdr = hdr;
        ++sections;
        const float* tb = reinterpret_cast<const float*>(tree);
        const int cx = static_cast<int>(floorf((tb[0] + tb[3]) * 0.5f / kCell)), cy = static_cast<int>(floorf((tb[1] + tb[4]) * 0.5f / kCell));
        const BYTE* infos = *reinterpret_cast<const BYTE* const*>(hdr + 0x18);
        const int infoN = *reinterpret_cast<const int*>(hdr + 0x1C);
        const BYTE* insts = *reinterpret_cast<const BYTE* const*>(hdr + 0x20);
        const int instN = *reinterpret_cast<const int*>(hdr + 0x24);
        for (int i = 0; i < instN && i < 65535; ++i) {
            const BYTE* inst = insts + i * 0x40;
            ++instTotal;
            const int ii = *reinterpret_cast<const short*>(inst + 0x18);
            bool ok = ii >= 0 && ii < infoN && !overrides.count((static_cast<DWORD>(s) << 16) | static_cast<WORD>(i));
            const BYTE* info = ok ? infos + ii * 0x44 : nullptr;
            const BYTE* model = ok ? LodModelForBatch(info) : nullptr;
            const BYTE* mesh = nullptr;
            bool sway = false;
            ok = model && IsBatchable(inst, model, parts, mesh, sway);
            if (ok) {
                const float r = *reinterpret_cast<const float*>(info + 0x3C) + 6.0f;
                const int cls = ClassOf(r);
                const int eff = *reinterpret_cast<const int*>(*reinterpret_cast<const BYTE* const*>(mesh + 0x3C) + 4);
                const DWORD key = (static_cast<DWORD>(cx & 0x3FF) << 22) | (static_cast<DWORD>(cy & 0x3FF) << 12) | (cls << 4) | (sway ? 8 : 0) | eff;
                auto gi = groupOf.find(key);
                int g;
                if (gi == groupOf.end()) { g = static_cast<int>(groups.size()); groupOf[key] = g; Group ng; ng.key = key; ng.cls = cls; ng.eff = eff; ng.sway = sway; groups.push_back(ng); }
                else g = gi->second;
                Xform x; MakeXform(inst, x);
                const size_t firstItem = items.size();
                for (const BYTE* p : parts) {
                    const int nv = CountVerts(mesh, p);
                    if (nv < 0) { ok = false; break; }
                    Item it; it.group = g; it.ti = reinterpret_cast<const eSolidTexS* const*>(
                        *reinterpret_cast<const BYTE* const*>(model + 0x0C) + 0x2C)[0][p[0x1C]].ti;
                    it.section = static_cast<short>(s); it.inst = static_cast<WORD>(i); it.part = static_cast<WORD>((p - *reinterpret_cast<const BYTE* const*>(mesh + 0x14)) / 0x3C);
                    it.instPtr = inst; it.mesh = mesh; it.partPtr = p; it.nv = static_cast<DWORD>(nv);
                    // world AABB of the part: its 8 local corners transformed
                    const float* lmn = reinterpret_cast<const float*>(p); const float* lmx = reinterpret_cast<const float*>(p + 0x10);
                    it.mn[0] = it.mn[1] = it.mn[2] = 1e30f; it.mx[0] = it.mx[1] = it.mx[2] = -1e30f;
                    for (int c = 0; c < 8; ++c) {
                        const float lc[3] = { (c & 1) ? lmx[0] : lmn[0], (c & 2) ? lmx[1] : lmn[1], (c & 4) ? lmx[2] : lmn[2] };
                        float w[3]; Apply(x, lc, w); Grow(it.mn, it.mx, w, w);
                    }
                    items.push_back(it);
                }
                if (!ok) items.resize(firstItem);
            }
            if (!ok) si.leftover.push_back(static_cast<WORD>(i));
        }
    }

    // budget: groups by saved draws per MB, as many as fit (the rest stays per instance)
    for (const Item& it : items) { Group& g = groups[it.group]; ++g.items; g.bytes += it.nv * kStride + *reinterpret_cast<const DWORD*>(it.partPtr + 0x0C) * 2.0; }
    {
        std::vector<std::pair<int, void*>> gt; gt.reserve(items.size());
        for (const Item& it : items) gt.push_back({ it.group, it.ti });
        std::sort(gt.begin(), gt.end());
        for (size_t i = 0; i < gt.size(); ++i) if (i == 0 || gt[i] != gt[i - 1]) ++groups[gt[i].first].distinctTi;
    }
    std::vector<int> order(groups.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return (groups[a].items - groups[a].distinctTi) / (groups[a].bytes + 1) > (groups[b].items - groups[b].distinctTi) / (groups[b].bytes + 1);
    });
    const double budget = g_cfg.batchBudgetMB * 1048576.0;
    double used = 0;
    for (int gi : order) {
        Group& g = groups[gi];
        if (g.items - g.distinctTi <= 0) continue;                          // nothing saved
        if (used + g.bytes > budget) continue;
        g.accepted = true; used += g.bytes;
    }
    // instances of rejected groups -> per instance (all parts of an instance share its group)
    std::vector<Item> keep; keep.reserve(items.size());
    for (size_t i = 0; i < items.size(); ) {
        size_t j = i;
        while (j < items.size() && items[j].section == items[i].section && items[j].inst == items[i].inst) ++j;
        if (groups[items[i].group].accepted) {
            keep.insert(keep.end(), items.begin() + i, items.begin() + j);
            if (groups[items[i].group].sway) g_sec[items[i].section].sway.push_back({ items[i].inst, items[i].group });
        } else g_sec[items[i].section].leftover.push_back(items[i].inst);
        i = j;
    }
    std::vector<Item>().swap(items);
    std::sort(keep.begin(), keep.end(), [](const Item& a, const Item& b) {
        if (a.group != b.group) return a.group < b.group;
        if (a.ti != b.ti) return a.ti < b.ti;
        if (a.section != b.section) return a.section < b.section;
        if (a.inst != b.inst) return a.inst < b.inst;
        return a.part < b.part;
    });

    g_groups.resize(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        GroupRt& r = g_groups[i]; r.sway = groups[i].sway; r.isNear = false;
        r.mn[0] = r.mn[1] = r.mn[2] = 1e30f; r.mx[0] = r.mx[1] = r.mx[2] = -1e30f;
    }
    for (const Item& it : keep) Grow(g_groups[it.group].mn, g_groups[it.group].mx, it.mn, it.mx);
    Builder bl; bl.dev = dev;
    int lastGroup = -1;
    for (const Item& it : keep) {
        if (it.group != lastGroup) { bl.Finish(); bl.Start(groups[it.group], it.group); lastGroup = it.group; }
        bl.Add(it, groups[it.group]);
        g_sec[it.section].batched = true;
    }
    bl.Finish();
    for (SecInfo& si : g_sec) { std::sort(si.leftover.begin(), si.leftover.end()); si.leftover.erase(std::unique(si.leftover.begin(), si.leftover.end()), si.leftover.end()); }
    int batchedSecs = 0;
    for (const SecInfo& si : g_sec) batchedSecs += si.batched;
    g_built = !g_batches.empty();
    g_st.meshes = static_cast<int>(g_batches.size());
    g_st.mb = static_cast<float>(bl.totalBytes / 1048576.0);
    g_st.buildMs = GetTickCount() - t0;
    {
        std::unordered_set<DWORD> inst;
        for (const Item& it : keep) inst.insert((static_cast<DWORD>(it.section) << 16) | it.inst);
        g_st.batchedInstances = static_cast<int>(inst.size());
    }
    Log("[batch] built: %d meshes, %.1f MB (budget %d MB), %d of %d instances batched in %d of %d sections, %d groups (%d over budget), %u ms",
        g_st.meshes, g_st.mb, g_cfg.batchBudgetMB, g_st.batchedInstances, instTotal, batchedSecs, sections,
        static_cast<int>(groups.size()), static_cast<int>(std::count_if(groups.begin(), groups.end(), [](const Group& g) { return !g.accepted && g.items > g.distinctTi; })),
        g_st.buildMs);
}

void RebuildParts()
{
    ++g_st.partRebuilds;
    int total = 0;
    for (Batch* b : g_batches) {
        if (b->dead) continue;
        b->parts.clear();
        b->mn[0] = b->mn[1] = b->mn[2] = 1e30f; b->mx[0] = b->mx[1] = b->mx[2] = -1e30f;
        int prev = -2;
        for (int i = 0; i < static_cast<int>(b->contribs.size()); ++i) {
            const Contrib& c = b->contribs[i];
            if (!g_active[c.section]) continue;
            if (prev == i - 1 && b->contribs[prev].slot == c.slot) {
                ePartS& p = b->parts.back(); p.indexCount += c.count; Grow(p.mn, p.mx, c.mn, c.mx);
            } else {
                ePartS p; memset(&p, 0, sizeof(p));
                memcpy(p.mn, c.mn, 12); memcpy(p.mx, c.mx, 12);
                p.indexCount = c.count; p.texSlot = c.slot; p.lmSlot = 0xFF;
                p.firstIndex = reinterpret_cast<WORD*>(static_cast<uintptr_t>(c.first) * 2);   // mesh+0x18 = 0: StartIndex = first
                b->parts.push_back(p);
            }
            Grow(b->mn, b->mx, c.mn, c.mx);
            prev = i;
        }
        b->mesh.parts = b->parts.data();
        b->mesh.partCount = static_cast<int>(b->parts.size());
        total += b->mesh.partCount;
    }
    g_st.activeParts = total;
}

bool g_view = false;   // the current DrawVisibleSections call batches
bool g_installed = false;
} // namespace

bool BatchBegin(BYTE* ci)
{
    g_view = false;
    if (!g_built || !g_installed || !g_cfg.staticBatching || g_cfg.profile != PROFILE_VANILLA) return false;
    if (*reinterpret_cast<const int*>(0x0089E7C0) == 2) return false;      // split screen: per instance
    if (*reinterpret_cast<void* const*>(ci + 0x40) != kGetView(1)) return false;   // main player view only (parts are per frame)
    memset(g_active, 0, sizeof(g_active));
    g_sig = 2166136261u;
    g_st.leftoverCalls = 0;
    // tree groups near the camera: their trees per instance (swaying) this frame, the merged mesh is skipped
    const float* cam = reinterpret_cast<const float*>(ci + 0x60);
    int nearTrees = 0;
    for (GroupRt& r : g_groups) {
        if (!r.sway) continue;
        float d2 = 0;
        for (int k = 0; k < 3; ++k) { const float d = cam[k] < r.mn[k] ? r.mn[k] - cam[k] : (cam[k] > r.mx[k] ? cam[k] - r.mx[k] : 0.f); d2 += d * d; }
        r.isNear = d2 < kTreeNear * kTreeNear;
        nearTrees += r.isNear;
    }
    g_st.nearTreeGroups = nearTrees;
    g_view = true;
    return true;
}

bool BatchTakeSection(int s, BYTE* hdr, BYTE* ci)
{
    if (!g_view || s < 0 || s >= kMaxSection) return false;
    SecInfo& si = g_sec[s];
    if (!si.batched || si.fallback) return false;
    if (hdr != si.hdr) { si.fallback = true; ++g_st.fallbackSections; ++g_st.staleSections; return false; }   // reloaded: per instance from now on
    g_active[s] = 1;
    g_sig = (g_sig ^ static_cast<DWORD>(s)) * 16777619u;
    for (WORD i : si.leftover) kAddInstance(hdr, i, ci, 1);
    g_st.leftoverCalls += static_cast<int>(si.leftover.size());
    for (const SwayInst& t : si.sway) if (g_groups[t.group].isNear) { kAddInstance(hdr, t.inst, ci, 1); ++g_st.leftoverCalls; }
    return true;
}

void BatchEmit(BYTE* ci)
{
    if (!g_view) return;
    g_view = false;
    if (g_sig != g_lastSig) { RebuildParts(); g_lastSig = g_sig; }
    void* view = *reinterpret_cast<void* const*>(ci + 0x40);
    const float* cam = reinterpret_cast<const float*>(ci + 0x60);
    const float scale = *reinterpret_cast<const float*>(ci + 0x80);
    int limit = g_cfg.extraMinPx > g_cfg.worldMinPx ? g_cfg.extraMinPx : g_cfg.worldMinPx;
    if (limit < 1) limit = 1;
    const float distK = 1.0f + scale / static_cast<float>(limit);
    void* const* effects = reinterpret_cast<void* const*>(0x0086EB28);
    int emitted = 0, distCulled = 0, frustum = 0;
    for (Batch* b : g_batches) {
        if (b->dead || !b->mesh.partCount) continue;
        if (g_groups[b->group].isNear) continue;   // near trees: drawn per instance (BatchTakeSection)
        if (b->rHi > 0) {
            float d2 = 0;
            for (int k = 0; k < 3; ++k) { const float d = cam[k] < b->mn[k] ? b->mn[k] - cam[k] : (cam[k] > b->mx[k] ? cam[k] - b->mx[k] : 0.f); d2 += d * d; }
            const float lim = b->rHi * distK;
            if (d2 > lim * lim) { ++distCulled; continue; }
        }
        const int state = kVisState(view, b->mn, b->mx, nullptr);
        if (!state) { ++frustum; continue; }
        b->mesh.effect = effects[b->effectId];
        if (!b->mesh.effect) continue;
        DWORD* e = *reinterpret_cast<DWORD**>(ci + 0x4C);
        if (reinterpret_cast<DWORD>(e) >= *reinterpret_cast<const DWORD*>(ci + 0x50)) { ++g_st.listFull; break; }
        *reinterpret_cast<DWORD**>(ci + 0x4C) = e + 3;
        e[0] = reinterpret_cast<DWORD>(&b->model) + static_cast<DWORD>(state);
        e[1] = 0;
        e[2] = reinterpret_cast<DWORD>(g_fakeInst);
        ++emitted;
    }
    g_st.emitted = emitted; g_st.distCulled = distCulled; g_st.frustumCulled = frustum;
    static DWORD lastLog = 0; const DWORD now = GetTickCount();
    if (now - lastLog >= 10000) {
        lastLog = now;
        Log("[batch] frame: meshes drawn %d, beyond class distance %d, outside frustum %d, leftover instances %d (tree groups near %d), active parts %d | %d meshes %.1f MB, part rebuilds %d, dead meshes %d, fallback sections %d (stale %d), list full %d",
            emitted, distCulled, frustum, g_st.leftoverCalls, g_st.nearTreeGroups, g_st.activeParts, g_st.meshes, g_st.mb, g_st.partRebuilds, g_st.deadMeshes,
            g_st.fallbackSections, g_st.staleSections, g_st.listFull);
    }
}

void BatchBuild(const BYTE* isPan, const BYTE* isNever)
{
    FreeAll();
    if (!g_installed || !g_cfg.staticBatching || g_cfg.profile != PROFILE_VANILLA) return;
    if (*reinterpret_cast<const int*>(0x0089E7C0) == 2) return;
    __try { Build(isPan, isNever); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[batch] build failed (exception) - per instance"); g_built = false; }
}

// texture pack chunk unloader 0x494DD0 (cdecl, chunk 0xB3300000): every mesh with a TextureInfo inside the chunk dies
// before the textures go - its sections are drawn per instance from then on
namespace {
typedef void (__cdecl* TexUnload_t)(BYTE* chunk);
TexUnload_t g_origTexUnload = nullptr;
void __cdecl TexUnloadHook(BYTE* chunk)
{
    if (!g_batches.empty() && chunk) {
        const DWORD lo = reinterpret_cast<DWORD>(chunk), hi = lo + 8 + *reinterpret_cast<const DWORD*>(chunk + 4);
        for (Batch* b : g_batches) {
            if (b->dead) continue;
            for (int i = 0; i < b->numTex; ++i) {
                const DWORD t = reinterpret_cast<DWORD>(b->tex[i].ti);
                if (t >= lo && t < hi) { KillBatch(b); break; }
            }
        }
    }
    g_origTexUnload(chunk);
}
} // namespace

bool BatchInstall()
{
    *reinterpret_cast<WORD*>(g_fakeInst + 0x1A) = 0x0100;
    static const BYTE orig[5] = { 0x51, 0x8B, 0x44, 0x24, 0x08 };
    int h = PatchAddHook("batching: texture pack unload", "batch", 0x00494DD0, orig, 5, reinterpret_cast<void*>(&TexUnloadHook),
                         reinterpret_cast<void**>(&g_origTexUnload), "meshes using textures of an unloaded pack are dropped first");
    if (h < 0 || !PatchIsOn(h)) { Log("[SKIP] static batching: texture unload hook failed - off"); return false; }
    g_installed = true;
    return true;
}

BatchStats BatchGet() { return g_st; }

