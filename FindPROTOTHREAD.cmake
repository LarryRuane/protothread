# SPDX-License-Identifier: BSL-1.0
# NOTE: third-party file, licensed separately from the rest of this
# project (which is MIT).  See the Boost notice below.
# - try to find the protothread headers
#
# Protothread is header-only, so there is no library to find or link.
#
# Cache Variables: (probably not for direct use in your scripts)
#  PROTOTHREAD_INCLUDE_DIR
#
# Non-cache variables you might use in your CMakeLists.txt:
#  PROTOTHREAD_FOUND
#  PROTOTHREAD_INCLUDE_DIRS
#
# Requires these CMake modules:
#  FindPackageHandleStandardArgs (known included with CMake >=2.6.2)
#
# Derived from FindGPM.cmake
# Original Author:
# 2009-2010 Ryan Pavlik <rpavlik@iastate.edu> <abiryan@ryand.net>
# http://academic.cleardefinition.com
# Iowa State University HCI Graduate Program/VRAC
#
# Copyright Iowa State University 2009-2010.
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)

find_path(PROTOTHREAD_INCLUDE_DIR
    NAMES protothread.h)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PROTOTHREAD
    DEFAULT_MSG
    PROTOTHREAD_INCLUDE_DIR)

if(PROTOTHREAD_FOUND)
    set(PROTOTHREAD_INCLUDE_DIRS "${PROTOTHREAD_INCLUDE_DIR}")
endif()

