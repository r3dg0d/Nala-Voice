cmake_minimum_required(VERSION 3.22)
project(Nala VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core DBus Gui Widgets Quick Qml ShaderTools Test)
# Wayland overlay placement on Hyprland. Optional so the app still builds and
# runs (as a plain floating window) on compositors without layer-shell.
find_package(LayerShellQt 6.7 QUIET CONFIG)

qt_add_executable(nala
  src/main.cpp
  src/activity.cpp src/activity.h
  src/backend.cpp src/backend.h
  src/compositor.cpp src/compositor.h
  src/cursor.cpp src/cursor.h
  src/mascot.cpp src/mascot.h
  src/music.cpp src/music.h
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
  Qt6::Core Qt6::DBus Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::Test)

if(LayerShellQt_FOUND)
  target_link_libraries(nala PRIVATE LayerShellQt::Interface)
  target_compile_definitions(nala PRIVATE NALA_HAVE_LAYER_SHELL
    LAYERSHELLQTINTERFACE_DISABLE_DEPRECATED_BEFORE_AND_AT=0x060600)
endif()

target_compile_options(nala PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
install(TARGETS nala RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/nala.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# systemd only searches <prefix>/lib/systemd/user for user units, never lib64,
# so this deliberately ignores CMAKE_INSTALL_LIBDIR. The unit is generated at
# install time so ExecStart names wherever the binary actually went, including
# when the prefix is overridden with `cmake --install --prefix`.
install(CODE "
  set(NALA_BINDIR \"${CMAKE_INSTALL_BINDIR}\")
  if(NOT IS_ABSOLUTE \"\${NALA_BINDIR}\")
    set(NALA_BINDIR \"\${CMAKE_INSTALL_PREFIX}/\${NALA_BINDIR}\")
  endif()
  configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/packaging/nala.service.in\"
                 \"${CMAKE_CURRENT_BINARY_DIR}/nala.service\" @ONLY)
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nala.service
        DESTINATION lib/systemd/user)
install(FILES assets/nala.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

enable_testing()
add_test(NAME nala-self-test COMMAND nala --self-test)
add_test(NAME nala-install
         COMMAND ${CMAKE_COMMAND} -E env
                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-install.sh
                 ${CMAKE_BINARY_DIR})
set_tests_properties(nala-self-test PROPERTIES ENVIRONMENT
  "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software")
