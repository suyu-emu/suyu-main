// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "common/assert.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/memory.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/memory_manager.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#endif

extern "C" {
#ifdef LIBVA_FOUND
// for querying VAAPI driver information
#include <libavutil/hwcontext_vaapi.h>
#endif

#include <libavutil/hwcontext.h>
}

namespace FFmpeg {

namespace {

constexpr AVPixelFormat PreferredGpuFormat = AV_PIX_FMT_NV12;
constexpr AVPixelFormat PreferredCpuFormat = AV_PIX_FMT_YUV420P;
constexpr std::array PreferredGpuDecoders = {
#if defined(_WIN32)
    AV_HWDEVICE_TYPE_CUDA,
    AV_HWDEVICE_TYPE_D3D11VA,
    AV_HWDEVICE_TYPE_DXVA2,
    AV_HWDEVICE_TYPE_D3D12VA,
#elif defined(__FreeBSD__)
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_VDPAU,
    AV_HWDEVICE_TYPE_DRM,
#elif defined(__APPLE__)
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(__ANDROID__)
    AV_HWDEVICE_TYPE_MEDIACODEC,
#elif defined(__unix__)
    AV_HWDEVICE_TYPE_CUDA,
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_VDPAU,
#endif
    AV_HWDEVICE_TYPE_VULKAN,
};

AVPixelFormat GetGpuFormat(AVCodecContext* codec_context, const AVPixelFormat* pix_fmts) {
    const auto desc = av_pix_fmt_desc_get(codec_context->pix_fmt);
    if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec_context->codec, i);
            if (!config) {
                break;
            }

            for (const auto type : PreferredGpuDecoders) {
                if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
                    codec_context->pix_fmt = config->pix_fmt;
                }
            }
        }
    }

    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == codec_context->pix_fmt) {
            return codec_context->pix_fmt;
        }
    }

    LOG_INFO(HW_GPU, "Could not find supported GPU pixel format, falling back to CPU decoder");
    av_buffer_unref(&codec_context->hw_device_ctx);
    codec_context->pix_fmt = PreferredCpuFormat;
    return codec_context->pix_fmt;
}

#if defined(__linux__) && !defined(__ANDROID__)
// Whether a V4L2 mem2mem device takes this codec's bitstream, as FFmpeg's v4l2m2m wrapper
// looks for one: a /dev/video node with the M2M capability and the codec among the formats
// of its output (bitstream) queue
bool HasV4L2M2MDecoder(AVCodecID codec) {
    const u32 fourcc = [codec]() -> u32 {
        switch (codec) {
        case AV_CODEC_ID_H264:
            return V4L2_PIX_FMT_H264;
        case AV_CODEC_ID_VP8:
            return V4L2_PIX_FMT_VP8;
        case AV_CODEC_ID_VP9:
            return V4L2_PIX_FMT_VP9;
        default:
            return 0;
        }
    }();
    if (fourcc == 0) {
        return false;
    }
    for (int node = 0; node < 64; ++node) {
        const std::string path = fmt::format("/dev/video{}", node);
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        SCOPE_EXIT {
            close(fd);
        };
        v4l2_capability cap{};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            continue;
        }
        const u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        const bool mplane = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0;
        if (!mplane && !(caps & V4L2_CAP_VIDEO_M2M)) {
            continue;
        }
        v4l2_fmtdesc desc{};
        desc.type = mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
        for (desc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
            if (desc.pixelformat == fourcc) {
                LOG_INFO(HW_GPU, "V4L2 mem2mem decoder for {}: {} ({})", avcodec_get_name(codec),
                         path, reinterpret_cast<const char*>(cap.card));
                return true;
            }
        }
    }
    return false;
}
#endif

std::string AVError(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_make_error_string(errbuf, sizeof(errbuf) - 1, errnum);
    return errbuf;
}

#if defined(__linux__)
size_t FindNalStartCode(std::span<const u8> data, size_t i) {
    const size_t n = data.size();
    if (i + 3 < n && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
        return 4;
    }
    if (i + 2 < n && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        return 3;
    }
    return 0;
}

std::vector<u8> ExtractH264ParameterSetExtradata(std::span<const u8> packet) {
    std::vector<u8> extradata;
    const size_t size = packet.size();
    size_t i = 0;
    while (i < size) {
        const size_t sc = FindNalStartCode(packet, i);
        if (sc == 0) {
            ++i;
            continue;
        }
        const size_t nal_start = i + sc;
        if (nal_start >= size) {
            break;
        }
        const u8 nal_type = packet[nal_start] & 0x1F;

        size_t j = nal_start + 1;
        while (j < size && FindNalStartCode(packet, j) == 0) {
            ++j;
        }

        if (nal_type == 7 || nal_type == 8) {
            constexpr u8 start[4] = {0, 0, 0, 1};
            extradata.insert(extradata.end(), start, start + sizeof(start));
            extradata.insert(extradata.end(), packet.begin() + nal_start, packet.begin() + j);
        } else if (nal_type == 1 || nal_type == 5) {
            break;
        }
        i = j;
    }
    return extradata;
}
#endif

}

Packet::Packet(std::span<const u8> data) {
    m_packet = av_packet_alloc();
    m_packet->data = const_cast<u8*>(data.data());
    m_packet->size = static_cast<s32>(data.size());
}

Packet::~Packet() {
    av_packet_free(&m_packet);
}

Frame::Frame() {
    m_frame = av_frame_alloc();
}

Frame::~Frame() {
    av_frame_free(&m_frame);
}

Decoder::Decoder(Tegra::Host1x::NvdecCommon::VideoCodec codec, std::string_view wrapper) {
    const AVCodecID av_codec = [&] {
        switch (codec) {
        case Tegra::Host1x::NvdecCommon::VideoCodec::H264:
            return AV_CODEC_ID_H264;
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP8:
            return AV_CODEC_ID_VP8;
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP9:
            return AV_CODEC_ID_VP9;
        default:
            UNIMPLEMENTED_MSG("Unknown codec {}", codec);
            return AV_CODEC_ID_NONE;
        }
    }();

    // A codec wrapper around a platform decoder (h264_v4l2m2m), when asked for
    if (!wrapper.empty() && av_codec != AV_CODEC_ID_NONE) {
        const std::string name = fmt::format("{}_{}", avcodec_get_name(av_codec), wrapper);
        m_codec = avcodec_find_decoder_by_name(name.c_str());
    }
#if defined(__ANDROID__)
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        const char* mc_name = nullptr;
        switch (av_codec) {
        case AV_CODEC_ID_H264: mc_name = "h264_mediacodec"; break;
        case AV_CODEC_ID_VP8:  mc_name = "vp8_mediacodec";  break;
        case AV_CODEC_ID_VP9:  mc_name = "vp9_mediacodec";  break;
        default: break;
        }
        if (mc_name) {
            m_codec = avcodec_find_decoder_by_name(mc_name);
        }
    }
#endif
    if (!m_codec) {
        m_codec = avcodec_find_decoder(av_codec);
    }
}

bool Decoder::SupportsDecodingOnDevice(AVPixelFormat* out_pix_fmt, AVHWDeviceType type) const {
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(m_codec, i);
        if (!config) {
            LOG_DEBUG(HW_GPU, "{} decoder does not support device type {}", m_codec->name, av_hwdevice_get_type_name(type));
            break;
        }

        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
            LOG_INFO(HW_GPU, "Using {} GPU decoder", av_hwdevice_get_type_name(type));
            *out_pix_fmt = config->pix_fmt;
            return true;
        }
    }

    return false;
}

std::vector<AVHWDeviceType> HardwareContext::GetSupportedDeviceTypes() {
    std::vector<AVHWDeviceType> types;
    AVHWDeviceType current_device_type = AV_HWDEVICE_TYPE_NONE;

    while (true) {
        current_device_type = av_hwdevice_iterate_types(current_device_type);
        if (current_device_type == AV_HWDEVICE_TYPE_NONE) {
            return types;
        }

        types.push_back(current_device_type);
    }
}

HardwareContext::~HardwareContext() {
    av_buffer_unref(&m_gpu_decoder);
}

bool HardwareContext::InitializeForDecoder(DecoderContext& decoder_context, const Decoder& decoder) {
    const auto supported_types = GetSupportedDeviceTypes();
    for (const auto type : PreferredGpuDecoders) {
        AVPixelFormat hw_pix_fmt;

        if (std::ranges::find(supported_types, type) == supported_types.end()) {
            LOG_DEBUG(HW_GPU, "{} explicitly unsupported", av_hwdevice_get_type_name(type));
            continue;
        }

        if (!this->InitializeWithType(type)) {
            continue;
        }

        if (decoder.SupportsDecodingOnDevice(&hw_pix_fmt, type)) {
            decoder_context.InitializeHardwareDecoder(*this, hw_pix_fmt);
            return true;
        }
    }

    return false;
}

bool HardwareContext::InitializeWithType(AVHWDeviceType type) {
    av_buffer_unref(&m_gpu_decoder);

    if (const int ret = av_hwdevice_ctx_create(&m_gpu_decoder, type, nullptr, nullptr, 0); ret < 0) {
        LOG_DEBUG(HW_GPU, "av_hwdevice_ctx_create({}) failed: {}", av_hwdevice_get_type_name(type), AVError(ret));
        return false;
    }

#ifdef LIBVA_FOUND
    if (type == AV_HWDEVICE_TYPE_VAAPI) {
        // We need to determine if this is an impersonated VAAPI driver.
        auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(m_gpu_decoder->data);
        auto* vactx = static_cast<AVVAAPIDeviceContext*>(hwctx->hwctx);
        const char* vendor_name = vaQueryVendorString(vactx->display);
        if (strstr(vendor_name, "VDPAU backend")) {
            // VDPAU impersonated VAAPI impls are super buggy, we need to skip them.
            LOG_DEBUG(HW_GPU, "Skipping VDPAU impersonated VAAPI driver");
            return false;
        } else {
            // According to some user testing, certain VAAPI drivers (Intel?) could be buggy.
            // Log the driver name just in case.
            LOG_DEBUG(HW_GPU, "Using VAAPI driver: {}", vendor_name);
        }
    }
#endif

    return true;
}

DecoderContext::DecoderContext(const Decoder& decoder) : m_decoder{decoder} {
    m_codec_context = avcodec_alloc_context3(m_decoder.GetCodec());
    av_opt_set(m_codec_context->priv_data, "tune", "zerolatency", 0);
    m_codec_context->thread_count = 0;
    m_codec_context->thread_type &= ~FF_THREAD_FRAME;
#if defined(__ANDROID__)
    m_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
    // A V4L2 mem2mem decoder hands out frames in its own capture buffers, and the frame
    // queue keeps a few of them for VIC: more buffers than its default, so it never waits
    // for one to come back
    m_v4l2_m2m = std::string_view{m_decoder.GetCodec()->name}.ends_with("_v4l2m2m");
    if (m_v4l2_m2m) {
        av_opt_set_int(m_codec_context->priv_data, "num_capture_buffers", 32, 0);
    }
}

DecoderContext::~DecoderContext() {
    av_buffer_unref(&m_codec_context->hw_device_ctx);
    avcodec_free_context(&m_codec_context);
}

void DecoderContext::InitializeHardwareDecoder(const HardwareContext& context, AVPixelFormat hw_pix_fmt) {
    m_codec_context->hw_device_ctx = av_buffer_ref(context.GetBufferRef());
    m_codec_context->get_format = GetGpuFormat;
    m_codec_context->pix_fmt = hw_pix_fmt;
}

bool DecoderContext::OpenContext(const Decoder& decoder, std::span<const u8> extradata) {
    if (!extradata.empty()) {
        av_freep(&m_codec_context->extradata);
        m_codec_context->extradata = static_cast<u8*>(
            av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_codec_context->extradata) {
            LOG_ERROR(HW_GPU, "Failed to allocate extradata");
            return false;
        }
        std::memcpy(m_codec_context->extradata, extradata.data(), extradata.size());
        m_codec_context->extradata_size = static_cast<int>(extradata.size());
    }

    if (const int ret = avcodec_open2(m_codec_context, decoder.GetCodec(), nullptr); ret < 0) {
        LOG_ERROR(HW_GPU, "avcodec_open2 error: {}", AVError(ret));
        return false;
    }

    LOG_INFO(HW_GPU, "Using decoder {}", decoder.GetCodec()->name);

    return true;
}

bool DecoderContext::SendPacket(const Packet& packet) {
    if (const int ret = avcodec_send_packet(m_codec_context, packet.GetPacket()); ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
        LOG_ERROR(HW_GPU, "avcodec_send_packet error: {}", AVError(ret));
        return false;
    }

    return true;
}

std::shared_ptr<Frame> DecoderContext::ReceiveFrame() {
    auto ReceiveImpl = [&](AVFrame* frame) -> int {
        const int ret = avcodec_receive_frame(m_codec_context, frame);
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            LOG_ERROR(HW_GPU, "avcodec_receive_frame error: {}", AVError(ret));
        }
        return ret;
    };

    std::shared_ptr<Frame> intermediate_frame = std::make_shared<Frame>();
    if (ReceiveImpl(intermediate_frame->GetFrame()) < 0) {
        return {};
    }

    m_final_frame = std::make_shared<Frame>();
    if (m_codec_context->hw_device_ctx) {
        m_final_frame->SetFormat(PreferredGpuFormat);
        if (const int ret = av_hwframe_transfer_data(m_final_frame->GetFrame(), intermediate_frame->GetFrame(), 0); ret < 0) {
            LOG_ERROR(HW_GPU, "av_hwframe_transfer_data error: {}", AVError(ret));
            return {};
        }
    } else if (m_v4l2_m2m) {
        // A V4L2 mem2mem frame lives in one of the decoder's capture buffers, which it
        // gets back only when the frame is freed; the frame queue keeps frames for VIC
        // for a while, and the decoder stalled waiting for buffers. A copy gives it back
        // at once.
        AVFrame* const source = intermediate_frame->GetFrame();
        AVFrame* const copy = m_final_frame->GetFrame();
        copy->format = source->format;
        copy->width = source->width;
        copy->height = source->height;
        if (const int ret = av_frame_get_buffer(copy, 0); ret < 0) {
            LOG_ERROR(HW_GPU, "av_frame_get_buffer error: {}", AVError(ret));
            return {};
        }
        if (const int ret = av_frame_copy(copy, source); ret < 0) {
            LOG_ERROR(HW_GPU, "av_frame_copy error: {}", AVError(ret));
            return {};
        }
        av_frame_copy_props(copy, source);
    } else {
        m_final_frame = std::move(intermediate_frame);
    }

    return std::move(m_final_frame);
}

void DecodeApi::Reset() {
    m_hardware_context.reset();
    m_decoder_context.reset();
    m_decoder.reset();
    m_opened = false;
    m_v4l2_m2m = false;
    m_defer_android_mediacodec_open = false;
    m_needs_h264_extradata = false;
    m_next_pts = 0;
    while (!m_pending_offsets.empty()) {
        m_pending_offsets.pop();
    }
}

bool DecodeApi::Initialize(Tegra::Host1x::NvdecCommon::VideoCodec codec) {
    this->Reset();
    m_codec = codec;
    m_decoder.emplace(codec);
    m_decoder_context.emplace(*m_decoder);

    bool is_mediacodec = false;
#if defined(__ANDROID__)
    const std::string_view decoder_name = m_decoder->GetCodec() ? m_decoder->GetCodec()->name : "";
    is_mediacodec = decoder_name == "h264_mediacodec" ||
                    decoder_name == "vp8_mediacodec" ||
                    decoder_name == "vp9_mediacodec";
#endif

    // Enable GPU decoding if requested.
    if (!is_mediacodec &&
        Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
#if defined(__linux__) && !defined(__ANDROID__)
        // The SoC's own decoder through V4L2 mem2mem where there is one (Qualcomm Venus,
        // Rockchip, Raspberry Pi, MediaTek): a phone-class CPU decoding a 1080p movie in
        // software slows the game itself down, and FFmpeg's Vulkan hwaccel, which such a
        // GPU's driver may offer, can end in software decoding all the same
        if (HasV4L2M2MDecoder(m_decoder->GetCodec()->id)) {
            m_decoder_context.reset();
            m_decoder.emplace(codec, "v4l2m2m");
            m_decoder_context.emplace(*m_decoder);
            m_v4l2_m2m = std::string_view{m_decoder->GetCodec()->name}.ends_with("_v4l2m2m");
        }
#endif
        if (!m_v4l2_m2m) {
            m_hardware_context.emplace();
            m_hardware_context->InitializeForDecoder(*m_decoder_context, *m_decoder);
        }
    }

    // Opened at the first packet, with the frame's size and the stream's parameter sets
    if (m_v4l2_m2m) {
        m_needs_h264_extradata = codec == Tegra::Host1x::NvdecCommon::VideoCodec::H264;
        return true;
    }

#if defined(__ANDROID__)
    m_defer_android_mediacodec_open = is_mediacodec;
    m_needs_h264_extradata = decoder_name == "h264_mediacodec";
    if (m_defer_android_mediacodec_open) {
        return true;
    }
#endif

    // Open the decoder context.
    if (!m_decoder_context->OpenContext(*m_decoder)) {
        this->Reset();
        return false;
    }
    m_opened = true;

    return true;
}

bool DecodeApi::SendPacket(std::span<const u8> packet_data, const FrameOffsets& offsets,
                           std::optional<FrameDimensions> dimensions) {
    if (!m_opened && m_v4l2_m2m) {
        return this->OpenV4L2M2M(packet_data, offsets, dimensions);
    }
    if (!m_opened) {
        std::vector<u8> extradata;
#if defined(__ANDROID__)
        if (m_defer_android_mediacodec_open) {
            if (!dimensions) {
                return true;
            }

            auto* ctx = m_decoder_context->GetCodecContext();
            ctx->width = dimensions->width;
            ctx->height = dimensions->height;
            ctx->coded_width = dimensions->width;
            ctx->coded_height = dimensions->height;
        }

        if (m_needs_h264_extradata) {
            extradata = ExtractH264ParameterSetExtradata(packet_data);
            if (extradata.empty()) {
                return true;
            }
        }
#endif
        if (!m_decoder_context->OpenContext(*m_decoder, extradata)) {
            this->Reset();
            return false;
        }
        m_opened = true;
    }
    if (!offsets.hidden) {
        m_pending_offsets.push(offsets);
    }
    FFmpeg::Packet packet(packet_data);
    packet.GetPacket()->pts = m_next_pts;
    packet.GetPacket()->dts = m_next_pts;
    ++m_next_pts;
    return m_decoder_context->SendPacket(packet);
}

bool DecodeApi::OpenV4L2M2M(std::span<const u8> packet_data, const FrameOffsets& offsets,
                            std::optional<FrameDimensions> dimensions) {
    if (!dimensions) {
        return true;
    }
    std::vector<u8> extradata;
#if defined(__linux__)
    if (m_needs_h264_extradata) {
        extradata = ExtractH264ParameterSetExtradata(packet_data);
        if (extradata.empty()) {
            return true;
        }
    }
#endif
    auto* ctx = m_decoder_context->GetCodecContext();
    ctx->width = dimensions->width;
    ctx->height = dimensions->height;
    ctx->coded_width = dimensions->width;
    ctx->coded_height = dimensions->height;
    if (!m_decoder_context->OpenContext(*m_decoder, extradata)) {
        // No such device, or it cannot take this stream: decode in software instead
        LOG_INFO(HW_GPU, "V4L2 mem2mem decoder unavailable, decoding on the CPU");
        m_v4l2_m2m = false;
        m_decoder_context.reset();
        m_decoder.emplace(m_codec);
        m_decoder_context.emplace(*m_decoder);
        if (!m_decoder_context->OpenContext(*m_decoder)) {
            this->Reset();
            return false;
        }
    }
    m_opened = true;
    return this->SendPacket(packet_data, offsets, dimensions);
}

std::optional<DecodeApi::DecodedFrame> DecodeApi::ReceiveFrame() {
    auto frame = m_decoder_context->ReceiveFrame();
    if (!frame) {
        return std::nullopt;
    }
    FrameOffsets offsets{};
    if (!m_pending_offsets.empty()) {
        offsets = m_pending_offsets.front();
        m_pending_offsets.pop();
    }
    return DecodedFrame{std::move(frame), offsets};
}

}
