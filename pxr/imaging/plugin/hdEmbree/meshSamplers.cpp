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
#include "pxr/imaging/plugin/hdEmbree/meshSamplers.h"

#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/vtBufferSource.h"

PXR_NAMESPACE_OPEN_SCOPE

// HdEmbreeRTCBufferAllocator

RTCBufferType
HdEmbreeRTCBufferAllocator::Allocate()
{
    for (size_t i = 0; i < _bitset.size(); ++i) {
        if (!_bitset.test(i)) {
            _bitset.set(i);
            return static_cast<RTCBufferType>(
                static_cast<size_t>(RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE) + i);
        }
    }
    return static_cast<RTCBufferType>(-1);
}

void
HdEmbreeRTCBufferAllocator::Free(RTCBufferType buffer)
{
    _bitset.reset(buffer - RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE);
}

// HdEmbreeConstantSampler

bool
HdEmbreeConstantSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    return _sampler.Sample(0, value, dataType);
}

// HdEmbreeUniformSampler

bool
HdEmbreeUniformSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    if (_primitiveParams.empty()) {
        return _sampler.Sample(element, value, dataType);
    }
    if (element >= _primitiveParams.size()) {
        return false;
    }
    return _sampler.Sample(
        HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
            _primitiveParams[element]),
        value, dataType);
}

// HdEmbreeTriangleVertexSampler

bool
HdEmbreeTriangleVertexSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    if (element >= _indices.size()) {
        return false;
    }
    HdEmbreeTypeHelper::PrimvarTypeContainer corners[3];
    if (!_sampler.Sample(_indices[element][0], &corners[0], dataType) ||
        !_sampler.Sample(_indices[element][1], &corners[1], dataType) ||
        !_sampler.Sample(_indices[element][2], &corners[2], dataType)) {
        return false;
    }
    void* samples[3] = { static_cast<void*>(&corners[0]),
                         static_cast<void*>(&corners[1]),
                         static_cast<void*>(&corners[2]) };
    // Embree specification of triangle interpolation:
    // t_uv = (1-u-v)*t0 + u*t1 + v*t2
    float weights[3] = { 1.0f - u - v, u, v };
    return _Interpolate(value, samples, weights, 3, dataType);
}

// HdEmbreeTriangleFaceVaryingSampler

bool
HdEmbreeTriangleFaceVaryingSampler::Sample(unsigned int element, float u,
    float v, void* value, HdTupleType dataType) const
{
    HdEmbreeTypeHelper::PrimvarTypeContainer corners[3];
    if (!_sampler.Sample(element*3 + 0, &corners[0], dataType) ||
        !_sampler.Sample(element*3 + 1, &corners[1], dataType) ||
        !_sampler.Sample(element*3 + 2, &corners[2], dataType)) {
        return false;
    }
    void* samples[3] = { static_cast<void*>(&corners[0]),
                         static_cast<void*>(&corners[1]),
                         static_cast<void*>(&corners[2]) };
    // Embree specification of triangle interpolation:
    // t_uv = (1-u-v)*t0 + u*t1 + v*t2
    float weights[3] = { 1.0f - u - v, u, v };
    return _Interpolate(value, samples, weights, 3, dataType);
}

/* static */ VtValue
HdEmbreeTriangleFaceVaryingSampler::_Triangulate(TfToken const& name,
    VtValue const& value, HdMeshUtil &meshUtil)
{
    HdVtBufferSource buffer(name, value);
    VtValue triangulated;
    if (!meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
            buffer.GetData(),
            buffer.GetNumElements(),
            buffer.GetTupleType().type,
            &triangulated)) {
        TF_CODING_ERROR("[%s] Could not triangulate face-varying data.",
            name.GetText());
        return VtValue();
    }
    return triangulated;
}

// HdEmbreeSubdivVertexSampler

HdEmbreeSubdivVertexSampler::HdEmbreeSubdivVertexSampler(TfToken const& name,
    VtValue const& value, RTCScene meshScene, unsigned meshId,
    HdEmbreeRTCBufferAllocator *allocator)
    : _embreeBufferId(static_cast<RTCBufferType>(-1))
    , _buffer(name, value)
    , _meshScene(meshScene)
    , _meshId(meshId)
    , _allocator(allocator)
{
    // The embree API only supports float-component primvars.
    if (HdGetComponentType(_buffer.GetTupleType().type) != HdTypeFloat) {
        TF_CODING_ERROR("Embree subdivision meshes only support float-based"
            " primvars for vertex interpolation mode");
        return;
    }
    _embreeBufferId = _allocator->Allocate();
    // The embree API has a constant number of primvar slots (16 at last
    // count), shared between vertex and face-varying modes.
    if (_embreeBufferId == static_cast<RTCBufferType>(-1)) {
        TF_CODING_ERROR("Embree subdivision meshes only support %d primvars"
            " in vertex interpolation mode", 65536);
        return;
    }
    // Tag the embree mesh object with the primvar buffer, for use by
    // rtcInterpolate.

    const HdTupleType bufType = _buffer.GetTupleType();
    assert(bufType.count == 1 && (bufType.type == HdTypeFloat || bufType.type == HdTypeFloatVec2 ||
                                  bufType.type == HdTypeFloatVec3 || bufType.type == HdTypeFloatVec4));

    rtcSetGeometryVertexAttributeCount(rtcGetGeometry(_meshScene,_meshId), (_embreeBufferId - RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE) + 1);
    rtcSetSharedGeometryBuffer(rtcGetGeometry(_meshScene,_meshId),
                               RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                               _embreeBufferId - RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                               RTC_FORMAT_FLOAT,
                               _buffer.GetData(),0,
                               HdDataSizeOfTupleType(bufType),
                               _buffer.GetNumElements() * HdGetComponentCount(bufType.type) * bufType.count);
/*
    switch (_buffer.GetTupleType().type) {
        case HdTypeFloat:
            rtcSetSharedGeometryBuffer(rtcGetGeometry(_meshScene,_meshId),
                                       RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                                       _embreeBufferId,
                                       RTC_FORMAT_FLOAT,
                                       _buffer.GetData(),0,
                                       sizeof(float),
                                       _buffer.GetNumElements());
            break;
        case HdTypeFloatVec2:
            rtcSetSharedGeometryBuffer(rtcGetGeometry(_meshScene,_meshId),
                                       RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                                       _embreeBufferId,
                                       RTC_FORMAT_FLOAT2,
                                       _buffer.GetData(),0,
                                       sizeof(GfVec2f),
                                       _buffer.GetNumElements());
            break;
        case HdTypeFloatVec3:
            rtcSetSharedGeometryBuffer(rtcGetGeometry(_meshScene,_meshId),
                                       RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                                       _embreeBufferId,
                                       RTC_FORMAT_FLOAT3,
                                       _buffer.GetData(),0,
                                       sizeof(GfVec3f),
                                       _buffer.GetNumElements());
            break;
        case HdTypeFloatVec4:
            rtcSetSharedGeometryBuffer(rtcGetGeometry(_meshScene,_meshId),
                                       RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                                       _embreeBufferId,
                                       RTC_FORMAT_FLOAT4,
                                       _buffer.GetData(),0,
                                       sizeof(GfVec4f),
                                       _buffer.GetNumElements());
            break;
    }
*/
}

HdEmbreeSubdivVertexSampler::~HdEmbreeSubdivVertexSampler()
{
    if (_embreeBufferId != static_cast<RTCBufferType>(-1)) {
        _allocator->Free(_embreeBufferId);
    }
}

bool
HdEmbreeSubdivVertexSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    // Make sure the buffer type and sample type have the same arity.
    // _embreeBufferId of -1 indicates this sampler failed to initialize.
    if (_embreeBufferId == static_cast<RTCBufferType>(-1) ||
        dataType != _buffer.GetTupleType()) {
        return false;
    }

    // Combine number of components in the underlying type and tuple arity.
    size_t numFloats = HdGetComponentCount(dataType.type) * dataType.count;

    rtcInterpolate1(rtcGetGeometry(_meshScene,_meshId),element,u,v,
                    RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                    _embreeBufferId - RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                    static_cast<float*>(value),nullptr,nullptr,numFloats);

    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
