cmake_minimum_required(VERSION 3.20)
project(psvr_steamvr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "This driver is Windows-only.")
endif()

add_library(driver_psvr SHARED
  src/hmd_driver_factory.cpp
  src/device_provider.cpp
  src/hmd_device.cpp
  src/display_component.cpp
  src/psvr_hw.cpp
  src/gearvr_ble.cpp
  src/controller_device.cpp
  src/wmr_hid.cpp
  src/wmr_controller_device.cpp
  src/madgwick.cpp
  src/driverlog.cpp
  src/driver_psvr.def
)

target_include_directories(driver_psvr PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/openvr/headers
)

target_compile_definitions(driver_psvr PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

target_link_libraries(driver_psvr PRIVATE
  setupapi
  winusb
  hid
  cfgmgr32
  windowsapp
)

set_target_properties(driver_psvr PROPERTIES
  OUTPUT_NAME driver_psvr
  PREFIX ""
)

add_executable(psvr_ctl src/psvr_ctl.cpp src/psvr_hw.cpp src/gearvr_ble.cpp src/madgwick.cpp src/driverlog.cpp)
target_include_directories(psvr_ctl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(psvr_ctl PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  PSVR_STANDALONE
)
target_link_libraries(psvr_ctl PRIVATE setupapi winusb hid cfgmgr32 windowsapp)

# HMD-free optical smoke-test path: PlayStation Camera + Odyssey/WMR controllers only.
add_executable(psvr_odyssey_optical_test
  src/psvr_odyssey_optical_test.cpp
  src/psvr_camera_capture.cpp
  src/led_blob_detector.cpp
  src/stereo_led_tracker.cpp
  src/wmr_hid.cpp
  src/madgwick.cpp
  src/driverlog.cpp
)
target_include_directories(psvr_odyssey_optical_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(psvr_odyssey_optical_test PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  PSVR_STANDALONE
)
target_link_libraries(psvr_odyssey_optical_test PRIVATE
  setupapi
  hid
  cfgmgr32
  ole32
  mfplat
  mfreadwrite
  mfuuid
)

set(DRIVER_DIR ${CMAKE_BINARY_DIR}/dist/psvr)

add_custom_command(TARGET driver_psvr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DRIVER_DIR}/bin/win64
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:driver_psvr> ${DRIVER_DIR}/bin/win64/driver_psvr.dll
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/driver/driver.vrdrivermanifest ${DRIVER_DIR}/driver.vrdrivermanifest
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/driver/resources ${DRIVER_DIR}/resources
  COMMENT "Assembling SteamVR driver folder"
)

add_custom_command(TARGET psvr_ctl POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DRIVER_DIR}/bin/win64
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:psvr_ctl> ${DRIVER_DIR}/bin/win64/psvr_ctl.exe
)

add_custom_command(TARGET psvr_odyssey_optical_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DRIVER_DIR}/bin/win64
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:psvr_odyssey_optical_test> ${DRIVER_DIR}/bin/win64/psvr_odyssey_optical_test.exe
)
