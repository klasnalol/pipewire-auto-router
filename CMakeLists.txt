cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" APP_VERSION LIMIT_COUNT 1)
project(pipewire_auto_router_qt VERSION "${APP_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(pipewire-auto-router-qt
    src/qt_main.cpp
)

target_link_libraries(pipewire-auto-router-qt PRIVATE Qt6::Widgets)
target_compile_definitions(pipewire-auto-router-qt PRIVATE
    APP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    APP_VERSION="${PROJECT_VERSION}"
)
