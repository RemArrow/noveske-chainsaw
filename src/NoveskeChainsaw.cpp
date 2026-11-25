// Noveske Chainsaw - GML plugin (com.remarrow.noveskechainsaw).
//
// Adds a "Noveske Chainsaw" entry to the Assault Rifle spawner, and makes the gun that entry
// spawns look like a Noveske - textured - WITHOUT replacing any game asset: the stock HK416
// entry, and every other HK416, stays an HK416.
//
// 1. Spawner entry: after BP_GameInstance::GetItems builds AvailableGuns, construct a new
//    GunAsset_C at runtime (no cooked DataAsset, no package), copy the HK416's settings and
//    append it to the Assault Rifle list.  10 -> 11 entries.  F8 re-runs it.
//
// 2. Visuals: the gun station spawns the entry's ReceiverClass (the HK416 blueprint, which
//    provides handling, grip points and attachment sockets). When a receiver begins play and
//    the station holding it as CurrentReceiver last had the Noveske entry picked in its spawner
//    widget, that one actor gets a Noveske DynamicMeshComponent (built at runtime from the
//    noveske.obj embedded in this DLL, textured with M_DefaultShader instances fed the embedded
//    <Part>_{Diffuse,Normal,ORM}.png) and its HK416 mesh is hidden. Collision, physics and grips
//    stay the HK416's; its attachment rails are moved onto the Noveske's, full handguard length.
//    The Noveske's own moving parts ride the bones the game poses on the HK416's movables skeleton
//    (bolt carrier, charging handle, dust cover, trigger, selector, bolt catch, mag releases), and
//    magazines in a Noveske - and the carrier's spares - become the pack's PMAG.
//    Guns the game rebuilds from your saved loadout (team room start, every operation) are
//    recognised through their save slot and dressed the same way (see "loadouts" below).
//
// 3. Probe (config [Debug] Probe = true): package-loading diagnostic - game packages vs the
//    never-registering new Noveske packages.
//
// Settings: GML\config\com.remarrow.noveskechainsaw.cfg. Build: build.bat [install].
// Assets: tools\prepare_runtime.py writes Assets\ from the purchased source art (not in the repo);
// tools\pack encrypts them into the payload that src\NoveskeChainsaw.rc builds into the DLL, which
// is all that gets installed. Nothing in the DLL is an extractable PNG or OBJ (src\payload.h).
#include <GML/GML.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <map>
#include "payload.h"
#include "payload_key.h"  // generated per build by tools\pack (build\gen), never committed
#include "version.h"
#pragma comment(lib, "user32.lib")  // F8 keybind

using namespace gml;

static constexpr uint8_t kAssaultRifle = 0;  // EGunType
static const char* kDonor = "/Game/Weapons/Data/DA_HK416a5_279.DA_HK416a5_279";
static const char* kDisplayName = "Noveske Chainsaw";

static GUObject* s_gun = nullptr;  // our runtime GunAsset

// ------------------------------------------------------------------ spawner entry

static GML_TArray* AssaultRifleList(Object gi) {
    GFProperty* mapProp = API->FindProperty(gi.Struct(), "AvailableGuns");
    void* map = gi.PropPtr("AvailableGuns");
    if (!mapProp || !map) return nullptr;
    struct Ctx { GML_TArray* found; GUStruct* valueStruct; } ctx{nullptr, nullptr};
    GML_PropInfo mi;
    API->GetPropertyInfo(mapProp, &mi);
    GML_PropInfo vi;
    API->GetPropertyInfo(mi.value, &vi);
    ctx.valueStruct = vi.structType;  // FGunAssetList
    API->MapForEach(map, mapProp, [](void* u, void* key, void* value) -> int {
        auto* c = (Ctx*)u;
        if (*(uint8_t*)key != kAssaultRifle) return 0;
        GFProperty* arr = API->FindProperty(c->valueStruct, "GunAssets");  // GUID-mangled in the BP struct
        GML_PropInfo ai;
        if (!arr || !API->GetPropertyInfo(arr, &ai)) return 1;
        c->found = (GML_TArray*)((uint8_t*)value + ai.offset);
        return 1;
    }, &ctx);
    return ctx.found;
}

static void AddGun(Object gi) {
    GML_TArray* raw = AssaultRifleList(gi);
    if (!raw) { Warn("AvailableGuns[AssaultRifle] not found yet"); return; }
    TArrayView<GUObject*> list{raw};
    if (s_gun && Object(s_gun) && list.Contains(s_gun)) return;  // GetItems ran again; already there
    for (GUObject* g : list) {  // e.g. the old UE4SS Lua mod is still installed
        char name[128];
        void* text = Object(g).PropPtr("DisplayName");
        if (text && API->TextToString(text, name, sizeof name) && !strcmp(name, kDisplayName)) {
            Log("'{}' already in the list (UE4SS Lua mod still active?) - not adding a duplicate", kDisplayName);
            return;
        }
    }

    Object donor = FindObject(kDonor);
    GUClass* gunClass = FindClass("GunAsset_C");
    if (!donor || !gunClass) { Error("donor {} or GunAsset_C missing", kDonor); return; }

    Object gun = NewObject(gunClass, gi);
    if (!gun) { Error("could not construct GunAsset_C"); return; }
    gun.Set("ReceiverClass", donor.Get<GUClass*>("ReceiverClass"));
    gun.Set("Icon", donor.Get<GUObject*>("Icon"));
    gun.SetBool("bEnabled", donor.GetBool("bEnabled"));
    gun.Set("Category", kAssaultRifle);
    uint8_t text[16];
    if (API->MakeText(kDisplayName, text)) std::memcpy(gun.PropPtr("DisplayName"), text, 16);

    int before = list.Num();
    if (list.Add(gun.ptr)) {
        s_gun = gun.ptr;
        Log("*** ADDED '{}' ({} -> {}) as {} ***", kDisplayName, before, list.Num(), gun.FullName());
    } else {
        Error("append failed");
    }
}

// ------------------------------------------------------------------ visuals

// The game's weapon shader and its three texture parameters (dumped from HK_416_279_v2's MICs).
static const char* kShader = "/Game/Art/Common/M_DefaultShader.M_DefaultShader";
// Texture sets / material slots; OBJ `usemtl` group names in kGroups (the pack's own material names).
static const char* kParts[] = {"Body", "Handguard", "Stock", "Grip", "Muzzle", "Feer", "PMag"};
static const char* kGroups[] = {"MI_Body", "MI_Handguard", "MI_Stock", "MI_Grip", "MI_Muzzle", "MI_Feer", "MI_PMAG_556"};
static constexpr int kNumParts = 7;

// The Noveske's own moving parts, each hung on the bone the game drives on the HK416's "movables"
// skeleton (GunReceiver's TriggerBoneName, ChargingHandleBoneName, ... name the same bones).
struct MovingPart { const char* asset; const char* bone; GUObject* mesh; };
static MovingPart s_movingParts[] = {
    {"noveske_bolt", "RootBoltCarrierGroup", nullptr},
    {"noveske_charginghandle", "RootChargingHandle", nullptr},
    {"noveske_dustcover", "RootDustCover", nullptr},
    {"noveske_trigger", "RootTrigger", nullptr},
    {"noveske_selector", "RootSelector", nullptr},
    {"noveske_boltcatch", "RootBoltRelease", nullptr},  // these three are cut out of the pack's body mesh
    {"noveske_magrelease", "RootMagRelease", nullptr},
    {"noveske_magreleaseambi", "RootMagReleaseAmbi", nullptr},
};

static bool s_visualsEnabled = true, s_ownMovingParts = true, s_ownMagazines = true;
static float s_offset[3] = {0, 0, 0};  // cm, added to the mesh after the axis conversion
static GUObject* s_mesh = nullptr;     // runtime UDynamicMesh (Geometry Scripting): body, handguard, stock, grip, FEER
static GUObject* s_muzzleMesh = nullptr;  // flash hider (hidden while a muzzle device is mounted)
static GUObject* s_pmagMesh = nullptr;    // the pack's PMAG, in gun space as if fully inserted
static constexpr int kPmagRounds = 4;
static GUObject* s_pmagRoundMesh[kPmagRounds] = {};  // the rounds the pack modelled at its top, top one first
static GUObject* s_mids[kNumParts] = {};  // one M_DefaultShader instance per texture set
static bool s_assetsTried = false;
static GUClass* s_receiverClass = nullptr;

struct Pending { GUObject* actor; ULONGLONG since; };
static std::vector<Pending> s_pending;

// Decrypts one asset from the payload into `out`. Callers wipe it as soon as the engine has it, so
// at most one asset exists in plain form, and only for that moment.
static bool Unpack(const char* name, std::vector<uint8_t>& out, bool optional = false) {
    static Blob blob = Resource("DATA");
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = kPayloadKeyA[i] ^ kPayloadKeyB[i];
    bool ok = blob && payload::Extract(blob.data, blob.size, key, name, out);
    SecureZeroMemory(key, sizeof key);
    if (!ok && !optional) Error("embedded asset '{}' is missing or damaged", name);
    return ok;
}

static bool BuildVisualAssets() {
    if (s_assetsTried) return s_mesh != nullptr;
    s_assetsTried = true;
    if (API->size < offsetof(GML_API, ImportDynamicMeshFromMemory) + sizeof(void*)) {
        Error("this plugin needs GML 2.2 or newer (assets embedded in the DLL)");
        return false;
    }
    Object shader = LoadObject(kShader);
    if (!shader) { Error("weapon shader {} not found", kShader); return false; }

    // Meshes and textures come from the encrypted payload inside this DLL, one at a time.
    std::vector<uint8_t> buf;
    for (int i = 0; i < kNumParts; i++) {
        s_mids[i] = API->CreateMaterialInstance(shader.ptr);
        if (!s_mids[i]) { Error("could not create a material instance for {}", kParts[i]); return false; }
        for (const char* param : {"Diffuse", "Normal", "ORM"}) {
            std::string res = std::string(kParts[i]) + "_" + param;
            GUObject* tex = Unpack(res.c_str(), buf) ? API->ImportTextureFromMemory(buf.data(), buf.size(), res.c_str()) : nullptr;
            payload::Wipe(buf);
            if (!tex || !API->SetMaterialTexture(s_mids[i], param, tex))
                Warn("{}: texture {} missing - that channel keeps the shader default", kParts[i], param);
        }
    }

    GML_MeshImport o{};
    o.size = sizeof o;
    o.axis = GML_AXIS_BLENDER_OBJ;  // same transform as the FBX import the alignment was tuned on
    std::memcpy(o.offset, s_offset, sizeof s_offset);
    o.flipWinding = -1;
    o.materialCount = kNumParts;
    o.materialNames = kGroups;  // OBJ usemtl group -> material ID i
    o.materials = s_mids;
    auto load = [&](const char* name, bool optional = false) -> GUObject* {
        if (!Unpack(name, buf, optional)) return nullptr;
        GUObject* m = API->ImportDynamicMeshFromMemory(buf.data(), buf.size(), name, &o);
        payload::Wipe(buf);
        if (!m) Error("could not build the Noveske mesh '{}'", name);
        return m;
    };
    s_mesh = load("noveske");
    s_muzzleMesh = load("noveske_muzzle");
    s_pmagMesh = load("noveske_pmag");
    for (int i = 0; i < kPmagRounds; i++) {  // cut out of the PMAG by tools\split_pmag.js
        std::string name = "noveske_pmag_round" + std::to_string(i + 1);
        s_pmagRoundMesh[i] = load(name.c_str(), true);
    }
    for (auto& p : s_movingParts) p.mesh = load(p.asset, true);  // a part the pack lacks stays the HK416's bone, unseen
    if (s_mesh) Log("Noveske visuals ready: {}", Object(s_mesh).FullName());
    return s_mesh != nullptr;
}

// ------------------------------------------------------------------ transforms (raw FTransform bytes)

using Xf = std::vector<uint8_t>;
static size_t XfSize() {
    static size_t size = 0;
    if (!size) {
        GML_PropInfo ri;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &ri);
        size = ri.size;
    }
    return size;
}
static Xf TakeXf(Params& p, const char* name = "ReturnValue") {
    auto* s = (uint8_t*)p.Ptr(name);
    return Xf(s, s + XfSize());
}
static void PutXf(Params& p, const char* name, const Xf& x) { std::memcpy(p.Ptr(name), x.data(), x.size()); }
static Xf XfCall(const char* fn, std::initializer_list<std::pair<const char*, const Xf*>> args) {
    Params p(Lib("KismetMathLibrary"), fn);
    for (auto& [n, x] : args) PutXf(p, n, *x);
    p.Call();
    return TakeXf(p);
}
static Xf Compose(const Xf& a, const Xf& b) { return XfCall("ComposeTransforms", {{"A", &a}, {"B", &b}}); }  // a then b
static Xf Invert(const Xf& t) { return XfCall("InvertTransform", {{"T", &t}}); }
static Xf RelativeTo(const Xf& a, const Xf& to) { return XfCall("MakeRelativeTransform", {{"A", &a}, {"RelativeTo", &to}}); }
static Xf WorldXf(Object obj) {  // actor or scene component
    Params p(obj, obj.IsA("Actor") ? "GetTransform" : "K2_GetComponentToWorld");
    p.Call();
    return TakeXf(p);
}
static void SetRelativeXf(Object comp, const Xf& x) {
    Params p(comp, "K2_SetRelativeTransform");
    PutXf(p, "NewTransform", x);
    p.Set("bSweep", false).Set("bTeleport", true).Call();
}
static void AttachTo(Object comp, Object parent, const char* socket) {
    Params at(comp, "K2_AttachToComponent");
    at.SetObj("Parent", parent).Set("SocketName", socket ? API->MakeName(socket) : uint64_t(0))
        .Set("LocationRule", (uint8_t)2).Set("RotationRule", (uint8_t)2).Set("ScaleRule", (uint8_t)2)  // SnapToTarget
        .Set("bWeldSimulatedBodies", false).Call();
}

// A bone's reference-pose transform in component space: its local ref transforms composed up the
// parent chain. The rest pose, whatever the gun is doing right now (bolt locked back, trigger held).
static Xf RefPoseComponent(Object skel, const char* bone) {
    Xf acc;
    uint64_t name = API->MakeName(bone);
    for (int guard = 0; guard < 64 && name; guard++) {
        Params bi(skel, "GetBoneIndex");
        *(uint64_t*)bi.Ptr("BoneName") = name;
        bi.Call();
        int idx = bi.Return<int32_t>();
        if (idx < 0) break;
        Params rp(skel, "GetRefPoseTransform");
        rp.Set("BoneIndex", (int32_t)idx).Call();
        Xf local = TakeXf(rp);
        acc = acc.empty() ? local : Compose(acc, local);  // child first, then its parent
        Params pb(skel, "GetParentBone");
        *(uint64_t*)pb.Ptr("BoneName") = name;
        pb.Call();
        name = pb.Return<uint64_t>();
    }
    return acc;
}

static void HideComponent(Object c) {
    Params v(c, "SetVisibility");
    v.Set("bNewVisibility", false).Set("bPropagateToChildren", false).Call();  // attached children stay visible
}

// Where attachments go. The HK416 receiver's attachment splines (Picatinny rails, M-LOK faces) and
// its muzzle attach point sit on the HK416's surfaces: the optic would float 4 mm over the Noveske's
// top rail and a foregrip hang 5.5 mm under its handguard. These are the same components moved onto
// the Noveske (start point, actor space, cm, for the default offset; tools/align prints this table).
struct AttachPoint { const char* name; double x, y, z; };
static const AttachPoint kAttachPoints[] = {
    {"ReceiverAttachmentSpline", -4.825, -0.026, 6.098},      // HK (0.000, 6.510)
    {"ReceiverAttachmentSplineHG12", 10.709, -0.026, 6.098},  // HK (0.000, 6.510)
    {"ReceiverAttachmentSplineHG45", 15.523, 1.427, 4.474},   // HK (1.592, 4.156)
    {"ReceiverAttachmentSplineHG9", 15.523, 2.028, 3.022},    // HK (2.100, 2.926)
    {"ReceiverAttachmentSplineHG135", 15.523, 1.427, 1.569},  // HK (1.592, 1.675)
    {"ReceiverAttachmentSplineHG6", 15.523, -0.026, 0.967},   // HK (0.000, 0.414)
    {"ReceiverAttachmentSplineHG-135", 15.523, -1.478, 1.569},  // HK (-1.592, 1.675)
    {"ReceiverAttachmentSplineHG-90", 15.523, -2.080, 3.022},   // HK (-2.100, 2.926)
    {"ReceiverAttachmentSplineHG-45", 15.523, -1.479, 4.474},   // HK (-1.592, 4.156)
    {"Fire Location", 37.013, -0.026, 3.022},  // muzzle attach + shot origin; HK (35.899, 0.000, 2.929)
};
static const float kDefaultOffset[3] = {2.154f, -0.466f, -0.909f};
static bool s_fitAttachments = true;

struct Vec3 { double x, y, z; };

// Actor-space attach point for table entry `p`: a non-default mesh offset moves the rails with it.
static Vec3 AttachTarget(const AttachPoint& p) {
    return {p.x + s_offset[0] - kDefaultOffset[0], p.y + s_offset[1] - kDefaultOffset[1], p.z + s_offset[2] - kDefaultOffset[2]};
}

// Actor transform <-> world, via KismetMathLibrary on the actor's FTransform.
struct ActorFrame {
    Params gt;
    size_t size;
    explicit ActorFrame(Object actor) : gt(actor, "GetTransform") {
        gt.Call();
        GML_PropInfo xf;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &xf);
        size = xf.size;
    }
    Vec3 Call(const char* fn, const char* arg, Vec3 v) {
        Params p(Lib("KismetMathLibrary"), fn);
        std::memcpy(p.Ptr("T"), gt.Ptr("ReturnValue"), size);
        p.Set(arg, v).Call();
        return p.Return<Vec3>();
    }
    Vec3 ToWorld(Vec3 local) { return Call("TransformLocation", "Location", local); }
    Vec3 ToLocal(Vec3 world) { return Call("InverseTransformLocation", "Location", world); }
    Vec3 DirToWorld(Vec3 local) { return Call("TransformDirection", "Direction", local); }
};

static const AttachPoint* FindAttachPoint(const std::string& componentName) {
    for (auto& p : kAttachPoints)
        if (componentName == p.name) return &p;
    return nullptr;
}

// Attachments already on the gun (a loadout restores them from their saved transforms) are snapped
// onto the Noveske's rail: their attach point keeps its position along the rail and moves onto the
// rail line; a muzzle device moves onto the muzzle point.
static int SnapMountedAttachments(Object gun, ActorFrame& frame) {
    Params ga(gun, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
    auto* arr = (GML_TArray*)ga.Ptr("OutActors");
    int snapped = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object att(((GUObject**)arr->Data)[i]);
        if (!att.IsA("GunAttachment")) continue;
        Object ap = att.GetObj("AttachPoint");
        if (!ap) continue;
        Params loc(ap, "K2_GetComponentLocation");
        loc.Call();
        Vec3 now = frame.ToLocal(loc.Return<Vec3>());
        const AttachPoint* rail = nullptr;
        for (Object c : {Object(att.GetObj("RootComponent").GetObj("AttachParent")), att.GetObj("AttachedSpline"),
                         att.GetObj("AttachablePoint")})
            if (!rail && c) rail = FindAttachPoint(c.Name());
        if (!rail) {  // restored onto the gun body: the nearest rail of its kind (HK and Noveske rails are < 1 cm apart)
            char tag[64] = "";
            API->NameToString(att.Get<uint64_t>("AttachPointTag"), tag, sizeof tag);
            bool muzzle = !strcmp(tag, "MuzzleAttachLocation");
            double best = 1.2;
            for (auto& p : kAttachPoints) {
                if (muzzle != !strcmp(p.name, "Fire Location")) continue;
                Vec3 t = AttachTarget(p);
                double d = muzzle ? std::hypot(t.x - now.x, t.y - now.y, t.z - now.z) : std::hypot(t.y - now.y, t.z - now.z);
                if (d < best) { best = d; rail = &p; }
            }
        }
        if (!rail) continue;
        Vec3 want = AttachTarget(*rail);
        if (strcmp(rail->name, "Fire Location") != 0) want.x = now.x;  // rails: keep the position along the rail
        if (std::fabs(want.x - now.x) + std::fabs(want.y - now.y) + std::fabs(want.z - now.z) < 0.001) continue;
        Vec3 d = frame.DirToWorld({want.x - now.x, want.y - now.y, want.z - now.z});
        Params mv(att, "K2_AddActorWorldOffset");
        mv.Set("DeltaLocation", d).Set("bSweep", false).Set("bTeleport", true).Call();
        snapped++;
    }
    return snapped;
}

// The Noveske's handguard runs to x = 36.7 cm where the HK416's ends near 33.3; its rails end this
// far short of the handguard's end, as the HK416's do (actor space, cm, default offset).
static const struct { const char* name; double endX; } kRailEnds[] = {
    {"ReceiverAttachmentSplineHG12", 33.7}, {"ReceiverAttachmentSplineHG45", 33.5}, {"ReceiverAttachmentSplineHG9", 33.5},
    {"ReceiverAttachmentSplineHG135", 33.5}, {"ReceiverAttachmentSplineHG6", 33.5}, {"ReceiverAttachmentSplineHG-135", 33.5},
    {"ReceiverAttachmentSplineHG-90", 33.5}, {"ReceiverAttachmentSplineHG-45", 33.5},
};

// Stretch a two-point rail spline so it ends at actor x = endX (its start stays).
static void ExtendRail(Object spline, const AttachPoint& start, double endX) {
    Params n(spline, "GetNumberOfSplinePoints");
    n.Call();
    if (n.Return<int32_t>() != 2) return;
    Params p1(spline, "GetLocationAtSplinePoint");
    p1.Set("PointIndex", (int32_t)1).Set("CoordinateSpace", (uint8_t)0).Call();  // Local
    Vec3 v = p1.Return<Vec3>();
    double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), want = endX - AttachTarget(start).x;
    if (len < 1 || want <= len) return;
    Vec3 nv{v.x * want / len, v.y * want / len, v.z * want / len};
    Params s(spline, "SetLocationAtSplinePoint");
    s.Set("PointIndex", (int32_t)1).Set("InLocation", nv).Set("CoordinateSpace", (uint8_t)0).Set("bUpdateSpline", true).Call();
}

static void FitAttachPoints(Object actor) {
    ActorFrame frame(actor);
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("SceneComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    int moved = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (const AttachPoint* p = FindAttachPoint(c.Name())) {
            Params sw(c, "K2_SetWorldLocation");
            sw.Set("NewLocation", frame.ToWorld(AttachTarget(*p))).Set("bSweep", false).Set("bTeleport", true).Call();
            for (auto& r : kRailEnds)
                if (c.Name() == r.name) ExtendRail(c, *p, r.endX + s_offset[0] - kDefaultOffset[0]);
            moved++;
        }
    }
    if (moved != (int)std::size(kAttachPoints))
        Warn("{}: moved {} of {} attach points - receiver layout differs from the HK416's", actor.Name(), moved,
             std::size(kAttachPoints));
    if (int n = SnapMountedAttachments(actor, frame)) Log("{}: {} mounted attachment(s) moved onto the Noveske's rails", actor.Name(), n);
}

// ------------------------------------------------------------------ magazines
//
// The HK416 takes the game's windowed PMAG (Magazine_STANAG_PMAG_30_Window). A magazine that goes
// into a Noveske - and the spare ones you carry while you have one - get the pack's own PMAG: its
// mesh replaces the magazine's (the game's rounds, handling and ammo stay). The PMAG geometry is in
// gun space as if fully inserted, so it hangs on the magazine at the inverse of where a fully inserted
// magazine sits in the gun (measured once from the first inserted magazine).

static std::vector<GUObject*> s_dressedMags;
static Xf s_magInGun;  // magazine actor transform relative to the gun, fully inserted

// The pack's PMAG shows the top of its stack: round k shows while the magazine holds k or more (CurrentAmmo,
// which replicates, so every player sees the same).
struct MagRounds { Object comps[kPmagRounds]; int shown = -1; };
static std::map<GUObject*, MagRounds> s_magRounds;

static void ShowRounds(GUObject* mag) {
    auto it = s_magRounds.find(mag);
    if (it == s_magRounds.end()) return;
    int n = std::clamp(Object(mag).Get<int32_t>("CurrentAmmo"), 0, kPmagRounds);
    if (n == it->second.shown) return;
    it->second.shown = n;
    for (int i = 0; i < kPmagRounds; i++)
        if (Object c = it->second.comps[i]) {
            Params v(c, "SetVisibility");
            v.Set("bNewVisibility", i < n).Set("bPropagateToChildren", false).Call();
        }
}

// Read off the actor itself, not a list: only this plugin gives an HK416 receiver or its magazine
// DynamicMeshComponents (the Noveske's meshes). Lists can go stale: in someone else's game no level-start
// event says when to forget them, and a new level's actors can reuse old addresses.
static bool IsDressed(Object actor) {
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("DynamicMeshComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    return arr && arr->Num > 0;
}

static bool IsDressedMag(GUObject* m) { return IsDressed(Object(m)); }

// The game draws the rounds (instanced meshes) for its windowed PMAG; they follow that magazine's curve
// and would poke through the pack's solid PMAG, which shows none - so they stay hidden.
static void HideRounds(Object mag) {
    Params gc(mag, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("InstancedStaticMeshComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (c.GetBool("bVisible")) HideComponent(c);
    }
}

static void DressMagazine(Object mag, Object insertedIn) {
    if (!mag || !s_pmagMesh || IsDressedMag(mag.ptr)) return;
    if (insertedIn && s_magInGun.empty()) {
        // The reference comes from a magazine seated in the gun (its CurrentMagazine) that sits in the
        // same place on two checks a quarter second apart - not one still sliding in or out.
        static Xf candidate;
        if (insertedIn.GetObj("CurrentMagazine").ptr != mag.ptr) return;
        Xf now = RelativeTo(WorldXf(mag), WorldXf(insertedIn));
        bool steady = false;
        if (!candidate.empty()) {
            Params eq(Lib("KismetMathLibrary"), "NearlyEqual_TransformTransform");
            PutXf(eq, "A", candidate);
            PutXf(eq, "B", now);
            eq.Set("LocationTolerance", 0.05f).Set("RotationTolerance", 0.2f).Set("Scale3DTolerance", 0.001f).Call();
            steady = eq.Return<bool>();
        }
        candidate = now;
        if (!steady) return;
        s_magInGun = now;
        Log("magazine reference taken from {} seated in {}", mag.Name(), insertedIn.Name());
    }
    if (s_magInGun.empty()) return;  // no reference yet: dressed once one has been seen seated
    Object root = mag.GetObj("RootComponent");
    Object comp = API->AddDynamicMeshComponent(mag.ptr, s_pmagMesh, s_mids, kNumParts);
    if (!root || !comp) return;
    SetRelativeXf(comp, Invert(s_magInGun));
    HideComponent(root);
    HideRounds(mag);
    MagRounds rounds;
    for (int i = 0; i < kPmagRounds; i++) {
        if (!s_pmagRoundMesh[i]) continue;
        Object rc = API->AddDynamicMeshComponent(mag.ptr, s_pmagRoundMesh[i], s_mids, kNumParts);
        if (rc) SetRelativeXf(rc, Invert(s_magInGun));
        rounds.comps[i] = rc;
    }
    s_magRounds[mag.ptr] = rounds;
    ShowRounds(mag.ptr);
    s_dressedMags.push_back(mag.ptr);
    Log("magazine {} is now the Noveske's PMAG", mag.Name());
}

// ------------------------------------------------------------------ dressing

struct DressedGun { GUObject* gun; GUObject* muzzle; };
static std::vector<DressedGun> s_dressedGuns;  // for the flash hider and magazine checks

// The pack's dust cover turns on its own hinge pin, 0.3 cm above and 0.1 cm inside the HK416's, and
// shuts a half turn from where the pack models it (open, hanging below the port), lying on the
// receiver's 22.5-degree slope around the port. Ridden on the HK416's bone it shut 0.75 cm low, and
// the game swings a cover the short way round between its angles, which with the HK416's (-177, 4)
// takes this one through the receiver. So the gun gets angles just short of a half turn apart, which
// swing it outward, and the cover turns as the bone does but about the Noveske's pin: the game
// re-poses the bone at the HK416's pin every frame, so the cover is set from the bone each frame
// instead of hanging on it.
static const char* kDustCoverBone = "RootDustCover";
static const double kDustCoverPin[2] = {1.615, 2.370};  // actor y, z (cm) at the default offset; the pin runs along the bore
static const float kDustCoverShut = -179.5f, kDustCoverOpen = 0.0f;  // the Rifle's angles: bone roll from its rest pose

struct Rot3 { double pitch, yaw, roll; };
static Vec3 XfPoint(const char* fn, const Xf& t, Vec3 v) {  // TransformLocation / InverseTransformLocation
    Params p(Lib("KismetMathLibrary"), fn);
    PutXf(p, "T", t);
    p.Set("Location", v).Call();
    return p.Return<Vec3>();
}
static Xf MovedTo(const Xf& t, Vec3 at) {  // t with its location replaced
    Params br(Lib("KismetMathLibrary"), "BreakTransform");
    PutXf(br, "InTransform", t);
    br.Call();
    Params mk(Lib("KismetMathLibrary"), "MakeTransform");
    mk.Set("Location", at).Set("Rotation", br.Get<Rot3>("Rotation")).Set("Scale", br.Get<Vec3>("Scale")).Call();
    return TakeXf(mk);
}

struct DustCover {
    GUObject* part;  // the Noveske's cover, on the Movables component
    GUObject* mov;
    Xf base;         // actor space -> the bone's local space at rest, about the Noveske's pin
    Vec3 pin;        // component space
    Xf last;         // the bone's transform the cover was last set from
};
static std::vector<DustCover> s_dustCovers;

static void PoseDustCover(DustCover& c) {
    static uint64_t bone = API->MakeName(kDustCoverBone);
    Params bt(Object(c.mov), "GetBoneTransformByName");
    *(uint64_t*)bt.Ptr("BoneName") = bone;
    bt.Set("BoneSpace", (uint8_t)1).Call();  // component space
    Xf now = TakeXf(bt);
    if (now == c.last) return;
    c.last = now;
    SetRelativeXf(Object(c.part), Compose(c.base, MovedTo(now, c.pin)));
}

static void PoseDustCovers() {  // every frame
    for (size_t i = 0; i < s_dustCovers.size();) {
        auto& c = s_dustCovers[i];
        if (!API->IsValid(c.part) || !API->IsValid(c.mov)) { s_dustCovers.erase(s_dustCovers.begin() + i); continue; }
        PoseDustCover(c);
        i++;
    }
}

// Gives the gun the Noveske's dust cover angles and hangs `part` on the Movables component, posed
// each frame about the Noveske's pin.
static void FitDustCover(Object gun, Object mov, Object part, const Xf& rest, const Xf& movInActor) {
    Params br(Lib("KismetMathLibrary"), "BreakTransform");
    PutXf(br, "InTransform", rest);
    br.Call();
    Vec3 pin = XfPoint("TransformLocation", movInActor, br.Get<Vec3>("Location"));  // the HK416's, actor space
    pin.y = kDustCoverPin[0] + s_offset[1] - kDefaultOffset[1];
    pin.z = kDustCoverPin[1] + s_offset[2] - kDefaultOffset[2];
    DustCover c{part.ptr, mov.ptr, {}, XfPoint("InverseTransformLocation", movInActor, pin), {}};
    c.base = Compose(Invert(movInActor), Invert(MovedTo(rest, c.pin)));

    // The cover goes to the Noveske's angle for whichever state it is in now (the game's angle can be
    // a turn away from its settings, e.g. -356 for open at 4).
    auto apart = [](float a, float b) { float d = std::fmod(std::fabs(a - b), 360.0f); return d < 180.0f ? d : 360.0f - d; };
    float cur = gun.Get<float>("CurrentDustCoverAngle");
    float now = apart(cur, gun.Get<float>("DustCoverOpenRotation")) < apart(cur, gun.Get<float>("DustCoverClosedRotation"))
                    ? kDustCoverOpen : kDustCoverShut;
    gun.Set("DustCoverClosedRotation", kDustCoverShut);
    gun.Set("DustCoverOpenRotation", kDustCoverOpen);
    gun.Set("CurrentDustCoverAngle", now);
    Rot3 r = br.Get<Rot3>("Rotation");
    r.roll += now;
    Params sr(mov, "SetBoneRotationByName");
    *(uint64_t*)sr.Ptr("BoneName") = API->MakeName(kDustCoverBone);
    sr.Set("InRotation", r).Set("BoneSpace", (uint8_t)1).Call();

    AttachTo(part, mov, nullptr);
    PoseDustCover(c);
    s_dustCovers.push_back(std::move(c));
}

static void HangMovingParts(Object actor) {
    Object mov = actor.GetObj("Movables");  // HK416 moving parts: a PoseableMesh the gun code poses by bone name
    if (!mov) { Warn("{} has no Movables component - keeping the HK416's moving parts", actor.Name()); return; }
    Xf movInActor = RelativeTo(WorldXf(mov), WorldXf(actor));
    int hung = 0;
    for (auto& p : s_movingParts) {
        if (!p.mesh) continue;
        Xf rest = RefPoseComponent(mov, p.bone);  // bone at rest, component space
        if (rest.empty()) { Warn("bone {} not found on {}", p.bone, actor.Name()); continue; }
        Object comp = API->AddDynamicMeshComponent(actor.ptr, p.mesh, s_mids, kNumParts);
        if (!comp) continue;
        if (!strcmp(p.bone, kDustCoverBone)) {
            FitDustCover(actor, mov, comp, rest, movInActor);
            hung++;
            continue;
        }
        // The part's vertices are in actor space; on the bone it must sit at the inverse of the bone's rest pose.
        AttachTo(comp, mov, p.bone);
        SetRelativeXf(comp, Invert(Compose(rest, movInActor)));
        hung++;
    }
    // Stop drawing the HK416's parts but keep the bones posed: the Noveske's parts ride on them.
    mov.Set("VisibilityBasedAnimTickOption", (uint8_t)0);  // AlwaysTickPoseAndRefreshBones
    HideComponent(mov);
    Log("{}: {} Noveske moving parts on the gun's bones", actor.Name(), hung);
}

static void DressAsNoveske(Object actor) {
    if (IsDressed(actor)) return;
    if (!BuildVisualAssets()) return;
    Object root = actor.GetObj("StaticMeshComponent");  // the HK416 mesh; also collision + physics
    if (!root) { Error("{} has no StaticMeshComponent", actor.FullName()); return; }
    Object comp = API->AddDynamicMeshComponent(actor.ptr, s_mesh, s_mids, kNumParts);
    if (!comp) { Error("could not add the Noveske mesh to {}", actor.FullName()); return; }
    Object muzzle = s_muzzleMesh ? Object(API->AddDynamicMeshComponent(actor.ptr, s_muzzleMesh, s_mids, kNumParts)) : Object();
    HideComponent(root);
    if (s_ownMovingParts) HangMovingParts(actor);
    if (s_fitAttachments) FitAttachPoints(actor);
    s_dressedGuns.push_back({actor.ptr, muzzle.ptr});
    if (s_ownMagazines) DressMagazine(actor.GetObj("CurrentMagazine"), actor);
    Log("*** {} dressed as Noveske Chainsaw{} ***", actor.Name(), s_fitAttachments ? ", attach points on its rails" : "");
}

// Topmost actor this one is attached to (a magazine in a pouch on a player -> the player).
static GUObject* AttachRoot(Object a) {
    for (int guard = 0; guard < 16 && a; guard++) {
        Params p(a, "GetAttachParentActor");
        p.Call();
        Object up = p.Return<GUObject*>();
        if (!up) break;
        a = up;
    }
    return a.ptr;
}

// ------------------------------------------------------------------ multiplayer
//
// Each machine dresses guns itself, and only the one whose player picked the Noveske (or loaded it from
// a save slot) knows the HK416 it got is one. It tells the others through the receiver's own
// ColorComponent: its ColorOption is an FName the game replicates to every player and keeps in a saved
// loadout, and on a dressed gun the receiver mesh that colour would paint is hidden. The plugin sets it
// to kMarker with the game's own colour RPC, and any machine with the plugin that sees an HK416 receiver
// with that colour - when it replicates in, or later through OnRep_ColorOption - dresses it. A machine
// without the plugin ignores the unknown colour and shows the HK416.

static constexpr char kMarker[] = "NoveskeChainsaw";
static const char* kSelectHook =
    "WB_GunCustomization_C:BndEvt__WB_GunCustomization_WB_Database_K2Node_ComponentBoundEvent_34_OnItemSelected__DelegateSignature";
static std::map<GUObject*, GUObject*> s_picks;  // gun station -> gun last picked at it on this machine

static bool HasAuthority(Object actor) {
    Params p(actor, "HasAuthority");
    p.Call();
    return p.Return<bool>();
}

static std::string ColorOf(Object receiver) {
    Object cc = receiver ? receiver.GetObj("ColorComponent") : Object();
    char b[128] = "";
    if (cc) API->NameToString(cc.Get<uint64_t>("ColorOption"), b, sizeof b);
    return b;
}

static bool IsMarked(Object receiver) {
    return receiver.Class() == s_receiverClass && _stricmp(ColorOf(receiver).c_str(), kMarker) == 0;
}

// This machine's player owns the actor (it was spawned for them, or they hold it): the server takes
// RPCs on it from here.
static bool LocallyOwned(Object actor) {
    Object o = actor;
    for (int guard = 0; guard < 4 && o; guard++) {
        if (o.IsA("Pawn")) {
            Params lc(o, "IsLocallyControlled");
            lc.Call();
            return lc.Return<bool>();
        }
        if (o.IsA("PlayerController")) {
            Params lc(o, "IsLocalController");
            lc.Call();
            return lc.Return<bool>();
        }
        Params go(o, "GetOwner");
        go.Call();
        o = go.Return<GUObject*>();
    }
    return false;
}

// Names the gun a Noveske for every player: on the server the colour applies at once and replicates;
// from the client that owns the gun it goes to the server first. At most once a second per gun.
static void Mark(Object receiver) {
    static std::map<GUObject*, ULONGLONG> asked;
    Object cc = receiver.GetObj("ColorComponent");
    if (!cc || IsMarked(receiver)) return;
    bool authority = HasAuthority(receiver);
    if (!authority && !LocallyOwned(receiver)) return;  // someone else's gun: not ours to name
    ULONGLONG now = GetTickCount64();
    if (auto it = asked.find(receiver.ptr); it != asked.end() && now - it->second < 1000) return;
    asked[receiver.ptr] = now;
    Params ac(cc, "ApplyColor_Server");
    ac.Set("ColorName", API->MakeName(kMarker)).Call();
    Log("{}: named a Noveske for every player{}", receiver.Name(), authority ? "" : " (asked the server)");
}

// Every quarter second: the magazine in each Noveske, the local player's spare magazines while they
// have a Noveske, and each Noveske's flash hider (hidden while a muzzle device replaces it).
static void WatchDressedGuns() {
    static ULONGLONG next = 0;
    if (s_dressedGuns.empty() || GetTickCount64() < next) return;
    next = GetTickCount64() + 250;
    Object pawn;
    {
        Params p(Lib("GameplayStatics"), "GetPlayerPawn");
        p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
        pawn = p.Return<GUObject*>();
    }
    bool playerHasNoveske = false;
    GML_TArray* accepted = nullptr;
    for (size_t i = 0; i < s_dressedGuns.size();) {
        Object gun(s_dressedGuns[i].gun);
        if (!gun || !API->IsValid(gun.ptr) || !IsDressed(gun)) { s_dressedGuns.erase(s_dressedGuns.begin() + i); continue; }
        if (s_ownMagazines) {
            DressMagazine(gun.GetObj("CurrentMagazine"), gun);
            if (pawn && AttachRoot(gun) == pawn.ptr) playerHasNoveske = true;
            accepted = (GML_TArray*)gun.PropPtr("AcceptedMagazines");
        }
        Mark(gun);  // repainted at a station since, or the first try didn't reach the server: name it again
        if (Object muzzle = s_dressedGuns[i].muzzle; muzzle && API->IsValid(muzzle.ptr)) {
            bool device = false;
            Params ga(gun, "GetAttachedActors");
            ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
            auto* arr = (GML_TArray*)ga.Ptr("OutActors");
            for (int k = 0; arr && k < arr->Num && !device; k++) {
                Object att(((GUObject**)arr->Data)[k]);
                char tag[64] = "";
                if (att.IsA("GunAttachment")) API->NameToString(att.Get<uint64_t>("AttachPointTag"), tag, sizeof tag);
                device = !strcmp(tag, "MuzzleAttachLocation") && att.GetBool("bAttached");
            }
            Params v(muzzle, "SetVisibility");
            v.Set("bNewVisibility", !device).Set("bPropagateToChildren", false).Call();
        }
        i++;
    }
    for (size_t i = 0; i < s_dressedMags.size();) {  // keep their rounds hidden if the game re-shows them
        if (!API->IsValid(s_dressedMags[i]) || !IsDressed(Object(s_dressedMags[i]))) {
            s_magRounds.erase(s_dressedMags[i]);
            s_dressedMags.erase(s_dressedMags.begin() + i);
            continue;
        }
        HideRounds(s_dressedMags[i]);
        ShowRounds(s_dressedMags[i++]);
    }
    if (playerHasNoveske && accepted && pawn) {
        for (int k = 0; k < accepted->Num; k++) {
            Object cls(((GUObject**)accepted->Data)[k]);
            for (auto& mag : FindAllOf(cls.Name().c_str()))
                if (!IsDressedMag(mag.ptr) && AttachRoot(mag) == pawn.ptr) DressMagazine(mag, {});
        }
    }
}

// ------------------------------------------------------------------ loadouts (team room, operations)
//
// The game saves each carried gun as a GunSerializable in SaveGames\SavedGun_<slot>.sav, with its
// GunAsset stored as an object path, and rebuilds it from there at team-room start and in every
// operation. The Noveske's GunAsset is created at runtime, so its path only resolves in the session
// that wrote it; next launch the game can't find it and hands out its default rifle instead.
// So once the game has saved the Noveske, the plugin re-saves that slot (with the game's own
// SaveGameToSlot) with the HK416 donor as GunAsset - same receiver, handling and attachments - and
// A loadout gun is the Noveske only if its slot was saved with the Noveske's colour name (a stock HK416
// is saved under our runtime asset too). Without the plugin the slot simply loads as an HK416 with the
// same attachments.

static std::string FStr8(Object o, const char* prop) {
    char b[256] = "";
    if (auto* s = (const GML_FString*)o.PropPtr(prop)) API->FStringToUtf8(s, b, sizeof b);
    return b;
}

static std::wstring Widen8(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

static std::string ReadFileBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(GetFileSize(f, nullptr), '\0');
    DWORD n = 0;
    if (!ReadFile(f, s.data(), (DWORD)s.size(), &n, nullptr)) n = 0;
    CloseHandle(f);
    s.resize(n);
    return s;
}

static std::string HashHex(const std::string& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return std::format("{:016x}-{}", h, bytes.size());
}

static std::wstring SaveGamesDir() {
    static std::wstring dir;
    if (dir.empty()) {
        Params p(Lib("KismetSystemLibrary"), "GetProjectSavedDirectory");
        p.Call();
        char b[1024] = "";
        if (auto* s = (const GML_FString*)p.Ptr("ReturnValue")) API->FStringToUtf8(s, b, sizeof b);
        if (*b) dir = Widen8(b) + L"SaveGames\\";
    }
    return dir;
}

// [Loadouts] <slot> = <hash of the file while it holds the Noveske>, in the plugin's .cfg.
static ConfigEntry<std::string> SlotRecord(const std::string& slot) {
    return Config.Bind<std::string>("Loadouts", slot.c_str(), "",
                                    "Written by the plugin: this save slot holds the Noveske (hash of its file).");
}
static std::string RecordedHash(const std::string& slot) { return SlotRecord(slot).Value(); }
static void RecordHash(const std::string& slot, const std::string& hash) { SlotRecord(slot).Set(hash); }

// A field of FGunSetupStruct inside a loaded GunSerializable.
static void* SetupField(Object save, const char* name) {
    GFProperty* setupProp = save ? API->FindProperty(save.Struct(), "GunSetup") : nullptr;
    GML_PropInfo setup{}, field{};
    if (!setupProp || !API->GetPropertyInfo(setupProp, &setup) || !setup.structType) return nullptr;
    GFProperty* fieldProp = API->FindProperty(setup.structType, name);
    if (!fieldProp || !API->GetPropertyInfo(fieldProp, &field)) return nullptr;
    return (uint8_t*)save.ptr + setup.offset + field.offset;
}
static GUObject** GunAssetField(Object save) { return (GUObject**)SetupField(save, "GunAsset"); }

// The receiver colour a setup was saved with: SaveGunSetup keeps the receiver's ColorOption, which the
// plugin sets to kMarker on every Noveske (see "multiplayer").
static bool SavedWithMarker(Object save) {
    auto* name = (uint64_t*)SetupField(save, "SelectedColor");
    char b[128] = "";
    if (name) API->NameToString(*name, b, sizeof b);
    return _stricmp(b, kMarker) == 0;
}

struct SavedGun { GUObject* asset; bool marked; };

// The gun a gear manager saved in slot `slotProp`, read with the game's own GetGunSetup.
static SavedGun SavedGunSetup(Object gear, const char* slotProp) {
    std::string slot = FStr8(gear, slotProp);
    if (slot.empty()) return {};
    Params gg(gear, "GetGunSetup");
    gg.SetString("SlotName", Widen8(slot.c_str())).Call();
    Object save = gg.Get<GUObject*>("GunSetup");
    GUObject** f = GunAssetField(save);
    return {f ? *f : nullptr, SavedWithMarker(save)};
}

// SaveGunSetup names a gun by a GunAsset in AvailableGuns with the receiver's class, and the HK416 and
// the Noveske share one: a stock HK416 is saved as our runtime asset too. So the GunAsset says nothing;
// only the Noveske's colour name, which every Noveske carries and SaveGunSetup keeps, marks a Noveske slot.
static bool IsNoveskeSlot(const std::string&, SavedGun saved) {
    if (!saved.asset || !saved.marked) return false;
    Object donor = FindObject(kDonor);
    return saved.asset == s_gun || saved.asset == donor.ptr;
}

// Re-save any gun slot the game wrote with the Noveske (or, from an earlier session, with a runtime
// GunAsset path that no longer resolves) so it names the donor; remember the new file's hash.
static void WatchLoadouts() {
    static ULONGLONG next = 0;
    static std::map<std::wstring, uint64_t> seen;  // file -> last write time
    if (!s_gun || GetTickCount64() < next) return;
    next = GetTickCount64() + 1000;
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"SavedGun_*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = fd.cFileName;
        uint64_t stamp = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (seen[file] == stamp) continue;
        seen[file] = stamp;
        std::wstring stemW = file.substr(0, file.size() - 4);
        char slotBuf[260] = "";
        WideCharToMultiByte(CP_UTF8, 0, stemW.c_str(), -1, slotBuf, sizeof slotBuf, nullptr, nullptr);
        std::string slot = slotBuf;
        std::string raw = ReadFileBytes(dir + file);
        Params lg(Lib("GameplayStatics"), "LoadGameFromSlot");
        lg.SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
        Object save = lg.Return<GUObject*>();
        GUObject** asset = GunAssetField(save);
        if (!asset) continue;
        // A runtime GunAsset path in the file can only be this plugin's Noveske.
        size_t t = raw.find("/Engine/Transient.");
        // Other plugins (the UAR-15, the Saiga) make runtime GunAssets too: only one in a slot that held the
        // Noveske before is ours.
        bool staleNoveske = !*asset && t != std::string::npos && raw.find(".GunAsset_C_", t) != std::string::npos &&
                            !RecordedHash(slot).empty();
        bool marked = SavedWithMarker(save);
        // Our runtime asset in the file: the Noveske if it carries the colour name, else a stock HK416 saved
        // under the shared receiver. Either way re-save it with the donor so the slot still loads next session.
        if (*asset == s_gun || staleNoveske) {
            Object donor = FindObject(kDonor);
            if (!donor) continue;
            *asset = donor.ptr;
            Params sv(Lib("GameplayStatics"), "SaveGameToSlot");
            sv.SetObj("SaveGameObject", save).SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
            if (!sv.Return<bool>()) { Error("could not re-save loadout slot {}", slot); continue; }
            RecordHash(slot, marked ? HashHex(ReadFileBytes(dir + file)) : std::string());
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW((dir + file).c_str(), GetFileExInfoStandard, &a))
                seen[file] = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
            Log("loadout slot {} holds {}: re-saved with a stable reference{}", slot, marked ? "the Noveske" : "a stock HK416",
                staleNoveske ? " (repaired a save from an earlier session)" : "");
        } else if (!RecordedHash(slot).empty() && !IsNoveskeSlot(slot, {*asset, marked})) {
            RecordHash(slot, "");  // the game saved a different gun there since
            Log("loadout slot {} no longer holds the Noveske", slot);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// A receiver the gear manager rebuilt from your saved loadout (team room start, operations): it is
// ours if it became a player's PrimaryGun/SecondaryGun and that slot holds the Noveske.
static bool ResolveLoadout(Object actor) {
    for (auto& gear : FindAllOf("BP_GearManagerComponent_C")) {
        // The save slots on this PC are the local player's; another player's gun is not ours to judge.
        Object pawn(API->GetOuter(gear.ptr));
        if (pawn.IsA("Pawn")) {
            Params lc(pawn, "IsLocallyControlled");
            lc.Call();
            if (!lc.Return<bool>()) continue;
        }
        const char* slotProp = gear.GetObj("PrimaryGun").ptr == actor.ptr     ? "PrimaryGunSetupSlotName"
                               : gear.GetObj("SecondaryGun").ptr == actor.ptr ? "SecondaryGunSetupSlotName"
                                                                               : nullptr;
        if (!slotProp) continue;
        SavedGun saved = SavedGunSetup(gear, slotProp);
        std::string slot = FStr8(gear, "GunSetupSlotName") + "_" + FStr8(gear, slotProp);  // SavedGun_MainPrimary
        bool noveske = IsNoveskeSlot(slot, saved);
        Log("{} loaded from {} ({}): saved gun '{}'{} -> {}", actor.Name(), slot, Object(API->GetOuter(gear.ptr)).Name(),
            saved.asset ? Object(saved.asset).Name() : std::string("?"), saved.marked ? " named a Noveske" : "",
            noveske ? "Noveske" : "not the Noveske");
        if (noveske) {
            DressAsNoveske(actor);
            Mark(actor);
        }
        return true;
    }
    return false;
}

// The spawner's menu items are WB_CustomizationItemGun widgets; the gun they stand for is their
// WeaponItem. (Selection events pass the item *widget*, not the GunAsset.)
static GUObject* GunOf(GUObject* itemOrGun) {
    Object o(itemOrGun);
    if (o && o.IsA("WB_CustomizationItemGun_C")) return o.GetObj("WeaponItem").ptr;
    return itemOrGun;
}

static GUObject* s_lastPressedGun = nullptr;  // any spawner menu item pressed, most recent
static ULONGLONG s_lastPressedAt = 0;

// A gun station whose player is someone else's. Each player picks guns at their own station, on their
// own machine; this machine's menu presses and selections say nothing about what that station spawned.
// (A station with no player yet is this machine's: single player.)
static bool ForeignStation(Object station) {
    Object player = station.GetObj("Player");
    if (!player) return false;
    Params lc(player, "IsLocallyControlled");
    lc.Call();
    return !lc.Return<bool>();
}

// A receiver of the Noveske's ReceiverClass began play. It is a Noveske if another machine named it one
// (see "multiplayer"), if the gun station now holding it as CurrentReceiver was last asked for the
// Noveske entry on this machine, or if it is a loadout gun from a Noveske slot. The station sets
// CurrentReceiver right after the spawn returns (on a client, when it replicates), so this is checked on
// the following frames.
static void ResolvePending() {
    if (s_pending.empty()) return;
    auto stations = FindAllOf("CustomizationStation_Gun_C");
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        Object actor(p.actor);
        bool done = !actor || !API->IsValid(actor.ptr);
        if (!done && IsMarked(actor)) {
            Log("{} is named a Noveske by the game it came from", actor.Name());
            DressAsNoveske(actor);
            done = true;
        }
        for (auto& st : stations) {
            if (done || st.GetObj("CurrentReceiver").ptr != p.actor) continue;
            done = true;
            if (ForeignStation(st)) break;  // another player's: their machine names it if it is a Noveske
            GUObject* picked = nullptr;
            const char* how = "?";
            if (auto pk = s_picks.find(st.ptr); pk != s_picks.end()) { picked = pk->second; how = "selection"; }
            if (!picked && GetTickCount64() - s_lastPressedAt < 30000) { picked = s_lastPressedGun; how = "last menu press"; }
            Log("{} spawned at {}: picked '{}' (via {}) -> {}", actor.Name(), st.Name(),
                picked ? Object(picked).Name() : std::string("?"), how, picked == s_gun ? "Noveske" : "stock HK416");
            if (picked == s_gun) {
                DressAsNoveske(actor);
                Mark(actor);
            }
        }
        if (!done) done = ResolveLoadout(actor);
        // Neither a station spawn nor a loadout gun (world pickup, other player's gun...): stays an HK416
        // unless the game it came from names it a Noveske later (OnRep_ColorOption puts it back here).
        if (!done && GetTickCount64() - p.since > 5000) done = true;
        if (done) s_pending.erase(s_pending.begin() + i);
        else i++;
    }
}

// A level started. Build the mesh and textures while the first one loads, not when the gun is first
// spawned. A new level's actors can reuse the old ones' addresses, so forget what was dressed. In someone
// else's game this machine has no game mode, so GML's WORLD_BEGIN_PLAY doesn't fire here: the game
// state's BeginPlay, which every machine runs, calls this too.
static void NewLevel() {
    BuildVisualAssets();
    // Keep only what is still a live, dressed Noveske (an address reused by a new actor is not).
    std::erase_if(s_dressedGuns, [](const DressedGun& d) { return !API->IsValid(d.gun) || !IsDressed(Object(d.gun)); });
    std::erase_if(s_dressedMags, [](GUObject* m) { return !API->IsValid(m) || !IsDressed(Object(m)); });
    std::erase_if(s_magRounds, [](const auto& r) { return std::find(s_dressedMags.begin(), s_dressedMags.end(), r.first) == s_dressedMags.end(); });
    std::erase_if(s_dustCovers, [](const DustCover& c) { return !API->IsValid(c.part) || !API->IsValid(c.mov); });
    s_picks.clear();
}

// The level changed if the game state did. GML's WORLD_BEGIN_PLAY comes from the game mode, which only
// the host (or a single-player game) has; the game state is on every machine.
static void WatchLevel() {
    static ULONGLONG next = 0;
    static std::string level;
    if (GetTickCount64() < next) return;
    next = GetTickCount64() + 500;
    Params gs(Lib("GameplayStatics"), "GetGameState");
    gs.SetObj("WorldContextObject", Object(API->WorldContext())).Call();
    Object state = gs.Return<GUObject*>();
    if (!state) return;
    std::string now = state.FullName();
    if (now == level) return;
    level = now;
    NewLevel();
}

static void QueueReceiver(GUObject* actor) {
    for (auto& pe : s_pending)
        if (pe.actor == actor) return;
    s_pending.push_back({actor, GetTickCount64()});
}

static void InstallVisuals() {
    // The spawner's selection (delegate broadcasts go through ProcessEvent). On a client the station asks
    // the server for the gun with the GunAsset it is handed, and the server can't resolve this machine's
    // runtime Noveske asset: it would get none, clear the station and spawn nothing. So for that call the
    // menu item stands for the HK416 donor (same receiver), and this machine remembers that the Noveske was
    // picked: it dresses the HK416 that comes back and names it a Noveske for everyone.
    static Object swapped;
    Hook(kSelectHook,
         [](Object widget, GUFunction* fn, void* params) {
             Object item(Param<GUObject*>(fn, params, "Item"));
             if (!item || !item.IsA("WB_CustomizationItemGun_C")) return false;
             GUObject* gun = item.GetObj("WeaponItem").ptr;
             Object station = widget.GetObj("OwningStation");
             if (station) s_picks[station.ptr] = gun;
             if (gun != s_gun || !station || HasAuthority(station)) return false;
             Object donor = FindObject(kDonor);
             if (!donor) return false;
             item.Set<GUObject*>("WeaponItem", donor.ptr);
             swapped = item;
             Log("{}: asking the server for the HK416 donor; this machine dresses it as the Noveske", station.Name());
             return false;
         },
         [](Object widget, GUFunction* fn, void* params) {
             if (swapped) {
                 swapped.Set<GUObject*>("WeaponItem", s_gun);
                 swapped = {};
             }
             Log("spawner selection in {}: {}", widget.Name(), Object(GunOf(Param<GUObject*>(fn, params, "Item"))).Name());
         });
    // The menu item's own button press - the first script the VR pointer reaches.
    HookAfter("WB_CustomizationItemGun_C:BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature",
              [](Object item, GUFunction*, void*) {
                  if (GUObject* gun = GunOf(item.ptr)) {
                      s_lastPressedGun = gun;
                      s_lastPressedAt = GetTickCount64();
                      Log("spawner menu press: {}", Object(gun).Name());
                  }
              });
    HookAfter("*:ReceiveBeginPlay", [](Object self, GUFunction*, void*) {
        if (!s_receiverClass) {
            Object donor = FindObject(kDonor);
            if (donor) s_receiverClass = donor.Get<GUClass*>("ReceiverClass");
        }
        if (s_receiverClass && self.Class() == s_receiverClass) QueueReceiver(self.ptr);
    });
    // A receiver's colour arriving from the server (OnRep), or applied on the server at another player's
    // request (ApplyColor_Server) or from a saved loadout (ApplyColor): the Noveske's name makes it one here too.
    for (const char* f : {"ColorComponent:OnRep_ColorOption", "ColorComponent:ApplyColor_Server", "ColorComponent:ApplyColor"})
        HookAfter(f, [](Object cc, GUFunction*, void*) {
            Params o(cc, "GetOwner");
            o.Call();
            Object actor = o.Return<GUObject*>();
            if (!actor || !s_receiverClass || actor.Class() != s_receiverClass) return;
            if (IsDressed(actor) || !IsMarked(actor)) return;
            QueueReceiver(actor.ptr);
        });
    On(GML_EVENT_TICK, [](void*) {
        WatchLevel();
        ResolvePending();
        WatchLoadouts();
        WatchDressedGuns();
        PoseDustCovers();
    });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) { NewLevel(); });
}

// ------------------------------------------------------------------ probe (diagnostic)

// Load game packages and the never-registering new Noveske packages through the identical
// path and compare.
static bool TryLoad(const char* label, const char* path) {
    bool find = (bool)FindObject(path);
    bool load = (bool)LoadObject(path);
    bool after = (bool)FindObject(path);
    Log("  {:<24} find={:<5} load={:<5} after={:<5} {}", label, find ? "yes" : "no", load ? "yes" : "no",
        after ? "YES" : "no", path);
    return after;
}

static void Probe() {
    static const char* control[][2] = {
        {"GAME mesh (HK416 body)", "/Game/Weapons/HK416a5/Mesh/HK_416_279_v2.HK_416_279_v2"},
        {"GAME mesh (HK stock)", "/Game/Weapons/HK416a5/Mesh/stock_ar15_HK_slim.stock_ar15_HK_slim"},
        {"GAME data (donor)", kDonor},
    };
    static const char* ours[][2] = {
        {"OURS Body", "/Game/Weapons/Noveske/Mesh/Noveske_Body.Noveske_Body"},
        {"OURS Grip", "/Game/Weapons/Noveske/Mesh/Noveske_Grip.Noveske_Grip"},
    };
    Log("==== CONTROLS (game packages) ====");
    int okc = 0, oko = 0;
    for (auto& e : control) okc += TryLoad(e[0], e[1]);
    Log("==== OURS (new packages) ====");
    for (auto& e : ours) oko += TryLoad(e[0], e[1]);
    Log("==== package-path form ====");
    TryLoad("OURS pkg-only", "/Game/Weapons/Noveske/Mesh/Noveske_Body");
    TryLoad("GAME pkg-only", "/Game/Weapons/HK416a5/Mesh/HK_416_279_v2");
    Log("==== resident StaticMesh objects matching 'Noveske' ====");
    auto all = FindAllOf("StaticMesh");
    int hits = 0;
    for (auto& o : all) {
        std::string n = o.FullName();
        if (n.find("Noveske") != std::string::npos) { Log("   {}", n); hits++; }
    }
    Log("   ({} of {} resident static meshes matched)", hits, all.size());
    Log("==== VERDICT: controls {}/3 loaded, ours {}/2 loaded ====", okc, oko);
    if (okc == 0) Log("     LoadObject does not work here even for GAME packages - probe method is at fault");
    else if (oko == 0) Log("     LoadObject works for game packages but NOT ours - new packages really do not register");
    else Log("     OUR PACKAGES LOAD");
}

// ------------------------------------------------------------------ entry

static bool s_probe = false;

// Probe (optional) + add the entry if it isn't there.
static void Run() {
    if (s_probe) Probe();
    if (Object gi = FindFirstOf("BP_GameInstance_C")) AddGun(gi);
    else Warn("BP_GameInstance_C not found");
}

static bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

GML_PLUGIN("com.remarrow.noveskechainsaw", "Noveske Chainsaw", NOVESKE_VERSION);

GML_AWAKE() {
    Message(NOVESKE_MODEL_CREDIT);
    s_probe = Config.Bind("Debug", "Probe", false,
        "Run the package-loading probe at the first level start and on every F8.\n"
        "It shows that newly added packages never register.").Value();
    HookAfter("BP_GameInstance_C:GetItems", [](Object gi, GUFunction*, void*) { AddGun(gi); });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) {
        static bool first = true;
        if (first) {
            first = false;
            if (s_probe) Probe();
        }
        // Fallback if GetItems ran before we could observe it.
        if (Object gi = FindFirstOf("BP_GameInstance_C")) AddGun(gi);
    });
    // F8 re-runs. Only while the game has focus.
    On(GML_EVENT_TICK, [](void*) {
        static bool down = false;
        bool now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (now && !down && GameHasFocus()) { Log("F8 - re-running"); Run(); }
        down = now;
    });
    s_visualsEnabled = Config.Bind("Visuals", "Enabled", true,
        "Give guns spawned from the Noveske entry the Noveske mesh and textures (built into the DLL).\n"
        "Only those actors change; no game asset is replaced.").Value();
    // Defaults computed by tools/align: trimmed ICP of the Noveske's lower receiver onto the HK416's
    // (the HK416's trigger, selector, bolt catch and magazine stay on the gun), 0.21 cm rms.
    s_offset[0] = Config.Bind("Visuals", "OffsetX", 2.154f, "cm, + = toward the muzzle").Value();
    s_offset[1] = Config.Bind("Visuals", "OffsetY", -0.466f, "cm, + = to the gun's right").Value();
    s_offset[2] = Config.Bind("Visuals", "OffsetZ", -0.909f, "cm, + = up").Value();
    s_fitAttachments = Config.Bind("Visuals", "FitAttachments", true,
        "Move the rails, M-LOK slots and muzzle point that attachments snap to onto the Noveske's own,\n"
        "full handguard length (false = where they are on the HK416).").Value();
    s_ownMovingParts = Config.Bind("Visuals", "OwnMovingParts", true,
        "The Noveske's own bolt carrier, charging handle, dust cover, trigger and selector, moved by the game\n"
        "(false = the HK416's).").Value();
    s_ownMagazines = Config.Bind("Visuals", "OwnMagazines", true,
        "Magazines in a Noveske, and the spares you carry while you have one, are the Noveske's own PMAG.").Value();
    if (s_visualsEnabled) InstallVisuals();
    Log("loaded - adds '{}' to the Assault Rifle spawner; visuals {}", kDisplayName, s_visualsEnabled ? "on" : "off");
    return 0;
}
