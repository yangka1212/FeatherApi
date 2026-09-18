cmake_minimum_required(VERSION 3.20)
project(FeatherApi LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "FeatherApi requires Windows and the Windows SDK.")
endif()
enable_language(RC)
include(CTest)
include(cmake/MsvcDependencies.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Use the same static runtime for development and the portable release package.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

add_library(FeatherBuildOptions INTERFACE)
target_include_directories(FeatherBuildOptions INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(FeatherBuildOptions INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(FeatherBuildOptions INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/WX;/permissive-;/Gy>")
endif()

# Domain code has no Windows, persistence, network or UI dependencies.
add_library(FeatherDomain STATIC src/domain/interaction_state.cpp)
target_link_libraries(FeatherDomain PUBLIC FeatherBuildOptions)

add_library(FeatherPlatform STATIC src/platform/encoding.cpp src/platform/identity.cpp)
target_link_libraries(FeatherPlatform PUBLIC FeatherBuildOptions PRIVATE ole32)

add_library(FeatherJson STATIC src/json/json.cpp)
target_link_libraries(FeatherJson PUBLIC FeatherBuildOptions PRIVATE FeatherPlatform)

add_library(FeatherApplication STATIC src/application/openapi_import.cpp src/application/request_operations.cpp)
target_link_libraries(FeatherApplication PUBLIC FeatherDomain PRIVATE FeatherJson FeatherPlatform)

add_library(FeatherInfrastructure STATIC src/infrastructure/storage.cpp src/infrastructure/http_client.cpp)
target_link_libraries(FeatherInfrastructure PUBLIC FeatherDomain PRIVATE FeatherJson FeatherPlatform winhttp shell32 ole32)

add_library(FeatherUi STATIC src/ui/main_window.cpp)
target_include_directories(FeatherUi PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(FeatherUi PUBLIC FeatherBuildOptions PRIVATE
  FeatherApplication FeatherInfrastructure FeatherJson FeatherPlatform comctl32 comdlg32 shlwapi)

add_executable(FeatherApi WIN32 src/app/main.cpp resources.rc app.manifest)
target_link_libraries(FeatherApi PRIVATE FeatherUi FeatherInfrastructure)
install(TARGETS FeatherApi RUNTIME DESTINATION . COMPONENT Runtime)

if(BUILD_TESTING)
  add_test(NAME FeatherArchitecture COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckArchitecture.cmake")
  add_executable(FeatherApiSelfTests tests/self_tests.cpp)
  target_link_libraries(FeatherApiSelfTests PRIVATE
    FeatherApplication FeatherInfrastructure FeatherJson FeatherPlatform comctl32 ws2_32)
  add_test(NAME FeatherApiSelfTests COMMAND FeatherApiSelfTests)

  # White-box controller tests compile the UI once in the test translation unit.
  add_executable(FeatherApiUiTests tests/ui_self_tests.cpp resources.rc app.manifest)
  target_include_directories(FeatherApiUiTests PRIVATE "${PROJECT_SOURCE_DIR}")
  target_link_libraries(FeatherApiUiTests PRIVATE
    FeatherApplication FeatherInfrastructure FeatherJson FeatherPlatform comctl32 comdlg32 shlwapi)
  add_test(NAME FeatherApiUiTests COMMAND FeatherApiUiTests --test)
  set_tests_properties(FeatherApiSelfTests FeatherApiUiTests PROPERTIES TIMEOUT 60)
endif()
