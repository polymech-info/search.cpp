cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-search
    VERSION 0.1.0
    DESCRIPTION "PolyMech Search & Indexers"
    LANGUAGES CXX)
endif()

file(GLOB PM_SEARCH_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/search/*.cpp")

# search/host.cpp is the process factory. The host compiles it once.
# A standalone build keeps the table in this library.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  list(FILTER PM_SEARCH_SOURCES EXCLUDE REGEX "search[/\\\\]host\\.cpp$")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT picosha2_SOURCE_DIR)
  include(FetchContent)
  FetchContent_Declare(picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG v1.0.0
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(picosha2)
  if(NOT picosha2_POPULATED)
    FetchContent_Populate(picosha2)
  endif()
endif()

add_library(pm-search-lib STATIC ${PM_SEARCH_SOURCES})
add_library(pm::search ALIAS pm-search-lib)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  target_compile_definitions(pm-search-lib PUBLIC PM_SEARCH_HOST_SINGLETONS=1)
endif()
target_include_directories(pm-search-lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:include>)
target_compile_features(pm-search-lib PUBLIC cxx_std_20)
target_compile_definitions(pm-search-lib PUBLIC PM_SEARCH_STATIC=1)

if(MSVC)
  target_compile_definitions(pm-search-lib PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING=1
    NOMINMAX
    _WIN32_WINNT=0x0A00)
  target_compile_options(pm-search-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-search-lib PRIVATE -Wall -Wextra)
endif()

if(NOT TARGET pm::commons)
  if(TARGET pm-commons-lib)
    add_library(pm::commons ALIAS pm-commons-lib)
  else()
    find_package(pm-commons CONFIG REQUIRED)
  endif()
endif()
# Search headers do not include commons. The static archive still needs it at link time.
target_link_libraries(pm-search-lib PRIVATE pm::commons)
if(picosha2_SOURCE_DIR)
  target_include_directories(pm-search-lib PRIVATE "${picosha2_SOURCE_DIR}")
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(pm-search-lib PUBLIC nlohmann_json::nlohmann_json)
  endif()
  if(FEATURE_CODE_TREE_SITTER AND TARGET pm-tree-sitter-cpp)
    target_compile_definitions(pm-search-lib PRIVATE FEATURE_CODE_TREE_SITTER=1)
    target_link_libraries(pm-search-lib PRIVATE pm-tree-sitter-cpp)
  endif()
  foreach(_feat IN ITEMS FEATURE_LLAMA_CUDA FEATURE_LLAMA_VULKAN FEATURE_LLAMA_METAL)
    if(DEFINED ${_feat})
      if(${_feat})
        target_compile_definitions(pm-search-lib PRIVATE ${_feat}=1)
      else()
        target_compile_definitions(pm-search-lib PRIVATE ${_feat}=0)
      endif()
    endif()
  endforeach()
else()
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set_target_properties(pm-search-lib PROPERTIES EXPORT_NAME search)
  install(TARGETS pm-search-lib
    EXPORT pm-search-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/search"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pm-search-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pm-search-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pm-search)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/pm-search-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
  install(EXPORT pm-search-targets
    FILE pm-search-targets.cmake
    NAMESPACE pm::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pm-search)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pm-search-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pm-search-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pm-search)
endif()
