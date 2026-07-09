cmake_minimum_required(VERSION 3.28...3.30)

project(obs-lan-preview VERSION 0.1.5 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "obs-lan-preview v1 is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(${PROJECT_NAME} MODULE
  src/frame_capture.cpp
  src/frame_capture.hpp
  src/jpeg_encoder.cpp
  src/jpeg_encoder.hpp
  src/mjpeg_http_server.cpp
  src/mjpeg_http_server.hpp
  src/plugin-main.cpp
  src/settings.cpp
  src/settings.hpp
  src/settings_dialog.cpp
  src/settings_dialog.hpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  OBS::libobs
  OBS::obs-frontend-api
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  ole32
  oleaut32
  windowscodecs
  ws2_32
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  OUTPUT_NAME "obs-lan-preview"
  PREFIX ""
)

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION "obs-plugins/64bit"
  LIBRARY DESTINATION "obs-plugins/64bit"
)

install(FILES README.md
  DESTINATION "data/obs-plugins/obs-lan-preview"
)

install(FILES LICENSE
  DESTINATION "data/obs-plugins/obs-lan-preview"
)



