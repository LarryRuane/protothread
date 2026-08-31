# https://github.com/LarryRuane/protothread
# Copyright (c) 2008-present Larry Ruane
# Distributed under the MIT software license, see the accompanying
# file LICENSE or https://opensource.org/licenses/MIT.
# SPDX-License-Identifier: MIT
prefix=@CMAKE_INSTALL_PREFIX@
includedir=${prefix}/include

Name: @PROJECT_NAME@
Description: @PROJECT_DESCRIPTION@
Version: @PROJECT_VERSION@
Cflags: -I${includedir}
