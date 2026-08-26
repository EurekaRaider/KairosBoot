using System;
using System.Reflection;
using KairosBoot;

internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("expected the KairosBoot release version as one argument");
            return 2;
        }

        var managed = typeof(Context).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;
        if (managed == null ||
            !(string.Equals(managed, args[0], StringComparison.Ordinal) ||
              managed.StartsWith(args[0] + "+", StringComparison.Ordinal)))
        {
            Console.Error.WriteLine(
                $"managed version mismatch: expected {args[0]}, got {managed ?? "<missing>"}");
            return 1;
        }

        var actual = Context.Version.Value;
        if (!string.Equals(actual, args[0], StringComparison.Ordinal))
        {
            Console.Error.WriteLine($"native version mismatch: expected {args[0]}, got {actual}");
            return 1;
        }

        Console.WriteLine($"KairosBoot package smoke passed: managed={managed}, native={actual}");
        return 0;
    }
}
