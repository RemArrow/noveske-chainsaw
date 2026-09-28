// NoveskeDev - automated end-to-end test of the Noveske spawner entry. Not shipped; installed
// only for a test run. Drives the real spawner UI events and poses guns in front of the headset
// camera so the desktop spectator window can be screenshotted ("SNAP <name>" log lines).
#include <GML/GML.hpp>
#include <cmath>

using namespace gml;

GML_PLUGIN("com.remarrow.noveskedev", "NoveskeDev", "0.2.0",
           {{"com.remarrow.noveskechainsaw", "1.2.0", GML_DEPENDENCY_SOFT}});

struct Vec { double x, y, z; };
struct Rot { double pitch, yaw, roll; };

static const char* kSelect =
    "BndEvt__WB_GunCustomization_WB_Database_K2Node_ComponentBoundEvent_34_OnItemSelected__DelegateSignature";
static const char* kConfirm =
    "BndEvt__WB_GunCustomization_WB_ConfirmWindow_K2Node_ComponentBoundEvent_0_OnConfirm__DelegateSignature";

static Object s_station, s_widget, s_noveske, s_hk, s_hold;
static int s_step = -1, s_wait = 0;
static GUObject* s_before = nullptr;

static std::string TextOf(Object o, const char* prop) {
    char b[128] = "";
    if (void* t = o.PropPtr(prop)) API->TextToString(t, b, sizeof b);
    return b;
}

static Object s_pending;  // asset being spawned (for the direct-spawn fallback)

static const char* kItemPress =
    "BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature";

// Press the spawner menu item for `asset` exactly as the VR pointer does: fire the item widget's
// button-pressed handler; everything after that is the game's own logic.
static void Select(Object asset) {
    s_before = s_station.GetObj("CurrentReceiver").ptr;
    s_pending = asset;
    Params sw(s_widget, "ShowWeapons");  // open the Assault Rifles page so its items exist
    sw.Set("Type", (uint8_t)0).Call();
    Object item;
    for (auto& it : FindAllOf("WB_CustomizationItemGun_C"))
        if (it.GetObj("WeaponItem").ptr == asset.ptr) item = it;
    if (!item) { Error("no menu item for '{}'", TextOf(asset, "DisplayName")); return; }
    Params press(item, kItemPress);
    press.Call();
    Log("pressed menu item {} for '{}' -> widget PressedWeapon = '{}'", item.Name(), TextOf(asset, "DisplayName"),
        TextOf(s_widget.GetObj("PressedWeapon"), "DisplayName"));
}

// Stand the player at the station so its in-range check passes, as when you walk up to it.
static void MovePlayerTo(Object station) {
    Params pp(Lib("GameplayStatics"), "GetPlayerPawn");
    pp.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
    Object pawn = pp.Return<GUObject*>();
    Params sl(station, "K2_GetActorLocation");
    sl.Call();
    Vec at = sl.Return<Vec>();
    Params mv(pawn, "K2_SetActorLocation");
    mv.Set("NewLocation", at).Set("bSweep", false).Set("bTeleport", true).Call();
    Log("moved player {} to station {}", pawn.Name(), station.Name());
}

static Vec CameraLocation() {
    Params p(Lib("GameplayStatics"), "GetPlayerCameraManager");
    p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
    Object pcm = p.Return<GUObject*>();
    if (!pcm) return {};
    Params lp(pcm, "GetCameraLocation");
    lp.Call();
    return lp.Return<Vec>();
}

static Vec ActorLocation(Object a) {
    Params p(a, "K2_GetActorLocation");
    p.Call();
    return p.Return<Vec>();
}

static void Hold() {
    if (!s_hold) return;
    Object pcm;
    {
        Params p(Lib("GameplayStatics"), "GetPlayerCameraManager");
        p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
        pcm = p.Return<GUObject*>();
    }
    if (!pcm) return;
    Params lp(pcm, "GetCameraLocation"); lp.Call();
    Params rp(pcm, "GetCameraRotation"); rp.Call();
    Vec cam = lp.Return<Vec>();
    Rot rot = rp.Return<Rot>();
    Rot flat{0, rot.yaw, 0};
    Params fp(Lib("KismetMathLibrary"), "GetForwardVector");
    fp.Set("InRot", flat).Call();
    Vec f = fp.Return<Vec>();
    Vec at{cam.x + f.x * 60, cam.y + f.y * 60, cam.z - 2};  // whole gun in frame
    Rot side{0, rot.yaw + 90, 0};  // show the rifle's left side, muzzle to the right
    Params mv(s_hold, "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", at).Set("NewRotation", side).Set("bSweep", false).Set("bTeleport", true).Call();
}

static void Freeze(Object gun) {
    if (Object root = gun.GetObj("StaticMeshComponent")) {
        Params p(root, "SetSimulatePhysics");
        p.Set("bSimulate", false).Call();
    }
}

static std::vector<uint8_t> MakeXf(Vec loc, Rot rot) {
    Params p(Lib("KismetMathLibrary"), "MakeTransform");
    double one[3] = {1, 1, 1};
    p.Set("Location", loc).Set("Rotation", rot);
    std::memcpy(p.Ptr("Scale"), one, sizeof one);
    p.Call();
    GFProperty* ret = API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue");
    GML_PropInfo ri;
    API->GetPropertyInfo(ret, &ri);
    uint8_t* src = (uint8_t*)p.Ptr("ReturnValue");
    return {src, src + ri.size};
}

// Actor-space location of a world point (the actor's inverse transform).
static Vec ToActor(Object actor, Vec world) {
    Params gt(actor, "GetTransform");
    gt.Call();
    Params inv(Lib("KismetMathLibrary"), "InverseTransformLocation");
    GML_PropInfo ri;
    API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &ri);
    std::memcpy(inv.Ptr("T"), gt.Ptr("ReturnValue"), ri.size);
    inv.Set("Location", world).Call();
    return inv.Return<Vec>();
}

static std::string Tags(Object c) {
    std::string s;
    if (auto* arr = (GML_TArray*)c.PropPtr("ComponentTags"))
        for (int i = 0; i < arr->Num; i++) {
            char b[128] = "";
            API->NameToString(((uint64_t*)arr->Data)[i], b, sizeof b);
            s += (s.empty() ? "" : ",") + std::string(b);
        }
    return s;
}

// Every scene component with its tags, and every spline's points, in actor space (cm). These are
// where attachments snap (AGunAttachment: AttachType Spline / SceneComponent, AttachPointTag).
static void DumpAttachPoints(Object actor) {
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("SceneComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        Params loc(c, "K2_GetComponentLocation"); loc.Call();
        Params rot(c, "K2_GetComponentRotation"); rot.Call();
        Vec l = ToActor(actor, loc.Return<Vec>());
        Rot r = rot.Return<Rot>();
        Log("ATTACH comp {} [{}] tags=[{}] at ({:.3f},{:.3f},{:.3f}) worldrot ({:.1f},{:.1f},{:.1f})", c.Name(),
            Object((GUObject*)c.Class()).Name(), Tags(c), l.x, l.y, l.z, r.pitch, r.yaw, r.roll);
        if (!c.IsA("SplineComponent")) continue;
        Params np(c, "GetNumberOfSplinePoints"); np.Call();
        int n = np.Return<int32_t>();
        for (int k = 0; k < n; k++) {
            Params pl(c, "GetLocationAtSplinePoint");
            pl.Set("PointIndex", (int32_t)k).Set("CoordinateSpace", (uint8_t)1).Call();  // World
            Vec p = ToActor(actor, pl.Return<Vec>());
            Log("ATTACH   point {} ({:.3f},{:.3f},{:.3f})", k, p.x, p.y, p.z);
        }
    }
}

static Object FindComp(Object actor, const char* name) {
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("SceneComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (c.Name() == name) return c;
    }
    return {};
}

using Xf = std::vector<uint8_t>;  // an FTransform's raw bytes

static size_t XfSize() {
    static size_t size = 0;
    if (!size) {
        GML_PropInfo ri;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &ri);
        size = ri.size;
    }
    return size;
}
static Xf Take(Params& p, const char* name = "ReturnValue") {
    auto* s = (uint8_t*)p.Ptr(name);
    return Xf(s, s + XfSize());
}
static void Put(Params& p, const char* name, const Xf& x) { std::memcpy(p.Ptr(name), x.data(), x.size()); }

static Vec CompLocation(Object c) {
    Params loc(c, "K2_GetComponentLocation");
    loc.Call();
    return loc.Return<Vec>();
}

// Mount an attachment in the middle of a receiver rail (or on its muzzle point). Logs where its
// attach point ends up (actor space).
static void Mount(Object gun, const char* cls, const char* onto) {
    Object target = FindComp(gun, onto);
    if (!target) { Error("{} has no component {}", gun.Name(), onto); return; }
    Vec at = CompLocation(target);
    if (target.IsA("SplineComponent")) {  // aim for the middle of the rail
        Params len(target, "GetSplineLength"); len.Call();
        Params mid(target, "GetLocationAtDistanceAlongSpline");
        mid.Set("Distance", len.Return<float>() / 2).Set("CoordinateSpace", (uint8_t)1).Call();
        at = mid.Return<Vec>();
    }
    auto c = FindClass(cls);
    if (!c) { Error("attachment class {} is not loaded", cls); return; }
    auto xf = MakeXf(at, {0, 0, 0});
    Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
    b.SetObj("WorldContextObject", Object(API->WorldContext())).Set("ActorClass", c);
    std::memcpy(b.Ptr("SpawnTransform"), xf.data(), xf.size());
    b.Set("CollisionHandlingOverride", (uint8_t)1).Call();
    Object att = b.Return<GUObject*>();
    if (!att) { Error("could not spawn {}", cls); return; }
    Params f(Lib("GameplayStatics"), "FinishSpawningActor");
    f.SetObj("Actor", att);
    std::memcpy(f.Ptr("SpawnTransform"), xf.data(), xf.size());
    f.Call();

    // The game only gathers an attachment's candidate splines while a hand holds it, so compute the
    // pose it snaps to - its AttachPoint on the rail, in the rail's frame - and attach it there.
    Params sr(att, "SetAttachableReceiver"); sr.SetObj("NewReceiver", gun).Call();
    Object ap = att.GetObj("AttachPoint");
    if (!ap) { Error("{} has no AttachPoint", cls); return; }
    Xf onRail;
    if (target.IsA("SplineComponent")) {
        Params len(target, "GetSplineLength"); len.Call();
        Params ts(target, "GetTransformAtDistanceAlongSpline");
        ts.Set("Distance", len.Return<float>() / 2).Set("CoordinateSpace", (uint8_t)1).Set("bUseScale", false).Call();
        onRail = Take(ts);
    } else {
        Params tw(target, "K2_GetComponentToWorld"); tw.Call();
        onRail = Take(tw);
    }
    Params apw(ap, "K2_GetComponentToWorld"); apw.Call();
    Params aw(att, "GetTransform"); aw.Call();
    Params rel(Lib("KismetMathLibrary"), "MakeRelativeTransform");
    Put(rel, "A", Take(apw)); Put(rel, "RelativeTo", Take(aw)); rel.Call();
    Params inv(Lib("KismetMathLibrary"), "InvertTransform");
    Put(inv, "T", Take(rel)); inv.Call();
    Params cmp(Lib("KismetMathLibrary"), "ComposeTransforms");
    Put(cmp, "A", Take(inv)); Put(cmp, "B", onRail); cmp.Call();
    Xf pose = Take(cmp);

    Params a(att, "AttachToServer");
    a.SetObj("Comp", target);
    Put(a, "WorldTransform", pose);
    a.Call();
    bool viaGame = att.GetBool("bAttached");
    if (!viaGame) {  // plain engine attach as a fallback
        if (Object root = att.GetObj("RootComponent")) { Params sp(root, "SetSimulatePhysics"); sp.Set("bSimulate", false).Call(); }
        Params st(att, "K2_SetActorTransform");
        Put(st, "NewTransform", pose);
        st.Set("bSweep", false).Set("bTeleport", true).Call();
        Params at2(att, "K2_AttachToComponent");
        at2.SetObj("Parent", target).Set("SocketName", uint64_t(0)).Set("LocationRule", (uint8_t)1)
            .Set("RotationRule", (uint8_t)1).Set("ScaleRule", (uint8_t)1).Set("bWeldSimulatedBodies", false).Call();
    }
    Vec l = ToActor(gun, CompLocation(ap));
    Log("MOUNT {} on {} {}: attach point at ({:.3f},{:.3f},{:.3f}) via {}", cls, gun.Name(), onto, l.x, l.y, l.z,
        viaGame ? "AttachToServer" : "K2_AttachToComponent");
}

static void MountAll(Object gun) {
    Mount(gun, "FG_MLOK_BCM_Gunfighter_C", "ReceiverAttachmentSplineHG6");
    Mount(gun, "AimpointPRO_Short_C", "ReceiverAttachmentSpline");
    Mount(gun, "Muzzle_Suppressor_Surefire_RC3_C", "Fire Location");
}

static void Step() {
    if (s_wait > 0) { s_wait--; return; }
    switch (s_step) {
        case 0: {  // find everything
            auto stations = FindAllOf("CustomizationStation_Gun_C");
            Log("{} station(s)", stations.size());
            if (stations.empty()) { Error("no gun station in this level"); s_step = 99; return; }
            Vec cam = CameraLocation();
            double best = 1e30;
            for (auto& st : stations) {  // the one the player stands at
                Vec l = ActorLocation(st);
                double d = (l.x - cam.x) * (l.x - cam.x) + (l.y - cam.y) * (l.y - cam.y) + (l.z - cam.z) * (l.z - cam.z);
                Log("  {} at {:.0f} cm", st.Name(), std::sqrt(d));
                if (d < best && st.GetObj("WBGunBuilder")) { best = d; s_station = st; }
            }
            s_widget = s_station.GetObj("WBGunBuilder");
            for (auto& g : FindAllOf("GunAsset_C"))
                if (TextOf(g, "DisplayName") == "Noveske Chainsaw") s_noveske = g;
            s_hk = FindObject("/Game/Weapons/Data/DA_HK416a5_279.DA_HK416a5_279");
            Log("station {} widget {} noveske {} hk {}", s_station.Name(), s_widget.Name(), s_noveske.Name(), s_hk.Name());
            if (!s_widget || !s_noveske || !s_hk) { Error("missing pieces"); s_step = 99; return; }
            MovePlayerTo(s_station);
            Select(s_noveske);
            s_step = 1; s_wait = 90;
            return;
        }
        case 1: case 4: {  // confirm if the game asked, then take the new receiver
            Object cur = s_station.GetObj("CurrentReceiver");
            if (cur.ptr == s_before) {
                Log("no new receiver yet - confirming");
                Params c(s_widget, kConfirm);
                c.Call();
                s_wait = 90;
                s_step = s_step == 1 ? 2 : 5;
                return;
            }
            [[fallthrough]];
        }
        case 2: case 5: {
            Object cur = s_station.GetObj("CurrentReceiver");
            Log("station CurrentReceiver = {}", cur.FullName());
            if (!cur || cur.ptr == s_before) {
                static int fallbacks = 0;
                if (fallbacks++ < 2) {
                    // RequestWeapon is gated on the player standing at the station (no one is wearing
                    // the headset). SpawnWeapon is the step after that gate.
                    Log("FALLBACK: request was gated - calling the station's SpawnWeapon (real UI path NOT proven)");
                    Params sp(s_station, "SpawnWeapon");
                    sp.SetObj("GunAsset", s_pending).Call();
                    s_wait = 60;
                    return;
                }
                Error("spawn did not happen");
                s_step = 99;
                return;
            }
            Freeze(cur);
            s_hold = cur;
            bool second = s_step >= 4;
            s_step = second ? 6 : 3;
            s_wait = 120;
            return;
        }
        case 3: {
            Vec at = ActorLocation(s_hold), cam = CameraLocation();
            Log("held {} at ({:.0f},{:.0f},{:.0f}), camera ({:.0f},{:.0f},{:.0f}), hidden={}", s_hold.FullName(), at.x, at.y,
                at.z, cam.x, cam.y, cam.z, s_hold.GetBool("bHidden"));
            for (const char* cls : {"StaticMeshComponent", "DynamicMeshComponent", "SkeletalMeshComponent"}) {
                Params gc(s_hold, "K2_GetComponentsByClass");
                gc.Set("ComponentClass", FindClass(cls)).Call();
                auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
                for (int i = 0; arr && i < arr->Num; i++) {
                    Object c(((GUObject**)arr->Data)[i]);
                    Params vis(c, "IsVisible"); vis.Call();
                    Params loc(c, "K2_GetComponentLocation"); loc.Call();
                    Vec l = loc.Return<Vec>();
                    Log("  {} visible={} hiddenInGame={} at ({:.0f},{:.0f},{:.0f})", c.Name(), vis.Return<bool>(),
                        c.GetBool("bHiddenInGame"), l.x, l.y, l.z);
                }
            }
            DumpAttachPoints(s_hold);
            MountAll(s_hold);
        }
            s_wait = 400;  // let the attachments stream in
            s_step = 31;
            return;
        case 31:
            Log("SNAP noveske");
            s_wait = 900;  // hold still until the screenshot is taken (fast without a headset)
            s_step = 30;
            return;
        case 30:
            s_hold = Object();
            Select(s_hk);
            s_step = 4; s_wait = 90;
            return;
        case 6:
            MountAll(s_hold);
            s_wait = 400;
            s_step = 8;
            return;
        case 8:
            Log("SNAP hk416");
            s_wait = 900;
            s_step = 7;
            return;
        case 7:
            Log("DEV TEST COMPLETE");
            s_step = 99;
            return;
    }
}

GML_AWAKE() {
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gm) {
        std::string level = Object((GUObject*)gm).Path();
        Log("level: {}", level);
        if (level.find("/Game/Maps/MainMenu/") == 0) {
            Params p(Lib("GameplayStatics"), "OpenLevel");
            p.SetObj("WorldContextObject", Object(API->WorldContext()))
                .Set("LevelName", API->MakeName("/Game/Maps/TeamRoom/Teamroom"))
                .Set("bAbsolute", true)
                .Call();
            Log("opening the team room");
        } else if (level.find("Teamroom") != std::string::npos) {
            s_step = 0;
            s_wait = 240;  // let the room settle
        }
    });
    On(GML_EVENT_TICK, [](void*) {
        if (s_step >= 0 && s_step < 99) Step();
        Hold();
    });
    // The station eases its gun back to the table in its own tick; win that race.
    HookAfter("CustomizationStation_Gun_C:ReceiveTick", [](Object, GUFunction*, void*) { Hold(); });
    return 0;
}
