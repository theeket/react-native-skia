import { spawn } from "child_process";
import fs, {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  statSync,
} from "fs";
import { exit } from "process";
import path from "path";

export const ensureFolderExists = (dirPath: string) => {
  try {
    console.log(`Ensuring that ${dirPath} exists...`);
    mkdirSync(dirPath, { recursive: true });
  } catch (err) {
    // eslint-disable-next-line @typescript-eslint/ban-ts-comment
    // @ts-ignore
    if (err.code !== "EEXIST") {
      throw err;
    }
  }
};

// Windows Python installations expose python; Unix builds use python3.
export const pythonExecutable =
  process.platform === "win32" ? "python" : "python3";

// Preserve Node's overloads for commands whose output must be read immediately.
export { execFileSync as runSync } from "child_process";

/** Runs a command, streaming its output, and rejects on a non-zero exit. */
export const run = (
  command: string,
  args: string[],
  options: { cwd?: string; env?: NodeJS.ProcessEnv; label?: string } = {}
): Promise<void> =>
  new Promise((resolve, reject) => {
    console.log(`$ ${[command, ...args].join(" ")}`);
    const child = spawn(command, args, {
      cwd: options.cwd,
      env: options.env ?? process.env,
      stdio: options.label ? ["inherit", "pipe", "pipe"] : "inherit",
    });
    if (options.label) {
      // Label compiler output as well as headings when several SDKs are built.
      child.stdout?.on("data", (data) =>
        process.stdout.write(`${options.label} ${data}`)
      );
      child.stderr?.on("data", (data) =>
        process.stderr.write(`${options.label} ${data}`)
      );
    }
    child.on("error", reject);
    child.on("close", (code) => {
      if (code === 0) {
        resolve();
      } else {
        reject(
          new Error(`${options.label ?? command} exited with code ${code}`)
        );
      }
    });
  });

export const mapKeys = <T extends object>(obj: T) =>
  Object.keys(obj) as (keyof T)[];

export const checkFileExists = (filePath: string) => {
  const exists = existsSync(filePath);
  if (!exists) {
    console.log("");
    console.log("Failed:");
    console.log(filePath + " not found. (" + filePath + ")");
    console.log("");
    exit(1);
  } else {
    console.log("✅ " + filePath);
  }
};

export var copyRecursiveSync = function (src: string, dest: string) {
  if (!existsSync(src)) {
    throw new Error(`Cannot copy ${src}: it does not exist`);
  }
  var stats = statSync(src);
  var isDirectory = stats.isDirectory();
  if (isDirectory) {
    if (!existsSync(dest)) {
      mkdirSync(dest);
    }
    readdirSync(src).forEach((childItemName) => {
      copyRecursiveSync(
        path.join(src, childItemName),
        path.join(dest, childItemName)
      );
    });
  } else {
    copyFileSync(src, dest);
  }
};

// Cross-platform file operations abstraction
export const fileOps = {
  rm: (p: string) => {
    if (fs.existsSync(p)) {
      fs.rmSync(p, { recursive: true, force: true });
    }
  },

  mkdir: (p: string) => {
    fs.mkdirSync(p, { recursive: true });
  },

  cp: (src: string, dest: string) => {
    copyRecursiveSync(src, dest);
  },

  sed: (file: string, pattern: RegExp, replacement: string) => {
    if (fs.existsSync(file)) {
      const content = fs.readFileSync(file, "utf8");
      const updated = content.replace(pattern, replacement);
      fs.writeFileSync(file, updated, "utf8");
    }
  },
};
