using System;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using KairosBoot;

internal static class Program
{
    private static int checks;

    private static async Task<int> Main()
    {
        try
        {
            CheckVersion();
            CheckDevicesFailAccurately();
            await CheckFlashFailsAccurately().ConfigureAwait(false);
            await CheckPreCancellation().ConfigureAwait(false);
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

    private static void CheckDevicesFailAccurately()
    {
        using (var context = Context.Create())
        {
            var exception = Expect<KairosBootException>(() => { _ = context.Devices; });
            Check(exception.Status == KairosBootStatus.NotSupported, "device status");
            Check(exception.Message.IndexOf("transport", StringComparison.Ordinal) >= 0, "device message");
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
                    "设备-一",
                    progress,
                    CancellationToken.None)).ConfigureAwait(false);

            Check(exception.Status == KairosBootStatus.NotSupported, "flash status");
            Check(exception.TransferState == TransferState.NotSent, "transfer state");
            Check(exception.DeviceIdentifier == "设备-一", "UTF-8 serial");
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

    private static void CheckDisposedContext()
    {
        var context = Context.Create();
        context.Dispose();
        Expect<ObjectDisposedException>(() => { _ = context.Devices; });
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
