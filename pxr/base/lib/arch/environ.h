//
// Copyright 2017 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#ifndef ARCH_ENVIRON_H
#define ARCH_ENVIRON_H

/// \file arch/environ.h
/// \ingroup group_arch_SystemFunctions
/// Functions for dealing with system environment.

#include "pxr/base/arch/api.h"

#if defined(ARCH_OS_DARWIN)
#include <crt_externs.h>
#elif defined(__cplusplus)
extern "C" { extern char** environ; }
#else
extern char** environ;
#endif

/// \addtogroup group_arch_SystemFunctions
///@{

/// Return an array of the environment variables.
///
inline char** ArchEnviron() {
#if defined(ARCH_OS_DARWIN)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

///@}

#endif // ARCH_ENVIRON_H
