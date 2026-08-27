#!/usr/bin/env python3
"""Build and run the deterministic managed update-package contract shim."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


HERE = Path(__file__).resolve().parent
PROJECT = HERE / "KairosBoot.ContractTests.csproj"
SOURCE = HERE / "ScriptedUpdateNative.c"


def run(
    command: list[str],
    *,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
) -> None:
    subprocess.run(command, check=True, env=env, cwd=cwd)


def target_path(framework: str) -> Path:
    output_root = HERE / "bin" / "Release" / framework
    candidates = list(output_root.rglob("KairosBoot.ContractTests.dll"))
    if not candidates:
        raise RuntimeError(f"managed test output was not found below {output_root}")
    return max(candidates, key=lambda path: path.stat().st_mtime_ns)


def compile_shim(output_directory: Path) -> Path:
    system = platform.system()
    if system == "Windows":
        compiler = shutil.which("cl")
        if compiler is None:
            raise RuntimeError("cl.exe is required from a Visual Studio developer prompt")
        output = output_directory / "kairosboot_native.dll"
        run(
            [
                compiler,
                "/nologo",
                "/std:c11",
                "/O2",
                "/DNDEBUG",
                "/W4",
                "/WX",
                "/LD",
                str(SOURCE),
                "/link",
                f"/OUT:{output}",
            ],
            cwd=output_directory,
        )
        return output

    compiler = shutil.which(os.environ.get("CC", "cc"))
    if compiler is None:
        raise RuntimeError("a C11 compiler is required (set CC to override cc)")
    if system == "Darwin":
        output = output_directory / "libkairosboot_native.dylib"
        link = ["-dynamiclib"]
    elif system == "Linux":
        output = output_directory / "libkairosboot_native.so"
        link = ["-shared", "-fPIC"]
    else:
        raise RuntimeError(f"unsupported host for scripted native shim: {system}")

    run(
        [
            compiler,
            "-std=c11",
            "-O3",
            "-DNDEBUG",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fvisibility=hidden",
            *link,
            str(SOURCE),
            "-o",
            str(output),
        ]
    )
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--framework",
        action="append",
        choices=("net48", "net10.0"),
        help="framework to run; repeat to test both (default: net10.0)",
    )
    args = parser.parse_args()
    frameworks = args.framework or ["net10.0"]

    for framework in frameworks:
        if framework == "net48" and platform.system() != "Windows":
            raise RuntimeError("net48 execution requires a Windows x64 host")
        run(
            [
                "dotnet",
                "build",
                str(PROJECT),
                "-c",
                "Release",
                "-f",
                framework,
                "-m:1",
                "-nr:false",
            ]
        )
        managed = target_path(framework)
        native = compile_shim(managed.parent)
        env = os.environ.copy()
        env["KAIROSBOOT_UPDATE_SHIM"] = "1"
        if framework == "net48":
            executable = managed.with_suffix(".exe")
            run([str(executable)], env=env)
        else:
            run(["dotnet", str(managed)], env=env)
        print(f"scripted update contract passed: {framework}, {native.name}")

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"update shim test failed: {error}", file=sys.stderr)
        sys.exit(1)
