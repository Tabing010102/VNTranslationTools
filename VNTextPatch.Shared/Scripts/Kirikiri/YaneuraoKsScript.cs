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
        protected override Encoding GetWriteEncoding()
        {
            return StringUtil.SjisTunnelEncoding;
        }
    }
}
