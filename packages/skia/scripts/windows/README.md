# Windows DLL builds

From `packages/skia`, build Dawn before Skia for the same architecture:

```powershell
yarn build-dawn windows-arm64
yarn build-skia windows-arm64
```

Use `windows-x64` for x64, or `windows` to build both architectures.
The scripts initialize Visual Studio separately for each target.

Outputs:

- `libs/dawn/windows/<arch>/webgpu_dawn.dll` and its import `webgpu_dawn.lib`.
- `libs/windows/<arch>/skia_combined.dll` and its import `skia_combined.lib`.

Skia's GN build still generates its complete Dawn dependency. Its static
archives and object files remain in `externals/skia/out/windows/<arch>`.
A separate CMake link reuses the Skia/module objects, deduplicates shared
source sets, and generates DLL exports. Codec/text archives are internal
link dependencies; only the standalone Dawn DLL supplies WebGPU at runtime.
Only successfully combined targets replace their previously staged archives.

Prerequisites are unchanged: Visual Studio C++ tools for each target and the
native host, a Windows SDK, Python (`python`), CMake 3.21+, Ninja, Git and Yarn.

Consumer integration is a separate step. Link both import libraries, deploy
both DLLs, and compile with `SKIA_DLL`, `SKSHAPER_DLL`, `SKUNICODE_DLL`,
`WGPU_SHARED_LIBRARY` and `DAWN_NATIVE_SHARED_LIBRARY` without implementation
defines. Match the release `/MD` CRT and STL settings, including iterator
debugging, across the C++ DLL boundary. The import libraries are not static
fallbacks. Downloading, MSBuild linking and app deployment are not added here.
