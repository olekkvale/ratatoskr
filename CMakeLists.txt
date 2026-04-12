cmake_minimum_required(VERSION 3.16)
project(ratatoskr VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)
pkg_check_modules(SDBUS REQUIRED sdbus-c++)
pkg_check_modules(UDEV REQUIRED libudev)

# Daemon executable
add_executable(ratatoskrd
    src/main.cpp
    src/device_manager.cpp
    src/hid_device.cpp
    src/drivers/a50_gen5.cpp
)

target_include_directories(ratatoskrd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${HIDAPI_INCLUDE_DIRS}
    ${SDBUS_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
)

target_link_libraries(ratatoskrd PRIVATE
    ${HIDAPI_LIBRARIES}
    ${SDBUS_LIBRARIES}
    ${UDEV_LIBRARIES}
    pthread
)

# Install
install(TARGETS ratatoskrd DESTINATION bin)
install(FILES data/ratatoskrd.service DESTINATION lib/systemd/system)
install(FILES data/org.ratatoskr.conf DESTINATION share/dbus-1/system.d)
install(FILES data/90-ratatoskr.rules DESTINATION lib/udev/rules.d)
