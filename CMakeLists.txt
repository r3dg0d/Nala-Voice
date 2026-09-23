cmake_minimum_required(VERSION 3.22)
project(Nala VERSION 1.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# Keep the runtime path to Qt when it lives outside the system's library
# directories (a Nix store, a Qt in /opt); system paths are left out anyway.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core DBus Gui Widgets Quick Qml ShaderTools Test
  Network Multimedia Sql Concurrent)
# Wayland overlay placement on Hyprland. Optional so the app still builds and
# runs (as a plain floating window) on compositors without layer-shell.
find_package(LayerShellQt 6.7 QUIET CONFIG)
# The wake-word detector runs openWakeWord's feature models with ONNX Runtime.
# Optional: without it, wake-word mode falls back to transcribing speech.
find_package(onnxruntime QUIET CONFIG)
if(NOT onnxruntime_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(ONNXRUNTIME QUIET IMPORTED_TARGET libonnxruntime)
  endif()
endif()

# The assistant: speech, the model, tools and memory. A library so the unit
# tests link exactly the code the app runs.
qt_add_library(nala_assistant STATIC
  src/assistant/assistant.cpp src/assistant/assistant.h
  src/assistant/assistant_voice.cpp
  src/assistant/identity.cpp src/assistant/identity.h
  src/assistant/audio.cpp src/assistant/audio.h
  src/assistant/audioutil.cpp src/assistant/audioutil.h
  src/assistant/commandrouter.cpp src/assistant/commandrouter.h
  src/assistant/desktop.cpp src/assistant/desktop.h
  src/assistant/eventlog.cpp src/assistant/eventlog.h
  src/assistant/llm.cpp src/assistant/llm.h
  src/assistant/memory.cpp src/assistant/memory.h
  src/assistant/policy.cpp src/assistant/policy.h
  src/assistant/screenmemory.cpp src/assistant/screenmemory.h
  src/assistant/settings.cpp src/assistant/settings.h
  src/assistant/speech.cpp src/assistant/speech.h
  src/assistant/tools.cpp src/assistant/tools.h
  src/assistant/wakecli.cpp src/assistant/wakecli.h
  src/assistant/wakeword.cpp src/assistant/wakeword.h)
target_include_directories(nala_assistant PUBLIC src/assistant)
target_link_libraries(nala_assistant PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Multimedia Qt6::Sql Qt6::Concurrent)
target_compile_options(nala_assistant PRIVATE -Wall -Wextra -Wpedantic)
if(onnxruntime_FOUND)
  target_link_libraries(nala_assistant PUBLIC onnxruntime::onnxruntime)
  target_compile_definitions(nala_assistant PUBLIC NALA_HAVE_ONNX)
elseif(ONNXRUNTIME_FOUND)
  target_link_libraries(nala_assistant PUBLIC PkgConfig::ONNXRUNTIME)
  target_compile_definitions(nala_assistant PUBLIC NALA_HAVE_ONNX)
else()
  message(STATUS "ONNX Runtime not found: the neural wake-word detector is off")
endif()

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
  qml/Bubble.qml
  qml/MemoryTimeline.qml
  qml/Setup.qml
  qml/WakeTraining.qml
  assets/nala.svg)

target_link_libraries(nala PRIVATE nala_assistant
  Qt6::Core Qt6::DBus Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::Test)

if(LayerShellQt_FOUND)
  target_link_libraries(nala PRIVATE LayerShellQt::Interface)
  target_compile_definitions(nala PRIVATE NALA_HAVE_LAYER_SHELL
    LAYERSHELLQTINTERFACE_DISABLE_DEPRECATED_BEFORE_AND_AT=0x060600)
endif()

target_compile_options(nala PRIVATE -Wall -Wextra -Wpedantic)
# project(VERSION) above is the one place the version is written down.
target_compile_definitions(nala PRIVATE NALA_VERSION="${PROJECT_VERSION}")

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

# The assistant's own tests: routing, policy, memory, the privacy switch.
qt_add_executable(nala-assistant-tests tests/assistant_tests.cpp)
target_link_libraries(nala-assistant-tests PRIVATE nala_assistant Qt6::Test)
add_test(NAME nala-assistant-tests COMMAND nala-assistant-tests)
set_tests_properties(nala-assistant-tests PROPERTIES ENVIRONMENT
  "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;HYPRLAND_INSTANCE_SIGNATURE=")
