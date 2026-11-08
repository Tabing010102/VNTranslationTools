using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using VNTextPatch.Shared.Util;

internal static class Cp932FontEncodingTests
{
    private static int _checks;

    private static void Check(bool condition, string description)
    {
        _checks++;
        if (!condition)
            throw new Exception(description);
    }

    private static void Throws<T>(Action action) where T : Exception
    {
        _checks++;
        try { action(); }
        catch (T) { return; }
        throw new Exception("Expected " + typeof(T).Name);
    }

    private static KeyValuePair<char, char> Pair(char translated, char carrier)
    {
        return new KeyValuePair<char, char>(translated, carrier);
    }

    private static int Main(string[] args)
    {
        var cp932 = Encoding.GetEncoding(932, EncoderFallback.ExceptionFallback, DecoderFallback.ExceptionFallback);
        var encoding = new Cp932FontEncoding(new[] { Pair('这', '丐'), Pair('传', '丕') });
        string text = "ABC 日本語 这传\r\n";
        byte[] encoded = encoding.GetBytes(text);
        Check(cp932.GetString(encoded) == "ABC 日本語 丐丕\r\n", "Expected ordinary CP932 carrier characters");
        Check(encoding.GetString(encoded) == text, "Font encoding must reverse its mapping");
        Check(encoding.GetBytes("日本語ABC").SequenceEqual(cp932.GetBytes("日本語ABC")), "Native characters changed");
        Check(encoding.GetPreamble().Length == 0, "CP932 output must not have a BOM");
        Check(encoding.GetByteCount(text) == encoded.Length, "Byte count mismatch");
        var chars = new char[text.Length + 2];
        int n = encoding.GetChars(encoded, 0, encoded.Length, chars, 1);
        Check(new string(chars, 1, n) == text && chars[0] == '\0', "Offset character decoding failed");
        var buffer = new byte[encoded.Length + 2];
        Check(encoding.GetBytes(text.ToCharArray(), 0, text.Length, buffer, 1) == encoded.Length, "Offset encoding count");
        Check(buffer.Skip(1).Take(encoded.Length).SequenceEqual(encoded), "Offset encoding data");

        Throws<EncoderFallbackException>(() => encoding.GetBytes("丐"));
        Throws<EncoderFallbackException>(() => encoding.GetBytes("闻"));
        Throws<EncoderFallbackException>(() => encoding.GetBytes("\uD840\uDC00"));
        Throws<DecoderFallbackException>(() => encoding.GetString(new byte[] { 0x81, 0x03 }));
        Throws<ArgumentException>(() => new Cp932FontEncoding(new[] { Pair('这', 'A') }));
        Throws<ArgumentException>(() => new Cp932FontEncoding(new[] { Pair('日', '丐') }));
        Throws<ArgumentException>(() => new Cp932FontEncoding(new[] { Pair('这', '丐'), Pair('传', '丐') }));
        Throws<ArgumentException>(() => new Cp932FontEncoding(new[] { Pair('这', '丐'), Pair('这', '丕') }));
        Throws<ArgumentException>(() => encoding.GetChars(encoded, 0, encoded.Length, new char[1], 0));

        using (var stream = new MemoryStream())
        {
            using (var writer = new StreamWriter(stream, encoding, 128, true))
                for (int i = 0; i < 1000; i++) writer.Write(text);
            Check(encoding.GetString(stream.ToArray()) == string.Concat(Enumerable.Repeat(text, 1000)), "StreamWriter flush/chunk roundtrip");
        }

        if (args.Length > 0)
        {
            Cp932FontEncoding actual = Cp932FontEncoding.FromFile(args[0]);
            string[] lines = File.ReadAllLines(args[0], Encoding.UTF8);
            string[] header = lines[0].TrimStart('\uFEFF').Split('\t');
            int sourceColumn = Array.IndexOf(header, "replacement_char");
            int carrierColumn = Array.IndexOf(header, "original_char");
            int hexColumn = Array.IndexOf(header, "cp932_hex");
            foreach (string line in lines.Skip(1).Where(s => !string.IsNullOrWhiteSpace(s)))
            {
                string[] row = line.Split('\t');
                byte[] b = actual.GetBytes(row[sourceColumn]);
                Check(BitConverter.ToString(b).Replace("-", "") == row[hexColumn], "Real map bytes disagree");
                Check(cp932.GetString(b) == row[carrierColumn], "Real map carrier disagrees");
                Check(actual.GetString(b) == row[sourceColumn], "Real map reverse conversion failed");
            }
        }
        Console.WriteLine("PASS: " + _checks + " CP932 font encoding checks");
        return 0;
    }
}
