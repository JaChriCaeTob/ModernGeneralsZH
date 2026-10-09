# Replacing models (Blender workflow)

The tools in `tools/` convert the game's W3D models to `.glb` (opens in Blender with no extra plugin) and put an edited model back into a W3D
file that keeps the game's names, skeleton, materials and level-of-detail setup, so the game accepts it without any naming work.
The models are the game's property: extract them yourself from your own archives, and only commit art you made yourself (`assets/Art`).

## 1. Get the models out of your game

```powershell
python tools\bigtool.py extract "<GameDir>\W3DZH.big" E:\GeneralsModels\original
python tools\bigtool.py extract "<GameDir>\TexturesZH.big" E:\GeneralsModels\original
# the base game's archives (the Zero Hour units that come from Generals live there): <Generals folder>\W3D.big and Textures.big
python tools\w3d_to_glb.py E:\GeneralsModels\original\Art\W3D E:\GeneralsModels\glb --textures E:\GeneralsModels\original\Art\Textures
python tools\w3d_sizes.py E:\GeneralsModels\original\Art\W3D E:\GeneralsModels\model_sizes.csv      # triangle counts and sizes
```

One `.glb` per W3D file; textures are converted once to `glb\textures_png`. Needs Pillow for the textures (`python -m pip install pillow`).

## 2. Edit in Blender

1. *File > Import > glTF 2.0*, pick for example `glb\AvHummer.glb`. The hierarchy and object names are the game's (`AVHUMMER.CHASSIS`, bones as empties).
2. Model your replacement. **The object that replaces a mesh must carry the same name** (Blender's `.001` suffix is ignored). Keep the size of the
   original (see `model_sizes.csv`), stay near its triangle count, and unwrap UVs for one texture per mesh.
3. *File > Export > glTF 2.0*, format *glTF Binary (.glb)*, keep "Y Up" on, include normals and UVs, apply modifiers.

## 3. Put it into the game

```powershell
python tools\glb_to_w3d.py edited.glb E:\GeneralsModels\original\Art\W3D\AvHummer.W3D assets\Art\W3D\AvHummer.W3D
python tools\glb_to_w3d.py E:\GeneralsModels\original\Art\W3D\AvHummer.W3D --list      # meshes of the original and the texture each one uses
```

Meshes of the original that are not in your `.glb` stay as they were. Materials, shaders and texture names come from the original, so save your texture as
a DDS with the **same name** as the texture `--list` shows (Blender cannot write DDS; use GIMP, Paint.NET or ImageMagick) into `assets\Art\Textures`.
Then run `tools\install-to-game.ps1`: everything under `assets\Art` is copied into the game folder as loose files, which override the archives.
To go back to the original, delete those files from `<GameDir>\Art`.

## Limits

- Skinned meshes (characters with vertex weights) can be exported for reference but not written back. Rigid vehicles, buildings and props are fine.
- New meshes that the original does not have are not added; reuse or rename an existing mesh object.
- The collision tree of a mesh (if it has one) is dropped, the game works without it.
- Not yet tried in the game: replace one simple model first (a tree, a crate) to check the result before you build a whole unit.
