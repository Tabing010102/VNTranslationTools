using System.Text;
using VNTextPatch.Shared.Util;

namespace VNTextPatch.Shared.Scripts.Kirikiri
{
    // Yaneurao stores its .ks files as CP932. Kirikiri's normal writer is
    // UTF-16LE because that is the safest default for Kirikiri tools, but
    // this engine reads the files directly as Shift-JIS. Keep a separate
    // format so existing Kirikiri projects retain their original behavior.
    public class YaneuraoKsScript : KirikiriKsScript
    {
        // Only the explicit --fontmap option selects this build-time encoding.
        // Other formats and the existing tunnel mode remain unchanged.
        public static Cp932FontEncoding FontEncoding { get; set; }

        protected override Encoding GetReadEncoding(System.ArraySegment<byte> data)
        {
            Encoding encoding = base.GetReadEncoding(data);
            return encoding.CodePage == 932 && FontEncoding != null ? FontEncoding : encoding;
        }

        protected override Encoding GetWriteEncoding()
        {
            return FontEncoding ?? (Encoding)StringUtil.SjisTunnelEncoding;
        }
    }
}
