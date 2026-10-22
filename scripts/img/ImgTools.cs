// Image helpers for the RIP 3 translation: assemble tiled screens from data.pak,
// find green text boxes, erase them and draw Russian text, split back into tiles.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.IO.Compression;
using System.Linq;

public static class ImgTools
{
    public static bool IsText(Color c)
    {
        // the menu text is bright green; frame lines are white/red/grey
        return c.G > 90 && c.G > c.R + 40 && c.G > c.B + 40;
    }

    public static Bitmap LoadEntry(ZipArchive z, string name)
    {
        var e = z.GetEntry(name);
        using (var s = e.Open())
        using (var ms = new MemoryStream())
        {
            s.CopyTo(ms);
            using (var tmp = Image.FromStream(new MemoryStream(ms.ToArray())))
                return new Bitmap(tmp);
        }
    }

    // Assembles "<prefix>_00..NN.<ext>" tiles row by row, `cols` tiles per row.
    public static Bitmap Assemble(ZipArchive z, string prefix, string ext, int count, int cols)
    {
        var tiles = new List<Bitmap>();
        for (int i = 0; i < count; i++) tiles.Add(LoadEntry(z, string.Format("{0}_{1:00}.{2}", prefix, i, ext)));
        int tw = tiles[0].Width, th = tiles[0].Height, rows = (count + cols - 1) / cols;
        var bmp = new Bitmap(cols * tw, rows * th, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp))
            for (int i = 0; i < count; i++) g.DrawImage(tiles[i], (i % cols) * tw, (i / cols) * th, tw, th);
        foreach (var t in tiles) t.Dispose();
        return bmp;
    }

    // Groups green pixels into text-line boxes: rows with text pixels form bands,
    // inside a band columns are split where the gap is wider than maxGap.
    public static List<Rectangle> FindTextBoxes(Bitmap bmp, Rectangle area, int maxGap)
    {
        var boxes = new List<Rectangle>();
        bool[] rowHas = new bool[area.Height];
        for (int y = 0; y < area.Height; y++)
            for (int x = 0; x < area.Width; x++)
                if (IsText(bmp.GetPixel(area.X + x, area.Y + y))) { rowHas[y] = true; break; }
        int yy = 0;
        while (yy < area.Height)
        {
            if (!rowHas[yy]) { yy++; continue; }
            int y0 = yy;
            while (yy < area.Height && rowHas[yy]) yy++;
            int y1 = yy;
            bool[] colHas = new bool[area.Width];
            for (int x = 0; x < area.Width; x++)
                for (int y = y0; y < y1; y++)
                    if (IsText(bmp.GetPixel(area.X + x, area.Y + y))) { colHas[x] = true; break; }
            int xx = 0;
            while (xx < area.Width)
            {
                if (!colHas[xx]) { xx++; continue; }
                int x0 = xx, last = xx;
                while (xx < area.Width && (colHas[xx] || xx - last <= maxGap)) { if (colHas[xx]) last = xx; xx++; }
                if (y1 - y0 >= 6 && last - x0 >= 3)
                    boxes.Add(new Rectangle(area.X + x0, area.Y + y0, last - x0 + 1, y1 - y0));
            }
        }
        return boxes;
    }

    // Removes green text pixels inside `r` (padded) by painting them with `bg`.
    public static void Erase(Bitmap bmp, Rectangle r, int pad, Color bg)
    {
        for (int y = Math.Max(0, r.Top - pad); y < Math.Min(bmp.Height, r.Bottom + pad); y++)
            for (int x = Math.Max(0, r.Left - pad); x < Math.Min(bmp.Width, r.Right + pad); x++)
            {
                Color c = bmp.GetPixel(x, y);
                // keep only the frame (white/grey) and the red underlines; everything
                // else in the box is text or its JPEG fringe
                bool frame = c.R > 110 && c.G > 110 && c.B > 110 && Math.Abs(c.R - c.G) < 45;
                bool red = c.R > 90 && c.R > c.G + 50;
                if (!frame && !red) bmp.SetPixel(x, y, bg);
            }
    }

    // Draws `text` fitted into `r` (height = cap height of the original), align: 0 left, 1 center, 2 right.
    public static void DrawText(Bitmap bmp, Rectangle r, string text, string fontName, Color color, int align, float maxWidth)
    {
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            float size = r.Height * 1.25f;
            Font f = null;
            SizeF sz = SizeF.Empty;
            var fmt = StringFormat.GenericTypographic;
            for (; size > 6; size -= 0.5f)
            {
                if (f != null) f.Dispose();
                f = new Font(fontName, size, FontStyle.Regular, GraphicsUnit.Pixel);
                sz = g.MeasureString(text, f, 10000, fmt);
                if (sz.Width <= maxWidth && sz.Height <= r.Height * 1.6f) break;
            }
            float x = align == 0 ? r.Left : align == 1 ? r.Left + (r.Width - sz.Width) / 2 : r.Right - sz.Width;
            float y = r.Top + (r.Height - sz.Height) / 2;
            using (var br = new SolidBrush(color)) g.DrawString(text, f, br, x, y, fmt);
            f.Dispose();
        }
    }

    // Average colour of the text pixels in r (to reuse the original green).
    public static Color TextColor(Bitmap bmp, Rectangle r)
    {
        long R = 0, G = 0, B = 0, n = 0;
        for (int y = r.Top; y < r.Bottom; y++)
            for (int x = r.Left; x < r.Right; x++)
            {
                Color c = bmp.GetPixel(x, y);
                if (IsText(c) && c.G > 150) { R += c.R; G += c.G; B += c.B; n++; }
            }
        return n == 0 ? Color.FromArgb(0, 230, 0) : Color.FromArgb((int)(R / n), (int)(G / n), (int)(B / n));
    }

    // ---- generic text masks, inpainting and styled text (screens with busy backgrounds) ----

    // kind: "green" menu text, "white" white/grey text, "silver" metallic caps, "bright" anything light
    public static bool Match(Color c, string kind)
    {
        int mx = Math.Max(c.R, Math.Max(c.G, c.B)), mn = Math.Min(c.R, Math.Min(c.G, c.B));
        switch (kind)
        {
            case "green": return c.G > 90 && c.G > c.R + 40 && c.G > c.B + 40;
            case "lime": return c.G > 180 && c.B < 70;   // yellow-green text on a turquoise screen
            case "brightgreen": return c.A > 40 && c.G > 150 && c.G > c.R + 50 && c.G > c.B + 50;  // HUD text over a dark grid
            case "red": return c.A > 40 && c.R > 130 && c.R > c.G + 60 && c.R > c.B + 60;
            case "white": return mn > 150 && mx - mn < 60;
            case "silver": return mn > 95 && mx - mn < 50;
            case "bright": return (c.R * 3 + c.G * 6 + c.B) / 10 > 140;
            default: throw new ArgumentException(kind);
        }
    }

    public static List<Rectangle> FindBoxes(Bitmap bmp, Rectangle area, int maxGap, string kind)
    {
        var boxes = new List<Rectangle>();
        bool[] rowHas = new bool[area.Height];
        for (int y = 0; y < area.Height; y++)
            for (int x = 0; x < area.Width; x++)
                if (Match(bmp.GetPixel(area.X + x, area.Y + y), kind)) { rowHas[y] = true; break; }
        int yy = 0;
        while (yy < area.Height)
        {
            if (!rowHas[yy]) { yy++; continue; }
            int y0 = yy;
            while (yy < area.Height && rowHas[yy]) yy++;
            int y1 = yy;
            bool[] colHas = new bool[area.Width];
            for (int x = 0; x < area.Width; x++)
                for (int y = y0; y < y1; y++)
                    if (Match(bmp.GetPixel(area.X + x, area.Y + y), kind)) { colHas[x] = true; break; }
            int xx = 0;
            while (xx < area.Width)
            {
                if (!colHas[xx]) { xx++; continue; }
                int x0 = xx, last = xx;
                while (xx < area.Width && (colHas[xx] || xx - last <= maxGap)) { if (colHas[xx]) last = xx; xx++; }
                if (y1 - y0 >= 5 && last - x0 >= 2)
                    boxes.Add(new Rectangle(area.X + x0, area.Y + y0, last - x0 + 1, y1 - y0));
            }
        }
        return boxes;
    }

    // Marks text pixels of `kind` inside r, grown by `dilate` pixels (catches antialiasing/JPEG fringe).
    public static void AddMask(bool[,] mask, Bitmap bmp, Rectangle r, string kind, int dilate)
    {
        int W = bmp.Width, H = bmp.Height;
        for (int y = Math.Max(0, r.Top); y < Math.Min(H, r.Bottom); y++)
            for (int x = Math.Max(0, r.Left); x < Math.Min(W, r.Right); x++)
            {
                if (kind != "all" && !Match(bmp.GetPixel(x, y), kind)) continue;
                for (int dy = -dilate; dy <= dilate; dy++)
                    for (int dx = -dilate; dx <= dilate; dx++)
                    {
                        int xx = x + dx, yy2 = y + dy;
                        if (xx >= 0 && yy2 >= 0 && xx < W && yy2 < H) mask[xx, yy2] = true;
                    }
            }
    }

    // Fills masked pixels from their unmasked neighbours, layer by layer from the
    // outside in (simple diffusion inpainting, keeps alpha too).
    public static void Inpaint(Bitmap bmp, bool[,] mask)
    {
        int W = bmp.Width, H = bmp.Height;
        var hole = (bool[,])mask.Clone();
        var todo = new List<Point>();
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (hole[x, y]) todo.Add(new Point(x, y));
        while (todo.Count > 0)
        {
            var filled = new List<KeyValuePair<Point, Color>>();
            var rest = new List<Point>();
            foreach (var p in todo)
            {
                long a = 0, r = 0, g = 0, b = 0; int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int x = p.X + dx, y = p.Y + dy;
                        if ((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= W || y >= H || hole[x, y]) continue;
                        Color c = bmp.GetPixel(x, y);
                        a += c.A; r += c.R; g += c.G; b += c.B; n++;
                    }
                if (n == 0) { rest.Add(p); continue; }
                filled.Add(new KeyValuePair<Point, Color>(p, Color.FromArgb((int)(a / n), (int)(r / n), (int)(g / n), (int)(b / n))));
            }
            if (filled.Count == 0) break;
            foreach (var f in filled) { bmp.SetPixel(f.Key.X, f.Key.Y, f.Value); hole[f.Key.X, f.Key.Y] = false; }
            todo = rest;
        }
    }

    // Replaces r row by row with a linear blend between the pixels just left and right
    // of it: clean on smooth gradients (the help pages' mouse panel).
    public static void FillRows(Bitmap bmp, Rectangle r)
    {
        for (int y = Math.Max(0, r.Top); y < Math.Min(bmp.Height, r.Bottom); y++)
        {
            Color a = bmp.GetPixel(Math.Max(0, r.Left - 1), y), b = bmp.GetPixel(Math.Min(bmp.Width - 1, r.Right), y);
            for (int x = r.Left; x < r.Right; x++)
            {
                float t = (x - r.Left + 1f) / (r.Width + 1f);
                bmp.SetPixel(x, y, Color.FromArgb((int)(a.A + (b.A - a.A) * t), (int)(a.R + (b.R - a.R) * t),
                                                  (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t)));
            }
        }
    }

    // Text fitted into r: font size starts from r.Height*sizeK and shrinks until it fits maxWidth.
    // Optional vertical gradient (color -> color2) and outline.
    public static void DrawStyled(Bitmap bmp, Rectangle r, string text, string fontName, bool bold, Color color, Color color2,
                                  Color outline, float outlineW, int align, float maxWidth, float sizeK)
    {
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.CompositingQuality = CompositingQuality.HighQuality;
            var style = bold ? FontStyle.Bold : FontStyle.Regular;
            if (fontName.EndsWith("|U")) { style |= FontStyle.Underline; fontName = fontName.Substring(0, fontName.Length - 2); }
            var fam = new FontFamily(fontName);
            var fmt = StringFormat.GenericTypographic;
            float size = r.Height * sizeK;
            RectangleF bounds = RectangleF.Empty;
            GraphicsPath path = null;
            for (; size > 6; size -= 0.5f)
            {
                if (path != null) path.Dispose();
                path = new GraphicsPath();
                path.AddString(text, fam, (int)style, size, new PointF(0, 0), fmt);
                bounds = path.GetBounds();
                if (bounds.Width <= maxWidth && bounds.Height <= r.Height * 1.6f) break;
            }
            float x = align == 0 ? r.Left : align == 1 ? r.Left + (r.Width - bounds.Width) / 2 : r.Right - bounds.Width;
            // vertical position from the cap height ("Н"), not the word's own bounds,
            // so words with and without tall/descending letters line up
            RectangleF cap;
            using (var capPath = new GraphicsPath())
            {
                capPath.AddString("Н", fam, (int)style, size, new PointF(0, 0), fmt);
                cap = capPath.GetBounds();
            }
            float y = r.Top + (r.Height - cap.Height) / 2;
            using (var m = new Matrix()) { m.Translate(x - bounds.Left, y - cap.Top); path.Transform(m); }
            if (outlineW > 0)
                using (var pen = new Pen(outline, outlineW * 2) { LineJoin = LineJoin.Round }) g.DrawPath(pen, path);
            var pb = path.GetBounds();
            Brush br = color2.A == 0 ? (Brush)new SolidBrush(color)
                : new LinearGradientBrush(new RectangleF(pb.X, pb.Y - 1, Math.Max(1, pb.Width), pb.Height + 2), color, color2, LinearGradientMode.Vertical);
            g.FillPath(br, path);
            br.Dispose();
            path.Dispose();
        }
    }

    // Text along the bottom of a circle (centre cx,cy, baseline radius), reading left to right.
    public static void DrawArc(Bitmap bmp, float cx, float cy, float radius, string text, string fontName, bool bold,
                               float sizePx, Color color, Color outline, float outlineW, float spacing)
    {
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var fam = new FontFamily(fontName);
            var style = bold ? FontStyle.Bold : FontStyle.Regular;
            var fmt = StringFormat.GenericTypographic;
            var widths = new float[text.Length];
            float total = 0;
            for (int i = 0; i < text.Length; i++)
            {
                using (var p = new GraphicsPath())
                {
                    p.AddString(text[i] == ' ' ? "н" : text[i].ToString(), fam, (int)style, sizePx, new PointF(0, 0), fmt);
                    widths[i] = p.GetBounds().Width + spacing;
                }
                total += widths[i];
            }
            // baseline = bottom of a letter without a descender
            float baseBottom;
            using (var p = new GraphicsPath()) { p.AddString("н", fam, (int)style, sizePx, new PointF(0, 0), fmt); baseBottom = p.GetBounds().Bottom; }
            // angle 90 deg = straight down; the text is centred there and runs right-to-left in angle
            double a = Math.PI / 2 + total / 2 / radius;
            for (int i = 0; i < text.Length; i++)
            {
                double mid = a - widths[i] / 2 / radius;
                a -= widths[i] / radius;
                if (text[i] == ' ') continue;
                using (var p = new GraphicsPath())
                {
                    p.AddString(text[i].ToString(), fam, (int)style, sizePx, new PointF(0, 0), fmt);
                    var bb = p.GetBounds();
                    using (var m = new Matrix())
                    {
                        // glyph centred on x, baseline (bottom of the cap) on the circle, top towards the centre
                        float px = cx + (float)(Math.Cos(mid) * radius), py = cy + (float)(Math.Sin(mid) * radius);
                        m.Translate(px, py);
                        m.Rotate((float)((mid - Math.PI / 2) * 180 / Math.PI));
                        m.Translate(-(bb.Left + bb.Width / 2), -baseBottom);
                        p.Transform(m);
                    }
                    if (outlineW > 0) using (var pen = new Pen(outline, outlineW * 2) { LineJoin = LineJoin.Round }) g.DrawPath(pen, p);
                    using (var br = new SolidBrush(color)) g.FillPath(br, p);
                }
            }
        }
    }

    public static Color Hex(string s)
    {
        if (string.IsNullOrEmpty(s)) return Color.FromArgb(0, 0, 0, 0);
        s = s.TrimStart('#');
        int v = Convert.ToInt32(s, 16);
        return s.Length == 8 ? Color.FromArgb(v) : Color.FromArgb(255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    }

    // PNG (RGBA, keeps alpha) not larger than maxBytes, zero-padded to exactly maxBytes.
    // Own encoder: per-row adaptive filters + maximum deflate; GDI+'s PNGs are much bigger.
    public static byte[] PngPadded(Bitmap bmp, int maxBytes) { return PngPadded(bmp, maxBytes, null); }

    // paletteFrom: the original picture; when RGBA does not fit, an 8-bit palette
    // PNG is made from the original's colours (the game's PNGs are palette PNGs).
    public static byte[] PngPadded(Bitmap bmp, int maxBytes, Bitmap paletteFrom)
    {
        byte[] png = EncodePng(bmp);
        if (png.Length > maxBytes && paletteFrom != null) png = EncodePngIndexed(bmp, paletteFrom);
        if (png.Length > maxBytes) return null;
        var data = new byte[maxBytes];
        Array.Copy(png, data, png.Length);
        return data;
    }

    static readonly uint[] crcTable = MakeCrcTable();
    static uint[] MakeCrcTable()
    {
        var t = new uint[256];
        for (uint n = 0; n < 256; n++) { uint c = n; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[n] = c; }
        return t;
    }
    static uint Crc(byte[] buf, int off, int len, uint c = 0xFFFFFFFFu)
    {
        for (int i = off; i < off + len; i++) c = crcTable[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
        return c;
    }
    static void BE(Stream s, uint v) { s.WriteByte((byte)(v >> 24)); s.WriteByte((byte)(v >> 16)); s.WriteByte((byte)(v >> 8)); s.WriteByte((byte)v); }
    static void Chunk(Stream s, string type, byte[] data)
    {
        BE(s, (uint)data.Length);
        var td = new byte[4 + data.Length];
        System.Text.Encoding.ASCII.GetBytes(type, 0, 4, td, 0);
        Array.Copy(data, 0, td, 4, data.Length);
        s.Write(td, 0, td.Length);
        BE(s, Crc(td, 0, td.Length) ^ 0xFFFFFFFFu);
    }
    static int Paeth(int a, int b, int c) { int p = a + b - c, pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }

    public static byte[] EncodePngIndexed(Bitmap bmp, Bitmap paletteFrom)
    {
        // palette: the 256 most frequent colours of the original
        var freq = new Dictionary<int, int>();
        for (int y = 0; y < paletteFrom.Height; y++)
            for (int x = 0; x < paletteFrom.Width; x++)
            {
                Color c = paletteFrom.GetPixel(x, y);
                int k = c.A == 0 ? 0 : c.ToArgb();
                int n; freq.TryGetValue(k, out n); freq[k] = n + 1;
            }
        var pal = freq.OrderByDescending(kv => kv.Value).Take(256).Select(kv => Color.FromArgb(kv.Key)).ToList();
        int W = bmp.Width, H = bmp.Height;
        var idx = new byte[H * (W + 1)];
        var cache = new Dictionary<int, byte>();
        for (int y = 0; y < H; y++)
        {
            idx[y * (W + 1)] = 0; // filter: none
            for (int x = 0; x < W; x++)
            {
                Color c = bmp.GetPixel(x, y);
                int k = c.A == 0 ? 0 : c.ToArgb();
                byte bi;
                if (!cache.TryGetValue(k, out bi))
                {
                    long bestD = long.MaxValue;
                    for (int i = 0; i < pal.Count; i++)
                    {
                        Color p = pal[i];
                        long d = (long)(p.R - c.R) * (p.R - c.R) * 3 + (long)(p.G - c.G) * (p.G - c.G) * 4 + (long)(p.B - c.B) * (p.B - c.B) * 2 + (long)(p.A - c.A) * (p.A - c.A) * 6;
                        if (d < bestD) { bestD = d; bi = (byte)i; }
                    }
                    cache[k] = bi;
                }
                idx[y * (W + 1) + 1 + x] = bi;
            }
        }
        byte[] deflated;
        using (var ms = new MemoryStream())
        {
            using (var ds = new DeflateStream(ms, CompressionLevel.Optimal, true)) ds.Write(idx, 0, idx.Length);
            deflated = ms.ToArray();
        }
        uint s1 = 1, s2 = 0;
        foreach (byte bt in idx) { s1 = (s1 + bt) % 65521; s2 = (s2 + s1) % 65521; }
        var zlib = new byte[2 + deflated.Length + 4];
        zlib[0] = 0x78; zlib[1] = 0xDA;
        Array.Copy(deflated, 0, zlib, 2, deflated.Length);
        uint adler = (s2 << 16) | s1;
        zlib[zlib.Length - 4] = (byte)(adler >> 24); zlib[zlib.Length - 3] = (byte)(adler >> 16);
        zlib[zlib.Length - 2] = (byte)(adler >> 8); zlib[zlib.Length - 1] = (byte)adler;
        var plte = new byte[pal.Count * 3];
        var trns = new byte[pal.Count];
        for (int i = 0; i < pal.Count; i++) { plte[i * 3] = pal[i].R; plte[i * 3 + 1] = pal[i].G; plte[i * 3 + 2] = pal[i].B; trns[i] = pal[i].A; }
        using (var outp = new MemoryStream())
        {
            outp.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }, 0, 8);
            var ihdr = new byte[13];
            ihdr[0] = (byte)(W >> 24); ihdr[1] = (byte)(W >> 16); ihdr[2] = (byte)(W >> 8); ihdr[3] = (byte)W;
            ihdr[4] = (byte)(H >> 24); ihdr[5] = (byte)(H >> 16); ihdr[6] = (byte)(H >> 8); ihdr[7] = (byte)H;
            ihdr[8] = 8; ihdr[9] = 3;
            Chunk(outp, "IHDR", ihdr);
            Chunk(outp, "PLTE", plte);
            Chunk(outp, "tRNS", trns);
            Chunk(outp, "IDAT", zlib);
            Chunk(outp, "IEND", new byte[0]);
            return outp.ToArray();
        }
    }

    public static byte[] EncodePng(Bitmap bmp)
    {
        int W = bmp.Width, H = bmp.Height, stride = W * 4;
        var raw = new byte[H * stride];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                Color c = bmp.GetPixel(x, y);
                int o = y * stride + x * 4;
                // fully transparent pixels: zero RGB compresses better and looks the same
                if (c.A == 0) { raw[o] = raw[o + 1] = raw[o + 2] = raw[o + 3] = 0; continue; }
                raw[o] = c.R; raw[o + 1] = c.G; raw[o + 2] = c.B; raw[o + 3] = c.A;
            }
        var filtered = new byte[H * (stride + 1)];
        var cand = new byte[5][];
        for (int f = 0; f < 5; f++) cand[f] = new byte[stride];
        for (int y = 0; y < H; y++)
        {
            int best = 0; long bestSum = long.MaxValue;
            for (int f = 0; f < 5; f++)
            {
                long sum = 0;
                for (int i = 0; i < stride; i++)
                {
                    int cur = raw[y * stride + i];
                    int a = i >= 4 ? raw[y * stride + i - 4] : 0;
                    int b = y > 0 ? raw[(y - 1) * stride + i] : 0;
                    int c = (i >= 4 && y > 0) ? raw[(y - 1) * stride + i - 4] : 0;
                    int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : Paeth(a, b, c);
                    byte v = (byte)(cur - pred);
                    cand[f][i] = v;
                    sum += v < 128 ? v : 256 - v;
                }
                if (sum < bestSum) { bestSum = sum; best = f; }
            }
            filtered[y * (stride + 1)] = (byte)best;
            Array.Copy(cand[best], 0, filtered, y * (stride + 1) + 1, stride);
        }
        byte[] deflated;
        using (var ms = new MemoryStream())
        {
            using (var ds = new DeflateStream(ms, CompressionLevel.Optimal, true)) ds.Write(filtered, 0, filtered.Length);
            deflated = ms.ToArray();
        }
        uint s1 = 1, s2 = 0;
        foreach (byte bt in filtered) { s1 = (s1 + bt) % 65521; s2 = (s2 + s1) % 65521; }
        var zlib = new byte[2 + deflated.Length + 4];
        zlib[0] = 0x78; zlib[1] = 0xDA;
        Array.Copy(deflated, 0, zlib, 2, deflated.Length);
        uint adler = (s2 << 16) | s1;
        zlib[zlib.Length - 4] = (byte)(adler >> 24); zlib[zlib.Length - 3] = (byte)(adler >> 16);
        zlib[zlib.Length - 2] = (byte)(adler >> 8); zlib[zlib.Length - 1] = (byte)adler;
        using (var outp = new MemoryStream())
        {
            outp.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }, 0, 8);
            var ihdr = new byte[13];
            ihdr[0] = (byte)(W >> 24); ihdr[1] = (byte)(W >> 16); ihdr[2] = (byte)(W >> 8); ihdr[3] = (byte)W;
            ihdr[4] = (byte)(H >> 24); ihdr[5] = (byte)(H >> 16); ihdr[6] = (byte)(H >> 8); ihdr[7] = (byte)H;
            ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
            Chunk(outp, "IHDR", ihdr);
            Chunk(outp, "IDAT", zlib);
            Chunk(outp, "IEND", new byte[0]);
            return outp.ToArray();
        }
    }

    // Saves tile i of the screen as JPEG not larger than maxBytes (padded with zeros to exactly maxBytes).
    public static byte[] TileJpeg(Bitmap screen, int i, int cols, int tw, int th, int maxBytes)
    {
        using (var tile = screen.Clone(new Rectangle((i % cols) * tw, (i / cols) * th, tw, th), PixelFormat.Format24bppRgb))
        {
            var codec = ImageCodecInfo.GetImageEncoders().First(c => c.MimeType == "image/jpeg");
            for (long q = 95; q >= 30; q -= 5)
            {
                var ep = new EncoderParameters(1);
                ep.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, q);
                using (var ms = new MemoryStream())
                {
                    tile.Save(ms, codec, ep);
                    if (ms.Length <= maxBytes)
                    {
                        var data = new byte[maxBytes];
                        Array.Copy(ms.ToArray(), data, ms.Length);
                        return data;
                    }
                }
            }
            return null;
        }
    }
}
