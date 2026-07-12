cmake_minimum_required(VERSION 3.20)
project(NukeCSharp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(NUKE_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")

add_library(NukeCSharp SHARED
    src/NukeCSharp.cpp
)

# C# scripting backend (SHARED "scripting" service — loads beside NukeScript/Lua). Hosts
# the installed .NET runtime through hostfxr (found at run time, no build-time SDK link);
# the managed bridge (managed/NukeEngine.Managed) is built by dotnet below and shipped to
# modules/managed/.
target_compile_definitions(NukeCSharp PRIVATE
    WIN32 _WINDOWS NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    GLM_ENABLE_EXPERIMENTAL
    BOOST_ALL_DYN_LINK
    BOOST_ALL_NO_LIB     # autolink picked MIXED runtimes (release boost in a /MDd build) — link explicitly below
)

target_include_directories(NukeCSharp PRIVATE
    ${NUKE_ROOT}/NukeEngine
    ${NUKE_ROOT}/NukeEngine/include
    ${NUKE_ROOT}/NukeEngine/deps
    ${VCPKG_INSTALLED}/include                    # boost (NUKEModule header chain)
)

target_link_directories(NukeCSharp PRIVATE
    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib   # boost auto-link (header chain)
    ${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>             # NukeEngine.dll import lib
)

target_link_libraries(NukeCSharp PRIVATE
    NukeEngine
    debug boost_filesystem-vc143-mt-gd-x64-1_91 optimized boost_filesystem-vc143-mt-x64-1_91
    debug boost_thread-vc143-mt-gd-x64-1_91     optimized boost_thread-vc143-mt-x64-1_91
    debug boost_chrono-vc143-mt-gd-x64-1_91     optimized boost_chrono-vc143-mt-x64-1_91
)

# Deploy: the plugin DLL into modules/, the managed bridge into modules/managed/.
# `dotnet build` runs as part of the post-build (needs the .NET SDK on the dev machine —
# the SHIPPED game only needs the runtime).
set(MODULES_DIR "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/modules")
set(MANAGED_SRC "${CMAKE_CURRENT_SOURCE_DIR}/managed/NukeEngine.Managed")
add_custom_command(TARGET NukeCSharp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODULES_DIR}/managed"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NukeCSharp>" "${MODULES_DIR}/"
    COMMAND dotnet build "${MANAGED_SRC}" -c Release --nologo -v q
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MANAGED_SRC}/bin/Release/net8.0/NukeEngine.Managed.dll"
            "${MANAGED_SRC}/bin/Release/net8.0/NukeEngine.Managed.runtimeconfig.json"
            "${MODULES_DIR}/managed/"
    COMMENT "Deploying NukeCSharp.dll + the managed bridge to modules/"
)
