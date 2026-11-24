import fs from "fs";
import path from "path";

import { run, runSync } from "./utils";

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

// These pairs are the distributable Windows outputs; GN archives remain intermediates.
export const windowsSkiaOutputs = ["skia_combined.dll", "skia_combined.lib"];
export const windowsDawnOutputs = ["webgpu_dawn.dll", "webgpu_dawn.lib"];

/** Reject stale or mixed-architecture DLLs before linking or staging them. */
export const validateWindowsDll = (file: string, architecture: string) => {
  const machine = { x64: 0x8664, arm64: 0xaa64 }[architecture];
  if (!machine) {
    throw new Error(`Unsupported Windows architecture: ${architecture}`);
  }
  const descriptor = fs.openSync(file, "r");
  try {
    const header = Buffer.alloc(64);
    if (
      fs.readSync(descriptor, header, 0, 64, 0) !== 64 ||
      header.toString("ascii", 0, 2) !== "MZ"
    ) {
      throw new Error(`${file} is not a Windows DLL`);
    }
    const pe = Buffer.alloc(24);
    if (
      fs.readSync(descriptor, pe, 0, pe.length, header.readUInt32LE(60)) !==
        pe.length ||
      pe.readUInt32LE(0) !== 0x4550 ||
      !(pe.readUInt16LE(22) & 0x2000)
    ) {
      throw new Error(`${file} is not a Windows DLL`);
    }
    if (pe.readUInt16LE(4) !== machine) {
      throw new Error(`${file} does not match Windows ${architecture}`);
    }
  } finally {
    fs.closeSync(descriptor);
  }
};

/** A static Dawn archive cannot stand in for the DLL's import library. */
export const requireWindowsDawn = (
  directory: string,
  architecture: string,
  runtimeDirectory = directory
) => {
  const instruction = `Run yarn build-dawn windows-${architecture} first`;
  const dll = path.join(runtimeDirectory, windowsDawnOutputs[0]);
  const file = path.join(directory, windowsDawnOutputs[1]);
  for (const output of [dll, file]) {
    if (!fs.existsSync(output)) {
      throw new Error(`${output} is missing. ${instruction}`);
    }
  }
  validateWindowsDll(dll, architecture);
  const descriptor = fs.openSync(file, "r");
  try {
    // The first COFF archive member holds defined symbols, including the DLL descriptor.
    const header = Buffer.alloc(68);
    fs.readSync(descriptor, header, 0, header.length, 0);
    const size = Number(header.toString("ascii", 56, 66).trim());
    if (
      header.toString("ascii", 0, 8) !== "!<arch>\n" ||
      !Number.isSafeInteger(size) ||
      size < 4 ||
      size > 16 * 1024 * 1024
    ) {
      throw new Error(`${file} is not a Dawn import library. ${instruction}`);
    }
    const symbols = Buffer.alloc(size);
    fs.readSync(descriptor, symbols, 0, size, 68);
    if (!symbols.includes(Buffer.from("__IMPORT_DESCRIPTOR_webgpu_dawn\0"))) {
      throw new Error(
        `${file} is a static or incompatible Dawn library. ${instruction}`
      );
    }
  } finally {
    fs.closeSync(descriptor);
  }
};

/** Dawn puts runtime DLLs at the build root and import libraries beside the target. */
export const stageWindowsDawn = (
  buildDir: string,
  destination: string,
  architecture: string
) => {
  const nativeDir = path.join(buildDir, "src", "dawn", "native");
  // Validate both source files before replacing any previously staged binaries.
  requireWindowsDawn(nativeDir, architecture, buildDir);
  fs.mkdirSync(destination, { recursive: true });
  for (const name of windowsDawnOutputs) {
    const source = name.endsWith(".dll") ? buildDir : nativeDir;
    const output = path.join(destination, name);
    fs.copyFileSync(path.join(source, name), output);
    console.log(`Windows ${architecture}: ${output}`);
  }
};

type GnTarget = {
  type: string;
  outputs?: string[];
  deps?: string[];
  libs?: string[];
};

/** Resolve module dependencies from GN rather than maintaining a second codec/ICU list. */
export const windowsSkiaLinkInputs = (
  targets: Record<string, GnTarget>,
  sourceDir: string,
  buildDir: string,
  libraries: string[]
) => {
  const resolve = (file: string) =>
    file.startsWith("//")
      ? path.join(sourceDir, file.slice(2))
      : path.resolve(sourceDir, file);
  const expected = libraries.map((library) => path.join(buildDir, library));
  const roots = Object.values(targets).filter(
    (target) =>
      target.type === "static_library" &&
      target.outputs?.some((output) => expected.includes(resolve(output)))
  );
  for (const library of libraries) {
    if (
      !roots.some((target) =>
        target.outputs?.some((output) => path.basename(output) === library)
      )
    ) {
      throw new Error(`GN did not describe the Windows archive ${library}`);
    }
  }
  const archives = new Set<string>();
  const systemLibraries = new Map<string, string>();
  const visited = new Set<GnTarget>();
  const visit = (target: GnTarget) => {
    if (visited.has(target)) {
      return;
    }
    visited.add(target);
    if (target.type === "static_library") {
      target.outputs
        ?.filter((output) => output.endsWith(".lib"))
        .forEach((output) => archives.add(resolve(output)));
    }
    for (const library of target.libs ?? []) {
      // GN's full Dawn build remains a dependency. This separate DLL link uses only
      // webgpu_dawn.lib, and /MD instead of Dawn's inherited static C++ runtime.
      if (
        library.toLowerCase() !== "libcpmt.lib" &&
        !library.includes("/") &&
        !library.includes("\\")
      ) {
        systemLibraries.set(library.toLowerCase(), library);
      }
    }
    for (const dependency of target.deps ?? []) {
      if (dependency.startsWith("//third_party/dawn:")) {
        continue;
      }
      const child = targets[dependency];
      if (!child) {
        throw new Error(`GN did not describe ${dependency}`);
      }
      visit(child);
    }
  };
  roots.forEach(visit);
  return {
    archives: [...archives],
    systemLibraries: [...systemLibraries.values()],
  };
};

/** Link existing GN objects into one DLL; never embed GN's dawn_combined library. */
export const combineWindowsSkia = async (
  sourceDir: string,
  buildDir: string,
  destination: string,
  dawnDirectory: string,
  architecture: string,
  libraries: string[],
  env: NodeJS.ProcessEnv
) => {
  requireWindowsDawn(dawnDirectory, architecture);
  if (!env.VCToolsInstallDir || !env.VSCMD_ARG_HOST_ARCH) {
    throw new Error(
      "The Windows DLL link requires an initialized Visual Studio environment"
    );
  }
  const compiler = path.join(
    env.VCToolsInstallDir,
    "bin",
    `Host${env.VSCMD_ARG_HOST_ARCH}`,
    architecture,
    "cl.exe"
  );
  const targets = JSON.parse(
    runSync(
      path.join(sourceDir, "bin", "gn.exe"),
      ["desc", buildDir, "*", "--format=json"],
      { cwd: sourceDir, env, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 }
    )
  ) as Record<string, GnTarget>;
  const { archives, systemLibraries } = windowsSkiaLinkInputs(
    targets,
    sourceDir,
    buildDir,
    libraries
  );
  const modules = archives.filter((archive) =>
    libraries.includes(path.basename(archive))
  );
  const dependencies = archives.filter((archive) => !modules.includes(archive));
  const objects = new Set<string>();
  for (const archive of modules) {
    const members = runSync("lib.exe", ["/nologo", "/list", archive], {
      env,
      encoding: "utf8",
      maxBuffer: 16 * 1024 * 1024,
    });
    for (const member of members.split(/\r?\n/).filter((line) => line.trim())) {
      const object = path.resolve(buildDir, member.trim());
      if (!object.endsWith(".obj") || !fs.existsSync(object)) {
        throw new Error(`Cannot reuse GN object ${object} from ${archive}`);
      }
      objects.add(object);
    }
  }
  const combined = path.join(buildDir, "combined");
  fs.mkdirSync(combined, { recursive: true });
  // Bracket arguments preserve spaces and literal dollar signs in CMake paths.
  const cmakeList = (name: string, values: string[]) =>
    `set(${name}\n${values
      .map((value) => {
        const normalized = value.replace(/\\/g, "/");
        let equals = "=";
        while (normalized.includes(`]${equals}]`)) {
          equals += "=";
        }
        return `  [${equals}[${normalized}]${equals}]`;
      })
      .join("\n")}\n)\n`;
  const inputs = path.join(combined, "inputs.cmake");
  fs.writeFileSync(
    inputs,
    cmakeList("SKIA_OBJECTS", [...objects]) +
      cmakeList("SKIA_DEPENDENCIES", dependencies) +
      cmakeList("SKIA_SYSTEM_LIBRARIES", systemLibraries)
  );
  const label = `Windows ${architecture}`;
  await run(
    "cmake",
    [
      "-S",
      path.join(__dirname, "windows"),
      "-B",
      combined,
      "-G",
      "Ninja",
      "-DCMAKE_BUILD_TYPE=Release",
      `-DCMAKE_CXX_COMPILER=${compiler}`,
      `-DSKIA_LINK_INPUTS=${inputs}`,
      `-DDAWN_IMPORT_LIBRARY=${path.join(dawnDirectory, "webgpu_dawn.lib")}`,
    ],
    { env, label }
  );
  await run("cmake", ["--build", combined, "--target", "skia_combined"], {
    env,
    label,
  });
  validateWindowsDll(path.join(combined, windowsSkiaOutputs[0]), architecture);
  // Confirm the runtime dependency survived linking; no second Dawn implementation ships.
  const dependents = runSync(
    "dumpbin.exe",
    ["/nologo", "/dependents", path.join(combined, windowsSkiaOutputs[0])],
    { env, encoding: "utf8" }
  );
  if (!/webgpu_dawn\.dll/i.test(dependents)) {
    throw new Error("skia_combined.dll does not import webgpu_dawn.dll");
  }
  for (const file of windowsSkiaOutputs) {
    fs.accessSync(path.join(combined, file));
  }
  fs.mkdirSync(destination, { recursive: true });
  for (const file of windowsSkiaOutputs) {
    fs.copyFileSync(path.join(combined, file), path.join(destination, file));
  }
  // Remove only this architecture's previously staged static archives, after success.
  for (const file of libraries) {
    fs.rmSync(path.join(destination, file), { force: true });
  }
  console.log(
    `Windows ${architecture}: ${windowsSkiaOutputs.join(
      " + "
    )} in ${destination}`
  );
};
