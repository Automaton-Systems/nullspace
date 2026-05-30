cmake_minimum_required(VERSION 3.18)
project(NullOrbit)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_OBJCXX_STANDARD 14)
set(CMAKE_OBJC_STANDARD 11)

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(IOS_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(ANDROID_ASSETS_DIR "${PROJECT_ROOT}/android/app/src/main/assets")
set(HOME_ASSETS_DIR "$ENV{HOME}/Developer/nullspace-resources/client-assets")

if(DEFINED NULLSPACE_ASSETS_DIR)
    set(ASSETS_DIR "${NULLSPACE_ASSETS_DIR}")
elseif(EXISTS "${IOS_ASSETS_DIR}/graphics" AND EXISTS "${IOS_ASSETS_DIR}/sound")
    set(ASSETS_DIR "${IOS_ASSETS_DIR}")
elseif(EXISTS "${ANDROID_ASSETS_DIR}/graphics" AND EXISTS "${ANDROID_ASSETS_DIR}/sound")
    set(ASSETS_DIR "${ANDROID_ASSETS_DIR}")
elseif(EXISTS "${HOME_ASSETS_DIR}/graphics" AND EXISTS "${HOME_ASSETS_DIR}/sound")
    set(ASSETS_DIR "${HOME_ASSETS_DIR}")
else()
    message(FATAL_ERROR
        "NullOrbit assets not found. Set NULLSPACE_ASSETS_DIR or stage assets under ios/assets or android/app/src/main/assets.")
endif()

message(STATUS "NullOrbit assets: ${ASSETS_DIR}")

# ── Game C++ sources ─────────────────────────────────────────────────────────
# Same glob as Android; exclude the desktop entrypoint and Android-only files.
file(GLOB_RECURSE GAME_SOURCES ${PROJECT_ROOT}/src/null/*.cpp)
list(FILTER GAME_SOURCES EXCLUDE REGEX "null/main\\.cpp$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "null/android/.*\\.cpp$")

# iOS ObjC++ game-side files (IOSSettings.mm lives here)
file(GLOB IOS_GAME_SOURCES ${PROJECT_ROOT}/src/null/ios/*.mm)

# GameMenuMobile.cpp is now guarded with NULLSPACE_MOBILE and must be included
set(MOBILE_SHARED_SOURCES
    ${PROJECT_ROOT}/src/null/android/GameMenuMobile.cpp)

# ── imgui core (no platform backends needed — UIKit handles the menu) ────────
set(IMGUI_SOURCES
    ${PROJECT_ROOT}/lib/imgui/imgui.cpp
    ${PROJECT_ROOT}/lib/imgui/imgui_draw.cpp
    ${PROJECT_ROOT}/lib/imgui/imgui_tables.cpp
    ${PROJECT_ROOT}/lib/imgui/imgui_widgets.cpp)

# ── iOS wrapper sources ──────────────────────────────────────────────────────
set(IOS_WRAPPER_SOURCES
    NullOrbit/nullspace_ios.mm
    NullOrbit/ViewController.mm
    NullOrbit/AppDelegate.mm
    NullOrbit/GameView.mm
    NullOrbit/main.m)

# ── Target ───────────────────────────────────────────────────────────────────

# Bundle game assets (graphics/ and sound/) preserving directory structure
file(GLOB_RECURSE GRAPHICS_ASSETS "${ASSETS_DIR}/graphics/*")
file(GLOB_RECURSE SOUND_ASSETS    "${ASSETS_DIR}/sound/*")
foreach(asset ${GRAPHICS_ASSETS})
    file(RELATIVE_PATH rel "${ASSETS_DIR}" "${asset}")
    get_filename_component(dir "${rel}" DIRECTORY)
    set_source_files_properties("${asset}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${dir}")
endforeach()
foreach(asset ${SOUND_ASSETS})
    file(RELATIVE_PATH rel "${ASSETS_DIR}" "${asset}")
    get_filename_component(dir "${rel}" DIRECTORY)
    set_source_files_properties("${asset}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${dir}")
endforeach()

set(LAUNCH_STORYBOARD "${CMAKE_CURRENT_SOURCE_DIR}/NullOrbit/LaunchScreen.storyboard")
set_source_files_properties("${LAUNCH_STORYBOARD}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

# Asset catalog: register the .xcassets folder itself (NOT its contents) so
# Xcode runs actool on it and produces Assets.car with the AppIcon set.
set(IOS_ASSET_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/NullOrbit/Assets.xcassets")
set_source_files_properties("${IOS_ASSET_CATALOG}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(NullOrbit MACOSX_BUNDLE
    ${GAME_SOURCES}
    ${IOS_GAME_SOURCES}
    ${MOBILE_SHARED_SOURCES}
    ${IMGUI_SOURCES}
    ${PROJECT_ROOT}/lib/glad/src/glad.cpp
    ${IOS_WRAPPER_SOURCES}
    ${LAUNCH_STORYBOARD}
    ${IOS_ASSET_CATALOG}
    ${GRAPHICS_ASSETS}
    ${SOUND_ASSETS})

# Sound.cpp includes miniaudio.h which has ObjC @interface blocks — must be ObjC++
set_source_files_properties(${PROJECT_ROOT}/src/null/Sound.cpp
    PROPERTIES COMPILE_FLAGS "-x objective-c++")

target_compile_definitions(NullOrbit PRIVATE
    NULLSPACE_MOBILE=1
    IMGUI_IMPL_OPENGL_ES3)

target_include_directories(NullOrbit PRIVATE
    ${PROJECT_ROOT}/lib
    ${PROJECT_ROOT}/lib/imgui
    ${PROJECT_ROOT}/lib/glad/include
    ${PROJECT_ROOT}/src)

target_link_libraries(NullOrbit PRIVATE
    "-framework UIKit"
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework OpenGLES"
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework AVFoundation"
    "-framework CoreFoundation")

# ── Xcode / bundle properties ────────────────────────────────────────────────
set_target_properties(NullOrbit PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST             "${CMAKE_CURRENT_SOURCE_DIR}/NullOrbit/Info.plist"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY  "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC    "YES"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.systems.automaton.nullorbit"
    # Prefix header: forces Foundation/UIKit before glad's GL header guards
    XCODE_ATTRIBUTE_GCC_PREFIX_HEADER        "${CMAKE_CURRENT_SOURCE_DIR}/NullOrbit/NullOrbit-Prefix.pch"
    XCODE_ATTRIBUTE_GCC_PRECOMPILE_PREFIX_HEADER "YES"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM         "2A9K3N2KPN"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE          "Automatic"
    XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH         "YES"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
