import { exit } from "process";
import fs from "fs";
import path from "path";

import type { Platform, PlatformName } from "./skia-configuration";
import type { AppleArchitecture } from "./apple-build";
import { combineAppleLibraries } from "./apple-build";
import { validateAppleArchive } from "./apple-binaries-validation";
import { selectBuildTargets } from "./build-targets";
import { prepareDawnHeaders } from "./prepare-dawn-headers";
import {
  applySkiaPatches,
  commonArgs,
  configurations,
  copyHeaders,
  appleOutputNames,
  skiaBuildDir,
  skiaTargets,
  skiaTargetOutputDir,
  PackageRoot,
  ProjectRoot,
  SkiaSrc,
  WasmLibs,
} from "./skia-configuration";
import { applyDawnPatches } from "./dawn-configuration";
import {
  activateEmsdk,
  buildEmdawnwebgpuPackage,
  wasmArchiveSource,
  writeWasmMetadata,
} from "./wasm-configuration";
import { fileOps, pythonExecutable, run, runSync } from "./utils";
import { getWindowsBuildEnvironment } from "./windows-build";

const pathKey =
  Object.keys(process.env).find((key) => key.toUpperCase() === "PATH") ??
  "PATH";
const buildEnv: NodeJS.ProcessEnv = {
  ...process.env,
  GIT_SYNC_DEPS_SKIP_EMSDK: "1",
  [pathKey]: [
    path.join(ProjectRoot, "externals", "depot_tools"),
    path.join(ProjectRoot, "externals", "bin"),
    process.env[pathKey] ?? "",
  ].join(path.delimiter),
};
if (process.platform === "win32") {
  // Apply long-path support to Git processes launched by dependency sync.
  const count = Number(buildEnv.GIT_CONFIG_COUNT ?? "0");
  buildEnv.GIT_CONFIG_COUNT = String(count + 1);
  buildEnv[`GIT_CONFIG_KEY_${count}`] = "core.longpaths";
  buildEnv[`GIT_CONFIG_VALUE_${count}`] = "true";
}

/**
 * Locate GN intermediates, separately from distributable libraries under libs/.
 */
const getOutDir = (platform: PlatformName, targetName: string) => {
  return skiaBuildDir(platform, targetName);
};

/**
 * Generate one architecture's Ninja files with common, platform, and target GN args.
 * Runs from the Skia source directory; an unknown target terminates the script.
 * Return the resolved compiler environment for the build step.
 */
const configurePlatform = async (
  platformName: PlatformName,
  configuration: Platform,
  targetName: string
): Promise<NodeJS.ProcessEnv> => {
  process.chdir(SkiaSrc);
  console.log(
    `Configuring platform "${platformName}" for target "${targetName}"`
  );
  console.log("Current directory", process.cwd());

  if (configuration) {
    const target = configuration.targets[targetName];
    if (!target) {
      console.log(
        `Target ${targetName} not found for platform ${platformName}`
      );
      exit(1);
    }

    const windows =
      platformName === "windows"
        ? getWindowsBuildEnvironment(target.cpu, buildEnv)
        : undefined;
    const env = windows?.env ?? buildEnv;
    const gn = path.join(
      SkiaSrc,
      "bin",
      process.platform === "win32" ? "gn.exe" : "gn"
    );

    const args = configuration.args.reduce(
      (a, cur) => (a += `${cur[0]}=${cur[1]} \n`),
      ""
    );

    const common = commonArgs.reduce(
      (a, cur) => (a += `${cur[0]}=${cur[1]} \n`),
      ""
    );

    const targetArgs =
      target.args?.reduce((a, cur) => (a += `${cur[0]}=${cur[1]} \n`), "") ||
      "";

    const options =
      configuration.options?.map(([key, value]) => `--${key}=${value}`) ?? [];

    const targetOptions =
      target.options?.map(([key, value]) => `--${key}=${value}`) ?? [];

    await run(
      gn,
      [
        "gen",
        getOutDir(platformName, targetName),
        ...options,
        ...targetOptions,
        `--script-executable=${pythonExecutable}`,
        `--args=target_os="${target.platform}" target_cpu="${
          target.cpu
        }" ${common}${args}${targetArgs} ${windows?.gnArgs ?? ""}`,
      ],
      { env, label: "⚙️" }
    );
    return env;
  } else {
    throw new Error(
      `Could not find platform "${platformName}" for target "${targetName}"`
    );
  }
};

/**
 * Compile one configured target and verify its Apple archives before staging.
 * Every Mach-O object must match the requested SDK and single architecture.
 */
export const buildPlatform = async (
  platform: PlatformName,
  targetName: string,
  env: NodeJS.ProcessEnv = buildEnv
) => {
  process.chdir(SkiaSrc);
  console.log(`Building platform "${platform}" for target "${targetName}"`);
  // We need to include the path to our custom python2 -> python3 mapping script
  // to make sure we can run all scripts that uses #!/usr/bin/env python as shebang
  // https://groups.google.com/g/skia-discuss/c/BYyB-TwA8ow
  // Build only wasm archives: the default target also builds native Dawn/Tint.
  const ninjaTargets =
    platform === "wasm"
      ? configurations.wasm.outputNames.map((library) =>
          path.basename(wasmArchiveSource(library))
        )
      : [];
  // Apple CPU names overlap across SDKs; identify both in compiler output.
  let label = `🍏 ${platform}-${targetName}`;
  if (platform === "android") {
    label = `🤖 ${targetName}`;
  } else if (platform === "windows") {
    label = `🪟 ${targetName}`;
  } else if (platform === "wasm") {
    label = `🕸️ ${targetName}`;
  }
  await run("ninja", ["-C", getOutDir(platform, targetName), ...ninjaTargets], {
    env,
    label,
  });
  if (platform !== "android" && platform !== "windows" && platform !== "wasm") {
    // Fail this target before staging or compiling the next architecture.
    for (const library of configurations[platform].outputNames) {
      validateAppleArchive(
        path.join(getOutDir(platform, targetName), library),
        platform,
        [targetName as AppleArchitecture]
      );
    }
    console.log(`${label} ✅ Mach-O verified`);
  }
};

/**
 * Reject Android archives defining typeinfo owned by the shared C++ runtime.
 * Such definitions make librnskia.so bind catch clauses to its own copies
 * instead of libc++_shared's, breaking exception matching across libraries.
 */
const assertNoRuntimeTypeinfo = (libPath: string) => {
  const copies = runSync(
    `$ANDROID_NDK/toolchains/llvm/prebuilt/*/bin/llvm-nm --defined-only --just-symbol-name ${libPath}`,
    [],
    { shell: true, maxBuffer: Infinity }
  )
    .toString()
    .split("\n")
    .filter((symbol) => /^_ZT[IS]St/.test(symbol));
  if (copies.length > 0) {
    throw new Error(
      `${libPath} defines typeinfo the C++ runtime owns: ${copies.join(", ")}`
    );
  }
};

/**
 * Stage archives under libs/<os>/<platform>/, stripping Android debug symbols.
 * Sources remain in the GN output directory. Copy/strip failures stop the script.
 */
export const copyLib = (
  os: PlatformName,
  cpu: string,
  platform: string,
  outputNames: string[]
) => {
  const dstPath = path.join(PackageRoot, "libs", os, platform);
  fs.mkdirSync(dstPath, { recursive: true });

  outputNames
    .map((name) => path.join(getOutDir(os, cpu), name))
    .forEach((libPath) => {
      const destFile = path.join(dstPath, path.basename(libPath));
      console.log(`Copying ${libPath} to ${dstPath}`);
      fs.copyFileSync(libPath, destFile);
      if (os === "android") {
        // The archives ship as they are: drop the debug info, which is most
        // of their size.
        runSync(
          `$ANDROID_NDK/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-debug ${destFile}`,
          [],
          { shell: true }
        );
      }
    });
};

/**
 * Build SDK groups (e.g. iphonesimulator) as lipo'd universal libraries, or one
 * architecture (e.g. iphonesimulator-arm64). Single-architecture SDKs need no lipo.
 * Sync dependencies and apply patches, then configure/build/validate/stage each
 * selected target sequentially before combining Apple SDK outputs and copying headers.
 *
 * Usage (from packages/skia):
 *   yarn build-skia                              # every target supported by this host
 *   yarn build-skia android                      # all Android ABIs
 *   yarn build-skia android-arm64                # one Android architecture
 *   yarn build-skia windows                      # all Windows architectures
 *   yarn build-skia windows-x64                  # one Windows architecture
 *   yarn build-skia iphonesimulator              # lipo'd universal simulator libraries
 *   yarn build-skia iphonesimulator-arm64        # one simulator architecture
 *   yarn build-skia iphoneos iphonesimulator     # multiple SDK groups
 *   yarn build-skia wasm                         # Graphite on WebGPU, for the web
 *
 * Apple static libraries are staged under libs/apple/<sdk>-<arch>/
 * (e.g. libs/apple/iphonesimulator-arm64/), then assembled under libs/apple/<sdk>/
 * (e.g. libs/apple/iphonesimulator/). Staging directories are removed on successful
 * SDK assembly; GN intermediates remain. Android uses libs/android/<abi>/
 * (e.g. libs/android/arm64-v8a/) and Windows uses libs/windows/<arch>/
 * (e.g. libs/windows/x64/). package-binaries assembles npm XCFrameworks.
 * wasm uses libs/wasm/ with its own headers and emdawnwebgpu package next to
 * the archives; see wasm-configuration.ts.
 *
 * See apple-build.ts for SDK groups/architectures and
 * skia-configuration.ts (skiaTargets) for the full target list.
 */
(async () => {
  const targetSpecs = process.argv.slice(2);
  const selectedTargets = selectBuildTargets(
    targetSpecs,
    Object.keys(skiaTargets)
  );

  // Resolve local or published headers before syncing or deleting build outputs.
  await prepareDawnHeaders();

  console.log(`🎯 Building targets: ${selectedTargets.join(", ")}`);
  console.log("🪨 Skia Graphite");

  // Check Android environment variables if android is in target platforms
  const hasAndroid = selectedTargets.some(
    (name) => skiaTargets[name].platform === "android"
  );
  const hasWindows = selectedTargets.some(
    (name) => skiaTargets[name].platform === "windows"
  );
  const hasWasm = selectedTargets.some(
    (name) => skiaTargets[name].platform === "wasm"
  );
  if (hasAndroid) {
    ["ANDROID_NDK", "ANDROID_HOME"].forEach((name) => {
      // Test for existence of Android SDK
      if (!process.env[name]) {
        console.log(`${name} not set.`);
        exit(1);
      } else {
        console.log(`✅ ${name}`);
      }
    });
  }

  // Sync dependencies before applying the pinned source patches.
  console.log("Running gclient sync...");
  process.chdir(SkiaSrc);
  const syncEnv = { ...buildEnv };
  if (hasWasm) {
    // A truthy skip value prevents git-sync-deps from checking out Emsdk.
    delete syncEnv.GIT_SYNC_DEPS_SKIP_EMSDK;
  }
  await run(pythonExecutable, ["tools/git-sync-deps"], { env: syncEnv });
  console.log("gclient sync done");
  // Start the Skia tree from its pinned commit so leftovers from an earlier
  // build (old patches, string edits, local tweaks) can't make a current
  // patch look stale. This only resets Skia's tracked files: the Dawn
  // checkout under third_party/externals is a separate repository, so its
  // patches are still applied idempotently by applyDawnPatches().
  runSync("git", ["reset", "--hard", "HEAD"]);
  // Skia's GN build and the standalone Dawn build share the Dawn checkout.
  applySkiaPatches();
  applyDawnPatches();

  // Clear Android and Windows outputs before staging the selected architectures.
  if (hasAndroid) {
    fs.rmSync(path.join(PackageRoot, configurations.android.outputRoot), {
      recursive: true,
      force: true,
    });
  }
  if (hasWindows) {
    fs.rmSync(path.join(PackageRoot, configurations.windows.outputRoot), {
      recursive: true,
      force: true,
    });
  }

  // GN compiles Graphite against the emdawnwebgpu headers, so the package
  // comes first; it needs Emscripten for the struct layouts.
  if (hasWasm) {
    fileOps.rm(WasmLibs);
    activateEmsdk();
    buildEmdawnwebgpuPackage();
  }

  for (const name of selectedTargets) {
    const { platform, architecture } = skiaTargets[name];
    const configuration = configurations[platform];
    const env = await configurePlatform(platform, configuration, architecture);
    await buildPlatform(platform, architecture, env);
    process.chdir(ProjectRoot);
    if (platform === "android") {
      configuration.outputNames.forEach((library) =>
        assertNoRuntimeTypeinfo(
          `${getOutDir(platform, architecture)}/${library}`
        )
      );
      copyLib(
        platform,
        architecture,
        configurations.android.targets[architecture].output,
        configuration.outputNames
      );
    } else if (platform === "windows") {
      copyLib(
        platform,
        architecture,
        configurations.windows.targets[architecture].output,
        configuration.outputNames
      );
    } else if (platform === "wasm") {
      const destination = skiaTargetOutputDir(name);
      fs.mkdirSync(destination, { recursive: true });
      for (const library of configuration.outputNames) {
        fs.copyFileSync(
          wasmArchiveSource(library),
          path.join(destination, library)
        );
      }
    } else {
      // Stage each SDK/architecture separately before combining its libraries.
      const destination = skiaTargetOutputDir(name);
      fs.rmSync(destination, { recursive: true, force: true });
      fs.mkdirSync(destination, { recursive: true });
      for (const library of configuration.outputNames) {
        fs.copyFileSync(
          path.join(getOutDir(platform, architecture), library),
          path.join(destination, library)
        );
      }
    }
  }

  combineAppleLibraries(
    path.join(PackageRoot, "libs", "apple"),
    appleOutputNames,
    undefined,
    selectedTargets
  );
  await copyHeaders();
  if (hasWasm) {
    // The asset carries its headers: a consumer of the published archives has
    // no Skia checkout to copy them from.
    fileOps.cp(
      path.join(PackageRoot, "cpp", "skia"),
      path.join(WasmLibs, "skia")
    );
    fs.copyFileSync(
      path.join(SkiaSrc, "LICENSE"),
      path.join(WasmLibs, "LICENSE")
    );
    writeWasmMetadata();
    console.log(`✅ wasm libraries in ${WasmLibs}`);
  }
})().catch((error) => {
  console.error(`❌ ${error.message}`);
  exit(1);
});
