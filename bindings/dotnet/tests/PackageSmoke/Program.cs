using System;
using KairosBoot;

internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("expected the native KairosBoot version as one argument");
            return 2;
        }

        var actual = Context.Version.Value;
        if (!string.Equals(actual, args[0], StringComparison.Ordinal))
        {
            Console.Error.WriteLine($"native version mismatch: expected {args[0]}, got {actual}");
            return 1;
        }

        Console.WriteLine($"KairosBoot package smoke passed: {actual}");
        return 0;
    }
}
