// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;
using CUE4Parse.UE4.Assets.Exports.Texture;

namespace Mapc;

/// <summary>
/// mapc minimap: the game's own world map, for the GM web tools' Map (services/internal/gmhttp, GET /api/v1/gm/minimap/... and /api/v1/gm/mapdata).
///
/// The client's map is the record MapTileSet_Verra_World_Master (content.db): three layers of 512 x 512 BGRA tiles, AOC/Content/UI/MinimapImageTiles/Verra_World_Master/
/// TUI_s{S}_x{X}_y{Y}, World (S = 1,000,000 cm), Region (250,000) and Local (50,000), each tile's WorldPosition X*S, Y*S. That position is the tile's CENTRE: the record of
/// Start_Loot lists 3 x 3 tiles at -1..1 for its Local batch of -50,000..50,000 cm, and the Local tiles match the Region ones under it only so (checked by sampling both,
/// and by eye). The picture has world +X up and +Y to the right. The World tiles are an old capture of the Riverlands alone, so they are not written; the Region layer is
/// the whole of Verra as the game paints it, and the Local layer the close view.
///
/// Over the Local layer the client lays a node's town as it stands (NodeBackdrops: TUI_{Node}_L0..L4, 1024 x 1024, one picture a node level). A backdrop is the town
/// platform's own frame (the five Riverlands nodes share one picture): 100,800 cm across (the platform's extent, town_platforms.bin), centred on the node and turned by
/// its yaw (city_node), +X up as the tiles (fitted against the Local tiles of Joeva, Windansea and Winstead: no other turn or mirror comes close).
///
/// Written to DIR/minimap (a file that already has the same bytes is left alone; files of an older export that this one does not write are removed):
///   r_x{X}_y{Y}.jpg      every Region tile (512 x 512)
///   l_x{X}_y{Y}.jpg      every Local tile (512 x 512)
///   overview.jpg         the Region layer at 128 pixels a tile, for the first view
///   n_{Node}_L{n}.jpg    every node backdrop (1024 x 1024)
///   i_{Texture}.png      the map's icons: UI/Icons/Map, the minimap's own and the HUD minimap's (with their alpha)
///   minimap.json         where all of it lies
/// The tiles and backdrops are JPEG (a baseline encoder below, so that mapc needs no image library): as PNG the Local layer alone would be 1.4 GB.
/// </summary>
public static class Minimap
{
    public const string Prefix = "AOC/Content/UI/MinimapImageTiles/Verra_World_Master/";
    public const string Dir = "minimap";
    const int OverviewPx = 128;
    static readonly Regex Tile = new(@"^TUI_s(\d+)_x(-?\d+)_y(-?\d+)\.uasset$", RegexOptions.Compiled | RegexOptions.IgnoreCase);
    static readonly Regex Backdrop = new(@"^TUI_([A-Za-z]+)_L(\d)\.uasset$", RegexOptions.Compiled);
    static readonly string[] IconDirs = { "AOC/Content/UI/Icons/Map/", "AOC/Content/UI/UMGMinimap/MinimapComponent/MinimapRessources/", "AOC/Content/UI/Widgets/HUD/Minimap/Texture/" };
    static readonly string[] IconFiles = { "AOC/Content/UI/Widgets/Narrative/Textures/TUI_Icon_Event_PvETracker.uasset" };

    sealed record Layer(string Key, string Letter, long Size, int Quality);
    static readonly Layer[] Layers = { new("region", "r", 250000, 88), new("local", "l", 50000, 82) };

    public static int Export(Game g, string outDir)
    {
        var dest = Path.Combine(outDir, Dir);
        Directory.CreateDirectory(dest);
        var keep = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        int written = 0, same = 0;
        void Put(string name, byte[] bytes) { keep.Add(name); if (Write(Path.Combine(dest, name), bytes)) written++; else same++; }

        var tiles = new Dictionary<long, List<(string Path, int X, int Y)>>();
        var backdrops = new List<(string Path, string Node, int Level)>();
        foreach (var k in g.Provider.Files.Keys)
        {
            if (!k.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase)) continue;
            var rest = k.Substring(Prefix.Length);
            var m = Tile.Match(rest);
            if (m.Success)
            {
                long s = long.Parse(m.Groups[1].Value);
                if (!tiles.TryGetValue(s, out var list)) tiles[s] = list = new();
                list.Add((k[..^".uasset".Length], int.Parse(m.Groups[2].Value), int.Parse(m.Groups[3].Value)));
                continue;
            }
            var b = Backdrop.Match(rest);
            if (b.Success) backdrops.Add((k[..^".uasset".Length], b.Groups[1].Value, int.Parse(b.Groups[2].Value)));
        }
        if (!tiles.ContainsKey(250000) || !tiles.ContainsKey(50000))
        {
            Console.Error.WriteLine($"mapc: no Region or Local map tiles under {Prefix} in the game's containers (is this the game's Paks folder? --paks)");
            return 1;
        }

        var json = new StringBuilder();
        json.Append("{\"version\":2,\"centred\":true,\"up\":\"+X\",\"right\":\"+Y\",\"tile_px\":512,\"layers\":{");
        byte[]? overview = null;
        int ovMinX = 0, ovMaxX = 0, ovMinY = 0, ovMaxY = 0, ovW = 0, ovH = 0;
        for (int li = 0; li < Layers.Length; li++)
        {
            var layer = Layers[li];
            var list = tiles[layer.Size].OrderBy(t => t.X).ThenBy(t => t.Y).ToList();
            bool isRegion = layer.Key == "region";
            if (isRegion)
            {
                ovMinX = list.Min(t => t.X); ovMaxX = list.Max(t => t.X); ovMinY = list.Min(t => t.Y); ovMaxY = list.Max(t => t.Y);
                ovW = (ovMaxY - ovMinY + 1) * OverviewPx; ovH = (ovMaxX - ovMinX + 1) * OverviewPx;
                overview = new byte[ovW * ovH * 4];
            }
            var have = new List<string>();
            foreach (var t in list)
            {
                var (bgra, w, h) = Pixels(g, t.Path);
                if (bgra == null || w != h || w % OverviewPx != 0)
                {
                    Console.Error.WriteLine($"mapc: {t.Path} is not a square BGRA picture a multiple of {OverviewPx} wide: skipped");
                    continue;
                }
                Put($"{layer.Letter}_x{t.X}_y{t.Y}.jpg", Jpeg.Encode(bgra, w, h, layer.Quality));
                have.Add($"[{t.X},{t.Y}]");
                if (isRegion) Shrink(bgra, w, overview!, ovW, (t.Y - ovMinY) * OverviewPx, (ovMaxX - t.X) * OverviewPx);
            }
            json.Append(li == 0 ? "" : ",").Append($"\"{layer.Key}\":{{\"tile_cm\":{layer.Size},\"prefix\":\"{layer.Letter}\",\"tiles\":[{string.Join(",", have)}]}}");
            Console.WriteLine($"mapc: {have.Count} {layer.Key} tiles of {layer.Size / 100000.0:0.#} km");
        }
        Put("overview.jpg", Jpeg.Encode(overview!, ovW, ovH, 90, rgba: true));
        // the overview: the Region tiles' corners (centred tiles: half a tile beyond the outer centres)
        json.Append($"}},\"overview\":{{\"file\":\"overview.jpg\",\"px\":[{ovW},{ovH}],\"min_x\":{(ovMinX - 0.5) * 250000},\"max_x\":{(ovMaxX + 0.5) * 250000},\"min_y\":{(ovMinY - 0.5) * 250000},\"max_y\":{(ovMaxY + 0.5) * 250000}}}");

        var bd = new List<string>();
        foreach (var b in backdrops.OrderBy(b => b.Node).ThenBy(b => b.Level))
        {
            var (bgra, w, h) = Pixels(g, b.Path);
            if (bgra == null) { Console.Error.WriteLine($"mapc: {b.Path} is not a BGRA picture: skipped"); continue; }
            Put($"n_{b.Node}_L{b.Level}.jpg", Jpeg.Encode(bgra, w, h, 85));
            bd.Add($"\"{b.Node}_L{b.Level}\"");
        }
        json.Append($",\"backdrop_cm\":100800,\"backdrops\":[{string.Join(",", bd)}]");

        var icons = new List<string>();
        var iconKeys = g.Provider.Files.Keys.Where(k => k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) &&
            (IconDirs.Any(d => k.StartsWith(d, StringComparison.OrdinalIgnoreCase)) || IconFiles.Contains(k, StringComparer.OrdinalIgnoreCase))).OrderBy(k => k, StringComparer.Ordinal);
        foreach (var k in iconKeys)
        {
            var leaf = Path.GetFileNameWithoutExtension(k);
            if (icons.Contains($"\"{leaf}\"")) continue;
            var (bgra, w, h) = Pixels(g, k[..^".uasset".Length]);
            if (bgra == null) continue;   // a material or a compressed picture: the page draws its own mark instead
            var rgba = new byte[w * h * 4];
            for (int i = 0; i < w * h; i++) { rgba[i * 4] = bgra[i * 4 + 2]; rgba[i * 4 + 1] = bgra[i * 4 + 1]; rgba[i * 4 + 2] = bgra[i * 4]; rgba[i * 4 + 3] = bgra[i * 4 + 3]; }
            Put($"i_{leaf}.png", Png(rgba, w, h));
            icons.Add($"\"{leaf}\"");
        }
        json.Append($",\"icons\":[{string.Join(",", icons)}],\"source\":\"{Prefix}\"}}\n");
        Put("minimap.json", Encoding.UTF8.GetBytes(json.ToString()));

        int removed = 0;
        foreach (var f in Directory.GetFiles(dest))
            if (!keep.Contains(Path.GetFileName(f))) { File.Delete(f); removed++; }
        Console.WriteLine($"mapc: the map into {dest}: {backdrops.Count} node backdrops, {icons.Count} icons; {written} files written, {same} unchanged, {removed} old ones removed");
        return 0;
    }

    // a tile averaged down into the overview (RGBA) at (ox, oy)
    static void Shrink(byte[] bgra, int w, byte[] overview, int ow, int ox, int oy)
    {
        int step = w / OverviewPx;
        for (int y = 0; y < OverviewPx; y++)
            for (int x = 0; x < OverviewPx; x++)
            {
                int r = 0, gr = 0, b = 0;
                for (int sy = 0; sy < step; sy++)
                    for (int sx = 0; sx < step; sx++) { int o = ((y * step + sy) * w + x * step + sx) * 4; b += bgra[o]; gr += bgra[o + 1]; r += bgra[o + 2]; }
                int n = step * step, d = ((oy + y) * ow + ox + x) * 4;
                overview[d] = (byte)(r / n); overview[d + 1] = (byte)(gr / n); overview[d + 2] = (byte)(b / n); overview[d + 3] = 255;
            }
    }

    static (byte[]? Bgra, int W, int H) Pixels(Game g, string path)
    {
        var pkg = g.Load(path);
        if (pkg == null) return (null, 0, 0);
        foreach (var e in pkg.GetExports())
        {
            if (e is not UTexture2D t) continue;
            var mip = t.GetFirstMip();
            if (t.Format.ToString() != "PF_B8G8R8A8" || mip?.BulkData?.Data == null || mip.BulkData.Data.Length < mip.SizeX * mip.SizeY * 4) return (null, 0, 0);
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

    /// <summary>An 8-bit RGBA PNG, filter 0 on every row.</summary>
    public static byte[] Png(byte[] rgba, int w, int h)
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

/// <summary>A baseline JPEG encoder (JFIF, 4:4:4, the standard tables of ITU T.81 annex K; the luminance Huffman tables serve all three components).</summary>
public static class Jpeg
{
    static readonly byte[] ZigZag = {
        0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
    static readonly byte[] LumQ = {
        16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
        18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 };
    static readonly byte[] ChrQ = {
        17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };
    static readonly byte[] DcBits = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
    static readonly byte[] DcVals = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    static readonly byte[] AcBits = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
    static readonly byte[] AcVals = {
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
        0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
        0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
        0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
        0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
        0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
        0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
        0xf9, 0xfa };

    static readonly (ushort Code, byte Len)[] DcCode = Codes(DcBits, DcVals), AcCode = Codes(AcBits, AcVals);
    static readonly double[,] Cos = MakeCos();

    static (ushort, byte)[] Codes(byte[] bits, byte[] vals)
    {
        var t = new (ushort, byte)[256];
        int code = 0, k = 0;
        for (int len = 1; len <= 16; len++)
        {
            for (int i = 0; i < bits[len - 1]; i++) t[vals[k++]] = ((ushort)code++, (byte)len);
            code <<= 1;
        }
        return t;
    }

    static double[,] MakeCos()
    {
        var c = new double[8, 8];
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++) c[u, x] = (u == 0 ? Math.Sqrt(0.125) : 0.5) * Math.Cos((2 * x + 1) * u * Math.PI / 16);
        return c;
    }

    static byte[] Scale(byte[] q, int quality)
    {
        quality = Math.Clamp(quality, 1, 100);
        int s = quality < 50 ? 5000 / quality : 200 - quality * 2;
        return q.Select(v => (byte)Math.Clamp((v * s + 50) / 100, 1, 255)).ToArray();
    }

    /// <summary>The picture (BGRA, or RGBA with rgba) as a JPEG; the alpha is dropped.</summary>
    public static byte[] Encode(byte[] px, int w, int h, int quality, bool rgba = false)
    {
        var lq = Scale(LumQ, quality);
        var cq = Scale(ChrQ, quality);
        using var ms = new MemoryStream();
        void B(params byte[] b) => ms.Write(b);
        void Marker(byte m, byte[] body) { B(0xFF, m, (byte)((body.Length + 2) >> 8), (byte)(body.Length + 2)); ms.Write(body); }
        B(0xFF, 0xD8);
        Marker(0xE0, new byte[] { (byte)'J', (byte)'F', (byte)'I', (byte)'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 });
        var dqt = new byte[130];
        dqt[0] = 0; dqt[65] = 1;
        for (int k = 0; k < 64; k++) { dqt[1 + k] = lq[ZigZag[k]]; dqt[66 + k] = cq[ZigZag[k]]; }
        Marker(0xDB, dqt);
        Marker(0xC0, new byte[] { 8, (byte)(h >> 8), (byte)h, (byte)(w >> 8), (byte)w, 3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1 });
        var dht = new List<byte> { 0x00 };
        dht.AddRange(DcBits); dht.AddRange(DcVals);
        dht.Add(0x10); dht.AddRange(AcBits); dht.AddRange(AcVals);
        Marker(0xC4, dht.ToArray());
        Marker(0xDA, new byte[] { 3, 1, 0x00, 2, 0x00, 3, 0x00, 0, 63, 0 });

        var bits = new BitWriter(ms);
        int ri = rgba ? 0 : 2, bi = rgba ? 2 : 0;
        var blk = new double[3][] { new double[64], new double[64], new double[64] };
        var tmp = new double[64];
        var coef = new int[64];
        int[] pred = new int[3];
        for (int by = 0; by < h; by += 8)
            for (int bx = 0; bx < w; bx += 8)
            {
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                    {
                        int sx = Math.Min(bx + x, w - 1), sy = Math.Min(by + y, h - 1), o = (sy * w + sx) * 4;
                        double r = px[o + ri], g = px[o + 1], b = px[o + bi];
                        blk[0][y * 8 + x] = 0.299 * r + 0.587 * g + 0.114 * b - 128;
                        blk[1][y * 8 + x] = -0.168736 * r - 0.331264 * g + 0.5 * b;
                        blk[2][y * 8 + x] = 0.5 * r - 0.418688 * g - 0.081312 * b;
                    }
                for (int c = 0; c < 3; c++)
                {
                    var f = blk[c];
                    // rows, then columns
                    for (int y = 0; y < 8; y++)
                        for (int u = 0; u < 8; u++) { double s = 0; for (int x = 0; x < 8; x++) s += Cos[u, x] * f[y * 8 + x]; tmp[y * 8 + u] = s; }
                    var q = c == 0 ? lq : cq;
                    for (int u = 0; u < 8; u++)
                        for (int v = 0; v < 8; v++)
                        {
                            double s = 0; for (int y = 0; y < 8; y++) s += Cos[v, y] * tmp[y * 8 + u];
                            coef[v * 8 + u] = (int)Math.Round(s / q[v * 8 + u]);
                        }
                    int dc = coef[0], diff = dc - pred[c];
                    pred[c] = dc;
                    int cat = Category(diff);
                    bits.Put(DcCode[cat].Code, DcCode[cat].Len);
                    if (cat > 0) bits.Put(Extra(diff, cat), cat);
                    int run = 0;
                    for (int k = 1; k < 64; k++)
                    {
                        int a = coef[ZigZag[k]];
                        if (a == 0) { run++; continue; }
                        while (run > 15) { bits.Put(AcCode[0xF0].Code, AcCode[0xF0].Len); run -= 16; }
                        int ac = Category(a), sym = (run << 4) | ac;
                        bits.Put(AcCode[sym].Code, AcCode[sym].Len);
                        bits.Put(Extra(a, ac), ac);
                        run = 0;
                    }
                    if (run > 0) bits.Put(AcCode[0x00].Code, AcCode[0x00].Len);
                }
            }
        bits.Flush();
        B(0xFF, 0xD9);
        return ms.ToArray();
    }

    static int Category(int v) { v = Math.Abs(v); int n = 0; while (v > 0) { n++; v >>= 1; } return n; }
    static int Extra(int v, int cat) => v >= 0 ? v : v + (1 << cat) - 1;

    sealed class BitWriter(Stream s)
    {
        uint acc; int n;
        public void Put(int code, int len)
        {
            acc = (acc << len) | ((uint)code & ((1u << len) - 1)); n += len;
            while (n >= 8) { Emit((byte)(acc >> (n - 8))); n -= 8; acc &= (1u << n) - 1; }
        }
        void Emit(byte b) { s.WriteByte(b); if (b == 0xFF) s.WriteByte(0); }
        public void Flush() { if (n > 0) Put((1 << (8 - n)) - 1, 8 - n); }
    }
}
