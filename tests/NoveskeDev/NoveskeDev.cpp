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
    Vec at{cam.x + f.x * 40, cam.y + f.y * 40, cam.z + 6};
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

#include "Displays.inc"

static void Step() {
    if (s_wait > 0) { s_wait--; return; }
    switch (s_step) {
        case 50:
            SpawnDisplayRows();
            s_step = 51; s_wait = 150;  // displays
            return;
        case 51:
            Log("SNAP displays");
            s_step = 0; s_wait = 90;
            return;
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
            Log("SNAP noveske");
        }
            s_wait = 150;
            s_step = 30;
            return;
        case 30:
            s_hold = Object();
            Select(s_hk);
            s_step = 4; s_wait = 90;
            return;
        case 6:
            Log("SNAP hk416");
            s_wait = 150;
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
