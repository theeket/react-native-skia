/**
 * Builds the monolithic Dawn (libwebgpu_dawn) from the Dawn checkout Skia
 * pins, into packages/skia/libs/dawn. It is the Dawn the Skia Graphite
 * binaries were compiled against, shipped as the react-native-webgpu-dawn npm
 * package that react-native-skia and react-native-webgpu both link.
 *
 * Usage (from packages/skia):
 *   yarn build-dawn                              # every target supported by this host
 *   yarn build-dawn android                      # all Android ABIs
 *   yarn build-dawn android-arm64-v8a            # one Android ABI
 *   yarn build-dawn windows                      # all Windows architectures
 *   yarn build-dawn windows-x64                  # one Windows architecture
 *   yarn build-dawn iphonesimulator              # lipo'd universal simulator libraries
 *   yarn build-dawn iphonesimulator-arm64        # one simulator architecture
 *   yarn build-dawn iphoneos iphonesimulator     # multiple SDK groups
 *
 * Apple static libraries are staged under libs/dawn/apple/<sdk>-<arch>/
 * (e.g. libs/dawn/apple/iphonesimulator-arm64/), then assembled under libs/dawn/apple/<sdk>/
 * (e.g. libs/dawn/apple/iphonesimulator/). SDK groups with multiple architectures
 * produce lipo'd universal libraries; single-architecture builds copy the archive.
 * Staging directories are removed on successful SDK assembly; CMake intermediates remain.
 * Android uses libs/dawn/android/<abi>/ (e.g. libs/dawn/android/arm64-v8a/) and
 * Windows uses libs/dawn/windows/<arch>/ (e.g. libs/dawn/windows/x64/),
 * containing webgpu_dawn.dll and its import webgpu_dawn.lib.
 * package-binaries assembles XCFrameworks for npm distribution.
 *
 * See apple-build.ts for SDK groups/architectures and
 * dawn-configuration.ts (dawnTargets) for the full target list.
 *
 * Every run first syncs Dawn and its dependencies to the commits in Skia's
 * DEPS and applies scripts/dawn-patches. The headers are collected into
 * libs/dawn/include from the first target built.
 */

import { exit } from "process";
import path from "path";

import { combineAppleLibraries } from "./apple-build";
import { selectBuildTargets } from "./build-targets";
import {
  applyDawnPatches,
  buildDawnTarget,
  collectDawnHeaders,
  DawnLibs,
  dawnLibraryName,
  dawnTargetNames,
  syncDawnDeps,
} from "./dawn-configuration";

(async () => {
  const targets = selectBuildTargets(process.argv.slice(2), dawnTargetNames);

  if (targets.length > 0) {
    console.log(`🎯 Building Dawn for: ${targets.join(", ")}`);
    await syncDawnDeps();
    applyDawnPatches();
    let headersCollected = false;
    for (const target of targets) {
      const buildDir = await buildDawnTarget(target);
      if (!headersCollected) {
        collectDawnHeaders(buildDir);
        headersCollected = true;
      }
    }
  }

  // Limit combination to this run's targets, excluding stale architecture outputs.
  combineAppleLibraries(
    path.join(DawnLibs, "apple"),
    [dawnLibraryName("apple")],
    undefined,
    targets
  );
})().catch((error) => {
  console.error(`❌ ${error.message}`);
  exit(1);
});
