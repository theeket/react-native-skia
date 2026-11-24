cmake_minimum_required(VERSION 3.21)

# depot_tools also supplies a Unix "ninja" wrapper. Select the Windows executable
# before project() probes the generator, replacing caches that selected the wrapper.
find_program(SKIA_NINJA NAMES ninja.exe REQUIRED NO_CACHE)
set(CMAKE_MAKE_PROGRAM "${SKIA_NINJA}" CACHE FILEPATH "Ninja executable" FORCE)

project(skia_combined LANGUAGES CXX)

# Reuse GN's objects, including shared source sets only once. Keeping these as
# objects lets CMake generate exports for module APIs without DLL annotations.
include("${SKIA_LINK_INPUTS}")
add_library(skia_combined SHARED ${SKIA_OBJECTS})
set_target_properties(skia_combined PROPERTIES
  LINKER_LANGUAGE CXX
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  MSVC_RUNTIME_LIBRARY MultiThreadedDLL
)

# Codec/text dependencies remain internal. Only the standalone Dawn import
# library is linked: GN's dawn_combined and its /WHOLEARCHIVE flag stay unused.
target_link_libraries(skia_combined PRIVATE
  ${SKIA_DEPENDENCIES}
  "${DAWN_IMPORT_LIBRARY}"
  ${SKIA_SYSTEM_LIBRARIES}
)
target_link_options(skia_combined PRIVATE /OPT:REF /OPT:ICF)
