/*
* Copyright 2020 NVIDIA Corporation.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <algorithm>
#include <chrono>
#include <iostream>

#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "VkVideoDecoder/VkVideoDecoder.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

#undef max
#undef min

#define GPU_ALIGN(x) (((x) + 0xff) & ~0xff)

const uint64_t gFenceTimeout = 100 * 1000 * 1000 /* 100 mSec */;

const char* VkVideoDecoder::GetVideoCodecString(VkVideoCodecOperationFlagBitsKHR codec)
{
    static struct {
        VkVideoCodecOperationFlagBitsKHR eCodec;
        const char* name;
    } aCodecName[] = {
        { VK_VIDEO_CODEC_OPERATION_NONE_KHR, "None" },
        { VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "AVC/H.264" },
        { VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "H.265/HEVC" },
#ifdef VK_EXT_video_decode_vp9
        { VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "VP9" },
#endif // VK_EXT_video_decode_vp9
#ifdef VK_EXT_video_decode_av1
        { VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "AV1" },
#endif // VK_EXT_video_decode_av1
    };

    for (unsigned i = 0; i < sizeof(aCodecName) / sizeof(aCodecName[0]); i++) {
        if (codec == aCodecName[i].eCodec) {
            return aCodecName[codec].name;
        }
    }

    return "Unknown";
}

const char* VkVideoDecoder::GetVideoChromaFormatString(VkVideoChromaSubsamplingFlagBitsKHR chromaFormat)
{

    switch (chromaFormat) {
    case VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR:
        return "YCbCr 400 (Monochrome)";
    case VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR:
        return "YCbCr 420";
    case VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR:
        return "YCbCr 422";
    case VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR:
        return "YCbCr 444";
    default:
        assert(!"Unknown Chroma sub-sampled format");
    };

    return "Unknown";
}

uint32_t VkVideoDecoder::GetNumDecodeSurfaces(VkVideoCodecOperationFlagBitsKHR codec, uint32_t minNumDecodeSurfaces, uint32_t width,
    uint32_t height)
{

#ifdef VK_EXT_video_decode_vp9
    if (codec == VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) {
        return 12;
    }
#endif // VK_EXT_video_decode_vp9

    if (codec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
        // H264: minNumDecodeSurfaces plus 4 for non-reference render target plus 4 for display
        return minNumDecodeSurfaces + 4 + 4;
    }

    if (codec == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
        // ref HEVC spec: A.4.1 General tier and level limits
        // currently assuming level 6.2, 8Kx4K
        int maxLumaPS = 35651584;
        int maxDpbPicBuf = 6;
        int picSizeInSamplesY = (int)(width * height);
        int maxDpbSize;
        if (picSizeInSamplesY <= (maxLumaPS >> 2))
            maxDpbSize = maxDpbPicBuf * 4;
        else if (picSizeInSamplesY <= (maxLumaPS >> 1))
            maxDpbSize = maxDpbPicBuf * 2;
        else if (picSizeInSamplesY <= ((3 * maxLumaPS) >> 2))
            maxDpbSize = (maxDpbPicBuf * 4) / 3;
        else
            maxDpbSize = maxDpbPicBuf;
        return (std::min)(maxDpbSize, 16) + 4;
    }

    return 8;
}

/* Callback function to be registered for getting a callback when decoding of
 * sequence starts. Return value from HandleVideoSequence() are interpreted as :
 *  0: fail, 1: suceeded, > 1: override dpb size of parser (set by
 * nvVideoParseParameters::ulMaxNumDecodeSurfaces while creating parser)
 */
int32_t VkVideoDecoder::StartVideoSequence(VkParserDetectedVideoFormat* pVideoFormat)
{
    const bool testUseLargestSurfaceExtent = false;
    // Assume 4k content for testing surfaces
    const uint32_t surfaceMinWidthExtent  = 4096;
    const uint32_t surfaceMinHeightExtent = 4096;

    VkExtent2D codedExtent = { pVideoFormat->coded_width, pVideoFormat->coded_height };

    // Width and height of the image surface
    VkExtent2D imageExtent = VkExtent2D { std::max((uint32_t)(pVideoFormat->display_area.right  - pVideoFormat->display_area.left), pVideoFormat->coded_width),
                                          std::max((uint32_t)(pVideoFormat->display_area.bottom - pVideoFormat->display_area.top),  pVideoFormat->coded_height) };

    // If we are testing content with different sizes against max sized surface vs. images dynamic resize
    // then set the imageExtent to the max surface size selected.
    if (testUseLargestSurfaceExtent) {
        imageExtent = { std::max(surfaceMinWidthExtent,  imageExtent.width),
                        std::max(surfaceMinHeightExtent, imageExtent.height) };
    }

    std::cout << "Video Input Information" << std::endl
              << "\tCodec        : " << GetVideoCodecString(pVideoFormat->codec) << std::endl
              << "\tFrame rate   : " << pVideoFormat->frame_rate.numerator << "/" << pVideoFormat->frame_rate.denominator << " = "
              << ((pVideoFormat->frame_rate.denominator != 0) ? (1.0 * pVideoFormat->frame_rate.numerator / pVideoFormat->frame_rate.denominator) : 0.0) << " fps" << std::endl
              << "\tSequence     : " << (pVideoFormat->progressive_sequence ? "Progressive" : "Interlaced") << std::endl
              << "\tCoded size   : [" << codedExtent.width << ", " << codedExtent.height << "]" << std::endl
              << "\tDisplay area : [" << pVideoFormat->display_area.left << ", " << pVideoFormat->display_area.top << ", "
              << pVideoFormat->display_area.right << ", " << pVideoFormat->display_area.bottom << "]" << std::endl
              << "\tChroma       : " << GetVideoChromaFormatString(pVideoFormat->chromaSubsampling) << std::endl
              << "\tBit depth    : " << pVideoFormat->bit_depth_luma_minus8 + 8 << std::endl;

    m_numDecodeSurfaces = std::max(m_numDecodeSurfaces, GetNumDecodeSurfaces(pVideoFormat->codec, pVideoFormat->minNumDecodeSurfaces, codedExtent.width, codedExtent.height));

    VkResult result = VK_SUCCESS;

    int32_t videoQueueFamily = m_vkDevCtx->GetVideoDecodeQueueFamilyIdx();
    VkVideoCodecOperationFlagsKHR videoCodecs = VulkanVideoCapabilities::GetSupportedCodecs(m_vkDevCtx,
            m_vkDevCtx->getPhysicalDevice(),
            &videoQueueFamily,
            VK_QUEUE_VIDEO_DECODE_BIT_KHR,
            VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);
    assert(videoCodecs != VK_VIDEO_CODEC_OPERATION_NONE_KHR);

    if (m_dumpDecodeData) {
        std::cout << "\t" << std::hex << videoCodecs << " HW codec types are available: " << std::dec << std::endl;
    }

    VkVideoCodecOperationFlagBitsKHR videoCodec = pVideoFormat->codec;

    if (m_dumpDecodeData) {
        std::cout << "\tcodec " << VkVideoCoreProfile::CodecToName(videoCodec) << std::endl;
    }

    VkVideoCoreProfile videoProfile(videoCodec, pVideoFormat->chromaSubsampling, pVideoFormat->lumaBitDepth, pVideoFormat->chromaBitDepth,
                                    pVideoFormat->codecProfile);
    if (!VulkanVideoCapabilities::IsCodecTypeSupported(m_vkDevCtx,
                                                       m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                                       videoCodec)) {
        std::cout << "*** The video codec " << VkVideoCoreProfile::CodecToName(videoCodec) << " is not supported! ***" << std::endl;
        assert(!"The video codec is not supported");
        return -1;
    }

    if (m_videoFormat.coded_width && m_videoFormat.coded_height) {
        // CreateDecoder() has been called before, and now there's possible config change
        m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, m_defaultVideoQueueIndx);

        if (*m_vkDevCtx) {
            m_vkDevCtx->DeviceWaitIdle();
        }
    }

    std::cout << "Video Decoding Params:" << std::endl
              << "\tNum Surfaces : " << m_numDecodeSurfaces << std::endl
              << "\tResize       : " << codedExtent.width << " x " << codedExtent.height << std::endl;

    uint32_t maxDpbSlotCount   = pVideoFormat->maxNumDpbSlots;

    assert(VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR == pVideoFormat->chromaSubsampling ||
           VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR == pVideoFormat->chromaSubsampling);


    VkVideoCapabilitiesKHR videoCapabilities;
    VkVideoDecodeCapabilitiesKHR videoDecodeCapabilities;
    result = VulkanVideoCapabilities::GetVideoDecodeCapabilities(m_vkDevCtx, videoProfile,
                                                                 videoCapabilities,
                                                                 videoDecodeCapabilities);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get Video Capabilities :" << result << " ***" << std::endl;
        assert(!"Could not get Video Capabilities!");
        return -1;
    }

    VkFormat referencePicturesFormat = VK_FORMAT_UNDEFINED;
    VkFormat pictureFormat = VK_FORMAT_UNDEFINED;
    result = VulkanVideoCapabilities::GetSupportedVideoFormats(m_vkDevCtx, videoProfile,
                                                               videoDecodeCapabilities.flags,
                                                               pictureFormat,
                                                               referencePicturesFormat);
    if (result != VK_SUCCESS) {
        std::cout << "*** Could not get supported video formats :" << result << " ***" << std::endl;
        assert(!"Could not get supported video formats!");
        return -1;
    }

    imageExtent.width  = std::max(imageExtent.width, videoCapabilities.minCodedExtent.width);
    imageExtent.height = std::max(imageExtent.height, videoCapabilities.minCodedExtent.height);

    uint32_t alignWidth = videoCapabilities.pictureAccessGranularity.width - 1;
    imageExtent.width = ((imageExtent.width + alignWidth) & ~alignWidth);
    uint32_t alignHeight = videoCapabilities.pictureAccessGranularity.height - 1;
    imageExtent.height = ((imageExtent.height + alignHeight) & ~alignHeight);

    if (!m_videoSession ||
            !m_videoSession->IsCompatible( m_vkDevCtx,
                                           m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                           &videoProfile,
                                           pictureFormat,
                                           imageExtent,
                                           referencePicturesFormat,
                                           maxDpbSlotCount,
                                           std::max<uint32_t>(maxDpbSlotCount, VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS)) ) {
        result = NvVideoSession::Create( m_vkDevCtx,
                                         m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                         &videoProfile,
                                         pictureFormat,
                                         imageExtent,
                                         referencePicturesFormat,
                                         maxDpbSlotCount,
                                         std::max<uint32_t>(maxDpbSlotCount, VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS),
                                         m_videoSession);

        // after creating a new video session, we need codec reset.
        m_resetDecoder = true;
        assert(result == VK_SUCCESS);
    }

    int32_t ret =
    m_videoFrameBuffer->InitImagePool(videoProfile.GetProfile(),
                                       m_numDecodeSurfaces,
                                       referencePicturesFormat,
                                       codedExtent,
                                       imageExtent,
                                       VK_IMAGE_TILING_OPTIMAL,
                                       VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                                            VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                                       m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                                       m_useImageArray, m_useImageViewArray,
                                       m_useSeparateOutputImages, m_useLinearOutput);

    assert((uint32_t)ret == m_numDecodeSurfaces);
    if ((uint32_t)ret != m_numDecodeSurfaces) {
        fprintf(stderr, "\nERROR: InitImagePool() ret(%d) != m_numDecodeSurfaces(%d)\n", ret, m_numDecodeSurfaces);
    }

    if (m_dumpDecodeData) {
        std::cout << "Allocating Video Device Memory" << std::endl
                  << "Allocating " << m_numDecodeSurfaces << " Num Decode Surfaces and "
                  << maxDpbSlotCount << " Video Device Memory Images for DPB " << std::endl
                  << imageExtent.width << " x " << imageExtent.height << std::endl;
    }
    m_maxDecodeFramesCount = m_numDecodeSurfaces;

    m_decodeFramesData.resize(m_maxDecodeFramesCount,
                              codedExtent.width,
                              codedExtent.height,
                              pVideoFormat->chromaSubsampling,
                              videoCapabilities.minBitstreamBufferOffsetAlignment,
                              videoCapabilities.minBitstreamBufferSizeAlignment);


    // Save the original config
    m_videoFormat = *pVideoFormat;
    return m_numDecodeSurfaces;
}

bool VkVideoDecoder::UpdatePictureParameters(VkPictureParameters* pPictureParameters,
                                          VkSharedBaseObj<VkVideoRefCountBase>& pictureParametersObject,
                                          uint64_t updateSequenceCount)
{

    VkSharedBaseObj<StdVideoPictureParametersSet> pictureParametersSet(StdVideoPictureParametersSet::Create(pPictureParameters, updateSequenceCount));
    if (!pictureParametersSet) {
        assert(!"Invalid pictureParametersSet");
        return false;
    }

    int32_t nodeId = -1;
    bool isNodeId = false;
    StdVideoPictureParametersSet::ItemType nodeParent = StdVideoPictureParametersSet::INVALID_TYPE;
    StdVideoPictureParametersSet::ItemType nodeChild = StdVideoPictureParametersSet::INVALID_TYPE;
    switch (pictureParametersSet->m_itemType) {
    case StdVideoPictureParametersSet::PPS_TYPE:
        nodeParent = StdVideoPictureParametersSet::SPS_TYPE;
        nodeId = pictureParametersSet->GetPpsId(isNodeId);
        assert(isNodeId);
        if (m_lastPictParamsQueue[nodeParent]) {
            const int32_t spsParentId = pictureParametersSet->GetSpsId(isNodeId);
            assert(!isNodeId);
            if (spsParentId == m_lastIdInQueue[nodeParent]) {
                pictureParametersSet->m_parent = m_lastPictParamsQueue[nodeParent];
                assert(spsParentId == m_lastPictParamsQueue[nodeParent]->GetSpsId(isNodeId));
                assert(isNodeId);
            }
        }
        break;
    case StdVideoPictureParametersSet::SPS_TYPE:
        nodeParent = StdVideoPictureParametersSet::VPS_TYPE;
        nodeChild = StdVideoPictureParametersSet::PPS_TYPE;
        nodeId = pictureParametersSet->GetSpsId(isNodeId);
        if (!((uint32_t)nodeId < VkParserVideoPictureParameters::MAX_SPS_IDS)) {
            assert(!"SPS ID is out of bounds");
        }
        assert(isNodeId);
        if (m_lastPictParamsQueue[nodeChild]) {
            const int32_t spsChildId = m_lastPictParamsQueue[nodeChild]->GetSpsId(isNodeId);
            assert(!isNodeId);
            if (spsChildId == nodeId) {
                m_lastPictParamsQueue[nodeChild]->m_parent = pictureParametersSet;
            }
        }
        if (m_lastPictParamsQueue[nodeParent]) {
            const int32_t vpsParentId = pictureParametersSet->GetVpsId(isNodeId);
            assert(!isNodeId);
            if (vpsParentId == m_lastIdInQueue[nodeParent]) {
                pictureParametersSet->m_parent = m_lastPictParamsQueue[nodeParent];
                assert(vpsParentId == m_lastPictParamsQueue[nodeParent]->GetVpsId(isNodeId));
                assert(isNodeId);
            }
        }
        break;
    case StdVideoPictureParametersSet::VPS_TYPE:
        nodeChild = StdVideoPictureParametersSet::SPS_TYPE;
        nodeId = pictureParametersSet->GetVpsId(isNodeId);
        if (!((uint32_t)nodeId < VkParserVideoPictureParameters::MAX_VPS_IDS)) {
            assert(!"VPS ID is out of bounds");
        }
        assert(isNodeId);
        if (m_lastPictParamsQueue[nodeChild]) {
            const int32_t vpsParentId = m_lastPictParamsQueue[nodeChild]->GetVpsId(isNodeId);
            assert(!isNodeId);
            if (vpsParentId == nodeId) {
                m_lastPictParamsQueue[nodeChild]->m_parent = pictureParametersSet;
            }
        }
        break;
    default:
        assert("!Invalid STD type");
        return 0;
    }

    uint32_t nodesTypeMask = AddPictureParametersToQueue(pictureParametersSet);

    if (m_videoSession && nodesTypeMask) {
        FlushPictureParametersQueue();
    }

    pictureParametersObject = pictureParametersSet;
    return true;
}

uint32_t VkVideoDecoder::AddPictureParametersToQueue(VkSharedBaseObj<StdVideoPictureParametersSet>& pictureParametersSet)
{
    m_pictureParametersQueue.push(pictureParametersSet);
    return (1 << pictureParametersSet->m_itemType);
}

uint32_t VkVideoDecoder::FlushPictureParametersQueue()
{
    uint32_t numQueueItems = 0;
    while (!m_pictureParametersQueue.empty()) {
        VkSharedBaseObj<StdVideoPictureParametersSet>& ppItem = m_pictureParametersQueue.front();

        VkSharedBaseObj<StdVideoPictureParametersSet> emptyStdPictureParametersSet;

        switch (ppItem->m_itemType) {
        case StdVideoPictureParametersSet::PPS_TYPE:
            AddPictureParameters(emptyStdPictureParametersSet,
                                 emptyStdPictureParametersSet, ppItem);
            break;
        case StdVideoPictureParametersSet::SPS_TYPE:
            AddPictureParameters(emptyStdPictureParametersSet,
                                 ppItem, emptyStdPictureParametersSet);
            break;
        case StdVideoPictureParametersSet::VPS_TYPE:
            AddPictureParameters(ppItem, emptyStdPictureParametersSet,
                                 emptyStdPictureParametersSet);
            break;
        default:
            assert("!Invalid STD type");
        }

        m_pictureParametersQueue.pop();
        numQueueItems++;
    }

    return numQueueItems;
}

bool VkVideoDecoder::CheckStdObjectBeforeUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& stdPictureParametersSet)
{
    if (!stdPictureParametersSet) {
        return false;
    }

    bool stdObjectUpdate = (stdPictureParametersSet->m_updateSequenceCount > 0);

    if (!m_currentPictureParameters || stdObjectUpdate) {

        assert(m_videoSession);
        assert(stdObjectUpdate || (!stdPictureParametersSet->m_videoSession));
        // Create new Vulkan Picture Parameters object
        return true;

    } else { // new std object
        assert(!stdPictureParametersSet->m_vkObjectOwner);
        assert(!stdPictureParametersSet->m_videoSession);
        assert(m_currentPictureParameters);
        // Update the existing Vulkan Picture Parameters object
    }

    return false;
}

VkParserVideoPictureParameters* VkVideoDecoder::CheckStdObjectAfterUpdate(VkSharedBaseObj<StdVideoPictureParametersSet>& stdPictureParametersSet, VkParserVideoPictureParameters* pNewPictureParametersObject)
{
    if (!stdPictureParametersSet) {
        return nullptr;
    }

    if (pNewPictureParametersObject) {
        if (stdPictureParametersSet->m_updateSequenceCount == 0) {
            stdPictureParametersSet->m_videoSession = m_videoSession;
        } else {
            const VkParserVideoPictureParameters* pOwnerPictureParameters =
                    VkParserVideoPictureParameters::VideoPictureParametersFromBase(stdPictureParametersSet->m_vkObjectOwner);
            if (pOwnerPictureParameters) {
                assert(pOwnerPictureParameters->GetId() < pNewPictureParametersObject->GetId());
            }
        }
        // new object owner
        stdPictureParametersSet->m_vkObjectOwner = pNewPictureParametersObject;
        return pNewPictureParametersObject;

    } else { // new std object
        stdPictureParametersSet->m_videoSession = m_videoSession;
        stdPictureParametersSet->m_vkObjectOwner = m_currentPictureParameters;
    }

    return m_currentPictureParameters;
}

VkParserVideoPictureParameters*  VkVideoDecoder::AddPictureParameters(VkSharedBaseObj<StdVideoPictureParametersSet>& vpsStdPictureParametersSet,
                                                                   VkSharedBaseObj<StdVideoPictureParametersSet>& spsStdPictureParametersSet,
                                                                   VkSharedBaseObj<StdVideoPictureParametersSet>& ppsStdPictureParametersSet)
{

    if ((!ppsStdPictureParametersSet &&
         !spsStdPictureParametersSet &&
         !vpsStdPictureParametersSet)) {
        return nullptr;
    }

    VkParserVideoPictureParameters* pPictureParametersObject = nullptr;
    bool createNewObject = CheckStdObjectBeforeUpdate(ppsStdPictureParametersSet);
    createNewObject = createNewObject || CheckStdObjectBeforeUpdate(spsStdPictureParametersSet);
    createNewObject = createNewObject || CheckStdObjectBeforeUpdate(vpsStdPictureParametersSet);

    if (createNewObject) {
        pPictureParametersObject = VkParserVideoPictureParameters::Create(m_vkDevCtx,
                                                                          m_videoSession,
                                                                          vpsStdPictureParametersSet,
                                                                          spsStdPictureParametersSet,
                                                                          ppsStdPictureParametersSet,
                                                                          m_currentPictureParameters);
        m_currentPictureParameters = pPictureParametersObject;
    } else {
        m_currentPictureParameters->Update(vpsStdPictureParametersSet,
                                           spsStdPictureParametersSet,
                                           ppsStdPictureParametersSet);
    }

    CheckStdObjectAfterUpdate(vpsStdPictureParametersSet, pPictureParametersObject);
    CheckStdObjectAfterUpdate(spsStdPictureParametersSet, pPictureParametersObject);
    CheckStdObjectAfterUpdate(ppsStdPictureParametersSet, pPictureParametersObject);

    return pPictureParametersObject;
}

int VkVideoDecoder::CopyOptimalToLinearImage(VkCommandBuffer& commandBuffer,
                                          VkVideoPictureResourceInfoKHR& srcPictureResource,
                                          VulkanVideoFrameBuffer::PictureResourceInfo& srcPictureResourceInfo,
                                          VkVideoPictureResourceInfoKHR& dstPictureResource,
                                          VulkanVideoFrameBuffer::PictureResourceInfo& dstPictureResourceInfo,
                                          VulkanVideoFrameBuffer::FrameSynchronizationInfo *pFrameSynchronizationInfo)

{
    // Bind memory for the image.
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(srcPictureResourceInfo.imageFormat);

    // Currently formats that have more than 2 output planes are not supported. 444 formats have a shared CbCr planes in all current tests
    assert((mpInfo->vkPlaneFormat[2] == VK_FORMAT_UNDEFINED) && (mpInfo->vkPlaneFormat[3] == VK_FORMAT_UNDEFINED));

    // Copy src buffer to image.
    VkImageCopy copyRegion[3];
    memset(&copyRegion, 0, sizeof(copyRegion));
    copyRegion[0].extent.width = srcPictureResource.codedExtent.width;
    copyRegion[0].extent.height = srcPictureResource.codedExtent.height;
    copyRegion[0].extent.depth = 1;
    copyRegion[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].srcSubresource.mipLevel = 0;
    copyRegion[0].srcSubresource.baseArrayLayer = srcPictureResource.baseArrayLayer;
    copyRegion[0].srcSubresource.layerCount = 1;
    copyRegion[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].dstSubresource.mipLevel = 0;
    copyRegion[0].dstSubresource.baseArrayLayer = dstPictureResource.baseArrayLayer;
    copyRegion[0].dstSubresource.layerCount = 1;
    copyRegion[1].extent.width = copyRegion[0].extent.width;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledX != 0) {
        copyRegion[1].extent.width /= 2;
    }

    copyRegion[1].extent.height = copyRegion[0].extent.height;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledY != 0) {
        copyRegion[1].extent.height /= 2;
    }

    copyRegion[1].extent.depth = 1;
    copyRegion[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].srcSubresource.mipLevel = 0;
    copyRegion[1].srcSubresource.baseArrayLayer = srcPictureResource.baseArrayLayer;
    copyRegion[1].srcSubresource.layerCount = 1;
    copyRegion[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].dstSubresource.mipLevel = 0;
    copyRegion[1].dstSubresource.baseArrayLayer = dstPictureResource.baseArrayLayer;
    copyRegion[1].dstSubresource.layerCount = 1;

    m_vkDevCtx->CmdCopyImage(commandBuffer, srcPictureResourceInfo.image, srcPictureResourceInfo.currentImageLayout,
                                    dstPictureResourceInfo.image, dstPictureResourceInfo.currentImageLayout,
                                    (uint32_t)2, copyRegion);

    {
        VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               1, &memoryBarrier, 0,
                                0, 0, 0);
    }

    return 0;
}

/* Callback function to be registered for getting a callback when a decoded
 * frame is ready to be decoded. Return value from HandlePictureDecode() are
 * interpreted as: 0: fail, >=1: suceeded
 */
int VkVideoDecoder::DecodePictureWithParameters(VkParserPerFrameDecodeParameters* pPicParams,
                                                VkParserDecodePictureInfo* pDecodePictureInfo)
{
    if (!m_videoSession) {
        assert(!"Decoder not initialized!");
        return -1;
    }

    int32_t currPicIdx = pPicParams->currPicIdx;
    assert((uint32_t)currPicIdx < m_numDecodeSurfaces);

    int32_t picNumInDecodeOrder = m_decodePicCount++;
    m_videoFrameBuffer->SetPicNumInDecodeOrder(currPicIdx, picNumInDecodeOrder);

    NvVkDecodeFrameDataSlot frameDataSlot;
    int32_t retPicIdx = GetCurrentFrameData((uint32_t)currPicIdx, frameDataSlot);
    assert(retPicIdx == currPicIdx);

    if (retPicIdx != currPicIdx) {
        fprintf(stderr, "\nERROR: DecodePictureWithParameters() retPicIdx(%d) != currPicIdx(%d)\n", retPicIdx, currPicIdx);
    }

    assert(pPicParams->bitstreamData->GetMaxSize() >= pPicParams->bitstreamDataLen);

    pPicParams->decodeFrameInfo.srcBuffer = pPicParams->bitstreamData->GetBuffer();
    assert(pPicParams->bitstreamDataOffset == 0);
    assert(pPicParams->firstSliceIndex == 0);
    pPicParams->decodeFrameInfo.srcBufferOffset = pPicParams->bitstreamDataOffset;
    pPicParams->decodeFrameInfo.srcBufferRange = pPicParams->bitstreamDataLen;
    // pPicParams->decodeFrameInfo.dstImageView = VkImageView();

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = nullptr;

    m_vkDevCtx->BeginCommandBuffer(frameDataSlot.commandBuffer, &beginInfo);
    VkVideoBeginCodingInfoKHR decodeBeginInfo = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    // CmdResetQueryPool are NOT Supported yet.

    decodeBeginInfo.videoSession = m_videoSession->GetVideoSession();

    VulkanVideoFrameBuffer::PictureResourceInfo currentDpbPictureResourceInfo = VulkanVideoFrameBuffer::PictureResourceInfo();
    VulkanVideoFrameBuffer::PictureResourceInfo currentOutputPictureResourceInfo = VulkanVideoFrameBuffer::PictureResourceInfo();
    VkVideoPictureResourceInfoKHR currentOutputPictureResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR, nullptr};
    if (pPicParams->currPicIdx != m_videoFrameBuffer->GetCurrentImageResourceByIndex(pPicParams->currPicIdx,
                                                                 &pPicParams->decodeFrameInfo.dstPictureResource,
                                                                 &currentDpbPictureResourceInfo,
                                                                 VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                                                                 &currentOutputPictureResource,
                                                                 &currentOutputPictureResourceInfo,
                                                                 VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR)) {
        assert(!"GetImageResourcesByIndex has failed");
    }

    assert(pPicParams->decodeFrameInfo.srcBuffer);
    const VkBufferMemoryBarrier2KHR bitstreamBufferMemoryBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2_KHR,
        nullptr,
        VK_PIPELINE_STAGE_2_NONE_KHR,
        VK_ACCESS_2_HOST_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
        VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR,
        VK_QUEUE_FAMILY_IGNORED,
        (uint32_t)m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
        pPicParams->decodeFrameInfo.srcBuffer,
        pPicParams->decodeFrameInfo.srcBufferOffset,
        pPicParams->decodeFrameInfo.srcBufferRange
    };

    uint32_t baseArrayLayer = (m_useImageArray || m_useImageViewArray) ? pPicParams->currPicIdx : 0;
    const VkImageMemoryBarrier2KHR dpbBarrierTemplates[1] = {
        { // VkImageMemoryBarrier

            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR, // VkStructureType sType
            nullptr, // const void*     pNext
            VK_PIPELINE_STAGE_2_NONE_KHR, // VkPipelineStageFlags2KHR srcStageMask
            0, // VkAccessFlags2KHR        srcAccessMask
            VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, // VkPipelineStageFlags2KHR dstStageMask;
            VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR, // VkAccessFlags   dstAccessMask
            VK_IMAGE_LAYOUT_UNDEFINED, // VkImageLayout   oldLayout
            VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, // VkImageLayout   newLayout
            VK_QUEUE_FAMILY_IGNORED, // uint32_t        srcQueueFamilyIndex
            (uint32_t)m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(), // uint32_t   dstQueueFamilyIndex
            VkImage(), // VkImage         image;
            {
                // VkImageSubresourceRange   subresourceRange
                VK_IMAGE_ASPECT_COLOR_BIT, // VkImageAspectFlags aspectMask
                0, // uint32_t           baseMipLevel
                1, // uint32_t           levelCount
                baseArrayLayer, // uint32_t           baseArrayLayer
                1, // uint32_t           layerCount;
            } },
    };

    VkImageMemoryBarrier2KHR imageBarriers[VkParserPerFrameDecodeParameters::MAX_DPB_REF_AND_SETUP_SLOTS];
    uint32_t numDpbBarriers = 0;

    if (currentDpbPictureResourceInfo.currentImageLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        imageBarriers[numDpbBarriers] = dpbBarrierTemplates[0];
        imageBarriers[numDpbBarriers].oldLayout = currentDpbPictureResourceInfo.currentImageLayout;
        imageBarriers[numDpbBarriers].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
        imageBarriers[numDpbBarriers].image = currentDpbPictureResourceInfo.image;
        imageBarriers[numDpbBarriers].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        assert(imageBarriers[numDpbBarriers].image);
        numDpbBarriers++;
    }

    VulkanVideoFrameBuffer::PictureResourceInfo pictureResourcesInfo[VkParserPerFrameDecodeParameters::MAX_DPB_REF_AND_SETUP_SLOTS];
    memset(&pictureResourcesInfo[0], 0, sizeof(pictureResourcesInfo));
    const int8_t* pGopReferenceImagesIndexes = pPicParams->pGopReferenceImagesIndexes;
    if (pPicParams->numGopReferenceSlots) {
        if (pPicParams->numGopReferenceSlots != m_videoFrameBuffer->GetDpbImageResourcesByIndex(
                                                                        pPicParams->numGopReferenceSlots,
                                                                        pGopReferenceImagesIndexes,
                                                                        pPicParams->pictureResources,
                                                                        pictureResourcesInfo,
                                                                        VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR)) {
            assert(!"GetImageResourcesByIndex has failed");
        }
        for (int32_t resId = 0; resId < pPicParams->numGopReferenceSlots; resId++) {
            // slotLayer requires NVIDIA specific extension VK_KHR_video_layers, not enabled, just yet.
            // pGopReferenceSlots[resId].slotLayerIndex = 0;
            // pictureResourcesInfo[resId].image can be a nullptr handle if the picture is not-existent.
            if (pictureResourcesInfo[resId].image && (pictureResourcesInfo[resId].currentImageLayout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) && (pictureResourcesInfo[resId].currentImageLayout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR)) {
                imageBarriers[numDpbBarriers] = dpbBarrierTemplates[0];
                imageBarriers[numDpbBarriers].oldLayout = pictureResourcesInfo[resId].currentImageLayout;
                imageBarriers[numDpbBarriers].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
                imageBarriers[numDpbBarriers].image = pictureResourcesInfo[resId].image;
                assert(imageBarriers[numDpbBarriers].image);
                numDpbBarriers++;
            }
        }
    }

    decodeBeginInfo.referenceSlotCount = pPicParams->decodeFrameInfo.referenceSlotCount;
    decodeBeginInfo.pReferenceSlots = pPicParams->decodeFrameInfo.pReferenceSlots;

    if (pDecodePictureInfo->flags.unpairedField) {
        // assert(pFrameSyncinfo->frameCompleteSemaphore == VkSemaphore());
        pDecodePictureInfo->flags.syncFirstReady = true;
    }
    // FIXME: the below sequence for interlaced synchronization.
    pDecodePictureInfo->flags.syncToFirstField = false;

    VulkanVideoFrameBuffer::FrameSynchronizationInfo frameSynchronizationInfo = VulkanVideoFrameBuffer::FrameSynchronizationInfo();
    frameSynchronizationInfo.hasFrameCompleteSignalFence = true;
    frameSynchronizationInfo.hasFrameCompleteSignalSemaphore = true;

    FlushPictureParametersQueue();

    assert(pPicParams->pCurrentPictureParameters->m_vkObjectOwner);
    const VkParserVideoPictureParameters* pOwnerPictureParameters =
            VkParserVideoPictureParameters::VideoPictureParametersFromBase(pPicParams->pCurrentPictureParameters->m_vkObjectOwner);
    assert(pOwnerPictureParameters);
    assert(pOwnerPictureParameters->GetId() <= m_currentPictureParameters->GetId());

    bool isSps = false;
    int32_t spsId = pPicParams->pCurrentPictureParameters->GetSpsId(isSps);
    assert(!isSps);
    assert(spsId >= 0);
    assert(pOwnerPictureParameters->HasSpsId(spsId));
    bool isPps = false;
    int32_t ppsId =  pPicParams->pCurrentPictureParameters->GetPpsId(isPps);
    assert(isPps);
    assert(ppsId >= 0);
    assert(pOwnerPictureParameters->HasPpsId(ppsId));

    decodeBeginInfo.videoSessionParameters = *pOwnerPictureParameters;

    if (m_dumpDecodeData) {
        std::cout << "Using object " << decodeBeginInfo.videoSessionParameters << " with ID: (" << pOwnerPictureParameters->GetId() << ")" << " for SPS: " <<  spsId << ", PPS: " << ppsId << std::endl;
    }

    VkSharedBaseObj<VkVideoRefCountBase> bitstreamBuffer(pPicParams->bitstreamData);
    int32_t retVal = m_videoFrameBuffer->QueuePictureForDecode(currPicIdx, pDecodePictureInfo,
                                                               bitstreamBuffer,
                                                               pPicParams->pCurrentPictureParameters->m_vkObjectOwner,
                                                               &frameSynchronizationInfo);
    if (currPicIdx != retVal) {
        assert(!"QueuePictureForDecode has failed");
    }

    VkFence frameCompleteFence = frameSynchronizationInfo.frameCompleteFence;
    VkFence frameConsumerDoneFence = frameSynchronizationInfo.frameConsumerDoneFence;
    VkSemaphore frameCompleteSemaphore = frameSynchronizationInfo.frameCompleteSemaphore;
    VkSemaphore frameConsumerDoneSemaphore = frameSynchronizationInfo.frameConsumerDoneSemaphore;

    // m_vkDevCtx->ResetQueryPool(m_vkDev, queryFrameInfo.queryPool, queryFrameInfo.query, 1);

    m_vkDevCtx->CmdResetQueryPool(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId, frameSynchronizationInfo.numQueries);
    m_vkDevCtx->CmdBeginVideoCodingKHR(frameDataSlot.commandBuffer, &decodeBeginInfo);

    if (m_resetDecoder != false) {
        VkVideoCodingControlInfoKHR codingControlInfo = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
                                                          nullptr,
                                                          VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR };

        // Video spec requires mandatory codec reset before the first frame.
        m_vkDevCtx->CmdControlVideoCodingKHR(frameDataSlot.commandBuffer, &codingControlInfo);
        // Done with the reset
        m_resetDecoder = false;
    }

    const VkDependencyInfoKHR dependencyInfo = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
        nullptr,
        VK_DEPENDENCY_BY_REGION_BIT,
        0,
        nullptr,
        1,
        &bitstreamBufferMemoryBarrier,
        numDpbBarriers,
        imageBarriers,
    };
    m_vkDevCtx->CmdPipelineBarrier2KHR(frameDataSlot.commandBuffer, &dependencyInfo);

    m_vkDevCtx->CmdBeginQuery(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId, VkQueryControlFlags());

    m_vkDevCtx->CmdDecodeVideoKHR(frameDataSlot.commandBuffer, &pPicParams->decodeFrameInfo);

    m_vkDevCtx->CmdEndQuery(frameDataSlot.commandBuffer, frameSynchronizationInfo.queryPool, frameSynchronizationInfo.startQueryId);

    VkVideoEndCodingInfoKHR decodeEndInfo = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    m_vkDevCtx->CmdEndVideoCodingKHR(frameDataSlot.commandBuffer, &decodeEndInfo);

    if (m_useSeparateOutputImages || m_useLinearOutput) {
        CopyOptimalToLinearImage(frameDataSlot.commandBuffer,
                                 pPicParams->decodeFrameInfo.dstPictureResource,
                                 currentDpbPictureResourceInfo,
                                 currentOutputPictureResource,
                                 currentOutputPictureResourceInfo,
                                 &frameSynchronizationInfo);
    }

    m_vkDevCtx->EndCommandBuffer(frameDataSlot.commandBuffer);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr };
    submitInfo.waitSemaphoreCount = (frameConsumerDoneSemaphore == VkSemaphore()) ? 0 : 1;
    submitInfo.pWaitSemaphores = &frameConsumerDoneSemaphore;
    VkPipelineStageFlags videoDecodeSubmitWaitStages = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
    submitInfo.pWaitDstStageMask = &videoDecodeSubmitWaitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frameDataSlot.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frameCompleteSemaphore;

    VkResult result = VK_SUCCESS;
    if ((frameConsumerDoneSemaphore == VkSemaphore()) && (frameConsumerDoneFence != VkFence())) {
        result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameConsumerDoneFence, true, gFenceTimeout);
        assert(result == VK_SUCCESS);
        result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
    }

    result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
    if (result == VK_NOT_READY) {
        std::cout << "\t *************** WARNING: frameCompleteFence is not done *************< " << currPicIdx << " >**********************" << std::endl;
        assert(!"frameCompleteFence is not signaled yet");
    }

    const bool checkDecodeFences = false; // For decoder fences debugging
    if (checkDecodeFences) { // For fence/sync debugging
        result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameCompleteFence, true, gFenceTimeout);
        assert(result == VK_SUCCESS);

        result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
        if (result == VK_NOT_READY) {
            std::cout << "\t *********** WARNING: frameCompleteFence is still not done *************< " << currPicIdx << " >**********************" << std::endl;
        }
        assert(result == VK_SUCCESS);
    }

    result = m_vkDevCtx->ResetFences(*m_vkDevCtx, 1, &frameCompleteFence);
    assert(result == VK_SUCCESS);
    result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
    assert(result == VK_NOT_READY);

    m_vkDevCtx->MultiThreadedQueueSubmit(VulkanDeviceContext::DECODE, m_defaultVideoQueueIndx,
                                         1, &submitInfo, frameCompleteFence);

    if (m_dumpDecodeData) {
        std::cout << "\t +++++++++++++++++++++++++++< " << currPicIdx << " >++++++++++++++++++++++++++++++" << std::endl;
        std::cout << "\t => Decode Submitted for CurrPicIdx: " << currPicIdx << std::endl
                  << "\t\tm_nPicNumInDecodeOrder: " << picNumInDecodeOrder << "\t\tframeCompleteFence " << frameCompleteFence
                  << "\t\tframeCompleteSemaphore " << frameCompleteSemaphore << "\t\tdstImageView "
                  << pPicParams->decodeFrameInfo.dstPictureResource.imageViewBinding << std::endl;
    }

    const bool checkDecodeIdleSync = false; // For fence/sync/idle debugging
    if (checkDecodeIdleSync) { // For fence/sync debugging
        if (frameCompleteFence == VkFence()) {
            result = m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, m_defaultVideoQueueIndx);
            assert(result == VK_SUCCESS);
        } else {
            if (frameCompleteSemaphore == VkSemaphore()) {
                result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameCompleteFence, true, gFenceTimeout);
                assert(result == VK_SUCCESS);
                result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
                assert(result == VK_SUCCESS);
            }
        }
    }

    // For fence/sync debugging
    if (pDecodePictureInfo->flags.fieldPic) {
        result = m_vkDevCtx->WaitForFences(*m_vkDevCtx, 1, &frameCompleteFence, true, gFenceTimeout);
        assert(result == VK_SUCCESS);
        result = m_vkDevCtx->GetFenceStatus(*m_vkDevCtx, frameCompleteFence);
        assert(result == VK_SUCCESS);
    }

    const bool checkDecodeStatus = false; // Check the queries
    if (checkDecodeStatus) {
        VkQueryResultStatusKHR decodeStatus;
        result = m_vkDevCtx->GetQueryPoolResults(*m_vkDevCtx,
                                         frameSynchronizationInfo.queryPool,
                                         frameSynchronizationInfo.startQueryId,
                                         1,
                                         sizeof(decodeStatus),
                                         &decodeStatus,
                                         sizeof(decodeStatus),
                                         VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);

        assert(result == VK_SUCCESS);
        assert(decodeStatus == VK_QUERY_RESULT_STATUS_COMPLETE_KHR);

        if (m_dumpDecodeData) {
            std::cout << "\t +++++++++++++++++++++++++++< " << currPicIdx << " >++++++++++++++++++++++++++++++" << std::endl;
            std::cout << "\t => Decode Status for CurrPicIdx: " << currPicIdx << std::endl
                      << "\t\tdecodeStatus: " << decodeStatus << std::endl;
        }
    }

    return currPicIdx;
}

size_t VkVideoDecoder::GetBitstreamBuffer(size_t size,
                                          const uint8_t* pInitializeBufferMemory,
                                          size_t initializeBufferMemorySize,
                                          VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer)
{
    assert(initializeBufferMemorySize <= size);
    // size_t newSize = 4 * 1024 * 1024;
    size_t newSize = size;
    assert(m_vkDevCtx);

    VkSharedBaseObj<VulkanBitstreamBufferImpl> newBitstreamBuffer;

    const bool enablePool = true;
    const bool debugBitstreamBufferDumpAlloc = false;
    int32_t availablePoolNode = -1;
    if (enablePool) {
        availablePoolNode = m_decodeFramesData.GetBitstreamBuffersQueue().GetAvailableNodeFromPool(newBitstreamBuffer);
    }
    if (!(availablePoolNode >= 0)) {
        VkResult result = VulkanBitstreamBufferImpl::Create(m_vkDevCtx,
                m_vkDevCtx->GetVideoDecodeQueueFamilyIdx(),
                newSize, 256, 256, // FIXME: buffer offset and size alignment
                pInitializeBufferMemory, initializeBufferMemorySize, newBitstreamBuffer);
        assert(result == VK_SUCCESS);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "\nERROR: CreateVideoBitstreamBuffer() result: 0x%x\n", result);
            return 0;
        }
        if (debugBitstreamBufferDumpAlloc) {
            std::cout << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;
        }
        if (enablePool) {
            int32_t nodeAddedWithIndex = m_decodeFramesData.GetBitstreamBuffersQueue().AddNodeToPool(newBitstreamBuffer, true);
            if (nodeAddedWithIndex < 0) {
                assert("Could not add the new node to the pool");
            }
        }

    } else {

        assert(newBitstreamBuffer);
        newSize = newBitstreamBuffer->GetMaxSize();
        assert(initializeBufferMemorySize <= newSize);

        size_t copySize = std::min(initializeBufferMemorySize, newSize);
        newBitstreamBuffer->CopyDataFromBuffer((const uint8_t*)pInitializeBufferMemory,
                                               0, // srcOffset
                                               0, // dstOffset
                                               copySize);

        newBitstreamBuffer->MemsetData(0x0, copySize, newSize - copySize);

        if (debugBitstreamBufferDumpAlloc) {
            std::cout << "\t\tFrom bitstream buffer pool with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;

            std::cout << "\t\t\t FreeNodes " << m_decodeFramesData.GetBitstreamBuffersQueue().GetFreeNodesNumber();
            std::cout << " of MaxNodes " << m_decodeFramesData.GetBitstreamBuffersQueue().GetMaxNodes();
            std::cout << ", AvailableNodes " << m_decodeFramesData.GetBitstreamBuffersQueue().GetAvailableNodesNumber();
            std::cout << std::endl;
        }
    }
    bitstreamBuffer = newBitstreamBuffer;
    if (newSize > m_maxStreamBufferSize) {
        std::cout << "\tAllocated bitstream buffer with size " << newSize << " B, " <<
                             newSize/1024 << " KB, " << newSize/1024/1024 << " MB" << std::endl;
        m_maxStreamBufferSize = newSize;
    }
    return bitstreamBuffer->GetMaxSize();
}

VkResult VkVideoDecoder::Create(const VulkanDeviceContext* vkDevCtx,
                                VkSharedBaseObj<VulkanVideoFrameBuffer>& videoFrameBuffer,
                                int32_t videoQueueIndx, bool useLinearOutput,
                                VkSharedBaseObj<VkVideoDecoder>& vkVideoDecoder)
{
    VkSharedBaseObj<VkVideoDecoder> vkDecoder(new VkVideoDecoder(vkDevCtx, videoFrameBuffer, videoQueueIndx, useLinearOutput));
    if (vkDecoder) {
        vkVideoDecoder = vkDecoder;
        return VK_SUCCESS;
    }

    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

void VkVideoDecoder::Deinitialize()
{
    if (m_vkDevCtx == nullptr) {
        return;
    }

    if (m_vkDevCtx->GetVideoDecodeNumQueues() > 1) {
        for (uint32_t queueId = 0; queueId <  (uint32_t)m_vkDevCtx->GetVideoDecodeNumQueues(); queueId++) {
            m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, queueId);
        }
    } else {
        m_vkDevCtx->MultiThreadedQueueWaitIdle(VulkanDeviceContext::DECODE, m_defaultVideoQueueIndx);
    }
    // m_vkDevCtx->DeviceWaitIdle();

    m_videoFrameBuffer = nullptr;
    m_decodeFramesData.deinit();
    m_videoSession = nullptr;
    m_vkDevCtx = nullptr;
}

VkVideoDecoder::~VkVideoDecoder()
{
    Deinitialize();
}

int32_t VkVideoDecoder::AddRef()
{
    return ++m_refCount;
}

int32_t VkVideoDecoder::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        delete this;
    }
    return ret;
}

const char* VkParserVideoPictureParameters::m_refClassId = "VkParserVideoPictureParameters";
int32_t VkParserVideoPictureParameters::m_currentId = 0;

int32_t VkParserVideoPictureParameters::PopulateH264UpdateFields(const StdVideoPictureParametersSet* pStdPictureParametersSet,
                                                                 VkVideoDecodeH264SessionParametersAddInfoKHR& h264SessionParametersAddInfo)
{
    int32_t currentId = -1;
    if (pStdPictureParametersSet == nullptr) {
        return currentId;
    }

    assert( (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H264_SPS) ||
            (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H264_PPS));

    assert(h264SessionParametersAddInfo.sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR);

    if (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H264_SPS) {
        h264SessionParametersAddInfo.stdSPSCount = 1;
        h264SessionParametersAddInfo.pStdSPSs = &pStdPictureParametersSet->m_data.h264Sps.stdSps;
        currentId = pStdPictureParametersSet->m_data.h264Sps.stdSps.seq_parameter_set_id;
    } else if (pStdPictureParametersSet->m_updateType ==  VK_PICTURE_PARAMETERS_UPDATE_H264_PPS ) {
        h264SessionParametersAddInfo.stdPPSCount = 1;
        h264SessionParametersAddInfo.pStdPPSs = &pStdPictureParametersSet->m_data.h264Pps.stdPps;
        currentId = pStdPictureParametersSet->m_data.h264Pps.stdPps.pic_parameter_set_id;
    } else {
        assert(!"Incorrect h.264 type");
    }

    return currentId;
}

int32_t VkParserVideoPictureParameters::PopulateH265UpdateFields(const StdVideoPictureParametersSet* pStdPictureParametersSet,
                                                                 VkVideoDecodeH265SessionParametersAddInfoKHR& h265SessionParametersAddInfo)
{
    int32_t currentId = -1;
    if (pStdPictureParametersSet == nullptr) {
        return currentId;
    }

    assert( (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_VPS) ||
            (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_SPS) ||
            (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_PPS));

    assert(h265SessionParametersAddInfo.sType == VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR);

    if (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_VPS) {
        h265SessionParametersAddInfo.stdVPSCount = 1;
        h265SessionParametersAddInfo.pStdVPSs = &pStdPictureParametersSet->m_data.h265Vps.stdVps;
        currentId = pStdPictureParametersSet->m_data.h265Vps.stdVps.vps_video_parameter_set_id;
    } else if (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_SPS) {
        h265SessionParametersAddInfo.stdSPSCount = 1;
        h265SessionParametersAddInfo.pStdSPSs = &pStdPictureParametersSet->m_data.h265Sps.stdSps;
        currentId = pStdPictureParametersSet->m_data.h265Sps.stdSps.sps_seq_parameter_set_id;
    } else if (pStdPictureParametersSet->m_updateType == VK_PICTURE_PARAMETERS_UPDATE_H265_PPS) {
        h265SessionParametersAddInfo.stdPPSCount = 1;
        h265SessionParametersAddInfo.pStdPPSs = &pStdPictureParametersSet->m_data.h265Pps.stdPps;
        currentId = pStdPictureParametersSet->m_data.h265Pps.stdPps.pps_pic_parameter_set_id;
    } else {
        assert(!"Incorrect h.265 type");
    }

    return currentId;
}

VkParserVideoPictureParameters*
VkParserVideoPictureParameters::Create(const VulkanDeviceContext* vkDevCtx,
                                       VkSharedBaseObj<NvVideoSession>& videoSession,
                                       const StdVideoPictureParametersSet* pVpsStdPictureParametersSet,
                                       const StdVideoPictureParametersSet* pSpsStdPictureParametersSet,
                                       const StdVideoPictureParametersSet* pPpsStdPictureParametersSet,
                                       VkParserVideoPictureParameters* pTemplatePictureParameters)
{
    VkParserVideoPictureParameters* pPictureParameters = new VkParserVideoPictureParameters(vkDevCtx);
    if (!pPictureParameters) {
        return pPictureParameters;
    }

    int32_t currentVpsId = -1;
    int32_t currentSpsId = -1;
    int32_t currentPpsId = -1;

    VkVideoSessionParametersCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR };

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264SessionParametersCreateInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    VkVideoDecodeH264SessionParametersAddInfoKHR h264SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR };

    VkVideoDecodeH265SessionParametersCreateInfoKHR h265SessionParametersCreateInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR };
    VkVideoDecodeH265SessionParametersAddInfoKHR h265SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};

    VkParserPictureParametersUpdateType updateType = pPpsStdPictureParametersSet ? pPpsStdPictureParametersSet->m_updateType :
            (pSpsStdPictureParametersSet ? pSpsStdPictureParametersSet->m_updateType : pVpsStdPictureParametersSet->m_updateType);
    switch (updateType)
    {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
        {

            createInfo.pNext =  &h264SessionParametersCreateInfo;

            h264SessionParametersCreateInfo.maxStdSPSCount = MAX_SPS_IDS;
            h264SessionParametersCreateInfo.maxStdPPSCount = MAX_PPS_IDS;
            h264SessionParametersCreateInfo.pParametersAddInfo = &h264SessionParametersAddInfo;

            currentSpsId = PopulateH264UpdateFields(pSpsStdPictureParametersSet, h264SessionParametersAddInfo);
            currentPpsId = PopulateH264UpdateFields(pPpsStdPictureParametersSet, h264SessionParametersAddInfo);

        }
        break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
        {

            createInfo.pNext =  &h265SessionParametersCreateInfo;

            h265SessionParametersCreateInfo.maxStdVPSCount = MAX_VPS_IDS;
            h265SessionParametersCreateInfo.maxStdSPSCount = MAX_SPS_IDS;
            h265SessionParametersCreateInfo.maxStdPPSCount = MAX_PPS_IDS;
            h265SessionParametersCreateInfo.pParametersAddInfo = &h265SessionParametersAddInfo;

            currentVpsId = PopulateH265UpdateFields(pVpsStdPictureParametersSet, h265SessionParametersAddInfo);
            currentSpsId = PopulateH265UpdateFields(pSpsStdPictureParametersSet, h265SessionParametersAddInfo);
            currentPpsId = PopulateH265UpdateFields(pPpsStdPictureParametersSet, h265SessionParametersAddInfo);

        }
        break;
        default:
            assert(!"Invalid Parser format");
            return nullptr;
    }

    createInfo.videoSessionParametersTemplate = pTemplatePictureParameters ? VkVideoSessionParametersKHR(*pTemplatePictureParameters) : VkVideoSessionParametersKHR();
    createInfo.videoSession = videoSession->GetVideoSession();
    VkResult result = vkDevCtx->CreateVideoSessionParametersKHR(*vkDevCtx,
                                                                &createInfo,
                                                                nullptr,
                                                                &pPictureParameters->m_sessionParameters);

    if (result != VK_SUCCESS) {

        assert(!"Could not create Session Parameters Object");
        delete pPictureParameters;
        pPictureParameters = nullptr;

    } else {

        pPictureParameters->m_videoSession = videoSession;

        if (pTemplatePictureParameters) {
            pPictureParameters->m_vpsIdsUsed = pTemplatePictureParameters->m_vpsIdsUsed;
            pPictureParameters->m_spsIdsUsed = pTemplatePictureParameters->m_spsIdsUsed;
            pPictureParameters->m_ppsIdsUsed = pTemplatePictureParameters->m_ppsIdsUsed;
        }

        assert ((currentVpsId >= 0) || (currentSpsId >= 0) || (currentPpsId >= 0));
        if (currentVpsId >= 0) {
            pPictureParameters->m_vpsIdsUsed.set(currentVpsId, true);
        }

        if (currentSpsId >= 0) {
            pPictureParameters->m_spsIdsUsed.set(currentSpsId, true);
        }

        if (currentPpsId >= 0) {
            pPictureParameters->m_ppsIdsUsed.set(currentPpsId, true);
        }

        pPictureParameters->m_Id = ++m_currentId;
    }

    return pPictureParameters;
}

VkResult VkParserVideoPictureParameters::Update(const StdVideoPictureParametersSet* pVpsStdPictureParametersSet,
                                                const StdVideoPictureParametersSet* pSpsStdPictureParametersSet,
                                                const StdVideoPictureParametersSet* pPpsStdPictureParametersSet)
{
    int32_t currentVpsId = -1;
    int32_t currentSpsId = -1;
    int32_t currentPpsId = -1;

    VkVideoSessionParametersUpdateInfoKHR updateInfo = { VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR };
    VkVideoDecodeH264SessionParametersAddInfoKHR h264SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR };
    VkVideoDecodeH265SessionParametersAddInfoKHR h265SessionParametersAddInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};

    VkParserPictureParametersUpdateType updateType = pPpsStdPictureParametersSet ? pPpsStdPictureParametersSet->m_updateType :
            (pSpsStdPictureParametersSet ? pSpsStdPictureParametersSet->m_updateType : pVpsStdPictureParametersSet->m_updateType);
    switch (updateType)
    {
        case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
        {

            updateInfo.pNext = &h264SessionParametersAddInfo;

            currentSpsId = PopulateH264UpdateFields(pSpsStdPictureParametersSet, h264SessionParametersAddInfo);
            currentPpsId = PopulateH264UpdateFields(pPpsStdPictureParametersSet, h264SessionParametersAddInfo);

        }
        break;
        case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
        case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
        {

            updateInfo.pNext = &h265SessionParametersAddInfo;

            currentVpsId = PopulateH265UpdateFields(pVpsStdPictureParametersSet, h265SessionParametersAddInfo);
            currentSpsId = PopulateH265UpdateFields(pSpsStdPictureParametersSet, h265SessionParametersAddInfo);
            currentPpsId = PopulateH265UpdateFields(pPpsStdPictureParametersSet, h265SessionParametersAddInfo);

        }
        break;
        default:
            assert(!"Invalid Parser format");
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pVpsStdPictureParametersSet) {
        updateInfo.updateSequenceCount = std::max(pVpsStdPictureParametersSet->m_updateSequenceCount, updateInfo.updateSequenceCount);
    }

    if (pSpsStdPictureParametersSet) {
        updateInfo.updateSequenceCount = std::max(pSpsStdPictureParametersSet->m_updateSequenceCount, updateInfo.updateSequenceCount);
    }

    if (pPpsStdPictureParametersSet) {
        updateInfo.updateSequenceCount = std::max(pPpsStdPictureParametersSet->m_updateSequenceCount, updateInfo.updateSequenceCount);
    }

    VkResult result = m_vkDevCtx->UpdateVideoSessionParametersKHR(*m_vkDevCtx,
                                                                  m_sessionParameters,
                                                                  &updateInfo);

    if (result == VK_SUCCESS) {

        assert ((currentSpsId >= 0) || (currentPpsId >= 0) || (currentVpsId >= 0));

        if (currentVpsId >= 0) {
            m_vpsIdsUsed.set(currentVpsId, true);
        }
        if (currentSpsId >= 0) {
            m_spsIdsUsed.set(currentSpsId, true);
        }
        if (currentPpsId >= 0) {
            m_ppsIdsUsed.set(currentPpsId, true);
        }

    } else {
        assert(!"Could not update Session Parameters Object");
    }

    return result;
}

VkParserVideoPictureParameters::~VkParserVideoPictureParameters()
{
    if (m_sessionParameters) {
        m_vkDevCtx->DestroyVideoSessionParametersKHR(*m_vkDevCtx, m_sessionParameters, nullptr);
        m_sessionParameters = VkVideoSessionParametersKHR();
    }
    m_videoSession = nullptr;
}

int32_t VkParserVideoPictureParameters::AddRef()
{
    return ++m_refCount;
}

int32_t VkParserVideoPictureParameters::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        delete this;
    }
    return ret;
}

const char* StdVideoPictureParametersSet::m_refClassId = "StdVideoPictureParametersSet";