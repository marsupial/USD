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
#include "pxr/imaging/glf/glew.h"

#include "pxr/imaging/hdSt/basisCurves.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/tf/envSetting.h"

#include "pxr/imaging/hd/basisCurvesShaderKey.h"
#include "pxr/imaging/hd/bufferSource.h"
#include "pxr/imaging/hd/computation.h"
#include "pxr/imaging/hd/geometricShader.h"
#include "pxr/imaging/hd/meshTopology.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/quadrangulate.h"
#include "pxr/imaging/hd/repr.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hd/basisCurvesComputations.h"
#include "pxr/base/vt/value.h"

TF_DEFINE_ENV_SETTING(HD_ENABLE_REFINED_CURVES, 0, 
                      "Force curves to always be refined.");

// static repr configuration
HdStBasisCurves::_BasisCurvesReprConfig HdStBasisCurves::_reprDescConfig;

HdStBasisCurves::HdStBasisCurves(HdSceneDelegate* delegate, SdfPath const& id,
                 SdfPath const& instancerId)
    : HdBasisCurves(delegate, id, instancerId)
    , _topologyId(0)
    , _customDirtyBitsInUse(0)
    , _refineLevel(0)
{
    /*NOTHING*/
}


HdStBasisCurves::~HdStBasisCurves()
{
    /*NOTHING*/
}

/* static */
bool
HdStBasisCurves::IsEnabledForceRefinedCurves()
{
    return TfGetEnvSetting(HD_ENABLE_REFINED_CURVES) == 1;
}

void
HdStBasisCurves::_UpdateDrawItem(HdDrawItem *drawItem,
                               HdChangeTracker::DirtyBits *dirtyBits,
                               const HdStBasisCurvesReprDesc &desc)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();

    /* VISIBILITY */
    _UpdateVisibility(dirtyBits);

    /* CONSTANT PRIMVARS, TRANSFORM AND EXTENT */
    _PopulateConstantPrimVars(drawItem, dirtyBits);

    /* INSTANCE PRIMVARS */
    _PopulateInstancePrimVars(drawItem, dirtyBits, InstancePrimVar);

    /* TOPOLOGY */
    // XXX: _PopulateTopology should be split into two phase
    //      for scene dirtybits and for repr dirtybits.
    if (*dirtyBits & (HdChangeTracker::DirtyTopology
                    | HdChangeTracker::DirtyRefineLevel
                    | DirtyIndices
                    | DirtyHullIndices)) {
        _PopulateTopology(drawItem, dirtyBits, desc);
    }

    /* PRIMVAR */
    if (HdChangeTracker::IsAnyPrimVarDirty(*dirtyBits, id)) {
        // XXX: curves don't use refined vertex primvars, however,
        // the refined renderpass masks the dirtiness of non-refined vertex
        // primvars, so we need to see refined dirty for updating coarse
        // vertex primvars if there is only refined reprs being updated.
        // we'll fix the change tracking in order to address this craziness.
        _PopulateVertexPrimVars(drawItem, dirtyBits);
        _PopulateElementPrimVars(drawItem, dirtyBits);
    }

    // Topology and VertexPrimVar may be null, if the curve has zero line
    // segments.
    TF_VERIFY(drawItem->GetConstantPrimVarRange());
}

void
HdStBasisCurves::_UpdateDrawItemGeometricShader(HdDrawItem *drawItem,
                                                const HdStBasisCurvesReprDesc &desc)
{
    if (drawItem->GetGeometricShader()) return;

    if (!TF_VERIFY(_topology)) return;

    // Check for authored normals, we could leverage dirtyBits here as an
    // optimization, however the BAR is the ground truth, so until there is a
    // known peformance issue, we just check them explicitly.
    bool hasAuthoredNormals = false;

    if (!hasAuthoredNormals) {
        // Check if we picked up normals on a previous update.
        typedef HdBufferArrayRangeSharedPtr HdBarPtr;
        if (HdBarPtr const& bar = drawItem->GetConstantPrimVarRange())
            hasAuthoredNormals |= bool(bar->GetResource(HdTokens->normals));
        if (HdBarPtr const& bar = drawItem->GetVertexPrimVarRange())
            hasAuthoredNormals |= bool(bar->GetResource(HdTokens->normals));
        if (HdBarPtr const& bar = drawItem->GetElementPrimVarRange())
            hasAuthoredNormals |= bool(bar->GetResource(HdTokens->normals));
        int instanceNumLevels = drawItem->GetInstancePrimVarNumLevels();
        for (int i = 0; i < instanceNumLevels; ++i) {
            if (HdBarPtr const& bar = drawItem->GetInstancePrimVarRange(i))
                hasAuthoredNormals |= bool(bar->GetResource(HdTokens->normals));
        }
    }

    Hd_BasisCurvesShaderKey shaderKey(_topology->GetCurveBasis(),
                                      hasAuthoredNormals,
                                      (_SupportsSmoothCurves(desc, _refineLevel)));

    drawItem->SetGeometricShader(Hd_GeometricShader::Create(shaderKey));
}


/* static */
void
HdStBasisCurves::ConfigureRepr(TfToken const &reprName,
                               HdStBasisCurvesReprDesc desc)
{
    HD_TRACE_FUNCTION();

    if (IsEnabledForceRefinedCurves()) {
        desc.geomStyle = HdBasisCurvesGeomStyleRefined;
    }

    _reprDescConfig.Append(reprName, _BasisCurvesReprConfig::DescArray{{desc}});
}

HdChangeTracker::DirtyBits
HdStBasisCurves::_PropagateDirtyBits(HdChangeTracker::DirtyBits dirtyBits)
{
    // propagate scene-based dirtyBits into rprim-custom dirtyBits
    if (dirtyBits & HdChangeTracker::DirtyTopology) {
        dirtyBits |= _customDirtyBitsInUse &
            (DirtyIndices|DirtyHullIndices);
    }

    return dirtyBits;
}

HdReprSharedPtr const &
HdStBasisCurves::_GetRepr(TfToken const &reprName,
                        HdChangeTracker::DirtyBits *dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    _BasisCurvesReprConfig::DescArray descs = _reprDescConfig.Find(reprName);

    _ReprVector::iterator it = std::find_if(_reprs.begin(), _reprs.end(),
                                            _ReprComparator(reprName));
    bool isNew = it == _reprs.end();
    if (isNew) {
        // add new repr
        it = _reprs.insert(_reprs.end(),
                           std::make_pair(reprName, HdReprSharedPtr(new HdRepr())));

        // allocate all draw items
        for (auto desc : descs) {
            if (desc.geomStyle == HdBasisCurvesGeomStyleInvalid) {
                continue;
            }

            HdDrawItem *drawItem = it->second->AddDrawItem(&_sharedData);
            if (desc.geomStyle == HdBasisCurvesGeomStyleLine) {
                HdDrawingCoord *drawingCoord = drawItem->GetDrawingCoord();
                drawingCoord->SetTopologyIndex(HdStBasisCurves::HullTopology);
                if (!(_customDirtyBitsInUse & DirtyHullIndices)) {
                    _customDirtyBitsInUse |= DirtyHullIndices;
                    *dirtyBits |= DirtyHullIndices;
                }
            } else {
                if (!(_customDirtyBitsInUse & DirtyIndices)) {
                    _customDirtyBitsInUse |= DirtyIndices;
                    *dirtyBits |= DirtyIndices;
                }
            }
        }
    }

    *dirtyBits = _PropagateDirtyBits(*dirtyBits);

    if (TfDebug::IsEnabled(HD_RPRIM_UPDATED)) {
        std::cout << "HdStBasisCurves::GetRepr " << GetId()
                  << " Repr = " << reprName << "\n";
        HdChangeTracker::DumpDirtyBits(*dirtyBits);
    }

    // for the bits geometric shader depends on, reset all geometric shaders.
    // they are populated again at the end of _GetRepr.
    if (*dirtyBits & (HdChangeTracker::DirtyRefineLevel)) {
        _ResetGeometricShaders();
    }

    // curves don't have multiple draw items (for now)
    if (isNew || HdChangeTracker::IsDirty(*dirtyBits)) {
        if (descs[0].geomStyle != HdPointsGeomStyleInvalid) {
            HdDrawItem *drawItem = it->second->GetDrawItem(0);
            _UpdateDrawItem(drawItem, dirtyBits, descs[0]);
            _UpdateDrawItemGeometricShader(drawItem, descs[0]);
        }
    }

    // if we need to rebuild geometric shader, make sure all reprs to have
    // their geometric shader up-to-date.
    if (*dirtyBits & (HdChangeTracker::DirtyRefineLevel)) {
        _SetGeometricShaders();
    }

    return it->second;
}

void
HdStBasisCurves::_ResetGeometricShaders()
{
    TF_FOR_ALL (it, _reprs) {
        TF_FOR_ALL (drawItem, *(it->second->GetDrawItems())) {
            drawItem->SetGeometricShader(Hd_GeometricShaderSharedPtr());
        }
    }
}

void
HdStBasisCurves::_SetGeometricShaders()
{
    TF_FOR_ALL (it, _reprs) {
        _BasisCurvesReprConfig::DescArray descs = _reprDescConfig.Find(it->first);
        int drawItemIndex = 0;
        for (auto desc : descs) {
            if (desc.geomStyle == HdBasisCurvesGeomStyleInvalid) continue;

            HdDrawItem *drawItem = it->second->GetDrawItem(drawItemIndex);
            _UpdateDrawItemGeometricShader(drawItem, desc);
            ++drawItemIndex;
        }
    }
}

void
HdStBasisCurves::_PopulateTopology(HdDrawItem *drawItem,
                                   HdChangeTracker::DirtyBits *dirtyBits,
                                   const HdStBasisCurvesReprDesc &desc)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();
    SdfPath const& id = GetId();
    HdResourceRegistry *resourceRegistry = &HdResourceRegistry::GetInstance();

    if (*dirtyBits & HdChangeTracker::DirtyRefineLevel) {
        _refineLevel = GetRefineLevel();
    }

    // XXX: is it safe to get topology even if it's not dirty?
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id) ||
        HdChangeTracker::IsRefineLevelDirty(*dirtyBits, id)) {

        HdBasisCurvesTopologySharedPtr topology(
            new HdBasisCurvesTopology(GetBasisCurvesTopology()));

        // compute id.
        _topologyId = topology->ComputeHash();
        boost::hash_combine(_topologyId, (bool)(_refineLevel>0));

        HdInstance<HdTopology::ID, HdBasisCurvesTopologySharedPtr> topologyInstance;

        // ask registry if there's a sharable mesh topology
        std::unique_lock<std::mutex> regLock =
            resourceRegistry->RegisterBasisCurvesTopology(_topologyId, &topologyInstance);

        if (topologyInstance.IsFirstInstance()) {
            // if this is the first instance, set this topology to registry.
            topologyInstance.SetValue(topology);
        }

        _topology = topologyInstance.GetValue();
        TF_VERIFY(_topology);

        // hash collision check
        if (TfDebug::IsEnabled(HD_SAFE_MODE)) {
            TF_VERIFY(*topology == *_topology);
        }
    }

    // bail out if the index bar is already synced
    TfToken indexToken;
    if (drawItem->GetDrawingCoord()->GetTopologyIndex()
        == HdStBasisCurves::HullTopology) {
        if ((*dirtyBits & DirtyHullIndices) == 0) return;
        *dirtyBits &= ~DirtyHullIndices;
        indexToken = HdTokens->hullIndices;
    } else {
        if ((*dirtyBits & DirtyIndices) == 0) return;
        *dirtyBits &= ~DirtyIndices;
        indexToken = HdTokens->indices;
    }

    {
        HdInstance<HdTopology::ID, HdBufferArrayRangeSharedPtr> rangeInstance;

        std::unique_lock<std::mutex> regLock =
            resourceRegistry->RegisterBasisCurvesIndexRange(
                _topologyId, indexToken, &rangeInstance);

        if(rangeInstance.IsFirstInstance()) {
            HdBufferSourceVector sources;
            HdBufferSpecVector bufferSpecs;

            bool refine = _SupportsSmoothCurves(desc, _refineLevel);

            sources.push_back(_topology->GetIndexBuilderComputation(refine));

            TF_FOR_ALL(it, sources) {
                (*it)->AddBufferSpecs(&bufferSpecs);
            }

            // allocate new range
            HdBufferArrayRangeSharedPtr range
                = resourceRegistry->AllocateNonUniformBufferArrayRange(
                    HdTokens->topology, bufferSpecs);

            // add sources to update queue
            resourceRegistry->AddSources(range, sources);
            rangeInstance.SetValue(range);
        }

        _sharedData.barContainer.Set(
            drawItem->GetDrawingCoord()->GetTopologyIndex(),
            rangeInstance.GetValue());
    }
}

void
HdStBasisCurves::_PopulateVertexPrimVars(HdDrawItem *drawItem,
                                       HdChangeTracker::DirtyBits *dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();
    HdResourceRegistry *resourceRegistry = &HdResourceRegistry::GetInstance();

    // The "points" attribute is expected to be in this list.
    TfTokenVector primVarNames = GetPrimVarVertexNames();
    TfTokenVector const& vars = GetPrimVarVaryingNames();
    primVarNames.insert(primVarNames.end(), vars.begin(), vars.end());

    HdBufferSourceVector sources;
    sources.reserve(primVarNames.size());

    TF_FOR_ALL(nameIt, primVarNames) {
        if (!HdChangeTracker::IsPrimVarDirty(*dirtyBits, id, *nameIt))
            continue;

        // TODO: We don't need to pull primvar metadata every time a value
        // changes, but we need support from the delegate.

        //assert name not in range.bufferArray.GetResources()
        VtValue value = GetPrimVar(*nameIt);
        if (!value.IsEmpty()) {
            if (*nameIt == HdTokens->points) {
                // We want to validate the topology by making sure the number of
                // verts is equal or greater than the number of verts the topology
                // references
                if (!_topology) {
                    TF_CODING_ERROR("No topology set for BasisCurve %s",
                                    id.GetName().c_str());
                }
                else if(!value.IsHolding<VtVec3fArray>() ||
                        (!_topology->HasIndices() &&
                        value.Get<VtVec3fArray>().size() != _topology->CalculateNeededNumberOfControlPoints())) {
                    TF_WARN("Topology and vertices do not match for "
                            "BasisCurve %s",id.GetName().c_str());
                }
            }

            // XXX: this really needs to happen for all primvars.
            if (*nameIt == HdTokens->widths) {
                sources.push_back(HdBufferSourceSharedPtr(
                        new Hd_BasisCurvesWidthsInterpolaterComputation(
                                      _topology.get(), value.Get<VtFloatArray>())));
            } else if (*nameIt == HdTokens->normals) {
                sources.push_back(HdBufferSourceSharedPtr(
                        new Hd_BasisCurvesNormalsInterpolaterComputation(
                                      _topology.get(), value.Get<VtVec3fArray>())));
            } else {
                sources.push_back(HdBufferSourceSharedPtr(
                        new HdVtBufferSource(*nameIt, value)));
            }
        }
    }

    // return before allocation if it's empty.
    if (sources.empty()) {
        return;
    }

    if (!drawItem->GetVertexPrimVarRange() ||
        !drawItem->GetVertexPrimVarRange()->IsValid()) {
        // initialize buffer array
        HdBufferSpecVector bufferSpecs;
        TF_FOR_ALL(it, sources) {
            (*it)->AddBufferSpecs(&bufferSpecs);
        }

        HdBufferArrayRangeSharedPtr range =
            resourceRegistry->AllocateNonUniformBufferArrayRange(
                HdTokens->primVar, bufferSpecs);
        _sharedData.barContainer.Set(
            drawItem->GetDrawingCoord()->GetVertexPrimVarIndex(), range);
    }

    // add sources to update queue
    resourceRegistry->AddSources(drawItem->GetVertexPrimVarRange(),
                                 sources);
}

void
HdStBasisCurves::_PopulateElementPrimVars(HdDrawItem *drawItem,
                                        HdChangeTracker::DirtyBits *dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();
    HdResourceRegistry *resourceRegistry = &HdResourceRegistry::GetInstance();

    TfTokenVector primVarNames = GetPrimVarUniformNames();

    HdBufferSourceVector sources;
    sources.reserve(primVarNames.size());

    TF_FOR_ALL(nameIt, primVarNames) {
        if (!HdChangeTracker::IsPrimVarDirty(*dirtyBits, id, *nameIt))
            continue;

        VtValue value = GetPrimVar(*nameIt);
        if (!value.IsEmpty()) {
            sources.push_back(HdBufferSourceSharedPtr(
                              new HdVtBufferSource(*nameIt, value)));
        }
    }

    // return before allocation if it's empty.
    if (sources.empty())
        return;

    // element primvars exist.
    if (!drawItem->GetElementPrimVarRange() ||
        !drawItem->GetElementPrimVarRange()->IsValid()) {
        HdBufferSpecVector bufferSpecs;
        TF_FOR_ALL(it, sources) {
            (*it)->AddBufferSpecs(&bufferSpecs);
        }
        HdBufferArrayRangeSharedPtr range =
            resourceRegistry->AllocateNonUniformBufferArrayRange(
                HdTokens->primVar, bufferSpecs);
        _sharedData.barContainer.Set(
            drawItem->GetDrawingCoord()->GetElementPrimVarIndex(), range);
    }

    resourceRegistry->AddSources(drawItem->GetElementPrimVarRange(),
                                 sources);
}


bool
HdStBasisCurves::_SupportsSmoothCurves(const HdStBasisCurvesReprDesc &desc,
                                       int refineLevel)
{
    if(!_topology) {
        TF_CODING_ERROR("Calling _SupportsSmoothCurves before topology is set");
        return false;
    }

    if (desc.geomStyle != HdBasisCurvesGeomStyleRefined) {
        return false;
    }

    TfToken curveType = _topology->GetCurveType(); 
    TfToken curveBasis = _topology->GetCurveBasis(); 

    if(curveType == HdTokens->cubic &&
       (curveBasis == HdTokens->bezier || 
        curveBasis == HdTokens->bSpline || 
        curveBasis == HdTokens->catmullRom)) {

        if (refineLevel > 0 || IsEnabledForceRefinedCurves()) {
            return true;
        }
    }

    return false;
}

/*static*/
int
HdStBasisCurves::GetDirtyBitsMask(TfToken const &reprName)
{
    int mask = HdChangeTracker::Clean;
    _BasisCurvesReprConfig::DescArray descs = _reprDescConfig.Find(reprName);

    for (auto desc : descs) {
        if (desc.geomStyle == HdBasisCurvesGeomStyleInvalid) {
            continue;
        }

        mask |= HdChangeTracker::DirtyNormals
             |  HdChangeTracker::DirtyPoints
             |  HdChangeTracker::DirtyPrimVar
             |  HdChangeTracker::DirtyRefineLevel
             |  HdChangeTracker::DirtyTopology
             |  HdChangeTracker::DirtyWidths;
    }

    return mask;
}

HdChangeTracker::DirtyBits 
HdStBasisCurves::_GetInitialDirtyBits() const
{
    int mask = HdChangeTracker::Clean
        | HdChangeTracker::DirtyExtent
        | HdChangeTracker::DirtyInstanceIndex
        | HdChangeTracker::DirtyNormals
        | HdChangeTracker::DirtyPoints
        | HdChangeTracker::DirtyPrimID
        | HdChangeTracker::DirtyPrimVar
        | HdChangeTracker::DirtyRefineLevel
        | HdChangeTracker::DirtyRepr
        | HdChangeTracker::DirtySurfaceShader
        | HdChangeTracker::DirtyTopology
        | HdChangeTracker::DirtyTransform 
        | HdChangeTracker::DirtyVisibility 
        | HdChangeTracker::DirtyWidths
        ;

    return (HdChangeTracker::DirtyBits)mask;
}
