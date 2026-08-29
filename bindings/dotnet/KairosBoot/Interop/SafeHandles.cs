using System;
using System.Runtime.InteropServices;

namespace KairosBoot.Interop;

internal abstract class KairosBootSafeHandle : SafeHandle
{
    protected KairosBootSafeHandle(IntPtr existingHandle)
        : base(IntPtr.Zero, true)
    {
        SetHandle(existingHandle);
    }

    public override bool IsInvalid => handle == IntPtr.Zero;
}

internal sealed class ContextSafeHandle : KairosBootSafeHandle
{
    internal ContextSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.ContextRelease(handle);
        return true;
    }
}

internal sealed class DeviceSafeHandle : KairosBootSafeHandle
{
    internal DeviceSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.DeviceRelease(handle);
        return true;
    }
}

internal sealed class ErrorSafeHandle : KairosBootSafeHandle
{
    internal ErrorSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.ErrorRelease(handle);
        return true;
    }
}

internal sealed class DeviceListSafeHandle : KairosBootSafeHandle
{
    internal DeviceListSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.DeviceListRelease(handle);
        return true;
    }
}

internal sealed class OperationSafeHandle : KairosBootSafeHandle
{
    internal OperationSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.OperationRelease(handle);
        return true;
    }
}

internal sealed class CommandResultSafeHandle : KairosBootSafeHandle
{
    internal CommandResultSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.CommandResultRelease(handle);
        return true;
    }
}

internal sealed class JobPlanSafeHandle : KairosBootSafeHandle
{
    internal JobPlanSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.JobPlanRelease(handle);
        return true;
    }
}

internal sealed class JobSafeHandle : KairosBootSafeHandle
{
    internal JobSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.JobRelease(handle);
        return true;
    }
}

internal sealed class JobReportSafeHandle : KairosBootSafeHandle
{
    internal JobReportSafeHandle(IntPtr existingHandle)
        : base(existingHandle)
    {
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.JobReportRelease(handle);
        return true;
    }
}
