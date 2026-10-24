# Desktop simulator + host unit tests.
#
#   cmake -S sim -B build-sim -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-sim -j
#   ./build-sim/desk_tests                         # protocol / controller / app tests
#   ./build-sim/desk_sim                           # interactive SDL window
#   ./build-sim/desk_sim --script sim/scripts/tour.sim --out shots   # headless screenshots
cmake_minimum_required(VERSION 3.20)
project(desk_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(COMPONENTS ${ROOT}/components)

# --- LVGL ---------------------------------------------------------------------
# Prefer the copy the ESP-IDF component manager downloaded, so the simulator
# renders with exactly the same LVGL version as the firmware.
set(LVGL_DIR ${ROOT}/managed_components/lvgl__lvgl)
if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
  include(FetchContent)
  FetchContent_Declare(lvgl_src GIT_REPOSITORY https://github.com/lvgl/lvgl.git GIT_TAG v9.6.0 GIT_SHALLOW TRUE)
  FetchContent_Populate(lvgl_src)
  set(LVGL_DIR ${lvgl_src_SOURCE_DIR})
endif()

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${LVGL_DIR}/src ${LVGL_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/conf)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w -O2)

# --- Portable firmware components ---------------------------------------------
add_library(desk STATIC ${COMPONENTS}/desk/protocol.cpp ${COMPONENTS}/desk/controller.cpp
                        ${COMPONENTS}/desk/sim_control_box.cpp)
target_include_directories(desk PUBLIC ${COMPONENTS}/desk/include)

add_library(app_core STATIC ${COMPONENTS}/app_core/app.cpp)
target_include_directories(app_core PUBLIC ${COMPONENTS}/app_core/include)
target_link_libraries(app_core PUBLIC desk)

file(GLOB UI_SOURCES ${COMPONENTS}/ui/*.cpp ${COMPONENTS}/ui/fonts/*.c)
add_library(ui STATIC ${UI_SOURCES})
target_include_directories(ui PUBLIC ${COMPONENTS}/ui/include PRIVATE ${COMPONENTS}/ui)
target_link_libraries(ui PUBLIC lvgl app_core)

set(WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
foreach(t desk app_core ui)
  target_compile_options(${t} PRIVATE ${WARNINGS})
endforeach()

# --- Tests ----------------------------------------------------------------------
add_executable(desk_tests ${ROOT}/test/host/test_desk.cpp)
target_link_libraries(desk_tests PRIVATE app_core)
target_compile_options(desk_tests PRIVATE ${WARNINGS})

# --- Simulator ------------------------------------------------------------------
find_package(ZLIB REQUIRED)
find_package(SDL2 QUIET)
file(GLOB SIM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(desk_sim ${SIM_SOURCES})
target_link_libraries(desk_sim PRIVATE ui ZLIB::ZLIB)
target_include_directories(desk_sim PRIVATE ${COMPONENTS}/ui)
target_compile_options(desk_sim PRIVATE ${WARNINGS})
if(SDL2_FOUND)
  target_compile_definitions(desk_sim PRIVATE SIM_HAVE_SDL=1)
  target_link_libraries(desk_sim PRIVATE SDL2::SDL2)
endif()
