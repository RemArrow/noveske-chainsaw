// srgb - stages the plugin's textures for GML's runtime import. GML builds textures with the engine's
// ImportBufferAsTexture2D, and those are always sampled as sRGB. That's right for a colour map, but the data maps
// (<Part>_Normal.png, <Part>_ORM.png) are linear: decoded as sRGB, a flat normal tilts about 50 degrees and roughness
// 0.55 reads as 0.26. So the build packs copies of them with every channel sRGB-encoded, and the shader reads back
// the values they were made with. Assets\ keeps the linear originals; this runs on the build's staged copy.
//
//   srgb <staged assets dir>     encodes every *_Normal.png and *_ORM.png in it, in place
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;

if (args.Length != 1 || !Directory.Exists(args[0])) { Console.WriteLine("usage: srgb <staged assets dir>"); return 1; }
var lut = new byte[256];
for (int i = 0; i < 256; i++) {
    double l = i / 255.0, s = l <= 0.0031308 ? l * 12.92 : 1.055 * Math.Pow(l, 1 / 2.4) - 0.055;
    lut[i] = (byte)Math.Clamp((int)Math.Round(s * 255), 0, 255);
}
int n = 0;
foreach (var f in Directory.EnumerateFiles(args[0], "*.png")) {
    string name = Path.GetFileNameWithoutExtension(f);
    if (!name.EndsWith("_Normal", StringComparison.OrdinalIgnoreCase) && !name.EndsWith("_ORM", StringComparison.OrdinalIgnoreCase)) continue;
    using (var img = Image.Load<Rgba32>(f)) {
        img.ProcessPixelRows(rows => {
            for (int y = 0; y < rows.Height; y++) {
                var r = rows.GetRowSpan(y);
                for (int x = 0; x < r.Length; x++) r[x] = new Rgba32(lut[r[x].R], lut[r[x].G], lut[r[x].B], r[x].A);
            }
        });
        img.SaveAsPng(f);
    }
    n++;
}
Console.WriteLine($"srgb: {n} data map(s) sRGB-encoded for GML's runtime import");
return 0;
