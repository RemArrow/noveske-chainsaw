// Display rows for isolating rendering problems: spawns StaticMeshActors in front of the camera.
// Included by NoveskeDev.cpp.

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

static Object SpawnDisplay(GUObject* mesh, const std::vector<GUObject*>& mats, Vec loc, Rot rot) {
    auto xf = MakeXf(loc, rot);
    Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
    b.SetObj("WorldContextObject", Object(API->WorldContext()));
    b.Set("ActorClass", FindClass("StaticMeshActor"));
    std::memcpy(b.Ptr("SpawnTransform"), xf.data(), xf.size());
    b.Set("CollisionHandlingOverride", (uint8_t)1);  // AlwaysSpawn
    b.Call();
    Object actor = b.Return<GUObject*>();
    if (!actor) { Error("display spawn failed"); return {}; }
    Params f(Lib("GameplayStatics"), "FinishSpawningActor");
    f.SetObj("Actor", actor);
    std::memcpy(f.Ptr("SpawnTransform"), xf.data(), xf.size());
    f.Call();
    Object comp = actor.GetObj("StaticMeshComponent");
    Params mob(comp, "SetMobility");
    mob.Set("NewMobility", (uint8_t)2).Call();  // Movable
    if (Object(mesh).IsA("DynamicMesh")) {  // the Noveske: GML 2.1 dynamic mesh path
        Object dc = API->AddDynamicMeshComponent(actor.ptr, mesh, mats.data(), (int)mats.size());
        Log("display {} at ({:.0f},{:.0f},{:.0f}): dynamic {}", actor.Name(), loc.x, loc.y, loc.z, dc ? "ok" : "FAILED");
        return actor;
    }
    Params sm(comp, "SetStaticMesh");
    sm.SetObj("NewMesh", mesh).Call();
    for (int i = 0; i < (int)mats.size(); i++) {
        Params m(comp, "SetMaterial");
        m.Set("ElementIndex", (int32_t)i).SetObj("Material", mats[i]).Call();
    }
    Log("display {} at ({:.0f},{:.0f},{:.0f}): {}", actor.Name(), loc.x, loc.y, loc.z, sm.Return<bool>() ? "ok" : "mesh rejected");
    return actor;
}

static GUObject* MakeMid(Object shader, const std::wstring& part, bool diffuseOnly) {
    std::wstring dir = GetPath(GML_PATH_PLUGINS) + L"\\NoveskeChainsaw\\Assets\\";
    GUObject* mid = API->CreateMaterialInstance(shader.ptr);
    API->SetMaterialTexture(mid, "Diffuse", API->ImportTexture((dir + part + L"_Diffuse.png").c_str()));
    if (!diffuseOnly) {
        API->SetMaterialTexture(mid, "Normal", API->ImportTexture((dir + part + L"_Normal.png").c_str()));
        API->SetMaterialTexture(mid, "ORM", API->ImportTexture((dir + part + L"_ORM.png").c_str()));
    }
    return mid;
}

static void SpawnDisplayRows() {
    Params p(Lib("GameplayStatics"), "GetPlayerCameraManager");
    p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
    Object pcm = p.Return<GUObject*>();
    Params lp(pcm, "GetCameraLocation"); lp.Call();
    Params rp(pcm, "GetCameraRotation"); rp.Call();
    Vec cam = lp.Return<Vec>();
    Rot rot = rp.Return<Rot>();
    Params fp(Lib("KismetMathLibrary"), "GetForwardVector");
    fp.Set("InRot", Rot{0, rot.yaw, 0}).Call();
    Vec f = fp.Return<Vec>();
    Vec base{cam.x + f.x * 150, cam.y + f.y * 150, cam.z};
    Rot side{0, rot.yaw + 90, 0};

    Object hk = LoadObject("/Game/Weapons/HK416a5/Mesh/HK_416_279_v2.HK_416_279_v2");
    Object hkMat = LoadObject("/Game/Weapons/HK416a5/Mesh/HK_416_receiver_RAL8000.HK_416_receiver_RAL8000");
    Object shader = LoadObject("/Game/Art/Common/M_DefaultShader.M_DefaultShader");
    const wchar_t* parts[] = {L"Body", L"Handguard", L"Stock", L"Grip", L"Muzzle", L"Feer"};
    const char* groups[] = {"MI_Body", "MI_Handguard", "MI_Stock", "MI_Grip", "MI_Muzzle", "MI_Feer"};
    std::vector<GUObject*> full, diffuse, hkAll(6, hkMat.ptr);
    for (auto* pt : parts) { full.push_back(MakeMid(shader, pt, false)); diffuse.push_back(MakeMid(shader, pt, true)); }

    GML_MeshImport o{};
    o.size = sizeof o;
    o.axis = GML_AXIS_BLENDER_OBJ;
    o.flipWinding = -1;
    o.materialCount = 6;
    o.materialNames = groups;
    o.materials = full.data();
    std::wstring obj = GetPath(GML_PATH_PLUGINS) + L"\\NoveskeChainsaw\\Assets\\noveske.obj";
    GUObject* nov = API->ImportDynamicMesh(obj.c_str(), &o);

    SpawnDisplay(hk.ptr, {}, {base.x, base.y, base.z + 100}, side);         // 1 reference
    SpawnDisplay(nov, hkAll, {base.x, base.y, base.z + 74}, side);          // 2 mesh with a known-good material
    SpawnDisplay(nov, diffuse, {base.x, base.y, base.z + 48}, side);        // 3 diffuse only
    SpawnDisplay(nov, full, {base.x, base.y, base.z + 22}, side);           // 4 full material
}
