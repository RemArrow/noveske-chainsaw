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
//    <Part>_{Diffuse,Normal,ORM}.png) and its HK416 mesh is hidden. Collision, physics, grips and
//    sockets stay the HK416's; its attachment rails are moved onto the Noveske's.
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
static const char* kParts[] = {"Body", "Handguard", "Stock", "Grip", "Muzzle", "Feer"};  // OBJ groups MI_<Part>
static constexpr int kNumParts = 6;

static bool s_visualsEnabled = true;
static float s_offset[3] = {0, 0, 0};  // cm, added to the mesh after the axis conversion
static GUObject* s_mesh = nullptr;     // runtime UDynamicMesh (Geometry Scripting)
static GUObject* s_mids[kNumParts] = {};  // one M_DefaultShader instance per part
static bool s_assetsTried = false;
static GUClass* s_receiverClass = nullptr;
static std::map<GUObject*, GUObject*> s_selected;  // spawner widget -> item last picked in it

struct Pending { GUObject* actor; ULONGLONG since; };
static std::vector<Pending> s_pending;
static std::vector<GUObject*> s_dressed;  // receivers already given the Noveske look

// Decrypts one asset from the payload into `out`. Callers wipe it as soon as the engine has it, so
// at most one asset exists in plain form, and only for that moment.
static bool Unpack(const char* name, std::vector<uint8_t>& out) {
    static Blob blob = Resource("DATA");
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = kPayloadKeyA[i] ^ kPayloadKeyB[i];
    bool ok = blob && payload::Extract(blob.data, blob.size, key, name, out);
    SecureZeroMemory(key, sizeof key);
    if (!ok) Error("embedded asset '{}' is missing or damaged", name);
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

    // Mesh and textures come from the encrypted payload inside this DLL, one at a time.
    std::vector<uint8_t> buf;
    std::string names[kNumParts];
    const char* namePtrs[kNumParts];
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
        names[i] = std::string("MI_") + kParts[i];  // OBJ usemtl group -> material ID i
        namePtrs[i] = names[i].c_str();
    }

    GML_MeshImport o{};
    o.size = sizeof o;
    o.axis = GML_AXIS_BLENDER_OBJ;  // same transform as the FBX import the alignment was tuned on
    std::memcpy(o.offset, s_offset, sizeof s_offset);
    o.flipWinding = -1;
    o.materialCount = kNumParts;
    o.materialNames = namePtrs;
    o.materials = s_mids;
    s_mesh = Unpack("noveske", buf) ? API->ImportDynamicMeshFromMemory(buf.data(), buf.size(), "noveske", &o) : nullptr;
    payload::Wipe(buf);
    if (!s_mesh) Error("could not build the Noveske mesh");
    else Log("Noveske visuals ready: {}", Object(s_mesh).FullName());
    return s_mesh != nullptr;
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
            moved++;
        }
    }
    if (moved != (int)std::size(kAttachPoints))
        Warn("{}: moved {} of {} attach points - receiver layout differs from the HK416's", actor.Name(), moved,
             std::size(kAttachPoints));
    if (int n = SnapMountedAttachments(actor, frame)) Log("{}: {} mounted attachment(s) moved onto the Noveske's rails", actor.Name(), n);
}

static void DressAsNoveske(Object actor) {
    if (std::find(s_dressed.begin(), s_dressed.end(), actor.ptr) != s_dressed.end()) return;
    if (!BuildVisualAssets()) return;
    Object root = actor.GetObj("StaticMeshComponent");  // the HK416 mesh; also collision + physics
    if (!root) { Error("{} has no StaticMeshComponent", actor.FullName()); return; }
    Object comp = API->AddDynamicMeshComponent(actor.ptr, s_mesh, s_mids, kNumParts);
    if (!comp) { Error("could not add the Noveske mesh to {}", actor.FullName()); return; }
    Params hide(root, "SetVisibility");
    hide.Set("bNewVisibility", false).Set("bPropagateToChildren", false).Call();  // children stay visible
    if (s_fitAttachments) FitAttachPoints(actor);
    s_dressed.push_back(actor.ptr);
    Log("*** {} dressed as Noveske Chainsaw{} ***", actor.Name(), s_fitAttachments ? ", attach points on its rails" : "");
}

// ------------------------------------------------------------------ loadouts (team room, operations)
//
// The game saves each carried gun as a GunSerializable in SaveGames\SavedGun_<slot>.sav, with its
// GunAsset stored as an object path, and rebuilds it from there at team-room start and in every
// operation. The Noveske's GunAsset is created at runtime, so its path only resolves in the session
// that wrote it; next launch the game can't find it and hands out its default rifle instead.
// So once the game has saved the Noveske, the plugin re-saves that slot (with the game's own
// SaveGameToSlot) with the HK416 donor as GunAsset - same receiver, handling and attachments - and
// keeps the file's hash in its config. A loadout gun whose slot still holds exactly that file is the
// Noveske. Without the plugin the slot simply loads as an HK416 with the same attachments.

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

// Pointer to FGunSetupStruct.GunAsset inside a loaded GunSerializable.
static GUObject** GunAssetField(Object save) {
    GFProperty* setupProp = save ? API->FindProperty(save.Struct(), "GunSetup") : nullptr;
    GML_PropInfo setup{}, asset{};
    if (!setupProp || !API->GetPropertyInfo(setupProp, &setup) || !setup.structType) return nullptr;
    GFProperty* assetProp = API->FindProperty(setup.structType, "GunAsset");
    if (!assetProp || !API->GetPropertyInfo(assetProp, &asset)) return nullptr;
    return (GUObject**)((uint8_t*)save.ptr + setup.offset + asset.offset);
}

// The gun a gear manager saved in slot `slotProp`, read with the game's own GetGunSetup.
static GUObject* SavedGunAsset(Object gear, const char* slotProp) {
    std::string slot = FStr8(gear, slotProp);
    if (slot.empty()) return nullptr;
    Params gg(gear, "GetGunSetup");
    gg.SetString("SlotName", Widen8(slot.c_str())).Call();
    GUObject** f = GunAssetField(gg.Get<GUObject*>("GunSetup"));
    return f ? *f : nullptr;
}

static bool IsNoveskeSlot(const std::string& slot, GUObject* savedAsset) {
    if (savedAsset && savedAsset == s_gun) return true;  // written this session, not re-saved yet
    Object donor = FindObject(kDonor);
    if (!savedAsset || savedAsset != donor.ptr) return false;
    std::string h = RecordedHash(slot);
    return !h.empty() && h == HashHex(ReadFileBytes(SaveGamesDir() + Widen8(slot.c_str()) + L".sav"));
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
        bool staleNoveske = !*asset && t != std::string::npos && raw.find(".GunAsset_C_", t) != std::string::npos;
        if (*asset == s_gun || staleNoveske) {
            Object donor = FindObject(kDonor);
            if (!donor) continue;
            *asset = donor.ptr;
            Params sv(Lib("GameplayStatics"), "SaveGameToSlot");
            sv.SetObj("SaveGameObject", save).SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
            if (!sv.Return<bool>()) { Error("could not re-save loadout slot {}", slot); continue; }
            std::string hash = HashHex(ReadFileBytes(dir + file));
            RecordHash(slot, hash);
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW((dir + file).c_str(), GetFileExInfoStandard, &a))
                seen[file] = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
            Log("loadout slot {} holds the Noveske: re-saved with a stable reference{}", slot,
                staleNoveske ? " (repaired a save from an earlier session)" : "");
        } else if (!RecordedHash(slot).empty() && !IsNoveskeSlot(slot, *asset)) {
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
        GUObject* saved = SavedGunAsset(gear, slotProp);
        std::string slot = FStr8(gear, "GunSetupSlotName") + "_" + FStr8(gear, slotProp);  // SavedGun_MainPrimary
        bool noveske = IsNoveskeSlot(slot, saved);
        Log("{} loaded from {} ({}): saved gun '{}' -> {}", actor.Name(), slot, Object(API->GetOuter(gear.ptr)).Name(),
            saved ? Object(saved).Name() : std::string("?"), noveske ? "Noveske" : "not the Noveske");
        if (noveske) DressAsNoveske(actor);
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

// A receiver of the Noveske's ReceiverClass began play. It is ours if the gun station now holding it
// as CurrentReceiver was last asked for the Noveske entry. The station sets CurrentReceiver right
// after the spawn returns, so this is checked on the following frames.
static void ResolvePending() {
    if (s_pending.empty()) return;
    auto stations = FindAllOf("CustomizationStation_Gun_C");
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        Object actor(p.actor);
        bool done = !actor;
        for (auto& st : stations) {
            if (done || st.GetObj("CurrentReceiver").ptr != p.actor) continue;
            Object widget = st.GetObj("WBGunBuilder");
            GUObject* picked = widget ? widget.GetObj("PressedWeapon").ptr : nullptr;  // the game's own record
            const char* how = "PressedWeapon";
            if (!picked) {
                auto sel = s_selected.find(widget.ptr);
                if (sel != s_selected.end()) { picked = sel->second; how = "selection event"; }
            }
            if (!picked && GetTickCount64() - s_lastPressedAt < 30000) { picked = s_lastPressedGun; how = "last menu press"; }
            Log("{} spawned at {}: picked '{}' (via {}) -> {}", actor.Name(), st.Name(),
                picked ? Object(picked).Name() : std::string("?"), how, picked == s_gun ? "Noveske" : "stock HK416");
            if (picked == s_gun) DressAsNoveske(actor);
            done = true;
        }
        if (!done) done = ResolveLoadout(actor);
        // Neither a station spawn nor a loadout gun (world pickup, other player's gun...): stays an HK416.
        if (!done && GetTickCount64() - p.since > 5000) done = true;
        if (done) s_pending.erase(s_pending.begin() + i);
        else i++;
    }
}

static void InstallVisuals() {
    // Remember what the spawner widget selected (delegate broadcasts go through ProcessEvent).
    HookAfter("WB_GunCustomization_C:BndEvt__WB_GunCustomization_WB_Database_K2Node_ComponentBoundEvent_34_OnItemSelected__DelegateSignature",
              [](Object widget, GUFunction* fn, void* params) {
                  GUObject* gun = GunOf(Param<GUObject*>(fn, params, "Item"));
                  s_selected[widget.ptr] = gun;
                  Log("spawner selection in {}: {}", widget.Name(), Object(gun).Name());
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
        if (s_receiverClass && self.Class() == s_receiverClass) s_pending.push_back({self.ptr, GetTickCount64()});
    });
    On(GML_EVENT_TICK, [](void*) {
        ResolvePending();
        WatchLoadouts();
    });
    // Build the mesh and textures while the first level loads, not when the gun is first spawned.
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) { BuildVisualAssets(); });
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
        "Move the rails, M-LOK slots and muzzle point that attachments snap to onto the Noveske's own\n"
        "(false = where they are on the HK416).").Value();
    if (s_visualsEnabled) InstallVisuals();
    Log("loaded - adds '{}' to the Assault Rifle spawner; visuals {}", kDisplayName, s_visualsEnabled ? "on" : "off");
    return 0;
}
