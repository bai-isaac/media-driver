/*
* Copyright (c) 2020-2021, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     media_copy.cpp
//! \brief    Common interface and structure used in media copy
//! \details  Common interface and structure used in media copy which are platform independent
//!

#include "media_copy.h"
#include "media_copy_common.h"
#include "media_debug_dumper.h"
#include "mhw_cp_interface.h"
#include "mos_utilities.h"

MediaCopyBaseState::MediaCopyBaseState():
    m_osInterface(nullptr)
{

}

MediaCopyBaseState::~MediaCopyBaseState()
{
    MOS_STATUS              eStatus;

    if (m_osInterface)
    {
        m_osInterface->pfnDestroy(m_osInterface, false);
        MOS_FreeMemory(m_osInterface);
        m_osInterface = nullptr;
    }

    if (m_inUseGPUMutex)
    {
        MosUtilities::MosDestroyMutex(m_inUseGPUMutex);
        m_inUseGPUMutex = nullptr;
    }

   #if (_DEBUG || _RELEASE_INTERNAL)
    if (m_surfaceDumper != nullptr)
    {
       MOS_Delete(m_surfaceDumper);
       m_surfaceDumper = nullptr;
    }
   #endif
}

//!
//! \brief    init Media copy
//! \details  init func.
//! \param    none
//! \return   MOS_STATUS
//!           Return MOS_STATUS_SUCCESS if success, otherwise return failed.
//!
MOS_STATUS MediaCopyBaseState::Initialize(PMOS_INTERFACE osInterface)
{
    if (m_inUseGPUMutex == nullptr)
    {
        m_inUseGPUMutex     = MosUtilities::MosCreateMutex();
        MCPY_CHK_NULL_RETURN(m_inUseGPUMutex);
    }
    MCPY_CHK_NULL_RETURN(m_osInterface);
    Mos_SetVirtualEngineSupported(m_osInterface, true);
    m_osInterface->pfnVirtualEngineSupported(m_osInterface, true, true);

   #if (_DEBUG || _RELEASE_INTERNAL)
    if (m_surfaceDumper == nullptr)
    {
       m_surfaceDumper = MOS_New(CommonSurfaceDumper, osInterface);
       MOS_OS_CHK_NULL_RETURN(m_surfaceDumper);
    }
    MediaUserSettingSharedPtr           userSettingPtr = nullptr;

    if (m_osInterface)
    {
        userSettingPtr = m_osInterface->pfnGetUserSettingInstance(m_osInterface);
        ReadUserSettingForDebug(
            userSettingPtr,
            m_MCPYForceMode,
            __MEDIA_USER_FEATURE_SET_MCPY_FORCE_MODE,
            MediaUserSetting::Group::Device);
    }

   #endif
    return MOS_STATUS_SUCCESS;
}

//!
//! \brief    check copy capability.
//! \details  to determine surface copy is supported or not.
//! \param    none
//! \return   MOS_STATUS
//!           Return MOS_STATUS_SUCCESS if support, otherwise return unspoort.
//!
MOS_STATUS MediaCopyBaseState::CapabilityCheck(MCPY_STATE_PARAMS& mcpySrc, MCPY_STATE_PARAMS& mcpyDst, MCPY_ENGINE_CAPS& caps)
{
    // derivate class specific check. include HW engine avaliable check.
    MCPY_CHK_STATUS_RETURN(FeatureSupport(mcpySrc.OsRes, mcpyDst.OsRes, mcpySrc, mcpyDst, caps));

    // common policy check
    // legal check
    // Blt engine does not support protection, allow the copy if dst is staging buffer in system mem
    if (mcpySrc.CpMode == MCPY_CPMODE_CP && mcpyDst.CpMode == MCPY_CPMODE_CLEAR && !m_allowCPBltCopy)
    {
        MCPY_ASSERTMESSAGE("illegal usage");
        return MOS_STATUS_INVALID_PARAMETER;
    }

    // vebox cap check.
    if (!IsVeboxCopySupported(mcpySrc.OsRes, mcpyDst.OsRes) || // format check, implemented on Gen derivate class.
        mcpySrc.bAuxSuface)
    {
        caps.engineVebox = false;
    }

    // Eu cap check.
    if (!RenderFormatSupportCheck(mcpySrc.OsRes, mcpyDst.OsRes) || // format check, implemented on Gen derivate class.
        mcpySrc.bAuxSuface)
    {
        caps.engineRender = false;
    }

    if (!caps.engineVebox && !caps.engineBlt && !caps.engineRender)
    {
        return MOS_STATUS_INVALID_PARAMETER; // unsupport copy on each hw engine.
    }

    return MOS_STATUS_SUCCESS;
}

//!
//! \brief    surface copy pre process.
//! \details  pre process before doing surface copy.
//! \param    none
//! \return   MOS_STATUS
//!           Return MOS_STATUS_SUCCESS if support, otherwise return unspoort.
//!
MOS_STATUS MediaCopyBaseState::PreCheckCpCopy(
    MCPY_STATE_PARAMS src, MCPY_STATE_PARAMS dest, MCPY_METHOD preferMethod)
{
    return MOS_STATUS_SUCCESS;
}

//!
//! \brief    dispatch copy task if support.
//! \details  dispatch copy task to HW engine (vebox, EU, Blt) based on customer and default.
//! \param    src
//!           [in] Pointer to source surface
//! \param    dst
//!           [in] Pointer to destination surface
//! \return   MOS_STATUS
//!           Return MOS_STATUS_SUCCESS if support, otherwise return unspoort.
//!
MOS_STATUS MediaCopyBaseState::CopyEnigneSelect(MCPY_METHOD preferMethod, MCPY_ENGINE& mcpyEngine, MCPY_ENGINE_CAPS& caps)
{
    // driver should make sure there is at least one he can process copy even customer choice doesn't match caps.
    switch (preferMethod)
    {
        case MCPY_METHOD_PERFORMANCE:
        case MCPY_METHOD_DEFAULT:
            mcpyEngine = caps.engineRender?MCPY_ENGINE_RENDER:(caps.engineBlt ? MCPY_ENGINE_BLT : MCPY_ENGINE_VEBOX);
            break;
        case MCPY_METHOD_BALANCE:
            mcpyEngine = caps.engineVebox?MCPY_ENGINE_VEBOX:(caps.engineBlt?MCPY_ENGINE_BLT:MCPY_ENGINE_RENDER);
            break;
        case MCPY_METHOD_POWERSAVING:
            mcpyEngine = caps.engineBlt?MCPY_ENGINE_BLT:(caps.engineVebox?MCPY_ENGINE_VEBOX:MCPY_ENGINE_RENDER);
            break;
        default:
            break;
    }
#if (_DEBUG || _RELEASE_INTERNAL)
    if (MCPY_METHOD_PERFORMANCE == m_MCPYForceMode)
    {
        mcpyEngine = MCPY_ENGINE_RENDER;
    }
    else if (MCPY_METHOD_POWERSAVING == m_MCPYForceMode)
    {
        mcpyEngine = MCPY_ENGINE_BLT;
    }
    else if (MCPY_METHOD_BALANCE == m_MCPYForceMode)
    {
        mcpyEngine = MCPY_ENGINE_VEBOX;
    }
    else if (4 == m_MCPYForceMode)
    {
        return MOS_STATUS_INVALID_PARAMETER; // bypass copy engine, just let APP handle it.
    }
#endif
    return MOS_STATUS_SUCCESS;
}

//!
//! \brief    surface copy func.
//! \details  copy surface.
//! \param    src
//!           [in] Pointer to source surface
//! \param    dst
//!           [in] Pointer to destination surface
//! \return   MOS_STATUS
//!           Return MOS_STATUS_SUCCESS if support, otherwise return unspoort.
//!
MOS_STATUS MediaCopyBaseState::SurfaceCopy(PMOS_RESOURCE src, PMOS_RESOURCE dst, MCPY_METHOD preferMethod)
{
    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    MOS_SURFACE ResDetails;
    MOS_ZeroMemory(&ResDetails, sizeof(MOS_SURFACE));
    ResDetails.Format = Format_Invalid;

    MCPY_STATE_PARAMS     mcpySrc = {nullptr, MOS_MMC_DISABLED, MOS_TILE_LINEAR, MCPY_CPMODE_CLEAR, false};
    MCPY_STATE_PARAMS     mcpyDst = {nullptr, MOS_MMC_DISABLED, MOS_TILE_LINEAR, MCPY_CPMODE_CLEAR, false};
    MCPY_ENGINE           mcpyEngine = MCPY_ENGINE_BLT;
    MCPY_ENGINE_CAPS      mcpyEngineCaps = {1, 1, 1, 1};
    MCPY_CHK_STATUS_RETURN(m_osInterface->pfnGetResourceInfo(m_osInterface, src, &ResDetails));
    MCPY_CHK_STATUS_RETURN(m_osInterface->pfnGetMemoryCompressionMode(m_osInterface, src, (PMOS_MEMCOMP_STATE)&(mcpySrc.CompressionMode)));
    mcpySrc.CpMode          = src->pGmmResInfo->GetSetCpSurfTag(false, 0)?MCPY_CPMODE_CP:MCPY_CPMODE_CLEAR;
    mcpySrc.TileMode        = ResDetails.TileType;
    mcpySrc.OsRes           = src;
    MCPY_NORMALMESSAGE("input surface's format %d, width %d; hight %d, pitch %d, tiledmode %d, mmc mode %d",
        ResDetails.Format, ResDetails.dwWidth, ResDetails.dwHeight, ResDetails.dwPitch, mcpySrc.TileMode, mcpySrc.CompressionMode);

    MOS_ZeroMemory(&ResDetails, sizeof(MOS_SURFACE));
    ResDetails.Format = Format_Invalid;
    MCPY_CHK_STATUS_RETURN(m_osInterface->pfnGetResourceInfo(m_osInterface, dst, &ResDetails));
    MCPY_CHK_STATUS_RETURN(m_osInterface->pfnGetMemoryCompressionMode(m_osInterface,dst, (PMOS_MEMCOMP_STATE) &(mcpyDst.CompressionMode)));
    mcpyDst.CpMode          = dst->pGmmResInfo->GetSetCpSurfTag(false, 0)?MCPY_CPMODE_CP:MCPY_CPMODE_CLEAR;
    mcpyDst.TileMode        = ResDetails.TileType;
    mcpyDst.OsRes           = dst;
    MCPY_NORMALMESSAGE("Output surface's format %d, width %d; hight %d, pitch %d, tiledmode %d, mmc mode %d",
        ResDetails.Format, ResDetails.dwWidth, ResDetails.dwHeight, ResDetails.dwPitch, mcpyDst.TileMode, mcpyDst.CompressionMode);

    MCPY_CHK_STATUS_RETURN(PreCheckCpCopy(mcpySrc, mcpyDst, preferMethod));

    MCPY_CHK_STATUS_RETURN(CapabilityCheck(mcpySrc, mcpyDst, mcpyEngineCaps));

    CopyEnigneSelect(preferMethod, mcpyEngine, mcpyEngineCaps);

    MCPY_CHK_STATUS_RETURN(TaskDispatch(mcpySrc, mcpyDst, mcpyEngine));

    return eStatus;
}

MOS_STATUS MediaCopyBaseState::TaskDispatch(MCPY_STATE_PARAMS mcpySrc, MCPY_STATE_PARAMS mcpyDst, MCPY_ENGINE mcpyEngine)
{
    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;


#if (_DEBUG || _RELEASE_INTERNAL)
    MOS_SURFACE sourceSurface = {};
    MOS_SURFACE targetSurface = {};

    char  dumpLocation_in[MAX_PATH];
    char  dumpLocation_out[MAX_PATH];

    MOS_ZeroMemory(dumpLocation_in, MAX_PATH);
    MOS_ZeroMemory(dumpLocation_out, MAX_PATH);

    targetSurface.Format = Format_Invalid;
    targetSurface.OsResource = *mcpyDst.OsRes;

#if !defined(LINUX) && !defined(ANDROID) && !EMUL
    MOS_ZeroMemory(&targetSurface.OsResource.AllocationInfo, sizeof(SResidencyInfo));
#endif

    sourceSurface.Format = Format_Invalid;
    sourceSurface.OsResource = *mcpySrc.OsRes;

    m_osInterface->pfnGetResourceInfo(m_osInterface, &sourceSurface.OsResource, &sourceSurface);
    m_osInterface->pfnGetResourceInfo(m_osInterface, &targetSurface.OsResource, &targetSurface);

    // Set the dump location like "dumpLocation before MCPY=path_to_dump_folder" in user feature configure file
    // Otherwise, the surface may not be dumped
    if (m_surfaceDumper)
    {
        m_surfaceDumper->GetSurfaceDumpLocation(dumpLocation_in, mcpy_in);

        if ((*dumpLocation_in == '\0') || (*dumpLocation_in == ' '))
        {
            MCPY_NORMALMESSAGE("Invalid dump location set, the surface will not be dumped");
        }
        else
        {
            m_surfaceDumper->DumpSurfaceToFile(m_osInterface, &sourceSurface, dumpLocation_in, m_surfaceDumper->m_frameNum, true, false, nullptr);
        }
    }
#endif

    MosUtilities::MosLockMutex(m_inUseGPUMutex);
    switch(mcpyEngine)
    {
        case MCPY_ENGINE_VEBOX:
            eStatus = MediaVeboxCopy(mcpySrc.OsRes, mcpyDst.OsRes);
            break;
        case MCPY_ENGINE_BLT:
            if ((mcpySrc.TileMode != MOS_TILE_LINEAR) && (mcpySrc.CompressionMode != MOS_MMC_DISABLED))
            {
                MCPY_NORMALMESSAGE("mmc on, mcpySrc.TileMode= %d, mcpySrc.CompressionMode = %d", mcpySrc.TileMode, mcpySrc.CompressionMode);
                eStatus = m_osInterface->pfnDecompResource(m_osInterface, mcpySrc.OsRes);
                if (MOS_STATUS_SUCCESS != eStatus)
                {
                    MosUtilities::MosUnlockMutex(m_inUseGPUMutex);
                    MCPY_CHK_STATUS_RETURN(eStatus);
                }
            }
            eStatus = MediaBltCopy(mcpySrc.OsRes, mcpyDst.OsRes);
            break;
        case MCPY_ENGINE_RENDER:
            eStatus = MediaRenderCopy(mcpySrc.OsRes, mcpyDst.OsRes);
            break;
        default:
            break;
    }
    MosUtilities::MosUnlockMutex(m_inUseGPUMutex);

#if (_DEBUG || _RELEASE_INTERNAL)
    std::string copyEngine = mcpyEngine ?(mcpyEngine == MCPY_ENGINE_BLT?"BLT":"Render"):"VeBox";
    MediaUserSettingSharedPtr userSettingPtr = m_osInterface->pfnGetUserSettingInstance(m_osInterface);
    ReportUserSettingForDebug(
        userSettingPtr,
        __MEDIA_USER_FEATURE_MCPY_MODE,
        copyEngine,
        MediaUserSetting::Group::Device);

    // Set the dump location like "dumpLocation after MCPY=path_to_dump_folder" in user feature configure file
    // Otherwise, the surface may not be dumped
    if (m_surfaceDumper)
    {
        m_surfaceDumper->GetSurfaceDumpLocation(dumpLocation_out, mcpy_out);

        if ((*dumpLocation_out == '\0') || (*dumpLocation_out == ' '))
        {
            MCPY_NORMALMESSAGE("Invalid dump location set, the surface will not be dumped");
        }
        else
        {
            m_surfaceDumper->DumpSurfaceToFile(m_osInterface, &targetSurface, dumpLocation_out, m_surfaceDumper->m_frameNum, true, false, nullptr);
        }
        m_surfaceDumper->m_frameNum++;
    }
#endif
    MCPY_NORMALMESSAGE("Media Copy works on %s Engine", mcpyEngine ?(mcpyEngine == MCPY_ENGINE_BLT?"BLT":"Render"):"VeBox");

    return eStatus;
}

//!
//! \brief    aux surface copy.
//! \details  copy surface.
//! \param    src
//!           [in] Pointer to source surface
//! \param    dst
//!           [in] Pointer to destination surface
//! \return   MOS_STATUS
//!           Return MOS_STATUS_SUCCESS if support, otherwise return unspoort.
//!
MOS_STATUS MediaCopyBaseState::AuxCopy(PMOS_RESOURCE src, PMOS_RESOURCE dst)
{
    // only support form Gen12+, will implement on deriavete class.
    MCPY_ASSERTMESSAGE("doesn't support");
    return MOS_STATUS_INVALID_HANDLE;
}

