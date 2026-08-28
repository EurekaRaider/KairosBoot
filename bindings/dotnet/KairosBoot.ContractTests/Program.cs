using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using KairosBoot;
using KairosBoot.Interop;

internal static class Program
{
    private static int checks;

    private const string kValidManifest =
        "apiVersion: kairosboot.io/v1\n" +
        "kind: FlashJob\n" +
        "artifacts:\n" +
        "  - id: system\n" +
        "    path: images/system.img\n" +
        "    sha256: \"1111111111111111111111111111111111111111111111111111111111111111\"\n" +
        "targets:\n" +
        "  - name: product-a\n" +
        "    selector:\n" +
        "      serials: [SERIAL-01]\n" +
        "    expectedProduct: product_a\n" +
        "    steps:\n" +
        "      - flash:\n" +
        "          partition: system\n" +
        "          artifact: system\n" +
        "policy:\n" +
        "  onDeviceFailure: continue\n" +
        "  maxParallelDevices: 32\n" +
        "  memoryBudget: auto\n";

    private static async Task<int> Main()
    {
        try
        {
            CheckInteropContract();
            CheckNativeLayouts();
            CheckNativeBinaryCopy();
            CheckCommandOptions();
            CheckUpdateOptions();
            CheckJobOptions();
            CheckCommandResultLifetime();
            CheckTypedPublicSurface();
            CheckFleetPublicSurface();
            CheckExtendedException();
            await CheckCancellationDrain().ConfigureAwait(false);
            await CheckCancellationCompletionRace().ConfigureAwait(false);
            await CheckThirtyTwoWaiters().ConfigureAwait(false);
            if (Environment.GetEnvironmentVariable("KAIROSBOOT_UPDATE_SHIM") == "1")
            {
                await CheckScriptedUpdateShim().ConfigureAwait(false);
                await CheckScriptedBootShim().ConfigureAwait(false);
                await CheckScriptedFleetShim().ConfigureAwait(false);
                Console.WriteLine($"KairosBoot scripted native checks passed: {checks}");
                return 0;
            }

            if (Environment.GetEnvironmentVariable("KAIROSBOOT_MANAGED_ONLY") == "1")
            {
                Console.WriteLine($"KairosBoot managed contract checks passed: {checks}");
                return 0;
            }

            CheckVersion();
            CheckFleetJobValidateAndPlan();
            CheckDevicesEnumerate();
            CheckFlashOptions();
            await CheckFlashFailsAccurately().ConfigureAwait(false);
            await CheckPreCancellation().ConfigureAwait(false);
            await CheckTypedPreCancellation().ConfigureAwait(false);
            await CheckTypedManagedPreflight().ConfigureAwait(false);
#if NET10_0_OR_GREATER
            await CheckScriptedTcpParity().ConfigureAwait(false);
#endif
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
        if (uniqueEntryPoints.Count != 107)
        {
            throw new InvalidOperationException(
                $"Contract check failed: expected 107 native ABI entry points, found {uniqueEntryPoints.Count}.");
        }

        checks++;
        Check(entryPoints.All(entryPoint => !string.IsNullOrEmpty(entryPoint)), "explicit entry points");
        Check(uniqueEntryPoints.Count == 107, "unique entry points");
        Check(entryPoints.Contains("kb_flash_raw_async"), "async flash:raw import");
        Check(entryPoints.Contains("kb_flash_raw"), "blocking flash:raw import");
        Check(entryPoints.Contains("kb_boot_file_async"), "async boot-file import");
        Check(entryPoints.Contains("kb_boot_file"), "blocking boot-file import");
        Check(entryPoints.Contains("kb_update_options_init"), "update options import");
        Check(entryPoints.Contains("kb_update_package_async"), "async update import");
        Check(entryPoints.Contains("kb_update_package"), "blocking update import");
        Check(entryPoints.Contains("kb_wipe_super_async"), "async wipe-super import");
        Check(entryPoints.Contains("kb_wipe_super"), "blocking wipe-super import");
        Check(entryPoints.Contains("kb_command_options_init"), "command options import");
        Check(entryPoints.Contains("kb_operation_command_result"), "result extraction import");
        Check(entryPoints.Contains("kb_upload_file_async"), "upload-file import");
        Check(entryPoints.Contains("kb_get_staged_file_async"), "get-staged-file import");
        Check(entryPoints.Contains("kb_fetch_file_async"), "fetch-file import");
        Check(entryPoints.Contains("kb_command_result_output_path"), "result output path import");
        Check(entryPoints.Contains("kb_command_result_received_bytes"), "result received bytes import");
        Check(entryPoints.Contains("kb_error_session_poisoned"), "extended error import");
        Check(entryPoints.Contains("kb_validate_job_file"), "fleet validate import");
        Check(entryPoints.Contains("kb_plan_job_file"), "fleet plan import");
        Check(entryPoints.Contains("kb_job_plan_canonical_json"), "fleet plan JSON import");
        Check(entryPoints.Contains("kb_job_plan_sha256_hex"), "fleet plan digest import");
        Check(entryPoints.Contains("kb_job_plan_release"), "fleet plan release import");
        Check(entryPoints.Contains("kb_job_options_init"), "fleet job options import");
        Check(entryPoints.Contains("kb_run_job_file_async"), "fleet async run import");
        Check(entryPoints.Contains("kb_run_job_file"), "fleet blocking run import");
        Check(entryPoints.Contains("kb_job_wait"), "fleet wait import");
        Check(entryPoints.Contains("kb_job_cancel"), "fleet cancel import");
        Check(entryPoints.Contains("kb_job_state"), "fleet state import");
        Check(entryPoints.Contains("kb_job_error"), "fleet error import");
        Check(entryPoints.Contains("kb_job_get_report"), "fleet report import");
        Check(entryPoints.Contains("kb_job_release"), "fleet job release import");
        Check(entryPoints.Contains("kb_job_report_json"), "fleet report JSON import");
        Check(entryPoints.Contains("kb_job_report_release"), "fleet report release import");
        Check(entryPoints.Contains("kb_flashing_async"), "flashing import");
        Check(entryPoints.Contains("kb_gsi_async"), "GSI import");
        Check(entryPoints.Contains("kb_snapshot_update_async"), "snapshot-update import");
        Check(
            entryPoints.Contains("kb_create_logical_partition_async"),
            "create logical partition import");
        Check(
            entryPoints.Contains("kb_delete_logical_partition_async"),
            "delete logical partition import");
        Check(
            entryPoints.Contains("kb_resize_logical_partition_async"),
            "resize logical partition import");
        CheckInteropCallingConventionAndStrings();
        Check(
            typeof(CommandResultSafeHandle).IsSubclassOf(typeof(SafeHandle)),
            "command result SafeHandle");
        using (var invalid = new CommandResultSafeHandle(IntPtr.Zero))
        {
            Check(invalid.IsInvalid, "invalid command result handle is inert");
        }
        Check(
            typeof(JobPlanSafeHandle).IsSubclassOf(typeof(SafeHandle)),
            "job plan SafeHandle");
        using (var invalidPlan = new JobPlanSafeHandle(IntPtr.Zero))
        {
            Check(invalidPlan.IsInvalid, "invalid job plan handle is inert");
        }
        Check(typeof(JobSafeHandle).IsSubclassOf(typeof(SafeHandle)), "job SafeHandle");
        Check(
            typeof(JobReportSafeHandle).IsSubclassOf(typeof(SafeHandle)),
            "job report SafeHandle");
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
        Check(methods.Count == 107, "net48 DllImport count");
        Check(
            methods.All(item => item.Import!.CallingConvention == CallingConvention.Cdecl),
            "net48 Cdecl imports");

        var stringParameters = methods
            .SelectMany(item => item.Method.GetParameters())
            .Where(parameter => parameter.ParameterType == typeof(string))
            .ToList();
        Check(stringParameters.Count == 91, "net48 native UTF-8 string parameters");
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
        Check(groups.Count == 107, "net10 LibraryImport count");
        Check(
            groups.All(group => group.Any(item =>
                item.Call?.CallConvs.Contains(
                    typeof(System.Runtime.CompilerServices.CallConvCdecl)) == true)),
            "net10 Cdecl imports");

        var stringMethods = methods
            .Where(item => item.Method.GetParameters().Any(
                parameter => parameter.ParameterType == typeof(string)))
            .ToList();
        Check(stringMethods.Count == 51, "net10 native UTF-8 string methods");
        Check(
            stringMethods.All(item =>
                item.Import!.StringMarshalling == StringMarshalling.Utf8),
            "net10 UTF-8 LibraryImport methods");
#endif
    }

    private static void CheckNativeLayouts()
    {
        Check(
            Marshal.OffsetOf<NativeUpdateOptions>(nameof(NativeUpdateOptions.StructSize)).ToInt32() == 0,
            "update options struct_size offset");
        Check(
            Marshal.OffsetOf<NativeUpdateOptions>(nameof(NativeUpdateOptions.ApiVersion)).ToInt32() == 4,
            "update options api_version offset");
        Check(
            Marshal.OffsetOf<NativeUpdateOptions>(nameof(NativeUpdateOptions.TimeoutMilliseconds)).ToInt32() == 8,
            "update options timeout offset");
        Check(
            Marshal.OffsetOf<NativeUpdateOptions>(nameof(NativeUpdateOptions.Wipe)).ToInt32() == 12,
            "update options wipe offset");
        Check(
            Marshal.OffsetOf<NativeUpdateOptions>(nameof(NativeUpdateOptions.ProgressCallback)).ToInt32() == 16,
            "update options callback offset");
        Check(
            Marshal.OffsetOf<NativeUpdateOptions>(nameof(NativeUpdateOptions.ProgressUserData)).ToInt32() ==
                (IntPtr.Size == 8 ? 24 : 20),
            "update options callback state offset");
        Check(
            Marshal.SizeOf<NativeUpdateOptions>() == (IntPtr.Size == 8 ? 32 : 24),
            "update options native size");
        Check(
            NativeMethods.UpdateOptionsStructSize == Marshal.SizeOf<NativeUpdateOptions>(),
            "update options declared size");

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

        Check(
            Marshal.OffsetOf<NativeJobOptions>(nameof(NativeJobOptions.StructSize)).ToInt32() == 0,
            "job options struct_size offset");
        Check(
            Marshal.OffsetOf<NativeJobOptions>(nameof(NativeJobOptions.ApiVersion)).ToInt32() == 4,
            "job options api_version offset");
        Check(
            Marshal.OffsetOf<NativeJobOptions>(nameof(NativeJobOptions.TimeoutMilliseconds)).ToInt32() == 8,
            "job options timeout offset");
        Check(
            Marshal.OffsetOf<NativeJobOptions>(nameof(NativeJobOptions.ProgressCallback)).ToInt32() ==
                (IntPtr.Size == 8 ? 16 : 12),
            "job options callback offset");
        Check(
            Marshal.SizeOf<NativeJobOptions>() == (IntPtr.Size == 8 ? 32 : 20),
            "job options native size");
        Check(
            NativeMethods.JobOptionsStructSize == Marshal.SizeOf<NativeJobOptions>(),
            "job options declared size");
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

    private static void CheckUpdateOptions()
    {
        Check(
            UpdateOptions.Default.Timeout == Timeout.InfiniteTimeSpan,
            "default update timeout");
        Check(!UpdateOptions.Default.Wipe, "default update preserves data");
        Check(
            default(UpdateOptions).Timeout == Timeout.InfiniteTimeSpan,
            "default struct update timeout");

        var infiniteWipe = new UpdateOptions(Timeout.InfiniteTimeSpan, wipe: true);
        Check(infiniteWipe.Timeout == Timeout.InfiniteTimeSpan, "explicit infinite update timeout");
        Check(infiniteWipe.Wipe, "explicit update wipe");

        var fractional = TimeSpan.FromTicks(TimeSpan.TicksPerMillisecond + 1);
        Check(new UpdateOptions(fractional).Timeout == fractional, "finite update timeout");

        Expect<ArgumentOutOfRangeException>(
            () => _ = new UpdateOptions(TimeSpan.FromTicks(-1)));
        Expect<ArgumentOutOfRangeException>(
            () => _ = new UpdateOptions(
                TimeSpan.FromTicks((long)uint.MaxValue * TimeSpan.TicksPerMillisecond)));

        var maximum = TimeSpan.FromTicks(
            ((long)uint.MaxValue - 1) * TimeSpan.TicksPerMillisecond);
        Check(new UpdateOptions(maximum).Timeout == maximum, "maximum finite update timeout");
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
        Check(result.OutputPath == "结果.bin", "result UTF-8 output path");
        Check(result.ReceivedBytes == 4, "result received byte count");
        Check(result.DeviceIdentifier == "设备-一", "result UTF-8 device identifier");
        Expect<InvalidOperationException>(
            () => CommandMessageKindMapping.FromNative(2, "contract test"));
    }

    private static void CheckJobOptions()
    {
        Check(JobOptions.Default.Timeout == Timeout.InfiniteTimeSpan, "default job timeout");
        Check(default(JobOptions).Timeout == Timeout.InfiniteTimeSpan, "default struct job timeout");
        var fractional = TimeSpan.FromTicks(TimeSpan.TicksPerMillisecond + 1);
        Check(new JobOptions(fractional).Timeout == fractional, "finite job timeout");
        Expect<ArgumentOutOfRangeException>(
            () => _ = new JobOptions(TimeSpan.FromTicks(-1)));
        Expect<ArgumentOutOfRangeException>(
            () => _ = new JobOptions(
                TimeSpan.FromTicks((long)uint.MaxValue * TimeSpan.TicksPerMillisecond)));
    }

    private static void CheckTypedPublicSurface()
    {
        var bootFileMethods = typeof(Context)
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(method => method.Name == "BootFileAsync")
            .ToList();
        Check(bootFileMethods.Count == 2, "managed boot-file overloads");
        Check(
            bootFileMethods.All(method => method.ReturnType == typeof(Task)),
            "managed boot-file returns Task");
        Check(
            bootFileMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(IProgress<FlashProgress>))),
            "managed boot-file progress type");
        Check(
            bootFileMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(CancellationToken))),
            "managed boot-file cancellation");

        var flashRawMethods = typeof(Context)
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(method => method.Name == "FlashRawAsync")
            .ToList();
        Check(flashRawMethods.Count == 2, "managed flash:raw overloads");
        Check(
            flashRawMethods.All(method => method.ReturnType == typeof(Task)),
            "managed flash:raw returns Task");
        Check(
            flashRawMethods.Count(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(FlashOptions))) == 1,
            "managed flash:raw typed options overload");
        Check(
            flashRawMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(IProgress<FlashProgress>))),
            "managed flash:raw progress type");
        Check(
            flashRawMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(CancellationToken))),
            "managed flash:raw cancellation");

        var updateMethods = typeof(Context)
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(method => method.Name == "UpdatePackageAsync")
            .ToList();
        Check(updateMethods.Count == 2, "managed update overloads");
        Check(
            updateMethods.All(method => method.ReturnType == typeof(Task)),
            "managed update returns Task");
        Check(
            updateMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(IProgress<UpdateProgress>))),
            "managed update progress type");
        Check(
            updateMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(CancellationToken))),
            "managed update cancellation");

        var wipeSuperMethods = typeof(Context)
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(method => method.Name == "WipeSuperAsync")
            .ToList();
        Check(wipeSuperMethods.Count == 2, "managed wipe-super overloads");
        Check(
            wipeSuperMethods.All(method => method.ReturnType == typeof(Task)),
            "managed wipe-super returns Task");
        Check(
            wipeSuperMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(IProgress<UpdateProgress>))),
            "managed wipe-super progress type");
        Check(
            wipeSuperMethods.All(method => method.GetParameters().Any(parameter =>
                parameter.ParameterType == typeof(CancellationToken))),
            "managed wipe-super cancellation");

        var expected = new[]
        {
            "GetVarAsync",
            "EraseAsync",
            "SetActiveAsync",
            "FlashingAsync",
            "GsiAsync",
            "SnapshotUpdateAsync",
            "CreateLogicalPartitionAsync",
            "DeleteLogicalPartitionAsync",
            "ResizeLogicalPartitionAsync",
            "RebootAsync",
            "ContinueBootAsync",
            "OemAsync",
            "RawCommandAsync",
            "BootAsync",
            "StageAsync",
            "UploadAsync",
            "FetchAsync",
            "UploadFileAsync",
            "GetStagedFileAsync",
            "FetchFileAsync",
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

        Check(typeof(FlashingCommand).IsEnum, "strong flashing enum");
        Check(typeof(GsiCommand).IsEnum, "strong GSI enum");
        Check(typeof(SnapshotUpdateCommand).IsEnum, "strong snapshot enum");
        Check((int)FlashingCommand.Lock == 0, "flashing lock enum value");
        Check((int)FlashingCommand.GetUnlockAbility == 4, "flashing ability enum value");
        Check((int)GsiCommand.Status == 2, "GSI status enum value");
        Check((int)SnapshotUpdateCommand.Merge == 1, "snapshot merge enum value");

        var managementMethods = expected
            .Skip(3)
            .Take(6)
            .Select(name => typeof(Context)
                .GetMethods(BindingFlags.Instance | BindingFlags.Public)
                .Single(method => method.Name == name))
            .ToList();
        Check(
            managementMethods.All(method =>
                method.GetParameters()
                    .Single(parameter => parameter.Name == "deviceSelector")
                    .DefaultValue == null),
            "management selector defaults to null");
        Check(
            managementMethods
                .Where(method => method.Name == "CreateLogicalPartitionAsync" ||
                    method.Name == "ResizeLogicalPartitionAsync")
                .All(method => method.GetParameters()[1].ParameterType == typeof(ulong)),
            "logical partition sizes are UInt64");
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

    private static async Task CheckScriptedUpdateShim()
    {
        ScriptedUpdateNativeMethods.Reset();

        using (var context = new ContextSafeHandle(new IntPtr(1)))
        {
            var nativeOptions = new NativeUpdateOptions();
            NativeMethods.UpdateOptionsInit(ref nativeOptions);
            nativeOptions.TimeoutMilliseconds = 17;
            nativeOptions.Wipe = 1;
            var status = NativeMethods.UpdatePackage(
                context,
                "usb:serial:blocking",
                "blocking.zip",
                ref nativeOptions,
                out var rawError);
            Check(status == (int)KairosBootStatus.Ok, "blocking update shim status");
            Check(rawError == IntPtr.Zero, "blocking update shim error ownership");

            var nativeFlashOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInit(ref nativeFlashOptions);
            nativeFlashOptions.TimeoutMilliseconds = 17;
            status = NativeMethods.FlashRaw(
                context,
                "usb:serial:blocking-raw",
                "boot",
                "blocking-kernel.bin",
                null,
                null,
                ref nativeFlashOptions,
                out rawError);
            Check(status == (int)KairosBootStatus.Ok, "blocking flash:raw shim status");
            Check(rawError == IntPtr.Zero, "blocking flash:raw shim error ownership");
        }

        var reports = new List<UpdateProgress>();
        using (var context = Context.Create())
        {
            await context.UpdatePackageAsync("default.zip").ConfigureAwait(false);

            var progress = new InlineProgress<UpdateProgress>(reports.Add);
            var fractional = TimeSpan.FromTicks(TimeSpan.TicksPerMillisecond + 1);
            await context.UpdatePackageAsync(
                "images/升级.zip",
                new UpdateOptions(fractional, wipe: true),
                "usb:serial:device",
                progress,
                CancellationToken.None).ConfigureAwait(false);

            Check(reports.Count == 3, "update progress count");
            Check(reports[0].Stage == "preflight", "update preflight progress");
            Check(reports[0].BytesTotal == 0, "update preflight byte total");
            Check(reports[1].Stage == "download", "update download progress");
            Check(
                reports[1].BytesCompleted == 2 && reports[1].BytesTotal == 4,
                "update download byte progress");
            Check(reports[2].Stage == "complete", "update completion progress");
            Check(
                reports.All(report => report.DeviceIdentifier == "usb:serial:device"),
                "update progress device identifier");

            using (var source = new CancellationTokenSource())
            {
                var cancellationProgress = new InlineProgress<UpdateProgress>(_ => source.Cancel());
                await ExpectAsync<OperationCanceledException>(
                    () => context.UpdatePackageAsync(
                        "cancel.zip",
                        deviceSelector: "tcp:127.0.0.1:5554",
                        progress: cancellationProgress,
                        cancellationToken: source.Token)).ConfigureAwait(false);
            }

            using (var source = new CancellationTokenSource())
            {
                source.Cancel();
                await ExpectAsync<OperationCanceledException>(
                    () => context.UpdatePackageAsync(
                        "not-started.zip",
                        cancellationToken: source.Token)).ConfigureAwait(false);
            }

            await ExpectAsync<ArgumentException>(
                () => context.UpdatePackageAsync(string.Empty)).ConfigureAwait(false);
            await ExpectAsync<ArgumentException>(
                () => context.UpdatePackageAsync(
                    "update.zip",
                    deviceSelector: string.Empty)).ConfigureAwait(false);

            var rawReports = new List<FlashProgress>();
            var rawProgress = new InlineProgress<FlashProgress>(rawReports.Add);
            await context.FlashRawAsync(
                "boot",
                "images/内核.bin",
                new FlashOptions(fractional),
                ramdiskPath: "ramdisk.img",
                deviceSelector: "usb:serial:raw",
                progress: rawProgress,
                cancellationToken: CancellationToken.None).ConfigureAwait(false);
            Check(rawReports.Count == 3, "flash:raw progress count");
            Check(
                rawReports[0].Stage == "download" &&
                rawReports[0].BytesCompleted == 0 &&
                rawReports[0].BytesTotal == 4,
                "flash:raw initial progress");
            Check(
                rawReports[1].Stage == "download" &&
                rawReports[1].BytesCompleted == 2,
                "flash:raw transfer progress");
            Check(rawReports[2].Stage == "complete", "flash:raw completion progress");
            Check(
                rawReports.All(report => report.DeviceIdentifier == "usb:serial:raw"),
                "flash:raw progress device identifier");

            using (var source = new CancellationTokenSource())
            {
                var cancellationProgress = new InlineProgress<FlashProgress>(_ => source.Cancel());
                await ExpectAsync<OperationCanceledException>(
                    () => context.FlashRawAsync(
                        "boot",
                        "cancel-kernel.bin",
                        deviceSelector: "tcp:127.0.0.1:5554",
                        progress: cancellationProgress,
                        cancellationToken: source.Token)).ConfigureAwait(false);
            }

            using (var source = new CancellationTokenSource())
            {
                source.Cancel();
                await ExpectAsync<OperationCanceledException>(
                    () => context.FlashRawAsync(
                        "boot",
                        "not-started-kernel.bin",
                        cancellationToken: source.Token)).ConfigureAwait(false);
            }
        }

        Check(ScriptedUpdateNativeMethods.FailureCode() == 0, "scripted native assertions");
        Check(ScriptedUpdateNativeMethods.OptionsInitCount() == 4, "update options init calls");
        Check(ScriptedUpdateNativeMethods.AsyncStartCount() == 3, "async update starts");
        Check(ScriptedUpdateNativeMethods.BlockingCount() == 1, "blocking update import call");
        Check(
            ScriptedUpdateNativeMethods.FlashOptionsInitCount() == 3,
            "flash:raw options init calls");
        Check(
            ScriptedUpdateNativeMethods.FlashRawAsyncStartCount() == 2,
            "async flash:raw starts");
        Check(
            ScriptedUpdateNativeMethods.FlashRawBlockingCount() == 1,
            "blocking flash:raw import call");
        Check(ScriptedUpdateNativeMethods.CancelCount() == 2, "native operation cancellation");
        Check(ScriptedUpdateNativeMethods.OperationReleaseCount() == 5, "operation release");
        Check(ScriptedUpdateNativeMethods.ContextReleaseCount() == 2, "update context release");
    }

    private static async Task CheckScriptedBootShim()
    {
        ScriptedUpdateNativeMethods.Reset();

        using (var context = new ContextSafeHandle(new IntPtr(1)))
        {
            var nativeOptions = new NativeFlashOptions();
            NativeMethods.FlashOptionsInit(ref nativeOptions);
            nativeOptions.TimeoutMilliseconds = 17;
            var status = NativeMethods.BootFile(
                context,
                "usb:serial:blocking-boot",
                "blocking-boot.img",
                ref nativeOptions,
                out var rawError);
            Check(status == (int)KairosBootStatus.Ok, "blocking boot-file shim status");
            Check(rawError == IntPtr.Zero, "blocking boot-file shim error ownership");
        }

        var reports = new List<FlashProgress>();
        using (var context = Context.Create())
        {
            await context.BootFileAsync("default-boot.img").ConfigureAwait(false);

            var progress = new InlineProgress<FlashProgress>(reports.Add);
            var fractional = TimeSpan.FromTicks(TimeSpan.TicksPerMillisecond + 1);
            await context.BootFileAsync(
                "images/启动.img",
                new FlashOptions(fractional),
                "usb:serial:boot-device",
                progress,
                CancellationToken.None).ConfigureAwait(false);

            Check(reports.Count == 3, "boot-file progress count");
            Check(reports[0].Stage == "preflight", "boot-file preflight progress");
            Check(reports[1].Stage == "download", "boot-file download progress");
            Check(
                reports[1].BytesCompleted == 2 && reports[1].BytesTotal == 4,
                "boot-file byte progress");
            Check(reports[2].Stage == "complete", "boot-file completion progress");
            Check(
                reports.All(report => report.DeviceIdentifier == "usb:serial:boot-device"),
                "boot-file progress device identifier");

            using (var source = new CancellationTokenSource())
            {
                var cancellationProgress = new InlineProgress<FlashProgress>(_ => source.Cancel());
                await ExpectAsync<OperationCanceledException>(
                    () => context.BootFileAsync(
                        "cancel-boot.img",
                        deviceSelector: "tcp:127.0.0.1:5554",
                        progress: cancellationProgress,
                        cancellationToken: source.Token)).ConfigureAwait(false);
            }

            using (var source = new CancellationTokenSource())
            {
                source.Cancel();
                await ExpectAsync<OperationCanceledException>(
                    () => context.BootFileAsync(
                        "not-started-boot.img",
                        cancellationToken: source.Token)).ConfigureAwait(false);
            }

            await ExpectAsync<ArgumentException>(
                () => context.BootFileAsync(string.Empty)).ConfigureAwait(false);
            await ExpectAsync<ArgumentException>(
                () => context.BootFileAsync(
                    "boot.img",
                    deviceSelector: string.Empty)).ConfigureAwait(false);
        }

        Check(ScriptedUpdateNativeMethods.FailureCode() == 0, "boot-file native assertions");
        Check(
            ScriptedUpdateNativeMethods.FlashOptionsInitCount() == 4,
            "boot-file options init calls");
        Check(ScriptedUpdateNativeMethods.BootAsyncStartCount() == 3, "async boot-file starts");
        Check(ScriptedUpdateNativeMethods.BootBlockingCount() == 1, "blocking boot-file import call");
        Check(ScriptedUpdateNativeMethods.CancelCount() == 1, "native boot-file cancellation");
        Check(
            ScriptedUpdateNativeMethods.OperationReleaseCount() == 3,
            "boot-file operation release");
        Check(ScriptedUpdateNativeMethods.ContextReleaseCount() == 2, "boot-file context release");
    }

    private static void CheckFleetPublicSurface()
    {
        Check(typeof(Fleet).IsAbstract && typeof(Fleet).IsSealed, "static fleet entry");
        var validate = typeof(Fleet).GetMethod("ValidateJobFile", new[] { typeof(string) });
        Check(validate != null && validate.ReturnType == typeof(void), "fleet validate signature");
        var plan = typeof(Fleet).GetMethod("PlanJobFile", new[] { typeof(string) });
        Check(plan != null && plan.ReturnType == typeof(JobPlan), "fleet plan signature");
        Check(
            typeof(JobPlan).IsSealed && typeof(JobPlan).GetInterfaces().Contains(typeof(IDisposable)),
            "sealed disposable job plan");
        Check(
            typeof(JobPlan).GetProperty("CanonicalJson")?.PropertyType == typeof(string) &&
                typeof(JobPlan).GetProperty("CanonicalJson")?.CanRead == true &&
                typeof(JobPlan).GetProperty("CanonicalJson")?.CanWrite == false,
            "job plan canonical JSON property");
        Check(
            typeof(JobPlan).GetProperty("Sha256Hex")?.PropertyType == typeof(string) &&
                typeof(JobPlan).GetProperty("Sha256Hex")?.CanRead == true &&
                typeof(JobPlan).GetProperty("Sha256Hex")?.CanWrite == false,
            "job plan digest property");
        Check(typeof(JobOptions).IsValueType, "fleet job options value type");
        Check(typeof(JobProgress).IsSealed, "fleet progress sealed");
        Check(
            typeof(FleetJob).IsSealed &&
                typeof(FleetJob).GetInterfaces().Contains(typeof(IDisposable)),
            "fleet job lifecycle owner");
        Check(
            typeof(JobReport).IsSealed &&
                typeof(JobReport).GetInterfaces().Contains(typeof(IDisposable)),
            "fleet report lifecycle owner");
        Check(
            typeof(FleetJob).GetProperty("State")?.PropertyType == typeof(OperationState),
            "fleet job state property");
        Check(
            typeof(FleetJob).GetProperty("Error")?.PropertyType == typeof(KairosBootException),
            "fleet job error property");
        Check(
            typeof(FleetJob).GetMethod("Wait", Type.EmptyTypes)?.ReturnType ==
                typeof(JobReport),
            "fleet blocking wait signature");
        Check(
            typeof(FleetJob).GetMethod("WaitAsync", new[] { typeof(CancellationToken) })?.ReturnType ==
                typeof(Task<JobReport>),
            "fleet async wait signature");
        Check(
            typeof(FleetJob).GetMethod("GetReport", Type.EmptyTypes)?.ReturnType ==
                typeof(JobReport),
            "fleet report extraction signature");
        Check(
            typeof(JobReport).GetProperty("Json")?.PropertyType == typeof(string),
            "fleet report JSON property");

        var startMethods = typeof(Context)
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(method => method.Name == "StartJobFile")
            .ToList();
        Check(startMethods.Count == 2, "fleet start overloads");
        Check(
            startMethods.All(method => method.ReturnType == typeof(FleetJob)),
            "fleet start returns lifecycle");
        Check(
            typeof(Context).GetMethod("RunJobFile")?.ReturnType == typeof(JobReport),
            "fleet blocking run signature");
        Check(
            typeof(Context).GetMethod("RunJobFileAsync")?.ReturnType == typeof(Task<JobReport>),
            "fleet async run signature");
    }

    private static async Task CheckScriptedFleetShim()
    {
        ScriptedUpdateNativeMethods.Reset();
        var progressEvents = new List<JobProgress>();
        var progress = new InlineProgress<JobProgress>(progressEvents.Add);

        using (var context = Context.Create())
        {
            var blocking = context.RunJobFile(
                "blocking.yaml", new JobOptions(TimeSpan.FromMilliseconds(17)));
            Check(ContainsOrdinal(blocking.Json, "\"blocking\":true"), "blocking fleet report");
            blocking.Dispose();
            Expect<ObjectDisposedException>(() => { _ = blocking.Json; });

            JobReport independent;
            using (var job = context.StartJobFile(
                "success.yaml",
                new JobOptions(TimeSpan.FromTicks(TimeSpan.TicksPerMillisecond + 1)),
                progress))
            {
                Check(job.State == OperationState.Running, "fleet running state");
                Check(job.Error == null, "fleet running error absent");
                var busy = Expect<KairosBootException>(() => job.GetReport());
                Check(busy.Status == KairosBootStatus.Busy, "fleet early report busy");
                independent = await job.WaitAsync().ConfigureAwait(false);
                Check(job.State == OperationState.Succeeded, "fleet succeeded state");
                Check(job.Error == null, "fleet success error absent");
            }
            Check(
                ContainsOrdinal(independent.Json, "\"state\":\"succeeded\""),
                "fleet report outlives job");
            independent.Dispose();

            using (var job = context.StartJobFile("failure.yaml"))
            {
                var failure = await ExpectAsync<KairosBootException>(
                    () => job.WaitAsync()).ConfigureAwait(false);
                Check(failure.Status == KairosBootStatus.Io, "fleet failure status");
                Check(job.State == OperationState.Failed, "fleet failed state");
                Check(job.Error?.Status == KairosBootStatus.Io, "fleet error snapshot status");
                Check(job.Error?.NativeCode == -55, "fleet error snapshot native code");
                using (var report = job.GetReport())
                {
                    Check(
                        ContainsOrdinal(report.Json, "\"state\":\"failed\""),
                        "fleet failure report");
                }
            }

            using (var cancellation = new CancellationTokenSource())
            using (var job = context.StartJobFile(
                "cancel.yaml",
                progress: new InlineProgress<JobProgress>(_ => cancellation.Cancel())))
            {
                await ExpectAsync<OperationCanceledException>(
                    () => job.WaitAsync(cancellation.Token)).ConfigureAwait(false);
                Check(job.State == OperationState.Cancelled, "fleet cancelled state");
                Check(
                    job.Error?.Status == KairosBootStatus.Cancelled,
                    "fleet cancellation error snapshot");
                using (var report = job.GetReport())
                {
                    Check(
                        ContainsOrdinal(report.Json, "\"state\":\"cancelled\""),
                        "fleet cancellation report");
                }
            }

            var disposed = context.StartJobFile("dispose.yaml");
            var disposingWait = disposed.WaitAsync();
            disposed.Dispose();
            disposed.Dispose();
            var disposeCancellation = await ExpectAsync<KairosBootException>(
                () => disposingWait).ConfigureAwait(false);
            Check(
                disposeCancellation.Status == KairosBootStatus.Cancelled,
                "fleet dispose cancels and drains wait");
            Expect<ObjectDisposedException>(() => { _ = disposed.State; });

            using (var convenience = await context.RunJobFileAsync(
                "success.yaml",
                new JobOptions(TimeSpan.FromMilliseconds(2)),
                progress).ConfigureAwait(false))
            {
                Check(
                    ContainsOrdinal(convenience.Json, "\"state\":\"succeeded\""),
                    "fleet async convenience report");
            }

            using (var preCancelled = new CancellationTokenSource())
            {
                preCancelled.Cancel();
                await ExpectAsync<OperationCanceledException>(
                    () => context.RunJobFileAsync(
                        "success.yaml",
                        new JobOptions(TimeSpan.FromMilliseconds(2)),
                        progress,
                        preCancelled.Token)).ConfigureAwait(false);
            }
        }

        Check(progressEvents.Count >= 2, "fleet progress observed");
        Check(progressEvents.All(item => item.Stage == "execute"), "fleet progress stage");
        Check(
            progressEvents.All(item => item.DeviceIdentifier == "SERIAL-01"),
            "fleet progress device");
        Check(ScriptedUpdateNativeMethods.FailureCode() == 0, "fleet native assertions");
        Check(ScriptedUpdateNativeMethods.JobOptionsInitCount() == 6, "fleet options init calls");
        Check(ScriptedUpdateNativeMethods.JobAsyncStartCount() == 5, "fleet async starts");
        Check(ScriptedUpdateNativeMethods.JobBlockingCount() == 1, "fleet blocking call");
        Check(ScriptedUpdateNativeMethods.JobCancelCount() == 2, "fleet cancellation count");
        Check(ScriptedUpdateNativeMethods.JobReleaseCount() == 5, "fleet job releases");
        Check(
            ScriptedUpdateNativeMethods.JobReportReleaseCount() == 5,
            "fleet report releases");
        Check(ScriptedUpdateNativeMethods.ContextReleaseCount() == 1, "fleet context release");
    }

    private static void CheckFleetJobValidateAndPlan()
    {
        Expect<ArgumentException>(() => Fleet.ValidateJobFile(null!));
        Expect<ArgumentException>(() => Fleet.ValidateJobFile(string.Empty));
        Expect<ArgumentException>(() => Fleet.PlanJobFile(null!));
        Expect<ArgumentException>(() => Fleet.PlanJobFile(string.Empty));
        Expect<ArgumentException>(() => Fleet.PlanJobFile("job\0.yaml"));

        var directory = Path.Combine(
            Path.GetTempPath(),
            "kairosboot-csharp-fleet-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var valid = Path.Combine(directory, "kb_fleet_valid.yaml");
            WriteUtf8(valid, kValidManifest);
            var syntax = Path.Combine(directory, "kb_fleet_syntax.yaml");
            WriteUtf8(syntax, "apiVersion: kairosboot.io/v1\nkind: [unclosed\n");
            var semantic = Path.Combine(directory, "kb_fleet_semantic.yaml");
            WriteUtf8(
                semantic,
                "apiVersion: kairosboot.io/v1\nkind: FlashJob\nbogusField: true\n");

            Fleet.ValidateJobFile(valid);
            using (var plan = Fleet.PlanJobFile(valid))
            {
                var json = plan.CanonicalJson;
                Check(json.Length > 1, "fleet canonical JSON size");
                Check(json[0] == '{' && json[json.Length - 1] == '}', "fleet canonical JSON framing");
                Check(json[json.Length - 1] != '\n', "fleet canonical JSON has no trailing LF");
                Check(
                    ContainsOrdinal(json, "\"kind\":\"FlashJob\""),
                    "fleet canonical kind");
                Check(
                    ContainsOrdinal(json, "\"schemaVersion\":1"),
                    "fleet canonical schema version");

                var digest = plan.Sha256Hex;
                Check(digest.Length == 64, "fleet digest length");
                Check(IsLowercaseHex(digest), "fleet digest alphabet");
                Check(digest == Sha256HexOf(json), "fleet digest matches canonical bytes");
                Check(plan.CanonicalJson == json, "fleet canonical JSON is stable");
            }

            var first = Fleet.PlanJobFile(valid);
            var second = Fleet.PlanJobFile(valid);
            Check(!ReferenceEquals(first, second), "fleet independent plan instances");
            Check(first.CanonicalJson == second.CanonicalJson, "fleet independent canonical JSON");
            Check(first.Sha256Hex == second.Sha256Hex, "fleet independent digests");
            first.Dispose();
            first.Dispose();
            Check(second.CanonicalJson.Length > 1, "fleet surviving plan JSON");
            Check(second.Sha256Hex.Length == 64, "fleet surviving plan digest");
            Expect<ObjectDisposedException>(() => { _ = first.CanonicalJson; });
            Expect<ObjectDisposedException>(() => { _ = first.Sha256Hex; });
            second.Dispose();

            var missing = Path.Combine(directory, "kb_fleet_missing.yaml");
            var missingValidate = Expect<KairosBootException>(() => Fleet.ValidateJobFile(missing));
            Check(missingValidate.Status == KairosBootStatus.Io, "fleet missing validate status");
            Check(missingValidate.NativeCode == 2, "fleet missing native errno");
            Check(
                ContainsOrdinal(missingValidate.Message, "kb_fleet_missing.yaml"),
                "fleet missing path message");
            var missingPlan = Expect<KairosBootException>(() => Fleet.PlanJobFile(missing));
            Check(missingPlan.Status == KairosBootStatus.Io, "fleet missing plan status");
            Check(missingPlan.NativeCode == 2, "fleet missing plan native errno");

            var syntaxValidate = Expect<KairosBootException>(() => Fleet.ValidateJobFile(syntax));
            Check(syntaxValidate.Status == KairosBootStatus.InvalidArgument, "fleet syntax status");
            Check(syntaxValidate.NativeCode == 0, "fleet syntax native code");
            Check(
                ContainsOrdinal(syntaxValidate.Message, "kb_fleet_syntax.yaml"),
                "fleet syntax path message");
            Check(ContainsLineColumn(syntaxValidate.Message), "fleet syntax line/column message");
            var syntaxPlan = Expect<KairosBootException>(() => Fleet.PlanJobFile(syntax));
            Check(syntaxPlan.Status == KairosBootStatus.InvalidArgument, "fleet syntax plan status");
            Check(
                ContainsOrdinal(syntaxPlan.Message, "kb_fleet_syntax.yaml"),
                "fleet syntax plan path message");

            var semanticValidate = Expect<KairosBootException>(() => Fleet.ValidateJobFile(semantic));
            Check(
                semanticValidate.Status == KairosBootStatus.InvalidArgument,
                "fleet semantic status");
            Check(
                ContainsOrdinal(semanticValidate.Message, ":3:1:"),
                "fleet semantic line/column");
            Check(
                ContainsOrdinal(semanticValidate.Message, "$.bogusField"),
                "fleet semantic schema path");
            var semanticPlan = Expect<KairosBootException>(() => Fleet.PlanJobFile(semantic));
            Check(
                semanticPlan.Status == KairosBootStatus.InvalidArgument,
                "fleet semantic plan status");
            Check(
                ContainsOrdinal(semanticPlan.Message, "$.bogusField"),
                "fleet semantic plan schema path");

            CheckFleetGoldenPlan();
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    private static void CheckFleetGoldenPlan()
    {
        var sourceRoot = FindTestSourceRoot();
        var fixture = Path.Combine(sourceRoot, "tests", "contracts", "fleet-job-v1.fixture.yaml");
        var goldenPath = Path.Combine(sourceRoot, "tests", "contracts", "job-plan-v1.golden.json");
        var golden = File.ReadAllBytes(goldenPath);
        Check(golden.Length > 1, "fleet golden size");
        Check(golden[golden.Length - 1] == (byte)'\n', "fleet golden trailing LF");
        Check(golden[golden.Length - 2] != (byte)'\n', "fleet golden single trailing LF");
        var expectedJson = Encoding.UTF8.GetString(golden, 0, golden.Length - 1);

        using (var plan = Fleet.PlanJobFile(fixture))
        {
            var json = plan.CanonicalJson;
            Check(json == expectedJson, "fleet golden canonical JSON");
            Check(
                plan.Sha256Hex == Sha256HexOf(expectedJson),
                "fleet golden digest matches golden bytes");
            Check(
                plan.Sha256Hex ==
                    "992daa21b5ea246910fc5d9ffffafed3e36e883d6a407b70abe3b04def3823f4",
                "fleet golden frozen digest");
            Check(
                ContainsOrdinal(
                    json,
                    "58539b1d8a0ba3108ffd0f0ea835d25efca9a6ce85b06cd15f0f1307d4b1c9ef"),
                "fleet manifest provenance digest");
        }
    }

    private static string FindTestSourceRoot()
    {
        var configured = Environment.GetEnvironmentVariable("KAIROSBOOT_TEST_SOURCE_DIR");
        if (!string.IsNullOrEmpty(configured) &&
            File.Exists(Path.Combine(configured, "tests", "contracts", "job-plan-v1.golden.json")))
        {
            return configured;
        }

        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory != null;
             directory = directory.Parent)
        {
            if (File.Exists(
                    Path.Combine(directory.FullName, "tests", "contracts", "job-plan-v1.golden.json")))
            {
                return directory.FullName;
            }
        }

        throw new InvalidOperationException(
            "KairosBoot contract fixtures were not found above " + AppContext.BaseDirectory +
                "; set KAIROSBOOT_TEST_SOURCE_DIR to the repository root.");
    }

    private static void WriteUtf8(string path, string text)
    {
        File.WriteAllText(path, text, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    private static bool ContainsOrdinal(string text, string fragment)
    {
        return text.IndexOf(fragment, StringComparison.Ordinal) >= 0;
    }

    private static bool IsLowercaseHex(string text)
    {
        foreach (var character in text)
        {
            if ((character < '0' || character > '9') && (character < 'a' || character > 'f'))
            {
                return false;
            }
        }

        return true;
    }

    private static bool ContainsLineColumn(string text)
    {
        for (var i = 0; i + 1 < text.Length; i++)
        {
            if (text[i] != ':')
            {
                continue;
            }

            var lineDigits = 0;
            while (i + 1 + lineDigits < text.Length &&
                   text[i + 1 + lineDigits] >= '0' && text[i + 1 + lineDigits] <= '9')
            {
                lineDigits++;
            }

            if (lineDigits == 0)
            {
                continue;
            }

            var columnStart = i + 1 + lineDigits;
            if (columnStart >= text.Length || text[columnStart] != ':')
            {
                continue;
            }

            var columnDigits = 0;
            while (columnStart + 1 + columnDigits < text.Length &&
                   text[columnStart + 1 + columnDigits] >= '0' &&
                   text[columnStart + 1 + columnDigits] <= '9')
            {
                columnDigits++;
            }

            if (columnDigits == 0)
            {
                continue;
            }

            var after = columnStart + 1 + columnDigits;
            if (after < text.Length && text[after] == ':')
            {
                return true;
            }
        }

        return false;
    }

    private static string Sha256HexOf(string value)
    {
        using (var sha256 = SHA256.Create())
        {
            var digest = sha256.ComputeHash(Encoding.UTF8.GetBytes(value));
            var builder = new StringBuilder(digest.Length * 2);
            foreach (var digit in digest)
            {
                builder.Append(digit.ToString("x2"));
            }

            return builder.ToString();
        }
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

            var rawException = await ExpectAsync<KairosBootException>(
                () => context.FlashRawAsync(
                    "boot",
                    "missing-内核.bin",
                    deviceSelector: "tcp:127.0.0.1:5554")).ConfigureAwait(false);
            Check(rawException.Status == KairosBootStatus.Io, "flash:raw file status");
            Check(rawException.TransferState == TransferState.NotSent, "flash:raw transfer state");
            Check(
                rawException.DeviceIdentifier == "tcp:127.0.0.1:5554",
                "flash:raw selector");
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
            await ExpectAsync<OperationCanceledException>(
                () => context.FlashRawAsync("boot", "kernel", cancellationToken: source.Token))
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

    private static async Task CheckTypedManagedPreflight()
    {
        using (var context = Context.Create())
        {
            Expect<ArgumentException>(() => _ = context.StageAsync(Array.Empty<byte>()));
            Expect<ArgumentException>(
                () => _ = context.CreateLogicalPartitionAsync("system\0other", 0));
            await ExpectAsync<ArgumentException>(
                () => context.FlashRawAsync(
                    "boot",
                    "kernel",
                    secondStagePath: "second")).ConfigureAwait(false);
            await ExpectAsync<ArgumentException>(
                () => context.FlashRawAsync(
                    "boot",
                    "kernel",
                    ramdiskPath: string.Empty)).ConfigureAwait(false);
        }
    }

#if NET10_0_OR_GREATER
    private static async Task CheckScriptedTcpParity()
    {
        using (var server = new ScriptedTcpDevice())
        using (var context = Context.Create())
        {
            var options = new CommandOptions(TimeSpan.FromSeconds(5), 1024);
            var getvar = await context.GetVarAsync(
                "product",
                server.Selector,
                options).ConfigureAwait(false);

            Check(
                getvar.TerminalPayload.SequenceEqual(new byte[]
                {
                    (byte)'p', (byte)'r', (byte)'o', (byte)'d', (byte)'u',
                    (byte)'c', (byte)'t', (byte)'_', (byte)'a', 0, 0xff,
                }),
                "scripted TCP binary OKAY");
            Check(getvar.Messages.Count == 2, "scripted TCP message count");
            Check(getvar.Messages[0].Kind == CommandMessageKind.Info, "scripted TCP INFO kind");
            Check(
                getvar.Messages[0].Payload.SequenceEqual(new byte[]
                {
                    (byte)'o', (byte)'n', (byte)'e', 0,
                    (byte)'t', (byte)'w', (byte)'o',
                }),
                "scripted TCP binary INFO");
            Check(getvar.Messages[1].Kind == CommandMessageKind.Text, "scripted TCP TEXT kind");
            Check(
                getvar.Messages[1].Payload.SequenceEqual(new byte[]
                {
                    (byte)'h', (byte)'u', (byte)'m', (byte)'a', (byte)'n', 0,
                    (byte)'t', (byte)'e', (byte)'x', (byte)'t', 0xff,
                }),
                "scripted TCP binary TEXT");
            Check(getvar.Data.Length == 0, "scripted TCP getvar has no DATA");
            Check(
                getvar.DeviceIdentifier == server.Selector,
                "scripted TCP success selector passthrough");

            var deviceFail = await ExpectAsync<KairosBootException>(
                () => context.EraseAsync(
                    "userdata",
                    server.Selector,
                    options)).ConfigureAwait(false);
            Check(deviceFail.Status == KairosBootStatus.DeviceFail, "scripted TCP FAIL status");
            Check(
                deviceFail.DeviceMessage.SequenceEqual(
                    System.Text.Encoding.ASCII.GetBytes("partition locked")),
                "scripted TCP FAIL device message");
            Check(deviceFail.CommandMessages.Count == 1, "scripted TCP FAIL INFO count");
            Check(
                deviceFail.CommandMessages[0].Kind == CommandMessageKind.Info,
                "scripted TCP FAIL INFO kind");
            Check(
                deviceFail.CommandMessages[0].Payload.SequenceEqual(
                    System.Text.Encoding.ASCII.GetBytes("warning")),
                "scripted TCP FAIL INFO payload");
            Check(!deviceFail.InboundExpectedBytes.HasValue, "scripted TCP FAIL inbound unset");
            Check(deviceFail.InboundTransferredBytes == 0, "scripted TCP FAIL inbound bytes");
            Check(
                deviceFail.InboundTransferState == TransferState.NotSent,
                "scripted TCP FAIL inbound certainty");
            Check(
                deviceFail.TransferState == TransferState.FullyTransferred,
                "scripted TCP FAIL outbound certainty");
            Check(!deviceFail.SessionPoisoned, "scripted TCP FAIL session reusable");
            Check(
                deviceFail.DeviceIdentifier == server.Selector,
                "scripted TCP FAIL selector passthrough");

            var disconnected = await ExpectAsync<KairosBootException>(
                () => context.UploadAsync(
                    server.Selector,
                    options)).ConfigureAwait(false);
            Check(disconnected.Status == KairosBootStatus.NoDevice, "scripted TCP disconnect status");
            Check(disconnected.InboundExpectedBytes == 5, "scripted TCP disconnect expected bytes");
            Check(disconnected.InboundTransferredBytes == 2, "scripted TCP disconnect committed bytes");
            Check(
                disconnected.InboundTransferState == TransferState.PartialOrUnknown,
                "scripted TCP disconnect inbound certainty");
            Check(
                disconnected.TransferState == TransferState.FullyTransferred,
                "scripted TCP disconnect outbound certainty");
            Check(disconnected.SessionPoisoned, "scripted TCP disconnect poisons session");
            Check(
                disconnected.DeviceIdentifier == server.Selector,
                "scripted TCP disconnect selector passthrough");

            await CheckManagementNativeValidation(context, server, options)
                .ConfigureAwait(false);
            await CheckManagementTrace(context, server, options)
                .ConfigureAwait(false);
            await CheckManagementFail(context, server, options)
                .ConfigureAwait(false);
            await CheckManagementCancellation(context, server, options)
                .ConfigureAwait(false);

            server.Finish();
            Check(server.HandshakeCount == 18, "scripted TCP handshake count");
            Check(
                server.Commands.SequenceEqual(new[]
                {
                    "getvar:product",
                    "erase:userdata",
                    "upload",
                    "flashing lock",
                    "flashing unlock",
                    "flashing lock_critical",
                    "flashing unlock_critical",
                    "flashing get_unlock_ability",
                    "gsi:wipe",
                    "gsi:disable",
                    "gsi:status",
                    "snapshot-update:cancel",
                    "snapshot-update:merge",
                    "create-logical-partition:system_ext:0",
                    "delete-logical-partition:system_ext",
                    "resize-logical-partition:system_ext:18446744073709551615",
                    "gsi:status",
                    "snapshot-update:merge",
                }),
                "scripted TCP normalized command trace");
        }
    }

    private static async Task CheckManagementNativeValidation(
        Context context,
        ScriptedTcpDevice server,
        CommandOptions options)
    {
        var invalidFlashing = await ExpectAsync<KairosBootException>(
            () => context.FlashingAsync(
                (FlashingCommand)int.MaxValue,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(invalidFlashing.Status == KairosBootStatus.InvalidArgument, "native flashing enum validation");
        Check(invalidFlashing.DeviceIdentifier == server.Selector, "invalid flashing selector");

        var invalidGsi = await ExpectAsync<KairosBootException>(
            () => context.GsiAsync(
                (GsiCommand)int.MaxValue,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(invalidGsi.Status == KairosBootStatus.InvalidArgument, "native GSI enum validation");

        var invalidSnapshot = await ExpectAsync<KairosBootException>(
            () => context.SnapshotUpdateAsync(
                (SnapshotUpdateCommand)int.MaxValue,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(invalidSnapshot.Status == KairosBootStatus.InvalidArgument, "native snapshot enum validation");

        var emptyName = await ExpectAsync<KairosBootException>(
            () => context.CreateLogicalPartitionAsync(
                string.Empty,
                0,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(emptyName.Status == KairosBootStatus.InvalidArgument, "native empty logical name validation");

        var nullName = await ExpectAsync<KairosBootException>(
            () => context.DeleteLogicalPartitionAsync(
                null!,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(nullName.Status == KairosBootStatus.InvalidArgument, "native null logical name validation");

        var injectedName = await ExpectAsync<KairosBootException>(
            () => context.DeleteLogicalPartitionAsync(
                "system:other",
                server.Selector,
                options)).ConfigureAwait(false);
        Check(injectedName.Status == KairosBootStatus.InvalidArgument, "native logical name injection validation");

        var controlName = await ExpectAsync<KairosBootException>(
            () => context.ResizeLogicalPartitionAsync(
                "bad\nname",
                1,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(controlName.Status == KairosBootStatus.InvalidArgument, "native logical control validation");

        var overlongName = await ExpectAsync<KairosBootException>(
            () => context.CreateLogicalPartitionAsync(
                new string('x', 4096),
                ulong.MaxValue,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(overlongName.Status == KairosBootStatus.InvalidArgument, "native logical length validation");
        Check(server.HandshakeCount == 3, "invalid management calls do not reach transport");
    }

    private static async Task CheckManagementTrace(
        Context context,
        ScriptedTcpDevice server,
        CommandOptions options)
    {
        var first = await context.FlashingAsync(
            FlashingCommand.Lock,
            server.Selector,
            options).ConfigureAwait(false);
        Check(first.TerminalPayload.SequenceEqual(new byte[] { (byte)'m', 0, 0xfd }), "management binary OKAY");
        Check(first.Messages.Count == 2, "management binary message count");
        Check(first.Messages[0].Kind == CommandMessageKind.Info, "management binary INFO kind");
        Check(first.Messages[0].Payload.SequenceEqual(new byte[] { (byte)'i', 0, 0xff }), "management binary INFO");
        Check(first.Messages[1].Kind == CommandMessageKind.Text, "management binary TEXT kind");
        Check(first.Messages[1].Payload.SequenceEqual(new byte[] { (byte)'t', 0, 0xfe }), "management binary TEXT");
        Check(first.DeviceIdentifier == server.Selector, "management selector passthrough");

        await CheckManagementSuccess(
            context.FlashingAsync(FlashingCommand.Unlock, server.Selector, options),
            server.Selector,
            "flashing unlock").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.FlashingAsync(FlashingCommand.LockCritical, server.Selector, options),
            server.Selector,
            "flashing lock critical").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.FlashingAsync(FlashingCommand.UnlockCritical, server.Selector, options),
            server.Selector,
            "flashing unlock critical").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.FlashingAsync(FlashingCommand.GetUnlockAbility, server.Selector, options),
            server.Selector,
            "flashing unlock ability").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.GsiAsync(GsiCommand.Wipe, server.Selector, options),
            server.Selector,
            "GSI wipe").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.GsiAsync(GsiCommand.Disable, server.Selector, options),
            server.Selector,
            "GSI disable").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.GsiAsync(GsiCommand.Status, server.Selector, options),
            server.Selector,
            "GSI status").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.SnapshotUpdateAsync(SnapshotUpdateCommand.Cancel, server.Selector, options),
            server.Selector,
            "snapshot cancel").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.SnapshotUpdateAsync(SnapshotUpdateCommand.Merge, server.Selector, options),
            server.Selector,
            "snapshot merge").ConfigureAwait(false);

        string? temporaryName = new string("system_ext".ToCharArray());
        var create = context.CreateLogicalPartitionAsync(
            temporaryName,
            0,
            server.Selector,
            options);
        temporaryName = null;
        GC.Collect();
        GC.WaitForPendingFinalizers();
        await CheckManagementSuccess(create, server.Selector, "create logical lifetime")
            .ConfigureAwait(false);
        await CheckManagementSuccess(
            context.DeleteLogicalPartitionAsync("system_ext", server.Selector, options),
            server.Selector,
            "delete logical").ConfigureAwait(false);
        await CheckManagementSuccess(
            context.ResizeLogicalPartitionAsync(
                "system_ext",
                ulong.MaxValue,
                server.Selector,
                options),
            server.Selector,
            "resize logical UInt64").ConfigureAwait(false);
    }

    private static async Task CheckManagementSuccess(
        Task<CommandResult> pending,
        string selector,
        string name)
    {
        var result = await pending.ConfigureAwait(false);
        Check(
            result.TerminalPayload.SequenceEqual(System.Text.Encoding.ASCII.GetBytes("done")),
            $"{name} terminal result");
        Check(result.DeviceIdentifier == selector, $"{name} selector");
    }

    private static async Task CheckManagementFail(
        Context context,
        ScriptedTcpDevice server,
        CommandOptions options)
    {
        var failure = await ExpectAsync<KairosBootException>(
            () => context.GsiAsync(
                GsiCommand.Status,
                server.Selector,
                options)).ConfigureAwait(false);
        Check(failure.Status == KairosBootStatus.DeviceFail, "management FAIL status");
        Check(failure.DeviceMessage.SequenceEqual(new byte[] { (byte)'e', 0, 0xfa }), "management binary FAIL");
        Check(failure.CommandMessages.Count == 2, "management FAIL message count");
        Check(failure.CommandMessages[0].Kind == CommandMessageKind.Info, "management FAIL INFO kind");
        Check(failure.CommandMessages[0].Payload.SequenceEqual(new byte[] { (byte)'w', 0, 0xfc }), "management FAIL INFO");
        Check(failure.CommandMessages[1].Kind == CommandMessageKind.Text, "management FAIL TEXT kind");
        Check(failure.CommandMessages[1].Payload.SequenceEqual(new byte[] { (byte)'h', 0, 0xfb }), "management FAIL TEXT");
        Check(failure.TransferState == TransferState.FullyTransferred, "management FAIL outbound certainty");
        Check(!failure.SessionPoisoned, "management FAIL session reusable");
        Check(failure.DeviceIdentifier == server.Selector, "management FAIL selector");
    }

    private static async Task CheckManagementCancellation(
        Context context,
        ScriptedTcpDevice server,
        CommandOptions options)
    {
        using (var cancellation = new CancellationTokenSource())
        {
            var pending = context.SnapshotUpdateAsync(
                SnapshotUpdateCommand.Merge,
                server.Selector,
                options,
                cancellationToken: cancellation.Token);
            Check(
                server.WaitForCancellationCommand(TimeSpan.FromSeconds(5)),
                "management cancellation command observed");
            cancellation.Cancel();
            await ExpectAsync<OperationCanceledException>(() => pending)
                .ConfigureAwait(false);
            Check(
                server.WaitForCancellationDrain(TimeSpan.FromSeconds(2)),
                "management cancellation drained connection");
        }
    }
#endif

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
        private readonly byte[] outputPath =
            System.Text.Encoding.UTF8.GetBytes("结果.bin\0");
        private readonly byte[] device =
            System.Text.Encoding.UTF8.GetBytes("设备-一\0");
        private readonly GCHandle terminalHandle;
        private readonly GCHandle infoHandle;
        private readonly GCHandle textHandle;
        private readonly GCHandle dataHandle;
        private readonly GCHandle outputPathHandle;
        private readonly GCHandle deviceHandle;

        internal FakeCommandResultSource()
        {
            terminalHandle = GCHandle.Alloc(terminal, GCHandleType.Pinned);
            infoHandle = GCHandle.Alloc(info, GCHandleType.Pinned);
            textHandle = GCHandle.Alloc(text, GCHandleType.Pinned);
            dataHandle = GCHandle.Alloc(data, GCHandleType.Pinned);
            outputPathHandle = GCHandle.Alloc(outputPath, GCHandleType.Pinned);
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

        public IntPtr OutputPath() => outputPathHandle.AddrOfPinnedObject();

        public ulong ReceivedBytes() => (ulong)data.LongLength;

        public IntPtr DeviceIdentifier() => deviceHandle.AddrOfPinnedObject();

        internal void Overwrite()
        {
            Overwrite(terminal);
            Overwrite(info);
            Overwrite(text);
            Overwrite(data);
            Overwrite(outputPath);
            Overwrite(device);
        }

        public void Dispose()
        {
            terminalHandle.Free();
            infoHandle.Free();
            textHandle.Free();
            dataHandle.Free();
            outputPathHandle.Free();
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

    private sealed class InlineProgress<T> : IProgress<T>
    {
        private readonly Action<T> report;

        internal InlineProgress(Action<T> report)
        {
            this.report = report;
        }

        public void Report(T value)
        {
            report(value);
        }
    }

    private static class ScriptedUpdateNativeMethods
    {
        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_reset", CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Reset();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_failure_code", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int FailureCode();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_options_init_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int OptionsInitCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_async_start_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int AsyncStartCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_blocking_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int BlockingCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_flash_options_init_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int FlashOptionsInitCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_boot_async_start_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int BootAsyncStartCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_boot_blocking_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int BootBlockingCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_flash_raw_async_start_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int FlashRawAsyncStartCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_flash_raw_blocking_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int FlashRawBlockingCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_cancel_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int CancelCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_operation_release_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int OperationReleaseCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_context_release_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int ContextReleaseCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_job_options_init_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int JobOptionsInitCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_job_async_start_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int JobAsyncStartCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_job_blocking_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int JobBlockingCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_job_cancel_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int JobCancelCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_job_release_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int JobReleaseCount();

        [DllImport(NativeMethods.LibraryName, EntryPoint = "kb_test_job_report_release_count", CallingConvention = CallingConvention.Cdecl)]
        internal static extern int JobReportReleaseCount();
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
