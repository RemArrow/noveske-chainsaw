# Noveske Chainsaw for Geronimo

A [GML](https://github.com/RemArrow/geronimo-mod-loader) plugin that adds a **Noveske Chainsaw**
to Geronimo's gun spawner. Guns spawned from its entry get the Noveske's own mesh and full PBR
textures. **Nothing in the game is replaced**: the stock HK416 entry, and every other HK416, stays
an HK416.

## How it works

1. **Spawner entry.** After `BP_GameInstance::GetItems` builds the gun lists, the plugin constructs
   a new `GunAsset_C` at runtime (no cooked asset, no package) from the HK416 (279 mm)'s settings.
   It then appends it to the Assault Rifles list, which goes from 10 to 11 entries.
2. **Which spawns are ours.** The entry borrows the HK416 receiver blueprint for handling, grips,
   attachment sockets and moving parts. When the gun station spawns that receiver, the plugin
   checks which menu item was picked in *that station's* spawner widget. The selection event
   passes the item widget, and its `WeaponItem` is the gun.
3. **Visuals.** Only that actor gets a `DynamicMeshComponent` with the Noveske mesh
   (GML `ImportDynamicMesh` / `AddDynamicMeshComponent`). The mesh uses six instances of the
   game's weapon shader `M_DefaultShader`, one per part, fed `Diffuse` / `Normal` / `ORM` textures.
   The HK416's own mesh is hidden; its collision, physics, trigger, bolt, magazine and attachment
   rails stay in place.
4. **Alignment.** The mesh is offset so the Noveske's lower receiver sits on the HK416's: its
   trigger, selector, bolt catch and magazine line up. The default offset was computed by
   `tools/align`, a translation-only trimmed ICP of the Noveske lower onto the HK416 lower
   extracted from the cooked game mesh, at 0.21 cm RMS.

## Requirements

- Geronimo (Steam, UE 5.7.4) with **GML 2.1.0 or newer**
  ([releases](https://github.com/RemArrow/geronimo-mod-loader/releases)).
- The **Noveske Chainsaw** 3D model from 3DMA (purchased). Its geometry and textures are **not**
  in this repo; `Assets\` is generated locally from your copy.
- Visual Studio 2022 and Blender (for the asset step).

## Build and install

```bat
git clone --recursive <this repo>
set NOVESKE_FBX=<assembled rifle .fbx>
set NOVESKE_TEXTURES=<...\Textures\PBR_Chainsaw_FDE_tx>
blender --background --python tools\prepare_runtime.py
build.bat install
```

`install` copies `NoveskeChainsaw.dll` and `Assets\` to
`...\Geronimo\Binaries\Win64\GML\plugins\NoveskeChainsaw\`, and refuses to run while the game
is running. Set `GML_GAME_DIR` if the game isn't in the default Steam library.

## Settings: `GML\config\com.remarrow.noveskechainsaw.cfg`

| Key | Default | |
|---|---|---|
| `[Visuals] Enabled` | true | dress guns spawned from the Noveske entry |
| `[Visuals] OffsetX / OffsetY / OffsetZ` | 2.154 / −0.466 / −0.909 | cm; + = muzzle / right / up |
| `[Debug] Probe` | false | package-loading diagnostic |

## Tools

| | |
|---|---|
| `tools/prepare_runtime.py` | source FBX + 4K PBR textures → `Assets\`: OBJ with 6 material groups; per part Diffuse, Normal and ORM PNGs at 2K/1K, with linear data pre-encoded for the game's sRGB import |
| `tools/align` (`build\align.exe`) | `align <HK_416_279_v2.uexp> Assets\noveske.obj`: computes the offset from the cooked HK416 mesh (extract it with retoc `to-legacy -f HK_416_279`) |
| `tests/NoveskeDev` | in-game test harness, never installed by `build.bat`. It loads the team room, presses the spawner's Noveske item through the game's own button handler, and poses the result in front of the headset camera for screenshots |

## Known limitations

- The moving parts (bolt, charging handle, trigger, selector) and the magazine are the HK416's.
- Attachments snap to the HK416's rail positions, which don't exactly match the Noveske's rails.
- A Noveske saved into a loadout comes back as an HK416: only gun-station spawns are dressed.
- Runtime textures are uncompressed and have no mipmaps, so expect some shimmer at a distance.
