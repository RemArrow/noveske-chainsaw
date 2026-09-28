# Prepares the Noveske Chainsaw's runtime assets for the GML plugin (no cooking, no pak):
#
#   Assets/noveske.obj          rifle, Blender default OBJ axes, one `usemtl` group per part
#                               (MI_Body, MI_Handguard, MI_Stock, MI_Grip, MI_Muzzle, MI_Feer)
#   Assets/<Part>_Diffuse.png   \
#   Assets/<Part>_Normal.png     > the three parameters of the game's weapon shader M_DefaultShader
#   Assets/<Part>_ORM.png       /
#
# Inputs come from the purchased 3DMA "Noveske Chainsaw" model, which is not in this repo:
#   NOVESKE_FBX       the assembled rifle as one FBX with 6 part materials, in the HK416's
#                     receiver space (X forward, Z up, metres). The plugin's [Visuals] Offset* in
#                     its .cfg adds the fine alignment on top (see tools/align).
#   NOVESKE_TEXTURES  folder with Chainsaw_FDE_<Part>_{BaseColor,Normal,Roughness,Metalness,AO}.png
#
# The game imports loose PNGs as sRGB textures (KismetRenderingLibrary::ImportFileAsTexture2D),
# so Normal and ORM - linear data - are pre-encoded with the sRGB curve here; the GPU's sRGB
# decode then hands the shader the original linear values.
#
# Run:  set NOVESKE_FBX=...  &  set NOVESKE_TEXTURES=...
#       blender.exe --background --python tools/prepare_runtime.py
import bpy, os
import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_FBX = os.environ.get("NOVESKE_FBX", "")
TEX_DIR = os.environ.get("NOVESKE_TEXTURES", "")
OUT_DIR = os.path.join(REPO, "Assets")
if not os.path.isfile(SRC_FBX) or not os.path.isdir(TEX_DIR):
    raise SystemExit("set NOVESKE_FBX (assembled rifle .fbx) and NOVESKE_TEXTURES (PBR texture folder)")

# Output resolution per part (VRAM: these textures are uncompressed, 4 bytes/pixel, no mips).
SIZES = {"Body": 2048, "Handguard": 2048, "Stock": 2048, "Grip": 1024, "Muzzle": 1024, "Feer": 1024}
ORM_SIZE_DIVISOR = 2          # ORM at half the colour resolution
FLIP_NORMAL_GREEN = False     # set True if the source normals are OpenGL-style (UE wants DirectX)

os.makedirs(OUT_DIR, exist_ok=True)


def load(name):
    path = os.path.join(TEX_DIR, f"Chainsaw_FDE_{name}.png")
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
    base = load(f"{part}_BaseColor")
    nrm = load(f"{part}_Normal")
    rough, metal, ao = load(f"{part}_Roughness"), load(f"{part}_Metalness"), load(f"{part}_AO")
    if base is None or nrm is None:
        raise SystemExit(f"missing BaseColor/Normal for {part}")

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

# Mesh: Blender default OBJ axes (forward -Z, up Y) = GML_AXIS_BLENDER_OBJ, one group per material.
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=SRC_FBX)
bpy.ops.wm.obj_export(filepath=os.path.join(OUT_DIR, "noveske.obj"), export_materials=True,
                      export_material_groups=False, export_uv=True, export_normals=True,
                      forward_axis='NEGATIVE_Z', up_axis='Y', global_scale=1.0, path_mode='STRIP')
print("WROTE noveske.obj")
