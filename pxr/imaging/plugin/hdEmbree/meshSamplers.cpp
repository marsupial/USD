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

/*! maximum number of user vertex buffers for subdivision surfaces */
#define RTC_MAX_USER_VERTEX_BUFFERS 65536

// HdEmbreeRTCBufferAllocator

uint16_t
HdEmbreeRTCBufferAllocator::Allocate()
{
    if (_available.empty()) {
        if (_next == RTC_MAX_USER_VERTEX_BUFFERS)
            return static_cast<value_type>(-1);
        return _next++;
    }

    auto itr = _available.begin();
    value_type buffer = *itr;
    _available.erase(itr);
    return buffer;
}

void
HdEmbreeRTCBufferAllocator::Free(value_type buffer)
{
    if (buffer == _next-1) {
        _next = buffer;
        return;
    }
    if (_available.size()+1 == _next) {
        decltype(_available)().swap(_available);
        _next = 0;
        return;
    }
    _available.insert(buffer);
}

unsigned
HdEmbreeRTCBufferAllocator::Slots() const {
    return _next - _available.size();
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
    : _embreeBufferId(HdEmbreeRTCBufferAllocator::Invalid)
    , _buffer(name, value)
    , _meshScene(meshScene)
    , _meshId(meshId)
    , _allocator(allocator)
{
    RTCFormat rtcFormat = RTC_FORMAT_UNDEFINED;
    switch (_buffer.GetTupleType().type) {
        case HdTypeFloat:
            rtcFormat = RTC_FORMAT_FLOAT;
            break;
        case HdTypeFloatVec2:
            rtcFormat = RTC_FORMAT_FLOAT2;
            break;
        case HdTypeFloatVec3:
            rtcFormat = RTC_FORMAT_FLOAT3;
            break;
        case HdTypeFloatVec4:
            rtcFormat = RTC_FORMAT_FLOAT4;
            break;
    }

    // The embree API only supports float-component primvars.
    if (rtcFormat == RTC_FORMAT_UNDEFINED) {
        TF_CODING_ERROR("Embree subdivision meshes only support float-based"
            " primvars for vertex interpolation mode");
        return;
    }

    // The embree API has a constant number of primvar slots (16 at last count)
    // shared between vertex and face-varying modes.
    _embreeBufferId = _allocator->Allocate();
    if (_embreeBufferId == HdEmbreeRTCBufferAllocator::Invalid) {
        TF_CODING_ERROR("Embree subdivision meshes only support %d primvars"
            " in vertex interpolation mode", RTC_MAX_USER_VERTEX_BUFFERS);
        return;
    }

    // Tag the embree mesh object with the primvar buffer, for use by
    // rtcInterpolate.
    RTCGeometry geo = rtcGetGeometry(_meshScene, _meshId);
    rtcSetGeometryVertexAttributeCount(geo, _allocator->Slots());

    rtcSetSharedGeometryBuffer(rtcGetGeometry(_meshScene,_meshId),
                               RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                               _embreeBufferId,
                               rtcFormat,
                               _buffer.GetData(),0,
                               HdDataSizeOfTupleType(_buffer.GetTupleType()),
                               _buffer.GetNumElements());
}

HdEmbreeSubdivVertexSampler::~HdEmbreeSubdivVertexSampler()
{
    if (_embreeBufferId != HdEmbreeRTCBufferAllocator::Invalid) {
        _allocator->Free(_embreeBufferId);
    }
}

bool
HdEmbreeSubdivVertexSampler::Sample(unsigned int element, float u, float v,
    void* value, HdTupleType dataType) const
{
    // Make sure the buffer type and sample type have the same arity.
    // _embreeBufferId of -1 indicates this sampler failed to initialize.
    if (_embreeBufferId == HdEmbreeRTCBufferAllocator::Invalid ||
        dataType != _buffer.GetTupleType()) {
        return false;
    }

    // Combine number of components in the underlying type and tuple arity.
    size_t numFloats = HdGetComponentCount(dataType.type) * dataType.count;

    rtcInterpolate1(rtcGetGeometry(_meshScene,_meshId),element,u,v,
                    RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,
                    _embreeBufferId,
                    static_cast<float*>(value),nullptr,nullptr,numFloats);

    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
