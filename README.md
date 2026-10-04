# Noveske Chainsaw for Geronimo

A [GML](https://github.com/RemArrow/geronimo-mod-loader) plugin that adds a **Noveske Chainsaw**
to Geronimo's gun spawner. Guns spawned from its entry get the Noveske's own mesh and full PBR
textures. **Nothing in the game is replaced**: the stock HK416 entry, and every other HK416, stays
an HK416.

## Credits

- **3D model and textures:** [NOVESKE IRREGULAR DEFENSE CHAINSAW RIFLE](https://www.3dmilitaryassets.com/products/noveske-irregular-defense-chainsaw-rifle)
  by **3DMA — 3D Military Assets** ([www.3dmilitaryassets.com](https://www.3dmilitaryassets.com)),
  used under license. It's 3DMA's work: the model ships only inside the DLL, encrypted, and must
  not be extracted or redistributed. If you like the gun, support 3DMA by buying from
  [their store](https://www.3dmilitaryassets.com) or joining their
  [Discord](https://discord.gg/UBCwpR9).
- **Plugin:** RemArrow, running on [GML](https://github.com/RemArrow/geronimo-mod-loader).
- **Real-world rifle:** the Chainsaw is a Noveske Rifleworks × Irregular Defense collaboration.

The same credits ship next to the DLL as `CREDITS.txt`, sit in the DLL's file properties
(Properties → Details), and are logged at every launch. This mod is not affiliated with or
endorsed by 3DMA, Noveske Rifleworks, Irregular Defense or Dark Matter Studios (Geronimo's
developer); all trademarks belong to their owners.

## How it works

1. **Spawner entry.** After `BP_GameInstance::GetItems` builds the gun lists, the plugin constructs
   a new `GunAsset_C` at runtime (no cooked asset, no package) from the HK416 (279 mm)'s settings.
   It then appends it to the Assault Rifles list, which goes from 10 to 11 entries.
2. **Which spawns are ours.** The entry borrows the HK416 receiver blueprint for handling, grips,
   attachment sockets and moving parts. When the gun station spawns that receiver, the plugin
   checks which menu item was picked in *that station's* spawner widget. The selection event
   passes the item widget, and its `WeaponItem` is the gun.
3. **Visuals.** Only that actor gets `DynamicMeshComponent`s with the Noveske's meshes. They use
   seven instances of the game's weapon shader `M_DefaultShader`, one per texture set (body,
   handguard, stock, grip, muzzle, FEER, PMAG), fed `Diffuse` / `Normal` / `ORM` textures. All
   meshes and textures are **built into `NoveskeChainsaw.dll` encrypted** (see
   [Asset protection](#asset-protection)) and loaded from memory with GML 2.2's
   `ImportDynamicMeshFromMemory` / `ImportTextureFromMemory`, so the plugin is a single file.
   The HK416's mesh is hidden; its collision, physics and handling stay.
4. **Alignment.** The mesh is offset so the Noveske's lower receiver sits on the HK416's: its
   trigger, selector, bolt catch and magazine line up. The default offset was computed by
   `tools/align`, a translation-only trimmed ICP of the Noveske lower onto the HK416 lower
   extracted from the cooked game mesh, at 0.21 cm RMS.
5. **Attachments.** Optics, grips, lights and muzzle devices snap to the receiver's attachment
   splines (two Picatinny, seven M-LOK) and its `Fire Location`. On the HK416 those sit on the
   HK416's rails: the optic would float 4 mm above the Noveske's top rail and a foregrip hang
   5.5 mm below its handguard. On dressed guns they are moved onto the Noveske's own rail tops,
   M-LOK faces and barrel end, which `tools/align` finds by ray-casting the mesh. The rails also run
   the Noveske's full handguard length (to x = 33.5 cm, not the HK416's 30.5), and the muzzle point
   moves 1.1 cm forward; shots come from there too. The Noveske's flash hider is hidden while a muzzle
   device or suppressor is mounted in its place.
7. **Moving parts.** The game animates a gun's moving parts as bones of its "movables" skeleton
   (`GunMovablesMeshComponent`, a PoseableMesh the gun code poses by bone name). The Noveske's own
   parts ride those bones: bolt carrier (`RootBoltCarrierGroup`), charging handle, dust cover,
   trigger, selector, bolt catch (`RootBoltRelease`) and both magazine releases. Each is attached
   to its bone at the inverse of the bone's reference pose, so it sits right whatever the gun is
   doing when it is dressed. The HK416's parts are hidden, but their bones keep being posed
   (`VisibilityBasedAnimTickOption` = always), so racking, firing, trigger pull, selector and bolt
   catch all move the Noveske's parts. The pack has no separate bolt catch or releases; the asset
   script cuts them out of the body mesh.

   The **dust cover** is the exception. The Noveske's turns on its own hinge pin, 0.3 cm above
   and 0.1 cm inside the HK416's, and lies a half turn from open on the receiver's 22.5° slope
   when shut. The game re-poses the HK416's bone at the HK416's pin every frame. So the cover hangs
   on the movables component instead, and each frame it is turned as the bone is turned, but about
   the Noveske's pin. The gun also gets the Noveske's open and shut angles (0 and −179.5°), just short
   of a half turn, so the game's shortest-way swing takes the cover outward and not through the
   receiver.
8. **Magazines.** The HK416 takes the game's windowed PMAG. Magazines in a Noveske, and the spares
   you carry while you have one, become the pack's own PMAG: its mesh replaces the magazine's (the
   game's ammo count, handling and reloads stay; the visible rounds are hidden, as a solid PMAG
   shows none). The PMAG is hung at the inverse of where a seated magazine sits in the gun,
   measured from the first one seen seated.
6. **Loadouts and operations.** The game saves the guns you carry to `SaveGames\SavedGun_<slot>.sav`
   and rebuilds them at team-room start and in every operation. Those rebuilt guns are recognised
   and dressed too: a receiver that becomes a player's `PrimaryGun`/`SecondaryGun` is checked
   against the save slot it was loaded from. The runtime Noveske asset's object path changes
   between launches, which used to turn a saved Noveske into the game's default rifle. So once the
   game saves the Noveske, the plugin re-saves that slot with the game's own `SaveGameToSlot`,
   naming the HK416 donor. It keeps the file's hash under `[Loadouts]` in its `.cfg`, and a slot
   whose file still has that hash is the Noveske. Saves written by earlier versions are repaired
   at startup. Attachments restored from a save are snapped onto the Noveske's rails. Without the
   plugin, such a slot loads as an HK416 with the same attachments.

## Requirements

- Geronimo (Steam, UE 5.7.4) with **GML 2.2.0 or newer**
  ([releases](https://github.com/RemArrow/geronimo-mod-loader/releases)).
- To build it: a licensed copy of 3DMA's
  [NOVESKE IRREGULAR DEFENSE CHAINSAW RIFLE](https://www.3dmilitaryassets.com/products/noveske-irregular-defense-chainsaw-rifle).
  Its geometry and textures are **not** in this repo: `Assets\` is generated locally from your copy
  and compiled into the DLL, encrypted. What you may do with a build depends on your 3DMA license
  tier ([licenses](https://www.3dmilitaryassets.com/pages/licences)). For example, the Extended
  license requires the credit above in the product description.
- Visual Studio 2022 and Blender (for the asset step).

## Asset protection

The 3DMA art must not be rippable from the plugin. 3DMA's license requires "reasonable measures"
to ensure its assets "cannot be easily ripped, extracted, or reverse-engineered". `build.bat`
runs `tools/pack` over `Assets\` on every build:

- Every asset is **AES-256-GCM encrypted** (the OBJ is LZMS-compressed first) with a **random key
  per build**. They are stored as one resource with no names, only salted hashes
  (`src/payload.h`). The DLL contains no PNG or OBJ signatures and no plain asset bytes, so
  resource editors, 7-Zip, binwalk-style carvers and pak tools (UModel/FModel) find nothing.
- The key is compiled in as two XOR halves (`build/gen/payload_key.h`, never committed). The whole
  key exists only for the moment an asset is decrypted.
- The plugin decrypts **one asset at a time**, hands it to the engine and wipes it, so at most
  one asset is ever in plain form, only briefly.
- GCM authenticates every asset: a modified DLL is refused ("embedded asset … is missing or
  damaged"), not decoded.
- `pack` proves each build before writing it. Every asset must decrypt byte-identical, a payload
  with one flipped byte must be refused, and no PNG/OBJ markers may appear. Symbols (`.pdb`) are
  not installed or released.

What no game can prevent, this one included: once the gun is rendered, GPU capture tools
(RenderDoc, NinjaRipper) can copy the mesh and textures the graphics card is drawing. A skilled
reverse engineer with a debugger can also find the key, because the game has to be able to
decrypt the art to show it. This protection is aimed at file ripping, which is how assets are
normally stolen from mods.

## Build and install

```bat
git clone --recursive <this repo>
set NOVESKE_FBX=<the pack's Mesh\Noveske_Chainsaw.fbx>
set NOVESKE_TEXTURES=<the pack's Textures folder>
blender --background --python tools\prepare_runtime.py
build.bat install
```

`install` copies the single `NoveskeChainsaw.dll` (about 50 MB with the encrypted art inside) and `CREDITS.txt` to
`...\Geronimo\Binaries\Win64\GML\plugins\NoveskeChainsaw\`. It removes a loose `Assets\` folder
left there by versions before 1.4, and refuses to run while the game is running. Set
`GML_GAME_DIR` if the game isn't in the default Steam library, and `GML_SDK` to build against a
GML checkout other than `external/gml`.

## Settings: `GML\config\com.remarrow.noveskechainsaw.cfg`

| Key | Default | |
|---|---|---|
| `[Visuals] Enabled` | true | dress guns spawned from the Noveske entry |
| `[Visuals] OffsetX / OffsetY / OffsetZ` | 2.154 / −0.466 / −0.909 | cm; + = muzzle / right / up |
| `[Visuals] FitAttachments` | true | move the attachment rails and muzzle point onto the Noveske, full handguard length |
| `[Visuals] OwnMovingParts` | true | the Noveske's own bolt carrier, charging handle, dust cover, trigger, selector, bolt catch and magazine releases |
| `[Visuals] OwnMagazines` | true | magazines in a Noveske, and the spares you carry with one, are the Noveske's PMAG |
| `[Debug] Probe` | false | package-loading diagnostic |

## Tools

| | |
|---|---|
| `tools/prepare_runtime.py` | the pack's FBX + 4K PBR textures → `Assets\` (the build input that gets encrypted and embedded): the static rifle, the flash hider, each moving part (the bolt catch and releases cut out of the body mesh) and the PMAG as separate OBJs in one frame; per part Diffuse, Normal and ORM PNGs at 2K/1K, with linear data pre-encoded for the game's sRGB import |
| `tools/pack` (`build\pack.exe`) | `Assets\` → `build\gen\payload.bin` + `payload_key.h`: the encrypted payload and this build's key (see [Asset protection](#asset-protection)); run by `build.bat` |
| `tools/align` (`build\align.exe`) | `align <HK_416_279_v2.uexp> Assets\noveske.obj [x y z]`: computes the offset from the cooked HK416 mesh (extract it with retoc `to-legacy -f HK_416_279`), then prints the plugin's `kAttachPoints` table for that offset (or for `x y z`) |
| `tests/NoveskeDev` | in-game test harness, never installed by `build.bat`. It loads the team room and presses the spawner's Noveske item through the game's own button handler. It then logs the receiver's attach points (`ATTACH`), mounts a foregrip, optic and suppressor through `AttachToServer` (`MOUNT`), and poses the gun in front of the headset camera for screenshots. It does the same for a stock HK416 to compare. `[Test] Scenario` in its `.cfg` picks the run: `station` (the above), `loadout` (saves the Noveske with the game's `SaveGunSetup` and rebuilds it with `LoadGunSetups`, as an operation does) `operation` (loads the CargoShip operation and checks the rifle you're given) or `movables` (checks the moving parts ride their bones through a trigger pull and a pulled charging handle, the PMAG, the rail lengths and the flash hider under a suppressor). Screenshots are rendered by the game (`shot`), never taken from the desktop |

## Known limitations

- Multiplayer (untested): the Noveske look is local. Only your own loadout gun is checked against
  your save slots, and other players get the HK416 donor from the game, so they see an HK416 with
  the same attachments and the game's PMAG.
- Spare magazines turn into the Noveske's PMAG while you carry a Noveske; one picked up while you
  don't stays the game's PMAG until it goes into a Noveske.
- If you save the Noveske and quit within about a second, before the plugin re-saves the slot,
  the next launch repairs it (tested). If the plugin is removed, the slot loads as an HK416.
- Runtime textures are uncompressed and have no mipmaps, so expect some shimmer at a distance.
