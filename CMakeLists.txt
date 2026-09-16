cmake_minimum_required(VERSION 3.22)
project(Nala VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Quick Qml ShaderTools Test)
# Wayland overlay placement on Hyprland. Optional so the app still builds and
# runs (as a plain floating window) on compositors without layer-shell.
find_package(LayerShellQt 6.7 QUIET CONFIG)

qt_add_executable(nala
  src/main.cpp
  src/activity.cpp src/activity.h
  src/backend.cpp src/backend.h
  src/cursor.cpp src/cursor.h
  src/mascot.cpp src/mascot.h
  src/orbits.cpp src/orbits.h
  src/selftest.cpp src/selftest.h
  src/trail.cpp src/trail.h
  src/theme.cpp src/theme.h)

qt_add_shaders(nala "shaders"
  PREFIX /
  FILES shaders/mascot.frag)

qt_add_resources(nala "ui" PREFIX / FILES
  qml/Mascot.qml
  qml/Settings.qml
  assets/nala.svg)

target_link_libraries(nala PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::Test)

if(LayerShellQt_FOUND)
  target_link_libraries(nala PRIVATE LayerShellQt::Interface)
  target_compile_definitions(nala PRIVATE NALA_HAVE_LAYER_SHELL
    LAYERSHELLQTINTERFACE_DISABLE_DEPRECATED_BEFORE_AND_AT=0x060600)
endif()

target_compile_options(nala PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME nala-self-test COMMAND nala --self-test)
set_tests_properties(nala-self-test PROPERTIES ENVIRONMENT
  "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software")
