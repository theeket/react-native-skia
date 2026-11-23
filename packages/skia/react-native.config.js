// Pins the SwiftPM target name. Without it the name is derived — today from the
// npm name (ReactNativeSkia), but an in-flight React Native change derives it
// from the podspec instead (react-native-skia). The name is also the header
// import prefix, so it must not move.
module.exports = {
  spm: {name: 'ReactNativeSkia'},
  dependency: {
    platforms: {
      windows: {
        sourceDir: 'windows',
        solutionFile: 'RNSkia.sln',
        projects: [
          {
            projectFile: 'RNSkia\\RNSkia.vcxproj',
            directDependency: true,
          },
        ],
      },
    },
  },
};
