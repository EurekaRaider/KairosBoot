using System;
using System.Runtime.InteropServices;
using System.Text;

namespace KairosBoot.Interop;

internal static class Utf8String
{
    internal static string FromNative(IntPtr value)
    {
        if (value == IntPtr.Zero)
        {
            return string.Empty;
        }

#if NET10_0_OR_GREATER
        return Marshal.PtrToStringUTF8(value) ?? string.Empty;
#else
        var length = 0;
        while (Marshal.ReadByte(value, length) != 0)
        {
            checked
            {
                length++;
            }
        }

        if (length == 0)
        {
            return string.Empty;
        }

        var bytes = new byte[length];
        Marshal.Copy(value, bytes, 0, length);
        return Encoding.UTF8.GetString(bytes);
#endif
    }
}
