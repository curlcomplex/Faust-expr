cmake_minimum_required(VERSION 3.25)
project(CombinedPolyState LANGUAGES C CXX)
add_subdirectory(../persistent-state-next previous)
include(FetchContent)
# The established host already provides JUCE. Tracktion's example JUCE submodule
# uses SSH and is neither needed nor permitted as an additional dependency here.
FetchContent_Declare(tracktion_dep GIT_REPOSITORY https://github.com/Tracktion/tracktion_engine.git GIT_TAG 4536d8a21664fe6ec2aa34b25abc87fa2a0d3b86 GIT_SUBMODULES "" SOURCE_SUBDIR not-a-cmake-project)
FetchContent_MakeAvailable(tracktion_dep)
execute_process(COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/patch_worker_context.py" "${tracktion_dep_SOURCE_DIR}/modules/tracktion_graph/tracktion_graph/tracktion_NodePlayerThreadPools.cpp" COMMAND_ERROR_IS_FATAL ANY)
juce_add_module("${tracktion_dep_SOURCE_DIR}/modules/tracktion_core")
juce_add_module("${tracktion_dep_SOURCE_DIR}/modules/tracktion_graph")
get_target_property(SUPPORT RetainedPatchingBench SOURCES)
list(FILTER SUPPORT EXCLUDE REGEX "retained_main.cpp$")
get_target_property(INCLUDES RetainedPatchingBench INCLUDE_DIRECTORIES)
get_target_property(DEFINES RetainedPatchingBench COMPILE_DEFINITIONS)
get_target_property(LINKS RetainedPatchingBench LINK_LIBRARIES)

# Both PR45 executables exercise the same renderer/support implementation.  The
# old harness added SUPPORT directly to each executable, so the device target
# compiled the full inherited CURLOP support set a second time and could exceed
# the bounded physical-Mac build timeout even though CombinedPolyBench had just
# compiled identical objects.  Compile those sources once.  This changes build
# ownership only: the two entry translation units, definitions, links and DSP
# code remain distinct and unchanged.
add_library(PR45SharedSupport OBJECT ${SUPPORT})
target_include_directories(PR45SharedSupport PRIVATE ${INCLUDES} .)
target_compile_definitions(PR45SharedSupport PRIVATE ${DEFINES} POLY_HAS_FADE_IN=${POLY_HAS_FADE_IN})
target_compile_features(PR45SharedSupport PRIVATE cxx_std_20)
target_compile_options(PR45SharedSupport PRIVATE -O2 -g -fno-omit-frame-pointer)
target_link_libraries(PR45SharedSupport PRIVATE ${LINKS} tracktion_graph tracktion_core)
add_dependencies(PR45SharedSupport catalogue)

function(pr45_target target source)
 add_executable(${target} ${source} $<TARGET_OBJECTS:PR45SharedSupport>)
 target_include_directories(${target} PRIVATE ${INCLUDES} .)
 target_compile_definitions(${target} PRIVATE ${DEFINES} POLY_HAS_FADE_IN=${POLY_HAS_FADE_IN})
 target_compile_features(${target} PRIVATE cxx_std_20)
 target_compile_options(${target} PRIVATE -O2 -g -fno-omit-frame-pointer)
 target_link_libraries(${target} PRIVATE ${LINKS} tracktion_graph tracktion_core)
 add_dependencies(${target} PR45SharedSupport catalogue)
 if(APPLE)
  target_link_options(${target} PRIVATE "LINKER:-stack_size,0x1000000")
 endif()
endfunction()
pr45_target(CombinedPolyBench main.native.cpp)
pr45_target(FinalDeviceBench device_main.cpp)
