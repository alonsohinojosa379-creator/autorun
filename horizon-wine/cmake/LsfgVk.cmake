# LSFG-VK's backend for Horizon comes built from the switch-dev image
# (liblsfg-vk.a, with its NVK shaders); the presentation glue in source/lsfg.cpp
# is Autorun's own. Its GPL license goes beside the build for the packager.
find_library(SWITCH_LSFG_VK lsfg-vk HINTS "$ENV{DEVKITPRO}/portlibs/switch/lib" REQUIRED)
set(LSFG_LICENSE "$ENV{DEVKITPRO}/portlibs/switch/share/licenses/lsfg-vk/LICENSE.md")
if(NOT EXISTS "${LSFG_LICENSE}")
    message(FATAL_ERROR "No LSFG-VK license in portlibs; build in the switch-dev image")
endif()
configure_file("${LSFG_LICENSE}" "${CMAKE_CURRENT_BINARY_DIR}/licenses/LSFG-VK-GPL-3.0.txt" COPYONLY)
add_library(wine-nx-lsfg STATIC source/lsfg.cpp)
set_target_properties(wine-nx-lsfg PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(wine-nx-lsfg PRIVATE "${WINE_NX_MESA_SWITCH_DIR}/../include")
target_compile_options(wine-nx-lsfg PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(wine-nx-lsfg PRIVATE ${SWITCH_LSFG_VK})
target_compile_definitions(wine-nx-runtime PRIVATE WINE_NX_LSFG)
target_compile_definitions(wine-win32u-real PRIVATE WINE_NX_LSFG)
target_link_libraries(wine-nx-runtime PRIVATE wine-nx-lsfg)
