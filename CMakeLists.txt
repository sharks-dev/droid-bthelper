cmake_minimum_required(VERSION 3.10)
project(droid-bthelper C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(GLIBUTIL REQUIRED libglibutil)

include_directories(
  ${GLIB_INCLUDE_DIRS}
  ${GLIBUTIL_INCLUDE_DIRS}
)

link_directories(
  ${GLIB_LIBRARY_DIRS}
  ${GLIBUTIL_LIBRARY_DIRS}
)

add_definitions(
  ${GLIB_CFLAGS_OTHER}
  ${GLIBUTIL_CFLAGS_OTHER}
)

add_executable(droid-bthelper
  src/main.c
  src/bt-monitor.c
  )

target_link_libraries(
  droid-bthelper
  ${GLIB_LIBRARIES}
  ${GLIBUTIL_LIBRARIES}
)

install(TARGETS droid-bthelper DESTINATION sbin)
