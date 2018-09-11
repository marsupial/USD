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
#include "pxr/imaging/glf/glew.h"
#include "pxr/imaging/hdEmbree/mesh.h"

#include "pxr/imaging/hdEmbree/context.h"
#include "pxr/imaging/hdEmbree/instancer.h"
#include "pxr/imaging/hdEmbree/renderParam.h"
#include "pxr/imaging/hdEmbree/renderPass.h"
#include "pxr/imaging/hd/extComputationUtils.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/pxOsd/tokens.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/matrix4d.h"

#include <algorithm> // sort

PXR_NAMESPACE_OPEN_SCOPE

HdEmbreeMesh::HdEmbreeMesh(SdfPath const& id,
                           SdfPath const& instancerId)
    : HdMesh(id, instancerId)
    , _rtcMeshId(RTC_INVALID_GEOMETRY_ID)
    , _rtcMeshScene(nullptr)
    , _adjacencyValid(false)
    , _normalsValid(false)
    , _refined(false)
    , _smoothNormals(false)
    , _doubleSided(false)
    , _cullStyle(HdCullStyleDontCare)
{
}

void
HdEmbreeMesh::Finalize(HdRenderParam *renderParam)
{
    RTCScene scene = static_cast<HdEmbreeRenderParam*>(renderParam)
        ->AcquireSceneForEdit();
    // Delete any instances of this mesh in the top-level embree scene.
    for (size_t i = 0; i < _rtcInstanceIds.size(); ++i) {
        // Delete the instance context first...
        delete _GetInstanceContext(scene, i);
        // ...then the instance object in the top-level scene.
        rtcDetachGeometry(scene, _rtcInstanceIds[i]);
        rtcReleaseGeometry(rtcGetGeometry(scene,_rtcInstanceIds[i]));
    }
    _rtcInstanceIds.clear();

    // Delete the prototype geometry and the prototype scene.
    if (_rtcMeshScene != nullptr) {
        if (_rtcMeshId != RTC_INVALID_GEOMETRY_ID) {
            // Delete the prototype context first...
            TF_FOR_ALL(it, _GetPrototypeContext()->primvarMap) {
                delete it->second;
            }
            delete _GetPrototypeContext();
            // ... then the geometry object in the prototype scene...
            //auto geo = rtcGetGeometry(_rtcMeshScene,_rtcMeshId);
            rtcDetachGeometry(scene, _rtcMeshId);
            //rtcReleaseGeometry(geo);
        }
        // ... then the prototype scene.
        rtcReleaseScene(_rtcMeshScene);
    }
    _rtcMeshId = RTC_INVALID_GEOMETRY_ID;
    _rtcMeshScene = nullptr;
}

HdDirtyBits
HdEmbreeMesh::GetInitialDirtyBitsMask() const
{
    // The initial dirty bits control what data is available on the first
    // run through _PopulateRtMesh(), so it should list every data item
    // that _PopulateRtMesh requests.
    int mask = HdChangeTracker::Clean
        | HdChangeTracker::InitRepr
        | HdChangeTracker::DirtyPoints
        | HdChangeTracker::DirtyTopology
        | HdChangeTracker::DirtyTransform
        | HdChangeTracker::DirtyVisibility
        | HdChangeTracker::DirtyCullStyle
        | HdChangeTracker::DirtyDoubleSided
        | HdChangeTracker::DirtyDisplayStyle
        | HdChangeTracker::DirtySubdivTags
        | HdChangeTracker::DirtyPrimvar
        | HdChangeTracker::DirtyNormals
        | HdChangeTracker::DirtyInstanceIndex
        ;

    return (HdDirtyBits)mask;
}

HdDirtyBits
HdEmbreeMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void
HdEmbreeMesh::_InitRepr(TfToken const &reprToken,
                        HdDirtyBits *dirtyBits)
{
    TF_UNUSED(dirtyBits);

    // Create an empty repr.
    _ReprVector::iterator it = std::find_if(_reprs.begin(), _reprs.end(),
                                            _ReprComparator(reprToken));
    if (it == _reprs.end()) {
        _reprs.emplace_back(reprToken, HdReprSharedPtr());
    }
}

void
HdEmbreeMesh::Sync(HdSceneDelegate *sceneDelegate,
                   HdRenderParam   *renderParam,
                   HdDirtyBits     *dirtyBits,
                   TfToken const   &reprToken)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    // XXX: A mesh repr can have multiple repr decs; this is done, for example, 
    // when the drawstyle specifies different rasterizing modes between front
    // faces and back faces.
    // With raytracing, this concept makes less sense, but
    // combining semantics of two HdMeshReprDesc is tricky in the general case.
    // For now, HdEmbreeMesh only respects the first desc; this should be fixed.
    _MeshReprConfig::DescArray descs = _GetReprDesc(reprToken);
    const HdMeshReprDesc &desc = descs[0];

    // Pull top-level embree state out of the render param.
    HdEmbreeRenderParam *embreeRenderParam =
        static_cast<HdEmbreeRenderParam*>(renderParam);
    RTCScene scene = embreeRenderParam->AcquireSceneForEdit();
    RTCDevice device = embreeRenderParam->GetEmbreeDevice();

    // Create embree geometry objects.
    _PopulateRtMesh(sceneDelegate, scene, device, dirtyBits, desc);
}

/* static */
void HdEmbreeMesh::_EmbreeCullFaces(const RTCFilterFunctionNArguments* args)
{
  if (*args->valid == 0) return;

  void* userData = args->geometryUserPtr;
  RTCRay* ray = (RTCRay* ) args->ray;
  RTCHit* hit = (RTCHit* ) args->hit;
  unsigned int N = args->N;
  assert(N == 1);
  assert(ray && hit);
    // Note: this is called to filter every candidate ray hit
    // with the bound object, so this function should be fast.

    // Only HdEmbreeMesh gets HdEmbreeMesh::_EmbreeCullFaces bound
    // as an intersection filter. The filter is bound to the prototype,
    // whose context's rprim always points back to the original HdEmbreeMesh.
    HdEmbreePrototypeContext *ctx =
        static_cast<HdEmbreePrototypeContext*>(userData);
    HdEmbreeMesh *mesh = static_cast<HdEmbreeMesh*>(ctx->rprim);

    // Calculate whether the provided hit is a front-face or back-face.
    bool isFrontFace = (hit->Ng_x * ray->dir_x +
                        hit->Ng_y * ray->dir_y +
                        hit->Ng_z * ray->dir_z) > 0;

    // Determine if we should ignore this hit. HdCullStyleBack means
    // cull back faces.
    bool cull = false;
    switch(mesh->_cullStyle) {
        case HdCullStyleBack:
            cull = !isFrontFace; break;
        case HdCullStyleFront:
            cull =  isFrontFace; break;

        case HdCullStyleBackUnlessDoubleSided:
            cull = !isFrontFace && !mesh->_doubleSided; break;
        case HdCullStyleFrontUnlessDoubleSided:
            cull =  isFrontFace && !mesh->_doubleSided; break;

        default: break;
    }
    if (cull) {
        // Setting ray.geomId to null tells embree to discard this hit and
        // keep tracing...
        hit->geomID = RTC_INVALID_GEOMETRY_ID;
    }
}

void
HdEmbreeMesh::_CreateEmbreeSubdivMesh(RTCScene scene, RTCDevice device)
{
    const PxOsdSubdivTags &subdivTags = _topology.GetSubdivTags();

    // The embree edge crease buffer expects ungrouped edges: a pair
    // of indices marking an edge and one weight per crease.
    // HdMeshTopology stores edge creases compactly. A crease length
    // buffer stores the number of indices per crease and groups the
    // crease index buffer, much like the face buffer groups the vertex index
    // buffer except that creases don't automatically close. Crease weights
    // can be specified per crease or per individual edge.
    //
    // For example, to add the edges [v0->v1@2.0f] and [v1->v2@2.0f],
    // HdMeshTopology might store length = [3], indices = [v0, v1, v2],
    // and weight = [2.0f], or it might store weight = [2.0f, 2.0f].
    //
    // This loop calculates the number of edge creases, in preparation for
    // unrolling the edge crease buffer below.
    VtIntArray const creaseLengths = subdivTags.GetCreaseLengths();
    int numEdgeCreases = 0;
    for (size_t i = 0; i < creaseLengths.size(); ++i) {
        numEdgeCreases += creaseLengths[i] - 1;
    }

    // For vertex creases, sanity check that the weights and indices
    // arrays are the same length.
    int numVertexCreases =
        static_cast<int>(subdivTags.GetCornerIndices().size());
    if (numVertexCreases !=
            static_cast<int>(subdivTags.GetCornerWeights().size())) {
        TF_WARN("Mismatch between vertex crease indices and weights");
        numVertexCreases = 0;
    }

    // Populate an embree subdiv object.
    // EMBREE_FIXME: check if geometry gets properly committed
    if (_rtcMeshId != RTC_INVALID_GEOMETRY_ID)
        rtcDetachGeometry(scene, _rtcMeshId);
    RTCGeometry geom_2 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_SUBDIVISION);
    rtcSetGeometryBuildQuality(geom_2,RTC_BUILD_QUALITY_REFIT);
    rtcSetGeometryTimeStepCount(geom_2,1);
    _rtcMeshId = rtcAttachGeometry(scene,geom_2);
    rtcReleaseGeometry(geom_2);

    // Fill the topology buffers.
    rtcSetSharedGeometryBuffer(geom_2,RTC_BUFFER_TYPE_FACE,0,RTC_FORMAT_UINT,_topology.GetFaceVertexCounts().cdata(),0,sizeof(int),_topology.GetFaceVertexCounts().size());
    rtcSetSharedGeometryBuffer(geom_2,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT,_topology.GetFaceVertexIndices().cdata(),0,sizeof(int),_topology.GetFaceVertexIndices().size());
    rtcSetSharedGeometryBuffer(geom_2,RTC_BUFFER_TYPE_HOLE,0,RTC_FORMAT_UINT,_topology.GetHoleIndices().cdata(),0,sizeof(int),_topology.GetFaceVertexCounts().size());

    // If this topology has edge creases, unroll the edge crease buffer.
    if (numEdgeCreases > 0) {
        int *embreeCreaseIndices = static_cast<int*>(rtcSetNewGeometryBuffer(geom_2,RTC_BUFFER_TYPE_EDGE_CREASE_INDEX,0,RTC_FORMAT_UINT2,2*sizeof(int),numEdgeCreases));
        float *embreeCreaseWeights = static_cast<float*>(rtcSetNewGeometryBuffer(geom_2,RTC_BUFFER_TYPE_EDGE_CREASE_WEIGHT,0,RTC_FORMAT_FLOAT,sizeof(float),numEdgeCreases));
        int embreeEdgeIndex = 0;

        VtIntArray const creaseIndices = subdivTags.GetCreaseIndices();
        VtFloatArray const creaseWeights =
            subdivTags.GetCreaseWeights();

        bool weightPerCrease =
            (creaseWeights.size() == creaseLengths.size());

        // Loop through the creases; for each crease, loop through
        // the edges.
        int creaseIndexStart = 0;
        for (size_t i = 0; i < creaseLengths.size(); ++i) {
            int numEdges = creaseLengths[i] - 1;
            for(int j = 0; j < numEdges; ++j) {
                // Store the crease indices.
                embreeCreaseIndices[2*embreeEdgeIndex+0] =
                    creaseIndices[creaseIndexStart+j];
                embreeCreaseIndices[2*embreeEdgeIndex+1] =
                    creaseIndices[creaseIndexStart+j+1];

                // Store the crease weight.
                embreeCreaseWeights[embreeEdgeIndex] = weightPerCrease ?
                    creaseWeights[i] : creaseWeights[embreeEdgeIndex];

                embreeEdgeIndex++;
            }
            creaseIndexStart += creaseLengths[i];
        }

        
        
    }

    if (numVertexCreases > 0) {
        rtcSetSharedGeometryBuffer(geom_2,RTC_BUFFER_TYPE_VERTEX_CREASE_INDEX,0,RTC_FORMAT_UINT,subdivTags.GetCornerIndices().cdata(),0,sizeof(int),numVertexCreases);
        rtcSetSharedGeometryBuffer(geom_2,RTC_BUFFER_TYPE_VERTEX_CREASE_WEIGHT,0,RTC_FORMAT_FLOAT,subdivTags.GetCornerWeights().cdata(),0,sizeof(float),numVertexCreases);
    }
}

void
HdEmbreeMesh::_CreateEmbreeTriangleMesh(RTCScene scene, RTCDevice device)
{
    // Triangulate the input faces.
    HdMeshUtil meshUtil(&_topology, GetId());
    meshUtil.ComputeTriangleIndices(&_triangulatedIndices,
        &_trianglePrimitiveParams);

    // Create the new mesh.
    // EMBREE_FIXME: check if geometry gets properly committed
    if (_rtcMeshId != RTC_INVALID_GEOMETRY_ID)
        rtcDetachGeometry(scene, _rtcMeshId);
    RTCGeometry geom_1 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
    rtcSetGeometryBuildQuality(geom_1,RTC_BUILD_QUALITY_REFIT);
    rtcSetGeometryTimeStepCount(geom_1,1);
    _rtcMeshId = rtcAttachGeometry(scene,geom_1);
    rtcReleaseGeometry(geom_1);
    if (_rtcMeshId == RTC_INVALID_GEOMETRY_ID) {
        TF_CODING_ERROR("Couldn't create RTC mesh");
        rtcCommitGeometry(geom_1);
        return ;
    }

    // Populate topology.
    rtcSetSharedGeometryBuffer(geom_1,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,_triangulatedIndices.cdata(),0,sizeof(GfVec3i),_triangulatedIndices.size());
}

void
HdEmbreeMesh::_UpdatePrimvarSources(HdSceneDelegate* sceneDelegate,
                                    HdDirtyBits dirtyBits)
{
    HD_TRACE_FUNCTION();
    SdfPath const& id = GetId();

    // Update _primvarSourceMap, our local cache of raw primvar data.
    // This function pulls data from the scene delegate, but defers processing.
    //
    // While iterating primvars, we skip "points" (vertex positions) because
    // the points primvar is processed by _PopulateRtMesh. We only call
    // GetPrimvar on primvars that have been marked dirty.
    //
    // Currently, hydra doesn't have a good way of communicating changes in
    // the set of primvars, so we only ever add and update to the primvar set.

    HdPrimvarDescriptorVector primvars;
    for (size_t i=0; i < HdInterpolationCount; ++i) {
        HdInterpolation interp = static_cast<HdInterpolation>(i);
        primvars = GetPrimvarDescriptors(sceneDelegate, interp);
        for (HdPrimvarDescriptor const& pv: primvars) {
            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name) &&
                pv.name != HdTokens->points) {
                _primvarSourceMap[pv.name] = {
                    GetPrimvar(sceneDelegate, pv.name),
                    interp
                };
            }
        }
    }
}

TfTokenVector
HdEmbreeMesh::_UpdateComputedPrimvarSources(HdSceneDelegate* sceneDelegate,
                                            HdDirtyBits dirtyBits)
{
    HD_TRACE_FUNCTION();
    
    SdfPath const& id = GetId();

    // Get all the dirty computed primvars
    HdExtComputationPrimvarDescriptorVector dirtyCompPrimvars;
    for (size_t i=0; i < HdInterpolationCount; ++i) {
        HdExtComputationPrimvarDescriptorVector compPrimvars;
        HdInterpolation interp = static_cast<HdInterpolation>(i);
        compPrimvars = sceneDelegate->GetExtComputationPrimvarDescriptors
                                    (GetId(),interp);

        for (auto const& pv: compPrimvars) {
            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name)) {
                dirtyCompPrimvars.emplace_back(pv);
            }
        }
    }

    if (dirtyCompPrimvars.empty()) {
        return TfTokenVector();
    }
    
    HdExtComputationUtils::ValueStore valueStore
        = HdExtComputationUtils::GetComputedPrimvarValues(
            dirtyCompPrimvars, sceneDelegate);

    TfTokenVector compPrimvarNames;
    // Update local primvar map and track the ones that were computed
    for (auto const& compPrimvar : dirtyCompPrimvars) {
        auto const it = valueStore.find(compPrimvar.name);
        if (!TF_VERIFY(it != valueStore.end())) {
            continue;
        }
        
        compPrimvarNames.emplace_back(compPrimvar.name);
        if (compPrimvar.name == HdTokens->points) {
            _points = it->second.Get<VtVec3fArray>();
            _normalsValid = false;
        } else {
            _primvarSourceMap[compPrimvar.name] = {it->second,
                                                compPrimvar.interpolation};
        }
    }

    return compPrimvarNames;
}

void
HdEmbreeMesh::_CreatePrimvarSampler(TfToken const& name, VtValue const& data,
                                    HdInterpolation interpolation,
                                    bool refined)
{
    // Delete the old sampler, if it exists.
    HdEmbreePrototypeContext *ctx = _GetPrototypeContext();
    if (ctx->primvarMap.count(name) > 0) {
        delete ctx->primvarMap[name];
    }
    ctx->primvarMap.erase(name);

    // Construct the correct type of sampler from the interpolation mode and
    // geometry mode.
    HdEmbreePrimvarSampler *sampler = nullptr;
    switch(interpolation) {
        case HdInterpolationConstant:
            sampler = new HdEmbreeConstantSampler(name, data);
            break;
        case HdInterpolationUniform:
            if (refined) {
                sampler = new HdEmbreeUniformSampler(name, data);
            } else {
                sampler = new HdEmbreeUniformSampler(name, data,
                    _trianglePrimitiveParams);
            }
            break;
        case HdInterpolationVertex:
            if (refined) {
                sampler = new HdEmbreeSubdivVertexSampler(name, data,
                    _rtcMeshScene, _rtcMeshId, &_embreeBufferAllocator);
            } else {
                sampler = new HdEmbreeTriangleVertexSampler(name, data,
                    _triangulatedIndices);
            }
            break;
        case HdInterpolationVarying:
            if (refined) {
                // XXX: Fixme! This isn't strictly correct, as "varying" in
                // the context of subdiv meshes means bilinear interpolation,
                // not reconstruction from the subdivision basis.
                sampler = new HdEmbreeSubdivVertexSampler(name, data,
                    _rtcMeshScene, _rtcMeshId, &_embreeBufferAllocator);
            } else {
                sampler = new HdEmbreeTriangleVertexSampler(name, data,
                    _triangulatedIndices);
            }
            break;
        case HdInterpolationFaceVarying:
            if (refined) {
                // XXX: Fixme! HdEmbree doesn't currently support face-varying
                // primvars on subdivision meshes.
                TF_WARN("HdEmbreeMesh doesn't support face-varying primvars"
                        " on refined meshes.");
            } else {
                HdMeshUtil meshUtil(&_topology, GetId());
                sampler = new HdEmbreeTriangleFaceVaryingSampler(name, data,
                    meshUtil);
            }
            break;
        default:
            TF_CODING_ERROR("Unrecognized interpolation mode");
            break;
    }

    // Put the new sampler back in the primvar map.
    if (sampler != nullptr) {
        ctx->primvarMap[name] = sampler;
    }
}

void
HdEmbreeMesh::_PopulateRtMesh(HdSceneDelegate* sceneDelegate,
                              RTCScene         scene,
                              RTCDevice        device,
                              HdDirtyBits*     dirtyBits,
                              HdMeshReprDesc const &desc)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();

    ////////////////////////////////////////////////////////////////////////
    // 1. Pull scene data.
    TfTokenVector computedPrimvars =
        _UpdateComputedPrimvarSources(sceneDelegate, *dirtyBits);

    bool pointsIsComputed =
        std::find(computedPrimvars.begin(), computedPrimvars.end(),
                  HdTokens->points) != computedPrimvars.end();
    if (!pointsIsComputed &&
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        VtValue value = sceneDelegate->Get(id, HdTokens->points);
        _points = value.Get<VtVec3fArray>();
        _normalsValid = false;
    }

    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        // When pulling a new topology, we don't want to overwrite the
        // refine level or subdiv tags, which are provided separately by the
        // scene delegate, so we save and restore them.
        PxOsdSubdivTags subdivTags = _topology.GetSubdivTags();
        int refineLevel = _topology.GetRefineLevel();
        _topology = HdMeshTopology(GetMeshTopology(sceneDelegate), refineLevel);
        _topology.SetSubdivTags(subdivTags);
        _adjacencyValid = false;
    }
    if (HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id) &&
        _topology.GetRefineLevel() > 0) {
        _topology.SetSubdivTags(sceneDelegate->GetSubdivTags(id));
    }
    if (HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id)) {
        HdDisplayStyle const displayStyle = sceneDelegate->GetDisplayStyle(id);
        _topology = HdMeshTopology(_topology,
            displayStyle.refineLevel);
    }

    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
        _transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
    }

    if (HdChangeTracker::IsCullStyleDirty(*dirtyBits, id)) {
        _cullStyle = GetCullStyle(sceneDelegate);
    }
    if (HdChangeTracker::IsDoubleSidedDirty(*dirtyBits, id)) {
        _doubleSided = IsDoubleSided(sceneDelegate);
    }
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->widths) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->primvar)) {
        _UpdatePrimvarSources(sceneDelegate, *dirtyBits);
    }

    ////////////////////////////////////////////////////////////////////////
    // 2. Resolve drawstyles

    // The repr defines a set of geometry styles for drawing the mesh
    // (see hd/enums.h). We're ignoring points and wireframe for now, so
    // HdMeshGeomStyleSurf maps to subdivs and everything else maps to
    // HdMeshGeomStyleHull (coarse triangulated mesh).
    bool doRefine = (desc.geomStyle == HdMeshGeomStyleSurf);

    // If the subdivision scheme is "none", force us to not refine.
    doRefine = doRefine && (_topology.GetScheme() != PxOsdOpenSubdivTokens->none);

    // If the refine level is 0, triangulate instead of subdividing.
    doRefine = doRefine && (_topology.GetRefineLevel() > 0);

    // The repr defines whether we should compute smooth normals for this mesh:
    // per-vertex normals taken as an average of adjacent faces, and
    // interpolated smoothly across faces.
    _smoothNormals = !desc.flatShadingEnabled;

    // If the subdivision scheme is "none" or "bilinear", force us not to use
    // smooth normals.
    _smoothNormals = _smoothNormals &&
        (_topology.GetScheme() != PxOsdOpenSubdivTokens->none) &&
        (_topology.GetScheme() != PxOsdOpenSubdivTokens->bilinear);

    // If the scene delegate has provided authored normals, force us to not use
    // smooth normals.
    bool authoredNormals = false;
    if (_primvarSourceMap.count(HdTokens->normals) > 0) {
        authoredNormals = true;
    }
    _smoothNormals = _smoothNormals && !authoredNormals;

    ////////////////////////////////////////////////////////////////////////
    // 3. Populate embree prototype object.

    // If the topology has changed, or the value of doRefine has changed, we
    // need to create or recreate the embree mesh object.
    // _GetInitialDirtyBits() ensures that the topology is dirty the first time
    // this function is called, so that the embree mesh is always created.
    bool newMesh = false;
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id) ||
        doRefine != _refined) {

        newMesh = true;

        // Destroy the old mesh, if it exists.
        if (_rtcMeshScene != nullptr &&
            _rtcMeshId != RTC_INVALID_GEOMETRY_ID) {
            // Delete the prototype context first...
            TF_FOR_ALL(it, _GetPrototypeContext()->primvarMap) {
                delete it->second;
            }
            delete _GetPrototypeContext();
            // Get the RTCGeometry before rtcDetachGeometry
            auto geo = rtcGetGeometry(_rtcMeshScene,_rtcMeshId);
            rtcDetachGeometry(scene, _rtcMeshId);
            // release the prototype geometry.
            rtcReleaseGeometry(geo);
            _rtcMeshId = RTC_INVALID_GEOMETRY_ID;
        }

        // Create the prototype mesh scene, if it doesn't exist yet.
        if (_rtcMeshScene == nullptr) {
            _rtcMeshScene = rtcNewScene(device);
            rtcSetSceneFlags(_rtcMeshScene, RTC_SCENE_FLAG_DYNAMIC);
            rtcSetSceneBuildQuality(_rtcMeshScene, RTC_BUILD_QUALITY_LOW);
        }

        // Populate either a subdiv or a triangle mesh object. The helper
        // functions will take care of populating topology buffers.
        if (doRefine) {
            _CreateEmbreeSubdivMesh(_rtcMeshScene, device);
        } else {
            _CreateEmbreeTriangleMesh(_rtcMeshScene, device);
        }
        _refined = doRefine;
        // In both cases, RTC_VERTEX_BUFFER will be populated below.

        // Prototype geometry gets tagged with a prototype context, that the
        // ray-hit algorithm can use to look up data.
        rtcSetGeometryUserData(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),new HdEmbreePrototypeContext);
        _GetPrototypeContext()->rprim = this;
        _GetPrototypeContext()->primitiveParams = (_refined ?
            _trianglePrimitiveParams : VtIntArray());

        // Add _EmbreeCullFaces as a filter function for backface culling.
        rtcSetGeometryIntersectFilterFunction(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),_EmbreeCullFaces);
        rtcSetGeometryOccludedFilterFunction(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),_EmbreeCullFaces);

        // Force the smooth normals code to rebuild the "normals" primvar the
        // next time smooth normals is enabled.
        _normalsValid = false;
    }

    // If the refine level changed or the mesh was recreated, we need to pass
    // the refine level into the embree subdiv object.
    if (newMesh || HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id)) {
        if (doRefine) {
            // Pass the target number of uniform refinements to Embree.
            // Embree refinement is specified as the number of quads to generate
            // per edge, whereas hydra refinement is the number of recursive
            // splits, so we need to pass embree (2^refineLevel).

            int tessellationRate = (1 << _topology.GetRefineLevel());
            // XXX: As of Embree 2.9.0, rendering with tessellation level 1
            // (i.e. coarse mesh) results in weird normals, so force at least
            // one level of subdivision.
            if (tessellationRate == 1) {
                tessellationRate++;
            }
            rtcSetGeometryTessellationRate(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),static_cast<float>(tessellationRate));
        }
    }

    // If the subdiv tags changed or the mesh was recreated, we need to update
    // the subdivision boundary mode.
    if (newMesh || HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id)) {
        if (doRefine) {
            TfToken const vertexRule =
                _topology.GetSubdivTags().GetVertexInterpolationRule();

            if (vertexRule == PxOsdOpenSubdivTokens->none) {
                rtcSetGeometrySubdivisionMode(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),0,RTC_SUBDIVISION_MODE_NO_BOUNDARY);
            } else if (vertexRule == PxOsdOpenSubdivTokens->edgeOnly) {
                rtcSetGeometrySubdivisionMode(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),0,RTC_SUBDIVISION_MODE_SMOOTH_BOUNDARY);
            } else if (vertexRule == PxOsdOpenSubdivTokens->edgeAndCorner) {
                rtcSetGeometrySubdivisionMode(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),0,RTC_SUBDIVISION_MODE_PIN_CORNERS);
            } else {
                if (!vertexRule.IsEmpty()) {
                    TF_WARN("Unknown vertex interpolation rule: %s",
                            vertexRule.GetText());
                }
            }
        }
    }

    // Update the smooth normals in steps:
    // 1. If the topology is dirty, update the adjacency table, a processed
    //    form of the topology that helps calculate smooth normals quickly.
    // 2. If the points are dirty, update the smooth normal buffer itself.
    if (_smoothNormals && !_adjacencyValid) {
        _adjacency.BuildAdjacencyTable(&_topology);
        _adjacencyValid = true;
        // If we rebuilt the adjacency table, force a rebuild of normals.
        _normalsValid = false;
    }
    if (_smoothNormals && !_normalsValid) {
        _computedNormals = Hd_SmoothNormals::ComputeSmoothNormals(
            &_adjacency, _points.size(), _points.cdata());
        _normalsValid = true;

        // Create a sampler for the "normals" primvar. If there are authored
        // normals, the smooth normals flag has been suppressed, so it won't
        // be overwritten by the primvar population below.
        _CreatePrimvarSampler(HdTokens->normals, VtValue(_computedNormals),
            HdInterpolationVertex, _refined);
    }

    // If smooth normals are off and there are no authored normals, make sure
    // there's no "normals" sampler so the renderpass can use its fallback
    // behavior.
    if (!_smoothNormals && !authoredNormals) {
        HdEmbreePrototypeContext *ctx = _GetPrototypeContext();
        if (ctx->primvarMap.count(HdTokens->normals) > 0) {
            delete ctx->primvarMap[HdTokens->normals];
        }
        ctx->primvarMap.erase(HdTokens->normals);

        // Force the smooth normals code to rebuild the "normals" primvar the
        // next time smooth normals is enabled.
        _normalsValid = false;
    }

    // Populate primvars if they've changed or we recreated the mesh.
    TF_FOR_ALL(it, _primvarSourceMap) {
        if (newMesh ||
            HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, it->first)) {
            _CreatePrimvarSampler(it->first, it->second.data,
                    it->second.interpolation, _refined);
        }
    }

    // Populate points in the RTC mesh.
    if (newMesh || 
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        rtcSetSharedGeometryBuffer(rtcGetGeometry(_rtcMeshScene,_rtcMeshId),
                RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                _points.cdata(), 0, sizeof(GfVec3f),  _points.size());
    }

    // Update visibility by pulling the object into/out of the embree BVH.
    if (_sharedData.visible) {
        rtcEnableGeometry(rtcGetGeometry(_rtcMeshScene,_rtcMeshId));
    } else {
        rtcDisableGeometry(rtcGetGeometry(_rtcMeshScene,_rtcMeshId));
    }

    // Mark embree objects dirty and rebuild the bvh.
    if (newMesh ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        rtcCommitGeometry(rtcGetGeometry(_rtcMeshScene,_rtcMeshId));
    }
    rtcCommitScene(_rtcMeshScene);

    ////////////////////////////////////////////////////////////////////////
    // 4. Populate embree instance objects.

    // If the mesh is instanced, create one new instance per transform.
    // XXX: The current instancer invalidation tracking makes it hard for
    // HdEmbree to tell whether transforms will be dirty, so this code
    // pulls them every frame.
    if (!GetInstancerId().IsEmpty()) {

        // Retrieve instance transforms from the instancer.
        HdRenderIndex &renderIndex = sceneDelegate->GetRenderIndex();
        HdInstancer *instancer =
            renderIndex.GetInstancer(GetInstancerId());
        VtMatrix4dArray transforms =
            static_cast<HdEmbreeInstancer*>(instancer)->
                ComputeInstanceTransforms(GetId());

        size_t oldSize = _rtcInstanceIds.size();
        size_t newSize = transforms.size();

        // Size down (if necessary).
        for(size_t i = newSize; i < oldSize; ++i) {
            // Delete instance context first...
            delete _GetInstanceContext(scene, i);
            // Then Embree instance.
            rtcDetachGeometry(scene, _rtcInstanceIds[i]);
            rtcReleaseGeometry(rtcGetGeometry(scene,_rtcInstanceIds[i]));
        }
        _rtcInstanceIds.resize(newSize);

        // Size up (if necessary).
        for(size_t i = oldSize; i < newSize; ++i) {
            // Create the new instance.
            // EMBREE_FIXME: check if geometry gets properly committed
            RTCGeometry geom_0 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
            rtcSetGeometryInstancedScene(geom_0, _rtcMeshScene);
            rtcSetGeometryTimeStepCount(geom_0, 1);
            _rtcInstanceIds[i] = rtcAttachGeometry(scene, geom_0);
            rtcReleaseGeometry(geom_0);
            // Create the instance context.
            HdEmbreeInstanceContext *ctx = new HdEmbreeInstanceContext;
            ctx->rootScene = _rtcMeshScene;
            rtcSetGeometryUserData(rtcGetGeometry(scene,_rtcInstanceIds[i]),ctx);
        }

        // Update transforms.
        for (size_t i = 0; i < transforms.size(); ++i) {
            // Combine the local transform and the instance transform.
            GfMatrix4f matf = _transform * GfMatrix4f(transforms[i]);
            // Update the transform in the BVH.
            rtcSetGeometryTransform(rtcGetGeometry(scene,_rtcInstanceIds[i]),0,RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,matf.GetArray());
            // Update the transform in the instance context.
            _GetInstanceContext(scene, i)->objectToWorldMatrix = matf;
            _GetInstanceContext(scene, i)->instanceId = i;
            // Mark the instance as updated in the BVH.
            rtcCommitGeometry(rtcGetGeometry(scene,_rtcInstanceIds[i]));
        }
    }
    // Otherwise, create our single instance (if necessary) and update
    // the transform (if necessary).
    else {
        bool newInstance = false;
        if (_rtcInstanceIds.size() == 0) {
            // Create our single instance.
            RTCGeometry geom_0 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
            rtcSetGeometryInstancedScene(geom_0, _rtcMeshScene);
            rtcSetGeometryTimeStepCount(geom_0, 1);
            _rtcInstanceIds.push_back(rtcAttachGeometry(scene, geom_0));
            rtcReleaseGeometry(geom_0);
            // Create the instance context.
            HdEmbreeInstanceContext *ctx = new HdEmbreeInstanceContext;
            ctx->rootScene = _rtcMeshScene;
            rtcSetGeometryUserData(rtcGetGeometry(scene,_rtcInstanceIds[0]),ctx);
            // Update the flag to force-set the transform.
            newInstance = true;
        }
        if (newInstance || HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
            // Update the transform in the BVH.
            rtcSetGeometryTransform(rtcGetGeometry(scene,_rtcInstanceIds[0]),0,RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,_transform.GetArray());
            // Update the transform in the render context.
            _GetInstanceContext(scene, 0)->objectToWorldMatrix = _transform;
            _GetInstanceContext(scene, 0)->instanceId = 0;
        }
        if (newInstance || newMesh ||
            HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
            HdChangeTracker::IsPrimvarDirty(*dirtyBits, id,
                                            HdTokens->points)) {
            // Mark the instance as updated in the top-level BVH.
            rtcCommitGeometry(rtcGetGeometry(scene,_rtcInstanceIds[0]));
        }
    }

    // Clean all dirty bits.
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

HdEmbreePrototypeContext*
HdEmbreeMesh::_GetPrototypeContext()
{
    return static_cast<HdEmbreePrototypeContext*>(
        rtcGetGeometryUserData(rtcGetGeometry(_rtcMeshScene,_rtcMeshId)));
}

HdEmbreeInstanceContext*
HdEmbreeMesh::_GetInstanceContext(RTCScene scene, size_t i)
{
    return static_cast<HdEmbreeInstanceContext*>(
        rtcGetGeometryUserData(rtcGetGeometry(scene,_rtcInstanceIds[i])));
}

PXR_NAMESPACE_CLOSE_SCOPE
