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
//    widget, that one actor gets a Noveske DynamicMeshComponent (built at runtime from
//    Assets\noveske.obj via GML's ImportDynamicMesh, textured with M_DefaultShader instances fed
//    Assets\<Part>_{Diffuse,Normal,ORM}.png) and its HK416 mesh is hidden. Collision, physics,
//    grips and sockets stay the HK416's.
//
// 3. Probe (config [Debug] Probe = true): package-loading diagnostic - game packages vs the
//    never-registering new Noveske packages.
//
// Settings: GML\config\com.remarrow.noveskechainsaw.cfg. Build: build.bat [install].
// Assets: tools\prepare_runtime.py writes Assets\ from the purchased source art (not in the repo).
#include <GML/GML.hpp>
#include <windows.h>
#include <map>
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

static bool BuildVisualAssets() {
    if (s_assetsTried) return s_mesh != nullptr;
    s_assetsTried = true;
    if (API->size < offsetof(GML_API, AddDynamicMeshComponent) + sizeof(void*)) {
        Error("this plugin needs GML 2.1 or newer (ImportDynamicMesh)");
        return false;
    }
    Object shader = LoadObject(kShader);
    if (!shader) { Error("weapon shader {} not found", kShader); return false; }

    std::string names[kNumParts];
    const char* namePtrs[kNumParts];
    for (int i = 0; i < kNumParts; i++) {
        std::wstring part(kParts[i], kParts[i] + strlen(kParts[i]));
        s_mids[i] = API->CreateMaterialInstance(shader.ptr);
        if (!s_mids[i]) { Error("could not create a material instance for {}", kParts[i]); return false; }
        for (auto [param, suffix] : {std::pair{"Diffuse", L"_Diffuse.png"}, std::pair{"Normal", L"_Normal.png"},
                                     std::pair{"ORM", L"_ORM.png"}}) {
            GUObject* tex = API->ImportTexture(PluginPath(L"Assets\\" + part + suffix).c_str());
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
    s_mesh = API->ImportDynamicMesh(PluginPath(L"Assets\\noveske.obj").c_str(), &o);
    if (!s_mesh) Error("could not build the Noveske mesh from Assets\\noveske.obj");
    else Log("Noveske visuals ready: {}", Object(s_mesh).FullName());
    return s_mesh != nullptr;
}

static void DressAsNoveske(Object actor) {
    if (!BuildVisualAssets()) return;
    Object root = actor.GetObj("StaticMeshComponent");  // the HK416 mesh; also collision + physics
    if (!root) { Error("{} has no StaticMeshComponent", actor.FullName()); return; }
    Object comp = API->AddDynamicMeshComponent(actor.ptr, s_mesh, s_mids, kNumParts);
    if (!comp) { Error("could not add the Noveske mesh to {}", actor.FullName()); return; }
    Params hide(root, "SetVisibility");
    hide.Set("bNewVisibility", false).Set("bPropagateToChildren", false).Call();  // children stay visible
    s_dressed.push_back(actor.ptr);
    Log("*** {} spawned from the Noveske entry: dressed as Noveske Chainsaw ***", actor.Name());
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
        // Not a station spawn (loadout, mission, ...): stays an HK416.
        if (!done && GetTickCount64() - p.since > 2000) done = true;
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
    On(GML_EVENT_TICK, [](void*) { ResolvePending(); });
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

GML_PLUGIN("com.remarrow.noveskechainsaw", "Noveske Chainsaw", "1.2.1");

GML_AWAKE() {
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
        "Give guns spawned from the Noveske entry the Noveske mesh and textures (Assets folder).\n"
        "Only those actors change; no game asset is replaced.").Value();
    // Defaults computed by tools/align: trimmed ICP of the Noveske's lower receiver onto the HK416's
    // (the HK416's trigger, selector, bolt catch and magazine stay on the gun), 0.21 cm rms.
    s_offset[0] = Config.Bind("Visuals", "OffsetX", 2.154f, "cm, + = toward the muzzle").Value();
    s_offset[1] = Config.Bind("Visuals", "OffsetY", -0.466f, "cm, + = to the gun's right").Value();
    s_offset[2] = Config.Bind("Visuals", "OffsetZ", -0.909f, "cm, + = up").Value();
    if (s_visualsEnabled) InstallVisuals();
    Log("loaded - adds '{}' to the Assault Rifle spawner; visuals {}", kDisplayName, s_visualsEnabled ? "on" : "off");
    return 0;
}
