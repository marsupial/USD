set(PXR_PREFIX pxr/imaging)
set(PXR_PACKAGE hdEmbree3)

if(NOT EMBREE_FOUND)
    message(WARNING "Not building ${PXR_PACKAGE} because of missing dependency: Embree")
    return()
endif()
if (NOT ${PXR_ENABLE_GL_SUPPORT})
    message(STATUS
        "Skipping ${PXR_PACKAGE} because PXR_ENABLE_GL_SUPPORT is OFF")
    return()
endif()

pxr_plugin(hdEmbree3
   LIBRARIES
        plug
        tf
        vt
        gf
        work
        hf
        hd
        hdx
        ${TBB_tbb_LIBRARY}
        #${EMBREE_LIBRARY}
		/Developer/local/release/lib/libembree3.dylib

    INCLUDE_DIRS
        ${TBB_INCLUDE_DIRS}
        ${EMBREE_INCLUDE_DIR}

    PUBLIC_CLASSES
        config
        instancer
        mesh
        meshSamplers
        renderBuffer
        renderer
        rendererPlugin
        renderDelegate
        renderPass
        sampler

    PUBLIC_HEADERS
        context.h
        renderParam.h

    RESOURCE_FILES
        plugInfo.json
)

if (X11_FOUND OR APPLE)
pxr_build_test(testHdEmbree3
    LIBRARIES
        hdEmbree3
        garch
        arch
    CPPFILES
        testenv/testHdEmbree.cpp
)
endif()
