//
// Copyright 2016 Pixar
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
#ifndef PXRUSDMAYA_PRIMWRITERCONTEXT_H
#define PXRUSDMAYA_PRIMWRITERCONTEXT_H

#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"

/// \class PxrUsdMayaPrimReaderContext
/// \brief This class provides an interface for writer plugins to communicate
/// state back to the core usd maya logic.
class PxrUsdMayaPrimWriterContext
{
public:

    PxrUsdMayaPrimWriterContext(
            const UsdTimeCode& timeCode,
            const SdfPath& authorPath,
            const UsdStageRefPtr& stage);

    /// \brief returns the time frame where data should be authored.
    const UsdTimeCode& GetTimeCode() const;

    /// \brief returns the path where the writer plugin should create 
    /// a prim.
    const SdfPath& GetAuthorPath() const;

    /// \brief returns the usd stage that is being written to.
    UsdStageRefPtr GetUsdStage() const; 
    
    /// \brief Returns the value provided by SetExportsGprims(), or \c false
    /// if SetExportsGprims() is not called.  
    ///
    /// May be used by export processes to reason about what kind of asset we
    /// are creating.
    bool GetExportsGprims() const;
    
    /// \brief Returns the value provided by SetExportsReferences(), or \c false
    /// if SetExportsReferences() is not called.  
    ///
    /// May be used by export processes to reason about what kind of asset we
    /// are creating.
    bool GetExportsReferences() const;
    
    /// Set the value that will be returned by GetExportsGprims().
    ///
    /// A plugin should set this to \c true if it directly creates any
    /// gprims, and should return the same value each time its write() 
    /// function is invoked.
    ///
    /// \sa GetExportsGprims()
    void SetExportsGprims(bool exportsGprims);

    /// Set the value that will be returned by GetExportsReferences().
    ///
    /// A plugin should set this to \c true if it adds any references,
    /// and should return the same value each time its write() 
    /// function is invoked.
    ///
    /// \sa GetExportsReferences
    void SetExportsReferences(bool exportsReferences);

    /// Set the value that will be returned by GetPruneChildren().
    ///
    /// A plugin should set this to \c true if it will handle writing
    /// child prims by itself, or if it does not wish for any children of
    /// the current node to be traversed by the export process.
    ///
    /// This should be called during the initial (unvarying) export for it
    /// to be considered by the export process. If it is called during the
    /// animated (varying) export, it will be ignored.
    void SetPruneChildren(bool pruneChildren);

    /// \brief Returns the value provided by SetPruneChildren(), or \c false
    /// if SetPruneChildren() is not called.
    ///
    /// Export processes should prune all descendants of the current node
    /// during traversal if this is set to \c true.
    bool GetPruneChildren() const;

private:
    const UsdTimeCode& _timeCode;
    const SdfPath& _authorPath;
    UsdStageRefPtr _stage;
    bool _exportsGprims;
    bool _exportsReferences;
    bool _pruneChildren;
};

#endif // PXRUSDMAYA_PRIMWRITERCONTEXT_H

