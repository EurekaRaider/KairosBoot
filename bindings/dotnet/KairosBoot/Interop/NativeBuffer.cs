using System;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal static class NativeBuffer
{
    internal static byte[] Copy(IntPtr pointer, UIntPtr nativeSize, string fieldName)
    {
        var size = nativeSize.ToUInt64();
        if (size == 0)
        {
            return Array.Empty<byte>();
        }

        if (pointer == IntPtr.Zero)
        {
            throw new InvalidOperationException(
                $"Native {fieldName} returned a null pointer for {size} bytes.");
        }

        if (size > int.MaxValue)
        {
            throw new InvalidOperationException(
                $"Native {fieldName} exceeds the managed byte-array limit.");
        }

        var result = new byte[(int)size];
        Marshal.Copy(pointer, result, 0, result.Length);
        return result;
    }
}
