// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;
using CUE4Parse.UE4.Assets.Exports.Texture;

namespace Mapc;

/// <summary>
/// mapc minimap: the game's own world map, for the GM web tools' Map (services/internal/gmhttp, GET /api/v1/gm/minimap/...).
///
/// The client paints its map from the tiles of AOC/Content/UI/MinimapImageTiles/Verra_World_Master: TUI_s{S}_x{X}_y{Y}, 512 x 512 uncompressed BGRA, at three sizes
/// (S = 1,000,000, 250,000 and 50,000 cm on a side). Tile (X, Y) covers world X from X*S to (X+1)*S and world Y from Y*S to (Y+1)*S, its picture with world +X up and
/// +Y to the right: checked by stitching all 110 tiles of 250,000 cm (their coasts meet at every seam) and putting the 65 city node records of content.db on them
/// (each stands on its town; the harbours just off the coast). The 1,000,000 cm tiles do not cover the west of the map under that rule and are not used.
///
/// Written to DIR/minimap: every 250,000 cm tile as s250000_x{X}_y{Y}.png (512 x 512), overview.png (all of them at 128 pixels a tile) and minimap.json, which says
/// where they lie. A file that already has the same bytes is left alone.
/// </summary>
public static class Minimap
{
    public const string Prefix = "AOC/Content/UI/MinimapImageTiles/Verra_World_Master/";
    public const string Dir = "minimap";
    public const long TileCm = 250000;
    const int OverviewPx = 128;
    static readonly Regex Tile = new(@"^TUI_s(\d+)_x(-?\d+)_y(-?\d+)\.uasset$", RegexOptions.Compiled | RegexOptions.IgnoreCase);

    public static int Export(Game g, string outDir)
    {
        var tiles = new List<(string Path, int X, int Y)>();
        foreach (var k in g.Provider.Files.Keys)
        {
            if (!k.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase)) continue;
            var m = Tile.Match(k.Substring(Prefix.Length));
            if (m.Success && long.Parse(m.Groups[1].Value) == TileCm) tiles.Add((k[..^".uasset".Length], int.Parse(m.Groups[2].Value), int.Parse(m.Groups[3].Value)));
        }
        if (tiles.Count == 0)
        {
            Console.Error.WriteLine($"mapc: no map tiles of {TileCm} cm under {Prefix} in the game's containers (is this the game's Paks folder? --paks)");
            return 1;
        }
        int minX = tiles.Min(t => t.X), maxX = tiles.Max(t => t.X), minY = tiles.Min(t => t.Y), maxY = tiles.Max(t => t.Y);
        int cols = maxY - minY + 1, rows = maxX - minX + 1;   // the overview: columns follow world Y, rows world X from the top (+X up)
        var overview = new byte[cols * OverviewPx * rows * OverviewPx * 4];
        var dest = Path.Combine(outDir, Dir);
        Directory.CreateDirectory(dest);
        int written = 0, same = 0, tilePx = 0;
        foreach (var t in tiles.OrderBy(t => t.X).ThenBy(t => t.Y))
        {
            var (bgra, w, h) = Pixels(g, t.Path);
            if (bgra == null || w != h || w % OverviewPx != 0)
            {
                Console.Error.WriteLine($"mapc: {t.Path} is not a square BGRA picture a multiple of {OverviewPx} wide: the map is not written");
                return 1;
            }
            tilePx = w;
            var rgba = new byte[w * h * 4];
            for (int i = 0; i < w * h; i++) { rgba[i * 4] = bgra[i * 4 + 2]; rgba[i * 4 + 1] = bgra[i * 4 + 1]; rgba[i * 4 + 2] = bgra[i * 4]; rgba[i * 4 + 3] = 255; }
            if (Write(Path.Combine(dest, $"s{TileCm}_x{t.X}_y{t.Y}.png"), Png(rgba, w, h))) written++; else same++;
            // into the overview, averaged down
            int step = w / OverviewPx, ox = (t.Y - minY) * OverviewPx, oy = (maxX - t.X) * OverviewPx, ow = cols * OverviewPx;
            for (int y = 0; y < OverviewPx; y++)
                for (int x = 0; x < OverviewPx; x++)
                {
                    int r = 0, gr = 0, b = 0;
                    for (int sy = 0; sy < step; sy++)
                        for (int sx = 0; sx < step; sx++) { int o = ((y * step + sy) * w + x * step + sx) * 4; r += rgba[o]; gr += rgba[o + 1]; b += rgba[o + 2]; }
                    int n = step * step, d = ((oy + y) * ow + ox + x) * 4;
                    overview[d] = (byte)(r / n); overview[d + 1] = (byte)(gr / n); overview[d + 2] = (byte)(b / n); overview[d + 3] = 255;
                }
        }
        Write(Path.Combine(dest, "overview.png"), Png(overview, cols * OverviewPx, rows * OverviewPx));
        var json = $"{{\"tile_cm\":{TileCm},\"tile_px\":{tilePx},\"overview_px\":{OverviewPx},\"min_x\":{minX},\"max_x\":{maxX},\"min_y\":{minY},\"max_y\":{maxY},\"tiles\":{tiles.Count}," +
                   $"\"up\":\"+X\",\"right\":\"+Y\",\"source\":\"{Prefix}TUI_s{TileCm}_x{{X}}_y{{Y}}\"}}\n";
        Write(Path.Combine(dest, "minimap.json"), Encoding.UTF8.GetBytes(json));
        Console.WriteLine($"mapc: {tiles.Count} map tiles of {TileCm / 100000.0:0.#} km (x {minX}..{maxX}, y {minY}..{maxY}) into {dest}: {written} written, {same} unchanged, and the overview");
        return 0;
    }

    static (byte[]? Bgra, int W, int H) Pixels(Game g, string path)
    {
        var pkg = g.Load(path);
        if (pkg == null) return (null, 0, 0);
        foreach (var e in pkg.GetExports())
        {
            if (e is not UTexture2D t) continue;
            var mip = t.GetFirstMip();
            if (t.Format.ToString() != "PF_B8G8R8A8" || mip?.BulkData?.Data == null) return (null, 0, 0);
            return (mip.BulkData.Data, mip.SizeX, mip.SizeY);
        }
        return (null, 0, 0);
    }

    static bool Write(string file, byte[] bytes)
    {
        if (File.Exists(file) && File.ReadAllBytes(file).AsSpan().SequenceEqual(bytes)) return false;
        File.WriteAllBytes(file, bytes);
        return true;
    }

    /// <summary>An 8-bit RGBA PNG, filter 0 on every row (the encoder is here so that mapc needs no image library).</summary>
    static byte[] Png(byte[] rgba, int w, int h)
    {
        using var file = new MemoryStream();
        file.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 });
        void Chunk(string type, byte[] data)
        {
            var head = Encoding.ASCII.GetBytes(type);
            file.Write(BigEndian((uint)data.Length));
            file.Write(head);
            file.Write(data);
            file.Write(BigEndian(Crc(head, data)));
        }
        var ihdr = new byte[13];
        BigEndian((uint)w).CopyTo(ihdr, 0);
        BigEndian((uint)h).CopyTo(ihdr, 4);
        ihdr[8] = 8; ihdr[9] = 6;
        Chunk("IHDR", ihdr);
        using var z = new MemoryStream();
        using (var zs = new ZLibStream(z, CompressionLevel.SmallestSize, leaveOpen: true))
            for (int y = 0; y < h; y++) { zs.WriteByte(0); zs.Write(rgba, y * w * 4, w * 4); }
        Chunk("IDAT", z.ToArray());
        Chunk("IEND", Array.Empty<byte>());
        return file.ToArray();
    }

    static byte[] BigEndian(uint v) => new[] { (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v };

    static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n =>
    {
        uint c = (uint)n;
        for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();

    static uint Crc(byte[] a, byte[] b)
    {
        uint c = 0xFFFFFFFFu;
        foreach (var x in a) c = CrcTable[(c ^ x) & 0xFF] ^ (c >> 8);
        foreach (var x in b) c = CrcTable[(c ^ x) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
}
