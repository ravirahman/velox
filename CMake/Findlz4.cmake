# Copyright (c) Facebook, Inc. and its affiliates.
# - Try to find lz4
# Once done, this will define
#
# LZ4_FOUND - system has Glog
# LZ4_INCLUDE_DIRS - deprecated
# LZ4_LIBRARIES -  deprecated
# lz4::lz4 will be defined based on CMAKE_FIND_LIBRARY_SUFFIXES priority

include(FindPackageHandleStandardArgs)
include(SelectLibraryConfigurations)

find_library(LZ4_LIBRARY_RELEASE lz4 PATHS $LZ4_LIBRARYDIR})
find_library(LZ4_LIBRARY_DEBUG lz4d PATHS ${LZ4_LIBRARYDIR})

find_path(LZ4_INCLUDE_DIR lz4.h PATHS ${LZ4_INCLUDEDIR})

select_library_configurations(LZ4)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(LZ4_LIBRARIES "${LZ4_LIBRARY_RELEASE}")
else ()
    set(LZ4_LIBRARIES "${LZ4_LIBRARY_DEBUG}")
endif ()

find_package_handle_standard_args(lz4 DEFAULT_MSG LZ4_LIBRARY LZ4_INCLUDE_DIR)

mark_as_advanced(LZ4_LIBRARY LZ4_INCLUDE_DIR)
set(liblz4_type STATIC)

if(NOT TARGET lz4::lz4)
  add_library(lz4::lz4 ${liblz4_type} IMPORTED)
  set_target_properties(lz4::lz4 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${LZ4_INCLUDE_DIR}")
  set_target_properties(
    lz4::lz4 PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                        IMPORTED_LOCATION "${LZ4_LIBRARIES}")
endif()
