#
# Copyright 2016 Pixar
#
# Licensed under the Apache License, Version 2.0 (the "Apache License")
# with the following modification; you may not use this file except in
# compliance with the Apache License and the following modification to it:
# Section 6. Trademarks. is deleted and replaced with:
#
# 6. Trademarks. This License does not grant permission to use the trade
#    names, trademarks, service marks, or product names of the Licensor
#    and its affiliates, except as required to comply with Section 4(c) of
#    the License and to reproduce the content of the NOTICE file.
#
# You may obtain a copy of the Apache License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the Apache License with the above modification is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied. See the Apache License for the specific
# language governing permissions and limitations under the Apache License.
#

# This file contains a set of flags/settings shared between our 
# GCC and Clang configs. This allows clangdefaults and gccdefaults
# to remain minimal, marking the points where divergence is required.
include(Options)

# By default, Release flavor builds in cmake set NDEBUG, which
# breaks things internally.  Turn it off.
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# Enable all warnings
_add_warning_flag("all")
# We use hash_map, suppress deprecation warning.
_add_warning_flag("no-deprecated")
_add_warning_flag("no-deprecated-declarations")

message("INFO CMAKE_COMPILER_IS_CLANG: ${CMAKE_COMPILER_IS_CLANG}, CLANG_VERSION_STRING: ${CLANG_VERSION_STRING}, APPLECLANG_VERSION_STRING: ${APPLECLANG_VERSION_STRING}")
message("INFO CLANG_VERSION_MAJOR: ${CLANG_VERSION_MAJOR}, CLANG_VERSION_MINOR: ${CLANG_VERSION_MINOR}")
message("INFO CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
if(CMAKE_COMPILER_IS_CLANG)
  message(CMAKE_COMPILER_IS_CLANG)
endif()
if(CLANG_VERSION_STRING)
  message(CLANG_VERSION_STRING)
endif()
if(APPLECLANG_VERSION_STRING)
  message(APPLECLANG_VERSION_STRING)
endif()

# Suppress unused typedef warnings eminating from boost.
if (NOT CMAKE_COMPILER_IS_CLANG OR (CLANG_VERSION_STRING VERSION_GREATER 3.5 OR
                                    APPLECLANG_VERSION_STRING VERSION_GREATER 6.1))
    _add_warning_flag("no-unused-local-typedefs")
endif()

# Turn on C++11, pxr won't build without it. 
set(_PXR_GCC_CLANG_SHARED_CXX_FLAGS "-std=c++11")

if (${PXR_MAYA_TBB_BUG_WORKAROUND})
    set(_PXR_GCC_CLANG_SHARED_CXX_FLAGS "${_PXR_GCC_CLANG_SHARED_CXX_FLAGS} -Wl,-Bsymbolic")
endif()

if (${PXR_STRICT_BUILD_MODE})
    _add_warning_flag("error")
endif()
