using System.IO.Compression;

namespace Atrium.Probe;

// Minimal PNG writer (RGBA8) so probe images can be reviewed without extra tools.
internal static class Png
{
    private static readonly uint[] s_crc = BuildCrc();

    public static void Save(string path, byte[] bgra, int w, int h, int scale = 1)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        int ow = w / scale, oh = h / scale;
        var raw = new byte[(ow * 4 + 1) * oh];
        for (int y = 0; y < oh; y++)
        {
            int row = y * (ow * 4 + 1);
            raw[row] = 0;
            for (int x = 0; x < ow; x++)
            {
                int s = ((y * scale) * w + x * scale) * 4, d = row + 1 + x * 4;
                raw[d] = bgra[s + 2]; raw[d + 1] = bgra[s + 1]; raw[d + 2] = bgra[s]; raw[d + 3] = 255;
            }
        }
        using var ms = new MemoryStream();
        using (var z = new ZLibStream(ms, CompressionLevel.Fastest, leaveOpen: true)) z.Write(raw);
        using var fs = File.Create(path);
        fs.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 });
        var ihdr = new byte[13];
        WriteBE(ihdr, 0, (uint)ow); WriteBE(ihdr, 4, (uint)oh);
        ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
        Chunk(fs, "IHDR", ihdr);
        Chunk(fs, "IDAT", ms.ToArray());
        Chunk(fs, "IEND", Array.Empty<byte>());
    }

    private static void Chunk(Stream s, string type, byte[] data)
    {
        var len = new byte[4]; WriteBE(len, 0, (uint)data.Length); s.Write(len);
        var t = System.Text.Encoding.ASCII.GetBytes(type); s.Write(t); s.Write(data);
        uint c = 0xFFFFFFFF;
        foreach (var b in t) c = s_crc[(c ^ b) & 0xFF] ^ (c >> 8);
        foreach (var b in data) c = s_crc[(c ^ b) & 0xFF] ^ (c >> 8);
        var crc = new byte[4]; WriteBE(crc, 0, c ^ 0xFFFFFFFF); s.Write(crc);
    }

    private static void WriteBE(byte[] b, int o, uint v)
    {
        b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v;
    }

    private static uint[] BuildCrc()
    {
        var t = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }
}
