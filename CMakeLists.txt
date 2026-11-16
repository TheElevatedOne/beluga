cmake_minimum_required(VERSION 3.16)
project(beluga VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BELUGA_INSTALL_FILEMANAGER1_SERVICE
  "Install a D-Bus service file so org.freedesktop.FileManager1 starts Beluga" OFF)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET glib-2.0>=2.84 gio-2.0 gio-unix-2.0 gdk-pixbuf-2.0)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.16)

set(BELUGA_APP_ID "org.beluga.Beluga")
set(BELUGA_WARN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
set(BELUGA_DEFS
  GETTEXT_PACKAGE="beluga"
  BELUGA_VERSION="${PROJECT_VERSION}"
  BELUGA_APP_ID="${BELUGA_APP_ID}"
  BELUGA_LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
  GDK_DISABLE_DEPRECATION_WARNINGS)

# File-manager engine: settings, places, trash, file operations, search,
# thumbnails, devices. GLib/GIO only, no GTK.
add_library(belugacore STATIC
  src/core/util.cpp
  src/core/settings.cpp
  src/core/viewprops.cpp
  src/core/places.cpp
  src/core/trash.cpp
  src/core/fileops.cpp
  src/core/undo.cpp
  src/core/search.cpp
  src/core/thumbnails.cpp
  src/core/terminal.cpp
  src/core/devices.cpp
  src/core/customactions.cpp
  src/core/dirsize.cpp
)
target_include_directories(belugacore PUBLIC src/core)
target_compile_definitions(belugacore PUBLIC ${BELUGA_DEFS})
target_compile_options(belugacore PRIVATE ${BELUGA_WARN})
target_link_libraries(belugacore PUBLIC PkgConfig::GIO Threads::Threads)

add_executable(beluga
  src/gui/main.cpp
  src/gui/app.cpp
  src/gui/style.cpp
  src/gui/item.cpp
  src/gui/dirmodel.cpp
  src/gui/itemview.cpp
  src/gui/view.cpp
  src/gui/viewcontainer.cpp
  src/gui/locationbar.cpp
  src/gui/statusbar.cpp
  src/gui/searchbar.cpp
  src/gui/tabpage.cpp
  src/gui/mainwindow.cpp
  src/gui/mainwindow_actions.cpp
  src/gui/contextmenu.cpp
  src/gui/placespanel.cpp
  src/gui/folderspanel.cpp
  src/gui/infopanel.cpp
  src/gui/jobs.cpp
  src/gui/dialogs.cpp
  src/gui/properties.cpp
  src/gui/openwith.cpp
  src/gui/preferences.cpp
  src/gui/selftest.cpp
)
target_include_directories(beluga PRIVATE src/gui)
target_compile_options(beluga PRIVATE ${BELUGA_WARN})
target_link_libraries(beluga PRIVATE belugacore PkgConfig::GTK4)

install(TARGETS beluga RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(data/beluga.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/${BELUGA_APP_ID}.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${BELUGA_APP_ID}.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${BELUGA_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
if(BELUGA_INSTALL_FILEMANAGER1_SERVICE)
  configure_file(data/org.freedesktop.FileManager1.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.FileManager1.service @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.FileManager1.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
endif()

# Translations: po/<lang>.po files are compiled and installed when present.
# "cmake --build build --target pot" refreshes po/beluga.pot from the sources.
find_program(MSGFMT msgfmt)
find_program(XGETTEXT xgettext)
file(GLOB BELUGA_PO_FILES ${CMAKE_SOURCE_DIR}/po/*.po)
if(MSGFMT AND BELUGA_PO_FILES)
  set(BELUGA_MO_FILES)
  foreach(po ${BELUGA_PO_FILES})
    get_filename_component(lang ${po} NAME_WE)
    set(mo ${CMAKE_CURRENT_BINARY_DIR}/po/${lang}.mo)
    add_custom_command(OUTPUT ${mo}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/po
      COMMAND ${MSGFMT} -o ${mo} ${po} DEPENDS ${po})
    install(FILES ${mo} DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${lang}/LC_MESSAGES RENAME beluga.mo)
    list(APPEND BELUGA_MO_FILES ${mo})
  endforeach()
  add_custom_target(translations ALL DEPENDS ${BELUGA_MO_FILES})
endif()
if(XGETTEXT)
  file(GLOB BELUGA_I18N_SOURCES RELATIVE ${CMAKE_SOURCE_DIR} src/core/*.cpp src/gui/*.cpp)
  add_custom_target(pot
    COMMAND ${XGETTEXT} --from-code=UTF-8 --c++ -k_ -kN_ -kg_dngettext:2,3 --package-name=beluga -o po/beluga.pot ${BELUGA_I18N_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
