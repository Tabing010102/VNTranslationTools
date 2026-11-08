using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace VNTextPatch.Shared.Util
{
    // Build-time character substitution, not SJIS tunneling. The game sees
    // ordinary CP932 characters; a matching font supplies the translated glyphs.
    public sealed class Cp932FontEncoding : Encoding
    {
        private static readonly Encoding Cp932 = GetEncoding(932, EncoderFallback.ExceptionFallback, DecoderFallback.ExceptionFallback);

        private readonly Dictionary<char, char> _toCarrier = new Dictionary<char, char>();
        private readonly Dictionary<char, char> _fromCarrier = new Dictionary<char, char>();
        private readonly HashSet<char> _nativeCharacters = new HashSet<char>();

        public Cp932FontEncoding(IEnumerable<KeyValuePair<char, char>> replacements)
        {
            if (replacements == null)
                throw new ArgumentNullException(nameof(replacements));

            foreach (KeyValuePair<char, char> replacement in replacements)
            {
                char translated = replacement.Key;
                char carrier = replacement.Value;
                if (char.IsSurrogate(translated) || char.IsControl(translated))
                    throw new ArgumentException($"Invalid replacement character U+{(int)translated:X4}");

                byte[] bytes = Cp932.GetBytes(carrier.ToString());
                if (bytes.Length != 2 || bytes[0] >= 0xF0 || bytes[1] < 0x40 || bytes[1] == 0x7F || Cp932.GetString(bytes) != carrier.ToString())
                    throw new ArgumentException($"Carrier U+{(int)carrier:X4} must round-trip as a non-private, two-byte CP932 character");

                if (IsNativeCharacter(translated))
                    throw new ArgumentException($"Replacement U+{(int)translated:X4} is already representable in CP932; preserve its native character instead");
                if (_toCarrier.ContainsKey(translated))
                    throw new ArgumentException($"Duplicate replacement U+{(int)translated:X4}");
                if (_fromCarrier.ContainsKey(carrier))
                    throw new ArgumentException($"Duplicate carrier U+{(int)carrier:X4}");

                _toCarrier.Add(translated, carrier);
                _fromCarrier.Add(carrier, translated);
            }
        }

        public static Cp932FontEncoding FromFile(string path)
        {
            string[] lines = File.ReadAllLines(path, Encoding.UTF8);
            if (lines.Length == 0)
                throw new InvalidDataException("The CP932 font map is empty");

            string[] header = lines[0].TrimStart('\uFEFF').Split('\t');
            int carrierColumn = Array.IndexOf(header, "original_char");
            int translatedColumn = Array.IndexOf(header, "replacement_char");
            int hexColumn = Array.IndexOf(header, "cp932_hex");
            if (carrierColumn < 0 || translatedColumn < 0 || hexColumn < 0)
                throw new InvalidDataException("Font map must be a TSV with cp932_hex, original_char and replacement_char columns");

            List<KeyValuePair<char, char>> replacements = new List<KeyValuePair<char, char>>();
            for (int i = 1; i < lines.Length; i++)
            {
                if (string.IsNullOrWhiteSpace(lines[i]))
                    continue;

                string[] columns = lines[i].Split('\t');
                if (columns.Length != header.Length || columns[carrierColumn].Length != 1 || columns[translatedColumn].Length != 1)
                    throw new InvalidDataException($"Invalid font map row {i + 1}");

                char carrier = columns[carrierColumn][0];
                string actualHex = BitConverter.ToString(Cp932.GetBytes(carrier.ToString())).Replace("-", "");
                string expectedHex = columns[hexColumn].Replace(" ", "").Replace("-", "");
                if (!actualHex.Equals(expectedHex, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"CP932 bytes do not match the carrier in font map row {i + 1}");

                replacements.Add(new KeyValuePair<char, char>(columns[translatedColumn][0], carrier));
            }
            return new Cp932FontEncoding(replacements);
        }

        public override string EncodingName => "CP932 with build-time font glyph substitution";

        public override int GetByteCount(char[] chars, int index, int count)
        {
            return Cp932.GetByteCount(MapForWrite(new string(chars, index, count)));
        }

        public override int GetByteCount(string text)
        {
            return Cp932.GetByteCount(MapForWrite(text));
        }

        public override int GetBytes(char[] chars, int charIndex, int charCount, byte[] bytes, int byteIndex)
        {
            string text = MapForWrite(new string(chars, charIndex, charCount));
            return Cp932.GetBytes(text, 0, text.Length, bytes, byteIndex);
        }

        public override int GetCharCount(byte[] bytes, int index, int count)
        {
            return Cp932.GetCharCount(bytes, index, count);
        }

        public override int GetChars(byte[] bytes, int byteIndex, int byteCount, char[] chars, int charIndex)
        {
            char[] decoded = Cp932.GetChars(bytes, byteIndex, byteCount);
            if (chars == null)
                throw new ArgumentNullException(nameof(chars));
            if (charIndex < 0 || charIndex > chars.Length)
                throw new ArgumentOutOfRangeException(nameof(charIndex));
            if (decoded.Length > chars.Length - charIndex)
                throw new ArgumentException("The destination character buffer is too small");

            for (int i = 0; i < decoded.Length; i++)
            {
                char c = decoded[i];
                chars[charIndex + i] = _fromCarrier.TryGetValue(c, out char translated) ? translated : c;
            }
            return decoded.Length;
        }

        public override int GetMaxByteCount(int charCount)
        {
            return Cp932.GetMaxByteCount(charCount);
        }

        public override int GetMaxCharCount(int byteCount)
        {
            return Cp932.GetMaxCharCount(byteCount);
        }

        private string MapForWrite(string text)
        {
            if (text == null)
                throw new ArgumentNullException(nameof(text));

            char[] mapped = text.ToCharArray();
            for (int i = 0; i < mapped.Length; i++)
            {
                char c = mapped[i];
                if (_toCarrier.TryGetValue(c, out char carrier))
                    mapped[i] = carrier;
                else if (_fromCarrier.ContainsKey(c))
                    throw new EncoderFallbackException($"Reserved carrier U+{(int)c:X4} occurs in source text; regenerate the font map");
                else if (!IsNativeCharacter(c))
                    throw new EncoderFallbackException($"No CP932 font mapping for U+{(int)c:X4}; regenerate the font map");
            }
            return new string(mapped);
        }

        private bool IsNativeCharacter(char c)
        {
            if (_nativeCharacters.Contains(c))
                return true;
            if (char.IsSurrogate(c))
                return false;

            try
            {
                string text = c.ToString();
                if (Cp932.GetString(Cp932.GetBytes(text)) != text)
                    return false;
                _nativeCharacters.Add(c);
                return true;
            }
            catch (EncoderFallbackException)
            {
                return false;
            }
        }
    }
}
