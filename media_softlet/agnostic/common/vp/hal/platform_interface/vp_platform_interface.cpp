/*
* Copyright (c) 2020-2022, Intel Corporation
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
//! \file     vp_platform_interface.cpp
//! \brief    render packet which used in by mediapipline.
//! \details  render packet provide the structures and generate the cmd buffer which mediapipline will used.
//!
#include "vp_platform_interface.h"
#include "vp_visa.h"
#include "vp_user_setting.h"

using namespace vp;
extern const Kdll_RuleEntry g_KdllRuleTable_Next[];
const std::string VpRenderKernel::s_kernelNameNonAdvKernels = "vpFcKernels";

VpPlatformInterface::VpPlatformInterface(PMOS_INTERFACE pOsInterface)
{
    m_pOsInterface = pOsInterface;
    if (m_pOsInterface)
    {
        m_userSettingPtr = m_pOsInterface->pfnGetUserSettingInstance(m_pOsInterface);
    }
    VpUserSetting::InitVpUserSetting(m_userSettingPtr);
}

MOS_STATUS VpRenderKernel::InitVPKernel(
    const Kdll_RuleEntry *kernelRules,
    const uint32_t *      kernelBin,
    uint32_t              kernelSize,
    const uint32_t *      patchKernelBin,
    uint32_t              patchKernelSize,
    void (*ModifyFunctionPointers)(PKdll_State) = nullptr)
{
    VP_FUNC_CALL();
    m_kernelDllRules = kernelRules;
    m_kernelBin      = (const void *)kernelBin;
    m_kernelBinSize  = kernelSize;
    m_fcPatchBin     = (const void *)patchKernelBin;
    m_fcPatchBinSize = patchKernelSize;

    void *pKernelBin  = nullptr;
    void *pFcPatchBin = nullptr;

    pKernelBin = MOS_AllocMemory(m_kernelBinSize);
    if (!pKernelBin)
    {
        VP_RENDER_ASSERTMESSAGE("local creat surface faile, retun no space");
        MOS_SafeFreeMemory(pKernelBin);
        return MOS_STATUS_NO_SPACE;
    }

    MOS_SecureMemcpy(pKernelBin,
        m_kernelBinSize,
        m_kernelBin,
        m_kernelBinSize);

    if ((m_fcPatchBin != nullptr) && (m_fcPatchBinSize != 0))
    {
        pFcPatchBin = MOS_AllocMemory(m_fcPatchBinSize);
        if (!pFcPatchBin)
        {
            VP_RENDER_ASSERTMESSAGE("local creat surface faile, retun no space");
            MOS_SafeFreeMemory(pKernelBin);
            MOS_SafeFreeMemory(pFcPatchBin);
            return MOS_STATUS_NO_SPACE;
        }

        MOS_SecureMemcpy(pFcPatchBin,
            m_fcPatchBinSize,
            m_fcPatchBin,
            m_fcPatchBinSize);
    }

    // Allocate KDLL state (Kernel Dynamic Linking)
    m_kernelDllState = KernelDll_AllocateStates(
        pKernelBin,
        m_kernelBinSize,
        pFcPatchBin,
        m_fcPatchBinSize,
        m_kernelDllRules,
        ModifyFunctionPointers);
    if (!m_kernelDllState)
    {
        VP_RENDER_ASSERTMESSAGE("Failed to allocate KDLL state.");
        MOS_SafeFreeMemory(pKernelBin);
        MOS_SafeFreeMemory(pFcPatchBin);
    }
    else
    {
        KernelDll_SetupFunctionPointers_Ext(m_kernelDllState);
    }

    SetKernelName(VpRenderKernel::s_kernelNameNonAdvKernels);

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpPlatformInterface::InitVPFCKernels(
    const Kdll_RuleEntry *kernelRules,
    const uint32_t *      kernelBin,
    uint32_t              kernelSize,
    const uint32_t *      patchKernelBin,
    uint32_t              patchKernelSize,
    void (*ModifyFunctionPointers)(PKdll_State))
{
    VP_FUNC_CALL();

    // For non-adv kernels.
    if (m_kernelPool.end() == m_kernelPool.find(VpRenderKernel::s_kernelNameNonAdvKernels))
    {
        // Need refine later. Should not push_back local variable, which will cause default
        // assign operator being used and may cause issue if we release any internal members in destruction.
        VpRenderKernel vpKernel;
        vpKernel.InitVPKernel(
            kernelRules,
            kernelBin,
            kernelSize,
            patchKernelBin,
            patchKernelSize,
            ModifyFunctionPointers);

        m_kernelPool.insert(std::make_pair(vpKernel.GetKernelName(), vpKernel));
    }

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpRenderKernel::Destroy()
{
    VP_FUNC_CALL();

    if (m_kernelDllState)
    {
        KernelDll_ReleaseStates(m_kernelDllState);
    }

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpPlatformInterface::InitPolicyRules(VP_POLICY_RULES &rules)
{
    VP_FUNC_CALL();
    rules.sfcMultiPassSupport.csc.enable = false;
    if (m_sfc2PassScalingEnabled)
    {
        rules.sfcMultiPassSupport.scaling.enable = true;
        // one pass SFC scaling range is [1/8, 8], two pass cover[1/16, 16](AVS Removal) for both X and Y direction.
        rules.sfcMultiPassSupport.scaling.downScaling.minRatioEnlarged = 0.5;
        rules.sfcMultiPassSupport.scaling.upScaling.maxRatioEnlarged   = 2;

        // For 2 pass upscaling: first pass do 2X, rest for others.
        rules.sfcMultiPassSupport.scaling.upScaling.ratioFor1stPass               = 2;
        rules.sfcMultiPassSupport.scaling.upScaling.scalingIn1stPassIf1PassEnough = false;

        if (m_sfc2PassScalingPerfMode)
        {
            // for 2 pass downscaling: first pass do 1/8, rest for others.
            rules.sfcMultiPassSupport.scaling.downScaling.ratioFor1stPass               = 1.0F / 8;
            rules.sfcMultiPassSupport.scaling.downScaling.scalingIn1stPassIf1PassEnough = true;
        }
        else
        {
            // for 2 pass downscaling: first pass do 1/2, rest for others.
            rules.sfcMultiPassSupport.scaling.downScaling.ratioFor1stPass               = 0.5;
            rules.sfcMultiPassSupport.scaling.downScaling.scalingIn1stPassIf1PassEnough = false;
        }
    }
    else
    {
        rules.sfcMultiPassSupport.scaling.enable = false;
    }

    rules.isAvsSamplerSupported = false;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpRenderKernel::SetKernelName(std::string kernelname)
{
    VP_FUNC_CALL();
    m_kernelName.assign(kernelname);

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpRenderKernel::SetKernelBinOffset(uint32_t offset)
{
    VP_FUNC_CALL();
    m_kernelBinOffset = offset;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpRenderKernel::SetKernelBinSize(uint32_t size)
{
    VP_FUNC_CALL();
    m_kernelBinSize = size;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpRenderKernel::SetKernelBinPointer(void *pBin)
{
    VP_FUNC_CALL();
    VP_RENDER_CHK_NULL_RETURN(pBin);
    m_kernelBin = pBin;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpRenderKernel::AddKernelArg(KRN_ARG &kernelArg)
{
    VP_FUNC_CALL();

    m_kernelArgs.push_back(kernelArg);
    return MOS_STATUS_SUCCESS;
}

void VpPlatformInterface::AddVpIsaKernelEntryToList(
    const uint32_t *kernelBin,
    uint32_t        kernelBinSize,
    std::string     postfix,
    DelayLoadedKernelType delayKernelType)
{
    VP_FUNC_CALL();

    VP_KERNEL_BINARY_ENTRY tmpEntry = {};
    tmpEntry.kernelBin     = kernelBin;
    tmpEntry.kernelBinSize = kernelBinSize;
    tmpEntry.postfix       = postfix;
    tmpEntry.kernelType    = delayKernelType;

    if (delayKernelType == KernelNone)
    {
        m_vpIsaKernelBinaryList.push_back(tmpEntry);
    }
    else
    {
        m_vpDelayLoadedBinaryList.push_back(tmpEntry);
        m_vpDelayLoadedFeatureSet.insert(std::make_pair(delayKernelType, false));
    }
}

void VpPlatformInterface::AddVpL0KernelEntryToList(
    const uint32_t *kernelBin,
    uint32_t        kernelBinSize,
    std::string     kernelName)
{
    VP_FUNC_CALL();

    VP_KERNEL_BINARY_ENTRY tmpEntry = {};
    tmpEntry.kernelBin     = kernelBin;
    tmpEntry.kernelBinSize = kernelBinSize;

    m_vpL0KernelBinaryList.insert(std::make_pair(kernelName, tmpEntry));
}

void       KernelDll_ModifyFunctionPointers_Next(Kdll_State *pState);

MOS_STATUS VpPlatformInterface::InitVpRenderHwCaps()
{
    VP_FUNC_CALL();

    if (m_isRenderDisabled)
    {
        VP_PUBLIC_NORMALMESSAGE("Bypass InitVpRenderHwCaps, since render disabled.");
        return MOS_STATUS_SUCCESS;
    }

    VP_RENDER_CHK_NULL_RETURN(m_vpKernelBinary.kernelBin);
    VP_RENDER_CHK_NULL_RETURN(m_vpKernelBinary.fcPatchKernelBin);
    // Only Lpm Plus will use this base function
    m_modifyKdllFunctionPointers = KernelDll_ModifyFunctionPointers_Next;
#if defined(ENABLE_KERNELS)
    InitVPFCKernels(
        g_KdllRuleTable_Next,
        m_vpKernelBinary.kernelBin,
        m_vpKernelBinary.kernelBinSize,
        m_vpKernelBinary.fcPatchKernelBin,
        m_vpKernelBinary.fcPatchKernelBinSize,
        m_modifyKdllFunctionPointers);
#endif

    if (!m_vpIsaKernelBinaryList.empty())
    {
        // Init CM kernel form VP ISA Kernel Binary List
        for (auto &curKernelEntry : m_vpIsaKernelBinaryList)
        {
            VP_PUBLIC_CHK_STATUS_RETURN(InitVpCmKernels(curKernelEntry.kernelBin, curKernelEntry.kernelBinSize, curKernelEntry.postfix));
        }
    }

    if (!m_vpL0KernelBinaryList.empty())
    {
        // Init L0 kernel form VP L0 Kernel Binary List
        for (auto &curKernelEntry : m_vpL0KernelBinaryList)
        {
            VP_PUBLIC_CHK_STATUS_RETURN(InitVpL0Kernels(curKernelEntry.first, curKernelEntry.second));
        }
    }
    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpPlatformInterface::InitVpL0Kernels(
    std::string            kernelName,
    VP_KERNEL_BINARY_ENTRY kernelBinaryEntry)
{
    VP_FUNC_CALL();

    VpRenderKernel vpKernel;

    vpKernel.SetKernelBinPointer((void *)kernelBinaryEntry.kernelBin);
    vpKernel.SetKernelName(kernelName);
    vpKernel.SetKernelBinOffset(0x0);
    vpKernel.SetKernelBinSize(kernelBinaryEntry.kernelBinSize);
    m_kernelPool.insert(std::make_pair(vpKernel.GetKernelName(), vpKernel));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpPlatformInterface::InitVpCmKernels(
    const uint32_t *cisaCode,
    uint32_t        cisaCodeSize,
    std::string     postfix)
{
    VP_FUNC_CALL();
    VP_RENDER_CHK_NULL_RETURN(cisaCode);

    if (cisaCodeSize == 0)
    {
        return MOS_STATUS_INVALID_PARAMETER;
    }

    uint8_t *pBuf             = (uint8_t *)cisaCode;
    uint32_t bytePos          = 0;
    uint32_t cisaMagicNumber  = 0;
    uint8_t  cisaMajorVersion = 0;
    uint8_t  cisaMinorVersion = 0;
    vISA::ISAfile *isaFile    = nullptr;

    READ_FIELD_FROM_BUF(cisaMagicNumber, uint32_t);
    READ_FIELD_FROM_BUF(cisaMajorVersion, uint8_t);
    READ_FIELD_FROM_BUF(cisaMinorVersion, uint8_t);

    auto getVersionAsInt = [](int major, int minor) { return major * 100 + minor; };
    if (getVersionAsInt(cisaMajorVersion, cisaMinorVersion) < getVersionAsInt(3, 2) ||
        cisaMagicNumber != CISA_MAGIC_NUMBER)
    {
        return MOS_STATUS_INVALID_PARAMETER;
    }

    isaFile = MOS_New(vISA::ISAfile, (uint8_t *)cisaCode, cisaCodeSize);
    VP_RENDER_CHK_NULL_RETURN(isaFile);

    if (!isaFile->readFile())
    {
        return MOS_STATUS_INVALID_PARAMETER;
    }

    vISA::Header *header = isaFile->getHeader();
    VP_PUBLIC_CHK_NULL_RETURN(header);

    for (uint32_t i = 0; i < header->getNumKernels(); i++)
    {
        vISA::Kernel *kernel = header->getKernelInfo()[i];

        VP_PUBLIC_CHK_NULL_RETURN(kernel);

        if (kernel->getName() == nullptr || kernel->getNameLen() < 1 || kernel->getNameLen() > 256)
        {
            VP_PUBLIC_CHK_STATUS_RETURN(MOS_STATUS_INVALID_PARAMETER);
        }

        std::string kernelName(kernel->getName(), kernel->getNameLen());
        std::string fullKernelName = kernelName;
        if (!postfix.empty())
        {
            fullKernelName += ('_' + postfix);
        }

        if (m_kernelPool.end() != m_kernelPool.find(fullKernelName))
        {
            continue;
        }

        VpRenderKernel vpKernel;
        vpKernel.SetKernelName(fullKernelName);
        vpKernel.SetKernelBinPointer((void *)cisaCode);

        uint8_t          numGenBinaries = kernel->getNumGenBinaries();
        vISA::GenBinary *genBinary      = kernel->getGenBinaryInfo()[numGenBinaries - 1];

        vpKernel.SetKernelBinOffset(genBinary->getBinaryOffset());
        vpKernel.SetKernelBinSize(genBinary->getBinarySize());

        vISA::KernelBody *kernelBody = isaFile->getKernelsData().at(i);
        VP_PUBLIC_CHK_NULL_RETURN(kernelBody);

        if (kernelBody->getNumInputs() > CM_MAX_ARGS_PER_KERNEL)
        {
            return MOS_STATUS_INVALID_PARAMETER;
        }

        for (uint32_t j = 0; j < kernelBody->getNumInputs(); j++)
        {
            KRN_ARG          kernelArg = {};
            vISA::InputInfo *inputInfo = kernelBody->getInputInfo()[j];
            VP_PUBLIC_CHK_NULL_RETURN(inputInfo);
            uint8_t          kind      = inputInfo->getKind();

            if (kind == 0x2)  // compiler value for surface
            {
                kind = ARG_KIND_SURFACE;  // runtime value for surface. surface will be further classified to 1D/2D/3D
            }
            else if (kind == 0x3)  // compiler value for vme index
            {
                kind = ARG_KIND_VME_INDEX;
            }
            else if (kind == 0x8)
            {
                kind = ARG_KIND_IMPLICT_LOCALSIZE;
            }
            else if (kind == 0x10)
            {
                kind = ARG_KIND_IMPLICT_GROUPSIZE;
            }
            else if (kind == 0x18)
            {
                kind = ARG_KIND_IMPLICIT_LOCALID;
            }
            else if (kind == 0x2A)
            {
                kind = ARG_KIND_SURFACE_2D_SCOREBOARD;
            }
            else if (kind == 0x20)
            {
                kind = ARG_KIND_GENERAL_DEPVEC;
            }
            else if (kind == 0x30)
            {
                kind = ARG_KIND_GENERAL_DEPCNT;
            }
            else if (kind == 0x80)
            {
                // IMP_PSEUDO_INPUT = 0x80 is pseudo input. All inputs after this
                // will be ignored by CMRT without checking and payload copied.
                // This resizes the argument count to achieve this.
                return MOS_STATUS_UNIMPLEMENTED;
            }

            kernelArg.uIndex           = j;
            kernelArg.eArgKind         = (KRN_ARG_KIND)kind;
            kernelArg.uOffsetInPayload = inputInfo->getOffset() - CM_PAYLOAD_OFFSET;
            kernelArg.uSize            = inputInfo->getSize();

            vpKernel.AddKernelArg(kernelArg);
        }

        m_kernelPool.insert(std::make_pair(vpKernel.GetKernelName(), vpKernel));
    }

    MOS_Delete(isaFile);

    return MOS_STATUS_SUCCESS;
}

VpPlatformInterface::~VpPlatformInterface()
{
    for (auto& kernel : m_kernelPool)
    {
        kernel.second.Destroy();
    }

    if (!m_vpDelayLoadedBinaryList.empty())
    {
        m_vpDelayLoadedBinaryList.clear();
    }
}

MOS_STATUS VpPlatformInterface::GetKernelParam(VpKernelID kernlId, RENDERHAL_KERNEL_PARAM &param)
{
    VP_FUNC_CALL();

    VP_PUBLIC_CHK_STATUS_RETURN(GetKernelConfig().GetKernelParam(kernlId, param));
    return MOS_STATUS_SUCCESS;
}

void VpPlatformInterface::SetVpFCKernelBinary(
                const uint32_t   *kernelBin,
                uint32_t         kernelBinSize,
                const uint32_t   *fcPatchKernelBin,
                uint32_t         fcPatchKernelBinSize)
{
    VP_FUNC_CALL();
    
    m_vpKernelBinary.kernelBin            = kernelBin;
    m_vpKernelBinary.kernelBinSize        = kernelBinSize;
    m_vpKernelBinary.fcPatchKernelBin     = fcPatchKernelBin;
    m_vpKernelBinary.fcPatchKernelBinSize = fcPatchKernelBinSize;
}

MOS_STATUS VpPlatformInterface::InitializeDelayedKernels(DelayLoadedKernelType type)
{
    VP_FUNC_CALL();
    auto feature = m_vpDelayLoadedFeatureSet.find(type);
    if (feature != m_vpDelayLoadedFeatureSet.end() && feature->second == false && !m_vpDelayLoadedBinaryList.empty())
    {
        // Init CM kernel form VP ISA Kernel Binary List
        for (auto it = m_vpDelayLoadedBinaryList.begin(); it != m_vpDelayLoadedBinaryList.end();)
        {
            if (it->kernelType == type)
            {
                VP_PUBLIC_CHK_STATUS_RETURN(InitVpCmKernels(it->kernelBin, it->kernelBinSize, it->postfix));
                m_vpDelayLoadedBinaryList.erase(it);
            }
            else
            {
                ++it;
            }
        }
        feature->second = true;
    }
    return MOS_STATUS_SUCCESS;
}

//only for get kernel binary in legacy path not being used in APO path.
MOS_STATUS VpPlatformInterface ::GetKernelBinary(const void *&kernelBin, uint32_t &kernelSize, const void *&patchKernelBin, uint32_t &patchKernelSize)
{
    VP_FUNC_CALL();

    kernelBin       = nullptr;
    kernelSize      = 0;
    patchKernelBin  = nullptr;
    patchKernelSize = 0;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS VpPlatformInterface::GetInputFrameWidthHeightAlignUnit(
    PVP_MHWINTERFACE          pvpMhwInterface,
    uint32_t                 &widthAlignUnit,
    uint32_t                 &heightAlignUnit,
    bool                      bVdbox,
    CODECHAL_STANDARD         codecStandard,
    CodecDecodeJpegChromaType jpegChromaType)
{
    VP_FUNC_CALL();

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;
    VP_PUBLIC_CHK_NULL_RETURN(m_sfcItf);
    VP_PUBLIC_CHK_STATUS_RETURN(m_sfcItf->GetInputFrameWidthHeightAlignUnit(widthAlignUnit, heightAlignUnit, bVdbox, codecStandard, jpegChromaType));

    return eStatus;
}

MOS_STATUS VpPlatformInterface::GetVeboxHeapInfo(
    PVP_MHWINTERFACE          pvpMhwInterface,
    const MHW_VEBOX_HEAP    **ppVeboxHeap)
{
    VP_FUNC_CALL();

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;
    const MHW_VEBOX_HEAP *pVeboxHeap = nullptr;
    VP_PUBLIC_CHK_NULL_RETURN(m_veboxItf);

    VP_RENDER_CHK_STATUS_RETURN(m_veboxItf->GetVeboxHeapInfo(
        &pVeboxHeap));
    *ppVeboxHeap = (const MHW_VEBOX_HEAP *)pVeboxHeap;

    return eStatus;
}

bool VpPlatformInterface::IsVeboxScalabilityWith4KNotSupported(
    VP_MHWINTERFACE          vpMhwInterface)
{
    if (m_veboxItf && !(m_veboxItf->IsVeboxScalabilitywith4K()))
    {
        return true;
    }
    else
    {
        return false;
    }
}

void VpPlatformInterface::DisableRender()
{
    // media sfc interface should come to here.
    VP_PUBLIC_NORMALMESSAGE("Disable Render.");
    m_isRenderDisabled = true;
}
