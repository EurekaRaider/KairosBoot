#if NET10_0_OR_GREATER
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

internal sealed class ScriptedTcpDevice : IDisposable
{
    private const int MaximumFrameBytes = 4 * 1024 * 1024;
    private static readonly byte[] Handshake = Encoding.ASCII.GetBytes("FB01");

    private readonly TcpListener listener;
    private readonly Thread worker;
    private readonly object commandsGate = new object();
    private readonly List<string> commands = new List<string>();
    private readonly ManualResetEventSlim cancellationCommandReceived =
        new ManualResetEventSlim(false);
    private readonly ManualResetEventSlim cancellationConnectionDrained =
        new ManualResetEventSlim(false);
    private Exception? failure;
    private int handshakeCount;
    private bool finished;
    private bool disposed;

    internal ScriptedTcpDevice()
    {
        listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        Selector = $"tcp:127.0.0.1:{endpoint.Port}";
        worker = new Thread(Run)
        {
            IsBackground = true,
            Name = "KairosBoot scripted Fastboot TCP device",
        };
        worker.Start();
    }

    internal string Selector { get; }

    internal int HandshakeCount => Volatile.Read(ref handshakeCount);

    internal IReadOnlyList<string> Commands
    {
        get
        {
            lock (commandsGate)
            {
                return commands.ToArray();
            }
        }
    }

    internal bool WaitForCancellationCommand(TimeSpan timeout)
    {
        return cancellationCommandReceived.Wait(timeout);
    }

    internal bool WaitForCancellationDrain(TimeSpan timeout)
    {
        return cancellationConnectionDrained.Wait(timeout);
    }

    internal void Finish()
    {
        if (finished)
        {
            ThrowFailure();
            return;
        }

        if (!worker.Join(TimeSpan.FromSeconds(10)))
        {
            listener.Stop();
            throw new TimeoutException("Scripted Fastboot TCP device did not finish.");
        }

        finished = true;
        ThrowFailure();
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        listener.Stop();
        var workerStopped = finished || worker.Join(TimeSpan.FromSeconds(6));
        if (workerStopped)
        {
            cancellationCommandReceived.Dispose();
            cancellationConnectionDrained.Dispose();
        }
    }

    private void Run()
    {
        try
        {
            ServeGetVarSuccess();
            ServeDeviceFail();
            ServePartialUploadDisconnect();
            ServeManagementCommands();
            ServeManagementFail();
            ServeCancellationDrain();
        }
        catch (Exception exception)
        {
            failure = exception;
        }
        finally
        {
            listener.Stop();
        }
    }

    private void ServeGetVarSuccess()
    {
        using (var client = Accept())
        using (var stream = client.GetStream())
        {
            ExpectCommand(stream, "getvar:product");
            WriteResponse(stream, "INFO", new byte[]
            {
                (byte)'o', (byte)'n', (byte)'e', 0, (byte)'t', (byte)'w', (byte)'o',
            });
            WriteResponse(stream, "TEXT", new byte[]
            {
                (byte)'h', (byte)'u', (byte)'m', (byte)'a', (byte)'n', 0,
                (byte)'t', (byte)'e', (byte)'x', (byte)'t', 0xff,
            });
            WriteResponse(stream, "OKAY", new byte[]
            {
                (byte)'p', (byte)'r', (byte)'o', (byte)'d', (byte)'u', (byte)'c',
                (byte)'t', (byte)'_', (byte)'a', 0, 0xff,
            });
        }
    }

    private void ServeDeviceFail()
    {
        using (var client = Accept())
        using (var stream = client.GetStream())
        {
            ExpectCommand(stream, "getvar:has-slot:userdata");
            WriteResponse(stream, "OKAY", Encoding.ASCII.GetBytes("no"));
            ExpectCommand(stream, "getvar:partition-type:userdata");
            WriteResponse(stream, "OKAY", Encoding.ASCII.GetBytes("raw"));
            ExpectCommand(stream, "erase:userdata");
            WriteResponse(stream, "INFO", Encoding.ASCII.GetBytes("warning"));
            WriteResponse(stream, "FAIL", Encoding.ASCII.GetBytes("partition locked"));
        }
    }

    private void ServePartialUploadDisconnect()
    {
        using (var client = Accept())
        using (var stream = client.GetStream())
        {
            ExpectCommand(stream, "upload");
            WriteFrame(stream, Encoding.ASCII.GetBytes("DATA00000005"));
            WriteFrame(stream, new byte[] { (byte)'a', 0 });
        }
    }

    private void ServeManagementCommands()
    {
        var expected = new[]
        {
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
        };

        for (var index = 0; index < expected.Length; index++)
        {
            using (var client = Accept())
            using (var stream = client.GetStream())
            {
                ExpectCommand(stream, expected[index]);
                if (index == 0)
                {
                    WriteResponse(stream, "INFO", new byte[] { (byte)'i', 0, 0xff });
                    WriteResponse(stream, "TEXT", new byte[] { (byte)'t', 0, 0xfe });
                    WriteResponse(stream, "OKAY", new byte[] { (byte)'m', 0, 0xfd });
                }
                else
                {
                    WriteResponse(stream, "OKAY", Encoding.ASCII.GetBytes("done"));
                }
            }
        }
    }

    private void ServeManagementFail()
    {
        using (var client = Accept())
        using (var stream = client.GetStream())
        {
            ExpectCommand(stream, "gsi:status");
            WriteResponse(stream, "INFO", new byte[] { (byte)'w', 0, 0xfc });
            WriteResponse(stream, "TEXT", new byte[] { (byte)'h', 0, 0xfb });
            WriteResponse(stream, "FAIL", new byte[] { (byte)'e', 0, 0xfa });
        }
    }

    private void ServeCancellationDrain()
    {
        using (var client = Accept())
        using (var stream = client.GetStream())
        {
            ExpectCommand(stream, "snapshot-update:merge");
            cancellationCommandReceived.Set();
            try
            {
                if (stream.ReadByte() >= 0)
                {
                    throw new InvalidDataException(
                        "Cancelled Fastboot TCP operation sent unexpected bytes.");
                }
            }
            catch (IOException exception)
            {
                if (exception.InnerException is SocketException socketException &&
                    IsReceiveTimeout(socketException))
                {
                    throw;
                }

                // Socket cancellation may surface as EOF, reset, or abort.
            }
            catch (SocketException exception)
            {
                if (IsReceiveTimeout(exception))
                {
                    throw;
                }

                // Socket cancellation may surface as EOF, reset, or abort.
            }

            cancellationConnectionDrained.Set();
        }
    }

    private TcpClient Accept()
    {
        var client = listener.AcceptTcpClient();
        client.NoDelay = true;
        client.ReceiveTimeout = 5000;
        var stream = client.GetStream();
        var received = ReadExactly(stream, Handshake.Length);
        if (!received.SequenceEqual(Handshake))
        {
            client.Dispose();
            throw new InvalidDataException("Fastboot TCP handshake was not FB01.");
        }

        stream.Write(Handshake, 0, Handshake.Length);
        Interlocked.Increment(ref handshakeCount);
        return client;
    }

    private void ExpectCommand(NetworkStream stream, string expected)
    {
        var payload = ReadFrame(stream);
        var actual = Encoding.UTF8.GetString(payload);
        lock (commandsGate)
        {
            commands.Add(actual);
        }

        if (!string.Equals(actual, expected, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Expected Fastboot command '{expected}', received '{actual}'.");
        }
    }

    private static byte[] ReadFrame(NetworkStream stream)
    {
        var header = ReadExactly(stream, 8);
        ulong length = 0;
        foreach (var value in header)
        {
            length = (length << 8) | value;
        }

        if (length > MaximumFrameBytes || length > int.MaxValue)
        {
            throw new InvalidDataException(
                $"Fastboot TCP frame length {length} exceeds the scripted limit.");
        }

        return ReadExactly(stream, (int)length);
    }

    private static void WriteResponse(NetworkStream stream, string kind, byte[] payload)
    {
        var prefix = Encoding.ASCII.GetBytes(kind);
        var response = new byte[prefix.Length + payload.Length];
        Buffer.BlockCopy(prefix, 0, response, 0, prefix.Length);
        Buffer.BlockCopy(payload, 0, response, prefix.Length, payload.Length);
        WriteFrame(stream, response);
    }

    private static void WriteFrame(NetworkStream stream, byte[] payload)
    {
        var header = new byte[8];
        var length = (ulong)payload.LongLength;
        for (var index = header.Length - 1; index >= 0; index--)
        {
            header[index] = (byte)(length & 0xff);
            length >>= 8;
        }

        stream.Write(header, 0, header.Length);
        stream.Write(payload, 0, payload.Length);
        stream.Flush();
    }

    private static byte[] ReadExactly(Stream stream, int size)
    {
        var result = new byte[size];
        var offset = 0;
        while (offset < size)
        {
            var read = stream.Read(result, offset, size - offset);
            if (read == 0)
            {
                throw new EndOfStreamException(
                    $"Fastboot TCP stream ended after {offset} of {size} bytes.");
            }

            offset += read;
        }

        return result;
    }

    private static bool IsReceiveTimeout(SocketException exception)
    {
        return exception.SocketErrorCode == SocketError.TimedOut ||
            exception.SocketErrorCode == SocketError.WouldBlock;
    }

    private void ThrowFailure()
    {
        if (failure != null)
        {
            throw new InvalidOperationException(
                "Scripted Fastboot TCP device failed.",
                failure);
        }
    }
}
#endif
