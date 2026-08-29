using System;
using System.Threading;
using System.Threading.Tasks;

namespace KairosBoot.Interop;

internal interface IOperationPollTarget
{
    int Poll();

    void RequestCancel();

    Exception CreateFailure(int status);
}

internal static class OperationPollingEngine
{
    internal static async Task WaitAsync(
        IOperationPollTarget target,
        CancellationToken cancellationToken,
        int pollDelayMilliseconds = NativeMethods.PollDelayMilliseconds)
    {
        if (target == null)
        {
            throw new ArgumentNullException(nameof(target));
        }

        if (pollDelayMilliseconds < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(pollDelayMilliseconds));
        }

        using (cancellationToken.Register(RequestCancel, target))
        {
            while (true)
            {
                var status = target.Poll();
                if (status == (int)KairosBootStatus.Timeout)
                {
                    await Task.Delay(pollDelayMilliseconds).ConfigureAwait(false);
                    continue;
                }

                if (status == (int)KairosBootStatus.Ok)
                {
                    return;
                }

                if (status == (int)KairosBootStatus.Cancelled &&
                    cancellationToken.IsCancellationRequested)
                {
                    throw new OperationCanceledException(cancellationToken);
                }

                throw target.CreateFailure(status);
            }
        }
    }

    private static void RequestCancel(object? state)
    {
        // Cancellation callbacks must not tear down the native operation. The
        // owning async method keeps polling until native transport/callback
        // drain reaches a terminal state, then releases the SafeHandle.
        try
        {
            ((IOperationPollTarget)state!).RequestCancel();
        }
        catch
        {
            // A failure to request cancellation is reported by the terminal
            // native status; exceptions cannot safely escape token callbacks.
        }
    }
}

internal sealed class NativeOperationPollTarget : IOperationPollTarget
{
    private readonly OperationSafeHandle operation;

    internal NativeOperationPollTarget(OperationSafeHandle operation)
    {
        this.operation = operation;
    }

    public int Poll()
    {
        return NativeMethods.OperationWait(operation, NativeMethods.PollTimeoutMilliseconds);
    }

    public void RequestCancel()
    {
        _ = NativeMethods.OperationCancel(operation);
    }

    public Exception CreateFailure(int status)
    {
        return NativeError.FromBorrowed(status, NativeMethods.OperationError(operation));
    }
}
