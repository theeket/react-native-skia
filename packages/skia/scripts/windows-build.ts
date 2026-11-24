import path from "path";

import { runSync } from "./utils";

export type WindowsBuildEnvironment = {
  // Compiler PATH, INCLUDE, LIB, and SDK settings inherited by build subprocesses.
  env: NodeJS.ProcessEnv;
  // Keep GN on the same native host, Visual Studio toolset, and SDK as the shell.
  gnArgs: string;
  // Target-specific cl.exe for Dawn, including builds with existing CMake caches.
  compiler: string;
};

/**
 * Initialize each Windows target in a child shell, leaving the caller unchanged.
 * Call only for a selected target; importing target catalogs must not initialize VS.
 */
export const getWindowsBuildEnvironment = (
  architecture: string,
  baseEnv: NodeJS.ProcessEnv = process.env
): WindowsBuildEnvironment => {
  if (process.platform !== "win32") {
    throw new Error("Windows builds require a Windows host");
  }
  if (architecture !== "x64" && architecture !== "arm64") {
    throw new Error(`Unsupported Windows architecture: ${architecture}`);
  }
  // Distinguish the JSON payload from any Visual Studio setup banners.
  const marker = "SKIA_WINDOWS_ENV=";
  // Machine-scoped architecture remains native even when Node/PowerShell is emulated.
  // Query the Installer instead of hardcoding a Visual Studio version or edition.
  // Require tools for this target rather than selecting a VS installation without C++.
  // -HostArch selects the compiler executable; -Arch selects the generated object code.
  // Native ARM64 tools can produce x64 objects without running the compiler under emulation.
  // Export the initialized child environment; leave the calling terminal unchanged.
  const script = `
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$nativeCpu = [Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE', 'Machine')
if ($nativeCpu -eq 'ARM64') { $hostCpu = 'arm64' }
elseif ($nativeCpu -eq 'AMD64') { $hostCpu = 'x64' }
else { throw "Unsupported Windows host architecture: $nativeCpu" }
$vswhere = Join-Path \${env:ProgramFiles(x86)} 'Microsoft Visual Studio\\Installer\\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) is required' }
$component = 'Microsoft.VisualStudio.Component.VC.Tools.${
    architecture === "arm64" ? "ARM64" : "x86.x64"
  }'
$installation = & $vswhere -latest -prerelease -products '*' -requires $component -property installationPath
if (!$installation) { throw 'Install Visual Studio Desktop development with C++, ${architecture} build tools, and a Windows SDK' }
$launcher = Join-Path $installation 'Common7\\Tools\\Launch-VsDevShell.ps1'
& $launcher -Arch ${architecture} -HostArch $hostCpu -SkipAutomaticLocation | Out-Null
if ($env:VSCMD_ARG_TGT_ARCH -ne '${architecture}') { throw 'Visual Studio target initialization failed' }
$environment = @{}
Get-ChildItem Env: | ForEach-Object { $environment[$_.Name] = $_.Value }
'${marker}' + (@{ hostCpu = $hostCpu; env = $environment } | ConvertTo-Json -Compress -Depth 3)
`;
  // EncodedCommand requires UTF-16LE and preserves paths with spaces and shell syntax.
  // ExecutionPolicy applies only to this PowerShell child process.
  const output = runSync(
    "powershell.exe",
    [
      "-NoProfile",
      "-NonInteractive",
      "-ExecutionPolicy",
      "Bypass",
      "-EncodedCommand",
      Buffer.from(script, "utf16le").toString("base64"),
    ],
    { env: baseEnv, encoding: "utf8", windowsHide: true }
  );
  const line = output.split(/\r?\n/).find((entry) => entry.startsWith(marker));
  if (!line) {
    throw new Error("Visual Studio did not return a Windows build environment");
  }
  const { hostCpu, env } = JSON.parse(line.slice(marker.length)) as {
    hostCpu: "x64" | "arm64";
    env: NodeJS.ProcessEnv;
  };
  // Quote GN strings, normalize Windows separators, and escape literal dollar signs.
  const gnValue = (name: string) => {
    const value = env[name];
    if (!value) {
      throw new Error(`Visual Studio did not initialize ${name}`);
    }
    return JSON.stringify(value.replace(/\\/g, "/").replace(/\/$/, "")).replace(
      /\$/g,
      "\\$"
    );
  };
  // An emulated x64 GN binary otherwise reports x64 as host_cpu on Windows on Arm.
  // Pin toolset/SDK paths so GN cannot discover a different Visual Studio installation.
  const gnArgs = [
    `host_cpu="${hostCpu}"`,
    `win_vc=${gnValue("VCINSTALLDIR")}`,
    `win_toolchain_version=${gnValue("VCToolsVersion")}`,
    `win_sdk=${gnValue("WindowsSdkDir")}`,
    `win_sdk_version=${gnValue("WindowsSDKVersion")}`,
  ].join(" ");
  if (!env.VCToolsInstallDir) {
    throw new Error("Visual Studio did not initialize VCToolsInstallDir");
  }
  // CMAKE_SYSTEM_PROCESSOR alone neither selects a compiler nor changes its output CPU.
  // Explicit compilers also replace CMake caches left by an emulated toolchain.
  const compiler = path.join(
    env.VCToolsInstallDir,
    "bin",
    `Host${hostCpu}`,
    architecture,
    "cl.exe"
  );
  return { env, gnArgs, compiler };
};
