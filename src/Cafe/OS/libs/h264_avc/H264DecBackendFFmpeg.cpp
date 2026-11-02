#include "H264DecInternal.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace H264
{
	bool H264_IsBotW();

	class H264FFmpegDecoder final : public H264DecoderBackend
	{
	  public:
		static H264FFmpegDecoder* Create()
		{
			auto* decoder = new (std::nothrow) H264FFmpegDecoder();
			if (!decoder || !decoder->Open())
			{
				delete decoder;
				return nullptr;
			}
			decoder->m_decoderThread = std::thread(&H264FFmpegDecoder::DecoderThread, decoder);
			return decoder;
		}

		~H264FFmpegDecoder() override
		{
			Destroy();
		}

		bool SupportsOutputPerFrame() const override { return false; }

		void Init(bool isBufferedMode) override
		{
			(void)isBufferedMode;
			if (m_codecContext)
				avcodec_flush_buffers(m_codecContext);
			ResetOutputState();
		}

		void Destroy()
		{
			if (!m_threadShouldExit.exchange(true, std::memory_order_acq_rel))
				m_decodeSem.increment();
			if (m_decoderThread.joinable())
			{
				m_decoderThread.join();
			}
			av_packet_free(&m_packet);
			av_frame_free(&m_decodedFrame);
			av_frame_free(&m_transferFrame);
			avcodec_free_context(&m_codecContext);
			av_buffer_unref(&m_deviceContext);
		}

	  private:
		struct OutputBufferState
		{
			void* address{};
			int width{};
			int height{};
			int stride{};
		};

		void ResetOutputState()
		{
			m_parserState = {};
			m_outputWidth = 0;
			m_outputHeight = 0;
			std::scoped_lock lock(m_outputBufferMutex);
			m_initializedOutputBuffers.clear();
		}

		bool InitializeOutputBuffer(void* imageOutput, int width, int height)
		{
			if (!imageOutput || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
				(width & 1) || (height & 1))
				return false;

			const int stride = (width + 0xFF) & ~0xFF;
			std::scoped_lock lock(m_outputBufferMutex);
			auto entry = std::ranges::find_if(m_initializedOutputBuffers,
				[imageOutput](const OutputBufferState& state) { return state.address == imageOutput; });
			if (entry != m_initializedOutputBuffers.end() && entry->width == width &&
				entry->height == height && entry->stride == stride)
				return true;

			const size_t lumaSize = static_cast<size_t>(stride) * height;
			auto* output = static_cast<uint8*>(imageOutput);
			std::memset(output, 0x10, lumaSize);
			std::memset(output + lumaSize, 0x80, lumaSize / 2);
			const OutputBufferState state{imageOutput, width, height, stride};
			if (entry == m_initializedOutputBuffers.end())
				m_initializedOutputBuffers.push_back(state);
			else
				*entry = state;
			return true;
		}

		void PrepareOutputBuffer(uint8* data, uint32 length, void* imageOutput) override
		{
			if (!data || !length || !imageOutput)
				return;

			h264ParserOutput_t parserOutput{};
			h264Parse(&m_parserState, &parserOutput, data, length, false);
			if (parserOutput.hasSPS)
			{
				m_outputWidth = static_cast<int>((m_parserState.sps.pic_width_in_mbs_minus1 + 1) * 16);
				m_outputHeight = static_cast<int>((m_parserState.sps.pic_height_in_map_units_minus1 + 1) * 16);
				if (!m_parserState.sps.frame_mbs_only_flag)
					m_outputHeight *= 2;
				if (H264_IsBotW() && m_outputWidth == 1920 && m_outputHeight == 1088)
					m_outputHeight = 1080;
			}

			InitializeOutputBuffer(imageOutput, m_outputWidth, m_outputHeight);
		}

		static AVPixelFormat SelectFormat(AVCodecContext*, const AVPixelFormat* formats)
		{
			for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
				if (*format == AV_PIX_FMT_NVTEGRA)
					return *format;
			return AV_PIX_FMT_NONE;
		}

		bool Open()
		{
			av_log_set_level(AV_LOG_ERROR);
			if (av_hwdevice_ctx_create(&m_deviceContext, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0) < 0)
				return false;

			const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
			if (!codec)
				return false;
			m_codecContext = avcodec_alloc_context3(codec);
			if (!m_codecContext)
				return false;

			m_codecContext->hw_device_ctx = av_buffer_ref(m_deviceContext);
			if (!m_codecContext->hw_device_ctx)
				return false;
			m_codecContext->get_format = SelectFormat;
			m_codecContext->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
			m_codecContext->thread_count = 1;
			m_codecContext->extra_hw_frames = 2;
			if (avcodec_open2(m_codecContext, codec, nullptr) < 0)
				return false;
			m_packet = av_packet_alloc();
			m_decodedFrame = av_frame_alloc();
			m_transferFrame = av_frame_alloc();
			return m_packet && m_decodedFrame && m_transferFrame;
		}

		void QueueResult(uint32 index, bool hasFrame, const AVFrame* frame = nullptr)
		{
			std::unique_lock lock(m_decodeQueueMtx);
			if (index >= m_decodedSliceArray.size())
				return;
			auto& slice = m_decodedSliceArray[index];
			if (!slice.isUsed || slice.result.isDecoded)
				return;

			slice.result.isDecoded = true;
			slice.result.hasFrame = hasFrame;
			if (frame)
			{
				slice.result.frameWidth = frame->width;
				slice.result.frameHeight = H264_IsBotW() && frame->width == 1920 && frame->height == 1088 ? 1080 : frame->height;
				slice.result.bytesPerRow = (frame->width + 0xFF) & ~0xFF;
				slice.result.cropTop = static_cast<sint32>(frame->crop_top);
				slice.result.cropBottom = static_cast<sint32>(frame->crop_bottom);
				slice.result.cropLeft = static_cast<sint32>(frame->crop_left);
				slice.result.cropRight = static_cast<sint32>(frame->crop_right);
				slice.result.cropEnable = frame->crop_top || frame->crop_bottom || frame->crop_left || frame->crop_right;
			}
			m_displayQueue.push_back(index);
			lock.unlock();
			coreinit::OSSignalEvent(m_displayQueueEvt);
		}

		uint32 FrameIndex(const AVFrame* frame, uint32 fallback) const
		{
			const uintptr_t opaque = reinterpret_cast<uintptr_t>(frame->opaque);
			if (opaque > 0 && opaque <= m_decodedSliceArray.size())
				return static_cast<uint32>(opaque - 1);
			if (frame->pts >= 0 && frame->pts < static_cast<int64_t>(m_decodedSliceArray.size()))
				return static_cast<uint32>(frame->pts);
			return fallback;
		}

		static void FreeTransferBuffer(void* allocation, uint8_t*)
		{
			av_free(allocation);
		}

		bool EnsureTransferFrame(int width, int height)
		{
			if (m_transferFrame->data[0] && m_transferFrame->width == width &&
				m_transferFrame->height == height)
				return true;

			av_frame_unref(m_transferFrame);
			if (width <= 0 || height <= 0)
				return false;

			constexpr size_t kVicAlignment = 256;
			const int pitch = static_cast<int>((static_cast<size_t>(width) + kVicAlignment - 1) & ~(kVicAlignment - 1));
			const size_t lumaSize = static_cast<size_t>(pitch) * height;
			const size_t total = lumaSize + lumaSize / 2;
			auto* allocation = static_cast<uint8_t*>(av_malloc(total + kVicAlignment));
			if (!allocation)
				return false;
			auto* aligned = reinterpret_cast<uint8_t*>(
				(reinterpret_cast<uintptr_t>(allocation) + kVicAlignment - 1) & ~(uintptr_t)(kVicAlignment - 1));

			m_transferFrame->buf[0] = av_buffer_create(aligned, total, FreeTransferBuffer, allocation, 0);
			if (!m_transferFrame->buf[0])
			{
				av_free(allocation);
				return false;
			}
			m_transferFrame->format = AV_PIX_FMT_NV12;
			m_transferFrame->width = width;
			m_transferFrame->height = height;
			m_transferFrame->data[0] = aligned;
			m_transferFrame->data[1] = aligned + lumaSize;
			m_transferFrame->linesize[0] = pitch;
			m_transferFrame->linesize[1] = pitch;
			return true;
		}

		bool CopyFrame(AVFrame* frame, uint32 index)
		{
			if (!EnsureTransferFrame(frame->width, frame->height))
				return false;
			if (av_hwframe_transfer_data(m_transferFrame, frame, 0) < 0)
				return false;

			if (index >= m_decodedSliceArray.size() || !m_decodedSliceArray[index].result.imageOutput)
				return false;

			const int width = frame->width;
			const int height = H264_IsBotW() && width == 1920 && frame->height == 1088 ? 1080 : frame->height;
			const int stride = (width + 0xFF) & ~0xFF;
			if (m_transferFrame->format != AV_PIX_FMT_NV12 || width <= 0 || height <= 0 ||
				(width & 1) || (height & 1) || m_transferFrame->width < width || m_transferFrame->height < height ||
				!m_transferFrame->data[0] || !m_transferFrame->data[1] ||
				m_transferFrame->linesize[0] < width || m_transferFrame->linesize[1] < width)
				return false;

			auto* output = static_cast<uint8*>(m_decodedSliceArray[index].result.imageOutput);
			if (!InitializeOutputBuffer(output, width, height))
				return false;
			const size_t lumaSize = static_cast<size_t>(stride) * height;
			auto* luma = output;
			auto* chroma = output + lumaSize;
			for (int row = 0; row < height; ++row)
				std::memcpy(luma + static_cast<size_t>(row) * stride,
					m_transferFrame->data[0] + static_cast<size_t>(row) * m_transferFrame->linesize[0], width);
			for (int row = 0; row < height / 2; ++row)
				std::memcpy(chroma + static_cast<size_t>(row) * stride,
					m_transferFrame->data[1] + static_cast<size_t>(row) * m_transferFrame->linesize[1], width);
			return true;
		}

		bool DrainFrames(uint32 fallbackIndex)
		{
			bool success = true;
			for (;;)
			{
				av_frame_unref(m_decodedFrame);
				const int result = avcodec_receive_frame(m_codecContext, m_decodedFrame);
				if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
					break;
				if (result < 0)
				{
					success = false;
					break;
				}
				const uint32 index = FrameIndex(m_decodedFrame, fallbackIndex);
				if (CopyFrame(m_decodedFrame, index))
					QueueResult(index, true, m_decodedFrame);
				else
					QueueResult(index, false);
			}
			return success;
		}

		void Decode(uint32 index)
		{
			if (!m_codecContext || !m_packet || index >= m_decodedSliceArray.size())
			{
				QueueResult(index, false);
				return;
			}

			auto& input = m_decodedSliceArray[index].dataToDecode;
			av_packet_unref(m_packet);
			if (input.m_length > static_cast<uint32>(std::numeric_limits<int>::max()) ||
				av_new_packet(m_packet, static_cast<int>(input.m_length)) < 0)
			{
				QueueResult(index, false);
				return;
			}
			std::memcpy(m_packet->data, input.m_data, input.m_length);
			m_packet->pts = index;
			m_packet->dts = index;
			m_packet->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1));

			int result = avcodec_send_packet(m_codecContext, m_packet);
			if (result == AVERROR(EAGAIN))
			{
				DrainFrames(index);
				result = avcodec_send_packet(m_codecContext, m_packet);
			}
			av_packet_unref(m_packet);
			if (result < 0 || !DrainFrames(index))
				QueueResult(index, false);
		}

		void Flush()
		{
			if (m_codecContext)
			{
				avcodec_send_packet(m_codecContext, nullptr);
				DrainFrames(0);
				avcodec_flush_buffers(m_codecContext);
			}
			for (uint32 index = 0; index < m_decodedSliceArray.size(); ++index)
				if (m_decodedSliceArray[index].isUsed && !m_decodedSliceArray[index].result.isDecoded)
					QueueResult(index, false);
			ResetOutputState();
		}

		void DecoderThread()
		{
			while (!m_threadShouldExit.load(std::memory_order_acquire))
			{
				m_decodeSem.decrementWithWait();
				if (m_threadShouldExit.load(std::memory_order_acquire))
					break;
				std::unique_lock lock(m_decodeQueueMtx);
				if (m_decodeQueue.empty())
					continue;
				const uint32 index = m_decodeQueue.front();
				m_decodeQueue.erase(m_decodeQueue.begin());
				lock.unlock();
				if (index == CMD_FLUSH)
				{
					Flush();
					coreinit::OSSignalEvent(m_flushEvt);
				}
				else
				{
					Decode(index);
				}
			}
		}

		AVBufferRef* m_deviceContext{};
		AVCodecContext* m_codecContext{};
		AVPacket* m_packet{};
		AVFrame* m_decodedFrame{};
		AVFrame* m_transferFrame{};
		h264ParserState_t m_parserState{};
		std::mutex m_outputBufferMutex;
		std::vector<OutputBufferState> m_initializedOutputBuffers;
		int m_outputWidth{};
		int m_outputHeight{};
		std::thread m_decoderThread;
		std::atomic_bool m_threadShouldExit{false};
	};

	H264DecoderBackend* CreateFFmpegDecoder()
	{
		return H264FFmpegDecoder::Create();
	}
}
