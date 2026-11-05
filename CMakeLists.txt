cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-search
    VERSION 0.1.0
    DESCRIPTION "PolyMech Search & Indexers"
    LANGUAGES CXX)
endif()

file(GLOB PM_SEARCH_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/search/*.cpp")

# One factory table for the process. The chat plugin links this static lib
# and would otherwise keep an empty copy of search/host.cpp.
option(PM_SEARCH_HOST_SINGLETONS "Host compiles search/host.cpp once per process" OFF)
if(PM_SEARCH_HOST_SINGLETONS)
  list(FILTER PM_SEARCH_SOURCES EXCLUDE REGEX "search[/\\\\]host\\.cpp$")
endif()

add_library(pm-search-lib STATIC ${PM_SEARCH_SOURCES})
if(PM_SEARCH_HOST_SINGLETONS)
  target_compile_definitions(pm-search-lib PUBLIC PM_SEARCH_HOST_SINGLETONS=1)
endif()
target_include_directories(pm-search-lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
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

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(TARGET pm-commons-lib)
    target_link_libraries(pm-search-lib PUBLIC pm-commons-lib)
  endif()
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(pm-search-lib PUBLIC nlohmann_json::nlohmann_json)
  endif()
  if(picosha2_SOURCE_DIR)
    target_include_directories(pm-search-lib PRIVATE "${picosha2_SOURCE_DIR}")
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
  set(_pm_search_commons "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/host-layout.cmake")
  if(EXISTS "${_pm_search_commons}")
    include("${_pm_search_commons}")
    pm_commons_add_sibling("${CMAKE_CURRENT_BINARY_DIR}/pm-commons-lib")
    target_link_libraries(pm-search-lib PUBLIC pm-commons-lib)
  endif()
endif()
