# Prepares the Noveske Chainsaw's runtime assets for the GML plugin (no cooking, no pak). They are
# the input to tools/pack, which encrypts them into the DLL; nothing here ships as loose files.
#
#   Assets/noveske.obj          body, handguard, stock, grip, FEER riser - one `usemtl` group per part
#   Assets/noveske_muzzle.obj   flash hider (hidden while a muzzle device is mounted)
#   Assets/noveske_bolt.obj, noveske_charginghandle.obj, noveske_dustcover.obj,
#   Assets/noveske_trigger.obj, noveske_selector.obj, noveske_boltcatch.obj, noveske_magrelease.obj,
#   Assets/noveske_magreleaseambi.obj
#                               the moving parts, each on its own so the plugin can hang it on the
#                               bone the game drives (bolt carrier, charging handle, ...)
#   Assets/noveske_pmag.obj     the PMAG that comes with the model
#   Assets/<Part>_Diffuse/Normal/ORM.png   textures for M_DefaultShader, per part (+ PMag)
#
# All meshes share one frame: Blender OBJ axes, muzzle +X, the frame the offset and the attach-point
# table in the plugin were measured in (tools/align).
#
# Inputs, from the purchased 3DMA "Noveske Chainsaw" pack (not in this repo):
#   NOVESKE_FBX       Mesh/Noveske_Chainsaw.fbx
#   NOVESKE_TEXTURES  the pack's Textures folder (containing PBR_Chainsaw_FDE_tx and PBR_PMAG_Black_tx)
#
# The game imports loose PNGs as sRGB textures (KismetRenderingLibrary::ImportBufferAsTexture2D), so
# Normal and ORM - linear data - are pre-encoded with the sRGB curve here; the GPU's sRGB decode then
# hands the shader the original linear values.
#
# Run:  set NOVESKE_FBX=...  &  set NOVESKE_TEXTURES=...
#       blender.exe --background --python tools/prepare_runtime.py
import bpy, math, os
import numpy as np
from mathutils import Matrix, Vector

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_FBX = os.environ.get("NOVESKE_FBX", "")
TEX_ROOT = os.environ.get("NOVESKE_TEXTURES", "")
OUT_DIR = os.path.join(REPO, "Assets")
if not os.path.isfile(SRC_FBX) or not os.path.isdir(TEX_ROOT):
    raise SystemExit("set NOVESKE_FBX (the pack's Noveske_Chainsaw.fbx) and NOVESKE_TEXTURES (its Textures folder)")
# Also accept the FDE folder itself, as earlier versions did.
FDE_DIR = TEX_ROOT if os.path.exists(os.path.join(TEX_ROOT, "Chainsaw_FDE_Body_BaseColor.png")) else os.path.join(TEX_ROOT, "PBR_Chainsaw_FDE_tx")
PMAG_DIR = os.path.join(os.path.dirname(FDE_DIR), "PBR_PMAG_Black_tx")
os.makedirs(OUT_DIR, exist_ok=True)

# Frame: the pack models the rifle muzzle +Y; turn it to +X and apply the alignment nudge.
ROT = Matrix.Rotation(math.radians(-90), 4, "Z")
DELTA = Vector((-0.0904, -0.0044, -0.0101))  # metres

# Output resolution per part (VRAM: these textures are uncompressed, 4 bytes/pixel, no mips).
SIZES = {"Body": 2048, "Handguard": 2048, "Stock": 2048, "Grip": 1024, "Muzzle": 1024, "Feer": 1024, "PMag": 1024}
ORM_SIZE_DIVISOR = 2          # ORM at half the colour resolution
FLIP_NORMAL_GREEN = False     # set True if the source normals are OpenGL-style (UE wants DirectX)


def load(folder, name):
    path = os.path.join(folder, name + ".png")
    if not os.path.exists(path):
        return None
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = 'Non-Color'  # read raw file values, no conversion
    return img


def pixels(img, size):
    if tuple(img.size) != (size, size):
        img.scale(size, size)
    return np.array(img.pixels[:], dtype=np.float32).reshape(size, size, 4)


def to_srgb(v):
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(np.clip(v, 0, 1), 1 / 2.4) - 0.055)


def save(arr, size, name):
    img = bpy.data.images.new(name, size, size, alpha=True)
    img.colorspace_settings.name = 'Non-Color'  # write values as-is
    img.pixels[:] = np.clip(arr, 0, 1).ravel()
    img.filepath_raw = os.path.join(OUT_DIR, name + ".png")
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)
    print("WROTE", name, size)


for part, size in SIZES.items():
    folder, prefix = (PMAG_DIR, "PMAG_Black") if part == "PMag" else (FDE_DIR, "Chainsaw_FDE_" + part)
    base = load(folder, prefix + "_BaseColor")
    nrm = load(folder, prefix + "_Normal")
    rough, metal, ao = load(folder, prefix + "_Roughness"), load(folder, prefix + "_Metalness"), load(folder, prefix + "_AO")
    if base is None or nrm is None:
        raise SystemExit(f"missing BaseColor/Normal for {part} in {folder}")

    d = pixels(base, size)
    d[..., 3] = 1.0
    save(d, size, f"{part}_Diffuse")  # already sRGB colour: written unchanged

    n = pixels(nrm, size)
    if FLIP_NORMAL_GREEN:
        n[..., 1] = 1.0 - n[..., 1]
    n[..., :3] = to_srgb(n[..., :3])
    n[..., 3] = 1.0
    save(n, size, f"{part}_Normal")

    osz = max(256, size // ORM_SIZE_DIVISOR)
    orm = np.ones((osz, osz, 4), dtype=np.float32)
    orm[..., 0] = pixels(ao, osz)[..., 0] if ao else 1.0      # R = ambient occlusion
    orm[..., 1] = pixels(rough, osz)[..., 0] if rough else 0.6  # G = roughness
    orm[..., 2] = pixels(metal, osz)[..., 0] if metal else 0.0  # B = metallic
    orm[..., :3] = to_srgb(orm[..., :3])
    save(orm, osz, f"{part}_ORM")
    for im in (base, nrm, rough, metal, ao):
        if im:
            bpy.data.images.remove(im)

# ---- meshes
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=SRC_FBX)
meshes = [o for o in bpy.data.objects if o.type == 'MESH']
world = {o.name: o.matrix_world.copy() for o in meshes}  # the pack parents parts to each other
for o in meshes:
    o.parent = None
done = set()
for o in meshes:
    if o.data.name in done:
        continue
    m = ROT @ world[o.name]
    m.translation += DELTA
    o.data.transform(m)  # baked into the mesh data: object transforms are unreliable with parents
    done.add(o.data.name)
    o.matrix_world = Matrix.Identity(4)
bpy.context.view_layer.update()

# The bolt catch and both magazine releases aren't objects of their own in the pack; they are loose
# pieces inside ChainSaw_Body. Cut them out so they can ride their bones too. Found as the small loose
# piece nearest to the game's pivot for each (gun space, cm, with the plugin's default offset).
import bmesh
OFFSET_CM = Vector((2.154, -0.466, -0.909))
PIVOTS = {"ChainSaw_BoltCatch": Vector((1.156, -1.621, 1.007)), "ChainSaw_MagRelease": Vector((1.031, 1.485, -0.786)),
          "ChainSaw_MagReleaseAmbi": Vector((2.651, -1.555, -0.635))}


def islands(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    seen, out = set(), []
    for v in bm.verts:
        if v.index in seen:
            continue
        stack, comp = [v], []
        seen.add(v.index)
        while stack:
            x = stack.pop()
            comp.append(x.index)
            for e in x.link_edges:
                y = e.other_vert(x)
                if y.index not in seen:
                    seen.add(y.index)
                    stack.append(y)
        out.append(comp)
    bm.free()
    return out


def gun_space(co):  # Blender (m) -> UE gun space (cm), as the plugin places the mesh
    return Vector((co.x * 100, -co.y * 100, co.z * 100)) + OFFSET_CM


body = bpy.data.objects["ChainSaw_Body"]
for name, pivot in PIVOTS.items():
    best, best_d = None, 2.0
    for comp in islands(body):
        pts = [gun_space(body.data.vertices[i].co) for i in comp]
        lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
        hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
        d = ((lo + hi) / 2 - pivot).length
        if max(hi - lo) < 5 and d < best_d:
            best, best_d = comp, d
    if best is None:
        print(f"WARNING: no loose piece near {name}'s pivot - it stays part of the body")
        continue
    bpy.ops.object.select_all(action='DESELECT')
    body.select_set(True)
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.mode_set(mode='EDIT')
    em = bmesh.from_edit_mesh(body.data)
    em.verts.ensure_lookup_table()
    keep = set(best)
    for f in em.faces:
        f.select_set(all(v.index in keep for v in f.verts))
    for v in em.verts:
        v.select_set(v.index in keep)
    bmesh.update_edit_mesh(body.data)
    bpy.ops.mesh.separate(type='SELECTED')
    bpy.ops.object.mode_set(mode='OBJECT')
    part = [o for o in bpy.context.selected_objects if o != body][0]
    part.name = name
    print(f"SPLIT {name}: {len(best)} verts, {best_d:.2f} cm from the pivot")

OUTPUTS = {
    "noveske": ["ChainSaw_Body", "ChainSaw_Handguard", "ChainSaw_Stock", "ChainSaw_Grip", "ChainSaw_Feer"],
    "noveske_boltcatch": ["ChainSaw_BoltCatch"],
    "noveske_magrelease": ["ChainSaw_MagRelease"],
    "noveske_magreleaseambi": ["ChainSaw_MagReleaseAmbi"],
    "noveske_muzzle": ["ChainSaw_Muzzle"],
    "noveske_bolt": ["ChainSaw_Bolt"],
    "noveske_charginghandle": ["ChainSaw_ChargingHandle"],
    "noveske_dustcover": ["ChainSaw_DustCover"],
    "noveske_trigger": ["ChainSaw_Trigger"],
    "noveske_selector": ["ChainSaw_Selector"],
    "noveske_pmag": ["ChainSaw_PMag"],
}
for name, parts in OUTPUTS.items():
    objs = [bpy.data.objects.get(p) for p in parts]
    if None in objs:
        if parts[0] in PIVOTS:  # optional: the piece stayed in the body
            continue
        raise SystemExit(f"{name}: part missing from the pack's FBX ({parts})")
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    # Blender default OBJ axes (forward -Z, up Y) = GML_AXIS_BLENDER_OBJ, one group per material.
    bpy.ops.wm.obj_export(filepath=os.path.join(OUT_DIR, name + ".obj"), export_selected_objects=True,
                          export_materials=True, export_material_groups=False, export_uv=True, export_normals=True,
                          forward_axis='NEGATIVE_Z', up_axis='Y', global_scale=1.0, path_mode='STRIP')
    print("WROTE", name + ".obj")
