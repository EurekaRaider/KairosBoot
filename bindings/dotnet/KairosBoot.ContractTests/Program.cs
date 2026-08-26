using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using KairosBoot;
using KairosBoot.Interop;

internal static class Program
{
    private static int checks;

    private static async Task<int> Main()
    {
        try
        {
            CheckInteropContract();
            CheckNativeLayouts();
            CheckNativeBinaryCopy();
            CheckCommandOptions();
            CheckCommandResultLifetime();
            CheckTypedPublicSurface();
            CheckExtendedException();
            await CheckCancellationDrain().ConfigureAwait(false);
            await CheckCancellationCompletionRace().ConfigureAwait(false);
            await CheckThirtyTwoWaiters().ConfigureAwait(false);
            if (Environment.GetEnvironmentVariable("KAIROSBOOT_MANAGED_ONLY") == "1")
            {
                Console.WriteLine($"KairosBoot managed contract checks passed: {checks}");
                return 0;
            }

            CheckVersion();
            CheckDevicesEnumerate();
            CheckFlashOptions();
            await CheckFlashFailsAccurately().ConfigureAwait(false);
            await CheckPreCancellation().ConfigureAwait(false);
            await CheckTypedPreCancellation().ConfigureAwait(false);
            CheckTypedManagedPreflight();
            CheckDisposedContext();
            Console.WriteLine($"KairosBoot .NET contract checks passed: {checks}");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }

    private static void CheckInteropContract()
    {
        var imports = typeof(NativeMethods)
            .GetMethods(BindingFlags.Static | BindingFlags.NonPublic)
            .Select(method => new
            {
                Method = method,
                Import = method.GetCustomAttributesData().FirstOrDefault(
                    attribute =>
                        attribute.AttributeType.FullName ==
                            "System.Runtime.InteropServices.DllImportAttribute" ||
                        attribute.AttributeType.FullName ==
                            "System.Runtime.InteropServices.LibraryImportAttribute"),
            })
            .Where(item => item.Import != null)
            .ToList();

        var entryPoints = imports
            .Select(item => item.Import!.NamedArguments
                .Where(argument => argument.MemberName == "EntryPoint")
                .Select(argument => argument.TypedValue.Value as string)
                .Single())
            .ToList();
        var uniqueEntryPoints = entryPoints.Distinct(StringComparer.Ordinal).ToList();
        if (uniqueEntryPoints.Count != 65)
        {
            throw new InvalidOperationException(
                $"Contract check failed: expected 65 native ABI entry points, found {uniqueEntryPoints.Count}.");
        }

        checks++;
        Check(entryPoints.All(entryPoint => !string.IsNullOrEmpty(entryPoint)), "explicit entry points");
        Check(uniqueEntryPoints.Count == 65, "unique entry points");
        Check(entryPoints.Contains("kb_command_options_init"), "command options import");
        Check(entryPoints.Contains("kb_operation_command_result"), "result extraction import");
        Check(entryPoints.Contains("kb_error_session_poisoned"), "extended error import");
        CheckInteropCallingConventionAndStrings();
        Check(
            typeof(CommandResultSafeHandle).IsSubclassOf(typeof(SafeHandle)),
            "command result SafeHandle");
        using (var invalid = new CommandResultSafeHandle(IntPtr.Zero))
        {
            Check(invalid.IsInvalid, "invalid command result handle is inert");
        }
    }

    private static void CheckInteropCallingConventionAndStrings()
    {
#if NET48
        var methods = typeof(NativeMethods)
            .GetMethods(BindingFlags.Static | BindingFlags.NonPublic)
            .Select(method => new
            {
                Method = method,
                Import = method.GetCustomAttribute<DllImportAttribute>(),
            })
            .Where(item => item.Import != null)
            .ToList();
        Check(methods.Count == 65, "net48 DllImport count");
        Check(
            methods.All(item => item.Import!.CallingConvention == CallingConvention.Cdecl),
            "net48 Cdecl imports");

        var stringParameters = methods
            .SelectMany(item => item.Method.GetParameters())
            .Where(parameter => parameter.ParameterType == typeof(string))
            .ToList();
        Check(stringParameters.Count == 40, "net48 native UTF-8 string parameters");
        Check(
            stringParameters.All(parameter =>
                parameter.GetCustomAttribute<MarshalAsAttribute>()?.Value ==
                    UnmanagedType.LPUTF8Str),
            "net48 LPUTF8Str parameters");
#else
        var methods = typeof(NativeMethods)
            .GetMethods(BindingFlags.Static | BindingFlags.NonPublic)
            .Select(method => new
            {
                Method = method,
                Import = method.GetCustomAttribute<LibraryImportAttribute>(),
                Call = method.GetCustomAttribute<UnmanagedCallConvAttribute>(),
            })
            .Where(item => item.Import != null)
            .ToList();
        var groups = methods.GroupBy(item => item.Import!.EntryPoint, StringComparer.Ordinal).ToList();
        Check(groups.Count == 65, "net10 LibraryImport count");
        Check(
            groups.All(group => group.Any(item =>
                item.Call?.CallConvs.Contains(
                    typeof(System.Runtime.CompilerServices.CallConvCdecl)) == true)),
            "net10 Cdecl imports");

        var stringMethods = methods
            .Where(item => item.Method.GetParameters().Any(
                parameter => parameter.ParameterType == typeof(string)))
            .ToList();
        Check(stringMethods.Count == 24, "net10 native UTF-8 string methods");
        Check(
            stringMethods.All(item =>
                item.Import!.StringMarshalling == StringMarshalling.Utf8),
            "net10 UTF-8 LibraryImport methods");
#endif
    }

    private static void CheckNativeLayouts()
    {
        Check(
            Marshal.OffsetOf<NativeCommandOptions>(nameof(NativeCommandOptions.StructSize)).ToInt32() == 0,
            "command options struct_size offset");
        Check(
            Marshal.OffsetOf<NativeCommandOptions>(nameof(NativeCommandOptions.ApiVersion)).ToInt32() == 4,
            "command options api_version offset");
        Check(
            Marshal.OffsetOf<NativeCommandOptions>(nameof(NativeCommandOptions.TimeoutMilliseconds)).ToInt32() == 8,
            "command options timeout offset");
        Check(
            Marshal.OffsetOf<NativeCommandOptions>(nameof(NativeCommandOptions.ProgressCallback)).ToInt32() ==
                (IntPtr.Size == 8 ? 16 : 12),
            "command options callback offset");
        Check(
            Marshal.OffsetOf<NativeCommandOptions>(nameof(NativeCommandOptions.MaximumReceiveBytes)).ToInt32() ==
                (IntPtr.Size == 8 ? 32 : 24),
            "command options receive bound offset");
        Check(
            Marshal.SizeOf<NativeCommandOptions>() == (IntPtr.Size == 8 ? 40 : 32),
            "command options native size");
        Check(
            NativeMethods.CommandOptionsStructSize == Marshal.SizeOf<NativeCommandOptions>(),
            "command options declared size");
    }

    private static void CheckNativeBinaryCopy()
    {
        var pointer = Marshal.AllocHGlobal(3);
        try
        {
            Marshal.Copy(new byte[] { 0, 0xff, 0x41 }, 0, pointer, 3);
            var copy = NativeBuffer.Copy(pointer, new UIntPtr(3), "contract payload");
            Check(copy.SequenceEqual(new byte[] { 0, 0xff, 0x41 }), "binary NUL and 0xff copy");
        }
        finally
        {
            Marshal.FreeHGlobal(pointer);
        }

        Check(
            NativeBuffer.Copy(IntPtr.Zero, UIntPtr.Zero, "empty payload").Length == 0,
            "empty binary payload");
        Expect<InvalidOperationException>(
            () => NativeBuffer.Copy(IntPtr.Zero, new UIntPtr(1), "invalid payload"));
    }

    private static void CheckCommandOptions()
    {
        Check(
            CommandOptions.Default.Timeout == Timeout.InfiniteTimeSpan,
            "default command timeout");
        Check(
            default(CommandOptions).MaximumReceiveBytes ==
                CommandOptions.DefaultMaximumReceiveBytes,
            "default command receive bound");

        var fractional = TimeSpan.FromTicks(TimeSpan.TicksPerMillisecond + 1);
        var explicitOptions = new CommandOptions(fractional, 1024);
        Check(explicitOptions.Timeout == fractional, "finite command timeout");
        Check(explicitOptions.MaximumReceiveBytes == 1024, "explicit command receive bound");
        Expect<ArgumentOutOfRangeException>(
            () => _ = new CommandOptions(TimeSpan.FromTicks(-1)));
        Expect<ArgumentOutOfRangeException>(
            () => _ = new CommandOptions(Timeout.InfiniteTimeSpan, 0));
        Expect<ArgumentOutOfRangeException>(
            () => _ = new CommandOptions(Timeout.InfiniteTimeSpan, (ulong)int.MaxValue + 1));
    }

    private static void CheckCommandResultLifetime()
    {
        CommandResult result;
        using (var source = new FakeCommandResultSource())
        {
            result = CommandResult.CopyFrom(source);
            source.Overwrite();
        }

        Check(result.TerminalPayload.SequenceEqual(new byte[] { 0, 0xff }), "terminal binary lifetime");
        Check(result.Messages.Count == 2, "ordered command message count");
        Check(result.Messages[0].Kind == CommandMessageKind.Info, "INFO order");
        Check(result.Messages[0].Payload.SequenceEqual(new byte[] { 0x49, 0 }), "INFO binary payload");
        Check(result.Messages[1].Kind == CommandMessageKind.Text, "TEXT order");
        Check(result.Messages[1].Payload.SequenceEqual(new byte[] { 0xff, 0x54 }), "TEXT binary payload");
        Check(result.Data.SequenceEqual(new byte[] { 1, 0, 0xff, 2 }), "data lifetime");
        Check(result.DeviceIdentifier == "设备-一", "result UTF-8 device identifier");
        Expect<InvalidOperationException>(
            () => CommandMessageKindMapping.FromNative(2, "contract test"));
    }

    private static void CheckTypedPublicSurface()
    {
        var expected = new[]
        {
            "GetVarAsync",
            "EraseAsync",
            "SetActiveAsync",
            "RebootAsync",
            "ContinueBootAsync",
            "OemAsync",
            "RawCommandAsync",
            "BootAsync",
            "StageAsync",
            "UploadAsync",
            "FetchAsync",
        };

        foreach (var name in expected)
        {
            var methods = typeof(Context)
                .GetMethods(BindingFlags.Instance | BindingFlags.Public)
                .Where(method => method.Name == name)
                .ToList();
            Check(methods.Count == 1, $"single managed {name} primitive");
            Check(
                methods[0].ReturnType == typeof(Task<CommandResult>),
                $"{name} returns Task<CommandResult>");
        }
    }

    private static void CheckExtendedException()
    {
        var messages = new List<CommandMessage>
        {
            new CommandMessage(CommandMessageKind.Info, new byte[] { 0, 0xff }),
            new CommandMessage(CommandMessageKind.Text, new byte[] { 0x54 }),
        }.AsReadOnly();
        var exception = new KairosBootException(
            KairosBootStatus.DeviceFail,
            "device failed",
            "usb:1-2",
            -7,
            TransferState.FullyTransferred,
            new byte[] { 0x46, 0, 0xff },
            messages,
            4096,
            1024,
            TransferState.PartialOrUnknown,
            true);

        Check(exception.Status == KairosBootStatus.DeviceFail, "device FAIL status");
        Check(exception.DeviceMessage.SequenceEqual(new byte[] { 0x46, 0, 0xff }), "device FAIL binary");
        Check(exception.CommandMessages.Count == 2, "error ordered messages");
        Check(exception.InboundExpectedBytes == 4096, "inbound expected bytes");
        Check(exception.InboundTransferredBytes == 1024, "inbound transferred bytes");
        Check(
            exception.InboundTransferState == TransferState.PartialOrUnknown,
            "inbound transfer state");
        Check(exception.SessionPoisoned, "session poisoned metadata");

        var unspecified = new KairosBootException(
            KairosBootStatus.Protocol,
            "protocol failed",
            string.Empty,
            0,
            TransferState.NotSent,
            Array.Empty<byte>(),
            new List<CommandMessage>().AsReadOnly(),
            null,
            0,
            TransferState.NotSent,
            false);
        Check(!unspecified.InboundExpectedBytes.HasValue, "unspecified inbound expected bytes");
    }

    private static async Task CheckCancellationDrain()
    {
        var target = FakePollTarget.CancelAfterDrain(3);
        using (var source = new CancellationTokenSource())
        {
            var wait = OperationPollingEngine.WaitAsync(target, source.Token, 1);
            source.Cancel();
            await ExpectAsync<OperationCanceledException>(() => wait).ConfigureAwait(false);
        }

        Check(target.CancelRequests == 1, "single native cancel request");
        Check(target.PollsAfterCancel >= 3, "cancel drains to terminal state");
    }

    private static async Task CheckCancellationCompletionRace()
    {
        var target = FakePollTarget.SuccessAfterCancel();
        using (var source = new CancellationTokenSource())
        {
            var wait = OperationPollingEngine.WaitAsync(target, source.Token, 1);
            source.Cancel();
            await wait.ConfigureAwait(false);
        }

        Check(target.CancelRequests == 1, "cancel/success race request");
        Check(target.PollsAfterCancel >= 1, "success wins after cancel drain");
    }

    private static async Task CheckThirtyTwoWaiters()
    {
        var targets = Enumerable.Range(0, 32)
            .Select(_ => FakePollTarget.SuccessAfterTimeouts(4))
            .ToArray();
        var waits = targets
            .Select(target => OperationPollingEngine.WaitAsync(target, CancellationToken.None, 1))
            .ToArray();

        await Task.WhenAll(waits).ConfigureAwait(false);
        Check(targets.All(target => target.Polls >= 5), "32 nonblocking operation waiters");
        Check(targets.All(target => target.CancelRequests == 0), "32 waiters do not fabricate cancellation");
    }

    private static void CheckFlashOptions()
    {
        Check(
            FlashOptions.Default.Timeout == Timeout.InfiniteTimeSpan,
            "default flash timeout");
        Check(
            default(FlashOptions).Timeout == Timeout.InfiniteTimeSpan,
            "default struct flash timeout");
        Check(
            new FlashOptions(Timeout.InfiniteTimeSpan).Timeout == Timeout.InfiniteTimeSpan,
            "explicit infinite flash timeout");

        var fractional = TimeSpan.FromTicks(TimeSpan.TicksPerMillisecond + 1);
        Check(new FlashOptions(fractional).Timeout == fractional, "finite flash timeout");

        Expect<ArgumentOutOfRangeException>(
            () => _ = new FlashOptions(TimeSpan.FromTicks(-1)));
        Expect<ArgumentOutOfRangeException>(
            () => _ = new FlashOptions(
                TimeSpan.FromTicks((long)uint.MaxValue * TimeSpan.TicksPerMillisecond)));

        var maximum = TimeSpan.FromTicks(
            ((long)uint.MaxValue - 1) * TimeSpan.TicksPerMillisecond);
        Check(new FlashOptions(maximum).Timeout == maximum, "maximum finite flash timeout");
    }

    private static void CheckVersion()
    {
        var version = Context.Version;
        Check(version.ApiVersion == 1, "API version");
        var expected = Environment.GetEnvironmentVariable("KAIROSBOOT_EXPECTED_VERSION");
        if (string.IsNullOrEmpty(expected))
        {
            Check(
                Regex.IsMatch(
                    version.Value,
                    @"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"),
                "runtime semantic version");
        }
        else
        {
            Check(version.Value == expected, "runtime version");
        }
    }

    private static void CheckDevicesEnumerate()
    {
        using (var context = Context.Create())
        {
            var devices = context.Devices;
            Check(devices != null, "device collection");
            foreach (var device in devices!)
            {
                Check(device.Serial != null, "device serial");
                Check(device.UsbPath != null, "device USB path");
                Check(device.Product != null, "device product");
            }
        }
    }

    private static async Task CheckFlashFailsAccurately()
    {
        using (var context = Context.Create())
        {
            var reports = 0;
            var progress = new Progress<FlashProgress>(_ => reports++);
            var exception = await ExpectAsync<KairosBootException>(
                () => context.FlashFileAsync(
                    "系统",
                    "镜像.img",
                    new FlashOptions(TimeSpan.FromSeconds(30)),
                    "设备-一",
                    progress,
                    CancellationToken.None)).ConfigureAwait(false);

            Check(exception.Status == KairosBootStatus.InvalidArgument, "flash status");
            Check(exception.TransferState == TransferState.NotSent, "transfer state");
            Check(exception.DeviceIdentifier == "设备-一", "UTF-8 serial");
            Check(!exception.InboundExpectedBytes.HasValue, "flash inbound size unspecified");
            Check(reports == 0, "no fabricated progress");
        }
    }

    private static async Task CheckPreCancellation()
    {
        using (var context = Context.Create())
        using (var source = new CancellationTokenSource())
        {
            source.Cancel();
            await ExpectAsync<OperationCanceledException>(
                () => context.FlashFileAsync("system", "system.img", cancellationToken: source.Token))
                .ConfigureAwait(false);
        }
    }

    private static async Task CheckTypedPreCancellation()
    {
        using (var context = Context.Create())
        using (var source = new CancellationTokenSource())
        {
            source.Cancel();
            await ExpectAsync<OperationCanceledException>(
                () => context.GetVarAsync("product", cancellationToken: source.Token))
                .ConfigureAwait(false);
        }
    }

    private static void CheckTypedManagedPreflight()
    {
        using (var context = Context.Create())
        {
            Expect<ArgumentException>(() => _ = context.StageAsync(Array.Empty<byte>()));
        }
    }

    private static void CheckDisposedContext()
    {
        var context = Context.Create();
        context.Dispose();
        Expect<ObjectDisposedException>(() => { _ = context.Devices; });
    }

    private sealed class FakeCommandResultSource : ICommandResultSource, IDisposable
    {
        private readonly byte[] terminal = { 0, 0xff };
        private readonly byte[] info = { 0x49, 0 };
        private readonly byte[] text = { 0xff, 0x54 };
        private readonly byte[] data = { 1, 0, 0xff, 2 };
        private readonly byte[] device =
            System.Text.Encoding.UTF8.GetBytes("设备-一\0");
        private readonly GCHandle terminalHandle;
        private readonly GCHandle infoHandle;
        private readonly GCHandle textHandle;
        private readonly GCHandle dataHandle;
        private readonly GCHandle deviceHandle;

        internal FakeCommandResultSource()
        {
            terminalHandle = GCHandle.Alloc(terminal, GCHandleType.Pinned);
            infoHandle = GCHandle.Alloc(info, GCHandleType.Pinned);
            textHandle = GCHandle.Alloc(text, GCHandleType.Pinned);
            dataHandle = GCHandle.Alloc(data, GCHandleType.Pinned);
            deviceHandle = GCHandle.Alloc(device, GCHandleType.Pinned);
        }

        public IntPtr TerminalPayload(out UIntPtr size)
        {
            size = new UIntPtr((uint)terminal.Length);
            return terminalHandle.AddrOfPinnedObject();
        }

        public UIntPtr MessageCount() => new UIntPtr(2);

        public int MessageKind(UIntPtr index)
        {
            return index.ToUInt64() == 0
                ? (int)CommandMessageKind.Info
                : (int)CommandMessageKind.Text;
        }

        public IntPtr MessagePayload(UIntPtr index, out UIntPtr size)
        {
            if (index.ToUInt64() == 0)
            {
                size = new UIntPtr((uint)info.Length);
                return infoHandle.AddrOfPinnedObject();
            }

            size = new UIntPtr((uint)text.Length);
            return textHandle.AddrOfPinnedObject();
        }

        public IntPtr Data(out UIntPtr size)
        {
            size = new UIntPtr((uint)data.Length);
            return dataHandle.AddrOfPinnedObject();
        }

        public IntPtr DeviceIdentifier() => deviceHandle.AddrOfPinnedObject();

        internal void Overwrite()
        {
            Overwrite(terminal);
            Overwrite(info);
            Overwrite(text);
            Overwrite(data);
            Overwrite(device);
        }

        public void Dispose()
        {
            terminalHandle.Free();
            infoHandle.Free();
            textHandle.Free();
            dataHandle.Free();
            deviceHandle.Free();
        }

        private static void Overwrite(byte[] value)
        {
            for (var index = 0; index < value.Length; index++)
            {
                value[index] = 0x7f;
            }
        }
    }

    private sealed class FakePollTarget : IOperationPollTarget
    {
        private readonly int timeoutsBeforeSuccess;
        private readonly int drainPolls;
        private readonly bool successAfterCancellation;
        private int cancelRequested;
        private int cancelRequests;
        private int polls;
        private int pollsAfterCancel;

        private FakePollTarget(
            int timeoutsBeforeSuccess,
            int drainPolls,
            bool successAfterCancellation)
        {
            this.timeoutsBeforeSuccess = timeoutsBeforeSuccess;
            this.drainPolls = drainPolls;
            this.successAfterCancellation = successAfterCancellation;
        }

        internal int CancelRequests => Volatile.Read(ref cancelRequests);

        internal int Polls => Volatile.Read(ref polls);

        internal int PollsAfterCancel => Volatile.Read(ref pollsAfterCancel);

        internal static FakePollTarget SuccessAfterTimeouts(int timeouts)
        {
            return new FakePollTarget(timeouts, 0, false);
        }

        internal static FakePollTarget CancelAfterDrain(int drainPolls)
        {
            return new FakePollTarget(int.MaxValue, drainPolls, false);
        }

        internal static FakePollTarget SuccessAfterCancel()
        {
            return new FakePollTarget(int.MaxValue, 0, true);
        }

        public int Poll()
        {
            var currentPoll = Interlocked.Increment(ref polls);
            if (Volatile.Read(ref cancelRequested) != 0)
            {
                var afterCancel = Interlocked.Increment(ref pollsAfterCancel);
                if (successAfterCancellation)
                {
                    return (int)KairosBootStatus.Ok;
                }

                return afterCancel >= drainPolls
                    ? (int)KairosBootStatus.Cancelled
                    : (int)KairosBootStatus.Timeout;
            }

            return currentPoll > timeoutsBeforeSuccess
                ? (int)KairosBootStatus.Ok
                : (int)KairosBootStatus.Timeout;
        }

        public void RequestCancel()
        {
            Interlocked.Increment(ref cancelRequests);
            Volatile.Write(ref cancelRequested, 1);
        }

        public Exception CreateFailure(int status)
        {
            return new InvalidOperationException($"Unexpected fake status {status}.");
        }
    }

    private static TException Expect<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException exception)
        {
            checks++;
            return exception;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    private static async Task<TException> ExpectAsync<TException>(Func<Task> action)
        where TException : Exception
    {
        try
        {
            await action().ConfigureAwait(false);
        }
        catch (TException exception)
        {
            checks++;
            return exception;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    private static void Check(bool condition, string name)
    {
        if (!condition)
        {
            throw new InvalidOperationException($"Contract check failed: {name}.");
        }

        checks++;
    }
}
