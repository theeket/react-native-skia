import type { AppleSDK } from "./apple-build";
import { appleSDKs } from "./apple-build";

/** Expand target groups; default to host-supported targets and reject incompatible requests. */
export const selectBuildTargets = (
  specs: string[],
  targets: string[],
  host: NodeJS.Platform = process.platform
) => {
  const supported = targets.filter((target) => {
    const [group] = target.split("-");
    if (group === "windows") {
      return host === "win32";
    }
    if (appleSDKs.includes(group as AppleSDK)) {
      return host === "darwin";
    }
    return (
      (group === "android" || group === "wasm") &&
      ["darwin", "win32", "linux"].includes(host)
    );
  });
  if (supported.length === 0) {
    throw new Error(`No build targets are supported on host '${host}'`);
  }
  if (specs.length === 0) {
    return supported;
  }
  const selected = new Set<string>();
  for (const spec of specs) {
    // Only catalogued groups expand; partial names must not select other SDKs.
    const isGroup =
      spec === "android" ||
      spec === "windows" ||
      spec === "wasm" ||
      appleSDKs.includes(spec as AppleSDK);
    let matches: string[] = [];
    if (targets.includes(spec)) {
      matches = [spec];
    } else if (isGroup) {
      matches = targets.filter((target) => target.startsWith(`${spec}-`));
    }
    if (matches.length === 0) {
      throw new Error(
        `Unknown target '${spec}'. Valid targets: ${targets.join(
          ", "
        )}; groups: android, windows, wasm, ${appleSDKs.join(", ")}`
      );
    }
    if (matches.some((target) => !supported.includes(target))) {
      throw new Error(
        `Target '${spec}' is not supported on host '${host}'. Supported targets: ${supported.join(", ")}`
      );
    }
    matches.forEach((target) => selected.add(target));
  }
  return [...selected];
};
