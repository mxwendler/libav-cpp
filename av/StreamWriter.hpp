#pragma once

#include <av/Encoder.hpp>
#include <av/Frame.hpp>
#include <av/OptSetter.hpp>
#include <av/OutputFormat.hpp>
#include <av/Resample.hpp>
#include <av/Scale.hpp>
#include <av/common.hpp>

extern "C"
{
#include "libavutil/audio_fifo.h"
}

static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
	if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
		output_codec_context->channels, 1))) {
		fprintf(stderr, "Could not allocate FIFO\n");
		return AVERROR(ENOMEM);
	}
	return 0;
}

static int add_samples_to_fifo(AVAudioFifo *fifo,
	uint8_t **converted_input_samples,
	const int frame_size)
{
	int error;

	if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
		fprintf(stderr, "Could not reallocate FIFO\n");
		return error;
	}

	if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
		frame_size) < frame_size) {
		fprintf(stderr, "Could not write data to FIFO\n");
		return AVERROR_EXIT;
	}
	return 0;
}

static int init_output_frame(AVFrame **frame,
	AVCodecContext *output_codec_context,
	int frame_size)
{
	int error;

	if (!(*frame = av_frame_alloc())) {
		fprintf(stderr, "Could not allocate output frame\n");
		return AVERROR_EXIT;
	}

	/* Set the frame's parameters, especially its size and format.
	 * av_frame_get_buffer needs this to allocate memory for the
	 * audio samples of the frame.
	 * Default channel layouts based on the number of channels
	 * are assumed for simplicity. */
	(*frame)->nb_samples = frame_size;
	(*frame)->channel_layout = output_codec_context->channel_layout;
	(*frame)->format = output_codec_context->sample_fmt;
	(*frame)->sample_rate = output_codec_context->sample_rate;

	/* Allocate the samples of the created frame. This call will make
	 * sure that the audio frame can hold as many samples as specified. */
	if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
		//fprintf(stderr, "Could not allocate output frame samples (error '%s')\n", av_err2str(error));
		av_frame_free(frame);
		return error;
	}

	return 0;
}

namespace av
{
class StreamWriter : NoCopyable
{
	StreamWriter() = default;

	AVAudioFifo *fifo = nullptr;

public:
	[[nodiscard]] static Expected<Ptr<StreamWriter>> create(std::string_view filename) noexcept
	{
		Ptr<StreamWriter> sw{new StreamWriter};
		sw->filename_ = filename;

		auto fcExp = OutputFormat::create(filename);
		if (!fcExp)
			FORWARD_AV_ERROR(fcExp);

		sw->formatContext_ = fcExp.value();

		return sw;
	}

	~StreamWriter()
	{
		flushAllStreams();
	}

	[[nodiscard]] Expected<void> open() noexcept
	{
		return formatContext_->open(filename_);
	}

	[[nodiscard]] Expected<int> addVideoStream(std::variant<AVCodecID, std::string_view> codecName, int inWidth, int inHeight, AVPixelFormat inPixFmt, AVRational frameRate, int outWidth, int outHeight, OptValueMap&& codecParams = {}) noexcept
	{
		auto stream  = makePtr<Stream>();
		stream->type = AVMEDIA_TYPE_VIDEO;

		auto expc = std::visit([](auto&& v) { return Encoder::create(v); }, codecName);

		if (!expc)
			FORWARD_AV_ERROR(expc);

		Ptr<Encoder> c = expc.value();



		c->setVideoParams(outWidth, outHeight, frameRate, std::move(codecParams));


		auto sIndExp = formatContext_->addStream(c);
		if (!sIndExp)
			FORWARD_AV_ERROR(sIndExp);


		auto cOpenEXp = c->open();
		if (!cOpenEXp)
			FORWARD_AV_ERROR(cOpenEXp);

		auto ret = avcodec_parameters_from_context(std::get<0>(formatContext_->streams_.at(0))->codecpar, c->native());

		auto frameExp = c->newWriteableVideoFrame();
		if (!frameExp)
			FORWARD_AV_ERROR(frameExp);

		stream->frame   = frameExp.value();
		stream->encoder = c;

		auto swsExp = Scale::create(inWidth, inHeight, inPixFmt, outWidth, outHeight, c->native()->pix_fmt);
		if (!swsExp)
			FORWARD_AV_ERROR(swsExp);

		stream->sws = swsExp.value();

		// set start time as real time wall clock.
		stream->recording_start = chrono::system_clock::now();

		stream->index = sIndExp.value();
		int index     = stream->index;

		streams_.emplace_back(std::move(stream));

		if ((int)streams_.size() - 1 != index)
			RETURN_AV_ERROR("Stream index {} != streams count - 1 {}", index, streams_.size() - 1);

		const AVCodec* codec = c->native()->codec;
		LOG_AV_INFO("Added video stream #{} codec: {} {}x{} {} fps", index, codec->long_name, c->native()->width, c->native()->height, av_q2d(av_inv_q(c->native()->time_base)));



		return index;
	}

	[[nodiscard]] Expected<int> addVideoStream(std::variant<AVCodecID, std::string_view> codecName, int inWidth, int inHeight, AVPixelFormat inPixFmt, AVRational frameRate, OptValueMap&& codecParams = {})
	{
		return addVideoStream(codecName, inWidth, inHeight, inPixFmt, frameRate, inWidth, inHeight, std::move(codecParams));
	}

	[[nodiscard]] Expected<int> addAudioStream(std::variant<AVCodecID, std::string_view> codecName, int inChannels, AVSampleFormat inSampleFmt, int inSampleRate,
	                                           int outChannels, int outSampleRate, int outBitRate, OptValueMap&& codecParams = {}) noexcept
	{
		auto stream  = makePtr<Stream>();
		stream->type = AVMEDIA_TYPE_AUDIO;

		auto expc = std::visit([](auto&& v) { return Encoder::create(v); }, codecName);

		if (!expc)
			FORWARD_AV_ERROR(expc);

		Ptr<Encoder> c = expc.value();

#if 0
		Ptr<Encoder> c;
		if(std::holds_alternative<AVCodecID>(codecName))
			c = makePtr<Encoder>(std::get<AVCodecID>(codecName));
		else
			c = makePtr<Encoder>(std::get<std::string_view>(codecName));
#endif
		c->setAudioParams(outChannels, outSampleRate, outBitRate, std::move(codecParams));
		auto cOpenExp = c->open();
		if (!cOpenExp)
			FORWARD_AV_ERROR(cOpenExp);

		auto frameExp = c->newWriteableAudioFrame();
		if (!frameExp)
			FORWARD_AV_ERROR(frameExp);

		stream->frame   = frameExp.value();
		stream->encoder = c;

		auto swrExp = Resample::create(inChannels, inSampleFmt, inSampleRate, outChannels, c->native()->sample_fmt, outSampleRate);
		if (!swrExp)
			FORWARD_AV_ERROR(swrExp);

		stream->swr = swrExp.value();

		auto sIndExp = formatContext_->addStream(c);
		if (!sIndExp)
			FORWARD_AV_ERROR(sIndExp);

		stream->index = sIndExp.value();
		int index     = stream->index;

		streams_.emplace_back(std::move(stream));

		if ((int)streams_.size() - 1 != index)
			RETURN_AV_ERROR("Stream index {} != streams count - 1 {}", index, streams_.size() - 1);

		const AVCodec* codec = c->native()->codec;
		LOG_AV_INFO("Added audio stream #{} codec: {} {} Hz {} channels", index, codec->long_name, c->native()->sample_rate, c->native()->channels);

		return index;
	}

	[[nodiscard]] Expected<void> write(Frame& frame, int streamIndex) noexcept
	{
		auto& stream = streams_[streamIndex];

		if (stream->type == AVMEDIA_TYPE_VIDEO)
		{
			stream->sws->scale(frame, *stream->frame);

			// set video frame pts. cannot simply increment, since live recordings
			// may have skipped frames. so use real time wall clock difference, calculate
			// frame and store as pts.
			// some info here:
			// https://blog.mi.hdm-stuttgart.de/index.php/2018/03/21/livestreaming-with-libav-tutorial-part-2/
			#if 0
				stream->frame->native()->pts = stream->nextPts++;
			#else
				auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now()-stream->recording_start);
				auto frames_happen_until_now = (ms.count() / 1000.0) / av_q2d(stream->encoder->native()->time_base);

				// increment up to calculated 'frames_happen_until_now' but at least 1
				auto incremented = max(stream->nextPts + 1.0, frames_happen_until_now);

				stream->nextPts = incremented;
				stream->frame->native()->pts = stream->nextPts;
				printf("%i\n", stream->nextPts);
			#endif
		}
		else if (stream->type == AVMEDIA_TYPE_AUDIO)
		{
			frame.native()->ch_layout = stream->frame->native()->ch_layout;
			frame.native()->channel_layout = stream->frame->native()->channel_layout;
			stream->swr->convert(frame, *stream->frame);

			// send the frame to the encoder
			if (!fifo)
			{
				init_fifo(&fifo, stream->encoder->native());
			}

			// send all samples to fifo
			add_samples_to_fifo(fifo, &stream->frame->native()->extended_data[0], stream->frame->native()->nb_samples);
		}
		else
			RETURN_AV_ERROR("Unsupported/unknown stream type: {}", av_get_media_type_string(stream->type));

		if (stream->type == AVMEDIA_TYPE_VIDEO)
		{
			auto[res, sz] = stream->encoder->encodeFrame(*stream->frame, stream->packets);

			if (res == Result::kFail)
				RETURN_AV_ERROR("Encoder returned failure");

			for (int i = 0; i < sz; ++i)
			{
				auto expected = formatContext_->writePacket(stream->packets[i], stream->index);
				if (!expected)
					LOG_AV_ERROR(expected.errorString());
			}
		}
		else if (stream->type == AVMEDIA_TYPE_AUDIO)
		{
			/* Use the maximum number of possible samples per frame.
			 * If there is less than the maximum possible frame size in the FIFO
			 * buffer use this number. Otherwise, use the maximum possible frame size. */
			auto c = stream->encoder->native();
			const int frame_size = FFMIN(av_audio_fifo_size(fifo), c->frame_size);

			/* Read as many samples from the FIFO buffer as required to fill the frame.
			 * The samples are stored in the frame temporarily. */
			while(av_audio_fifo_size(fifo) > frame_size)
			{
				AVFrame *output_frame;
				if (init_output_frame(&output_frame, c, frame_size))
				{
					// return AVERROR_EXIT;
					RETURN_AV_ERROR("Encoder returned failure");
				}

				if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
					fprintf(stderr, "Could not read data from FIFO\n");
					av_frame_free(&output_frame);
					// return AVERROR_EXIT;
					RETURN_AV_ERROR("Encoder returned failure");
				}

				av::Frame __frame(output_frame);

				output_frame->pts = stream->nextPts;
				stream->nextPts += output_frame->nb_samples;

				auto[res, sz] = stream->encoder->encodeFrame(__frame, stream->packets);

				if (res == Result::kFail)
					RETURN_AV_ERROR("Encoder returned failure");

				for (int i = 0; i < sz; ++i)
				{
					auto expected = formatContext_->writePacket(stream->packets[i], stream->index);
					if (!expected)
						LOG_AV_ERROR(expected.errorString());
				}
			}
		}


		return {};
	}

	void flushStream(int streamIndex) noexcept
	{
		auto& stream = streams_[streamIndex];

		if (stream->flushed)
			return;

		auto [res, sz]  = stream->encoder->flush(stream->packets);
		stream->flushed = true;

		if (res == Result::kFail)
			return;

		for (int i = 0; i < sz; ++i)
		{
			auto expected = formatContext_->writePacket(stream->packets[i], stream->index);
			if (!expected)
				LOG_AV_ERROR(expected.errorString());
		}
	}

	void flushAllStreams() noexcept
	{
		for (auto& stream : streams_)
		{
			flushStream(stream->index);
		}
	}

private:
	struct Stream
	{
		AVMediaType type{AVMEDIA_TYPE_UNKNOWN};
		int index{-1};
		Ptr<Encoder> encoder;
		Ptr<Scale> sws;
		Ptr<Resample> swr;
		Ptr<Frame> frame;
		std::vector<Packet> packets;
		int nextPts{0};
		int sampleCount{0};
		bool flushed{false};

		chrono::time_point<std::chrono::system_clock> recording_start;

	};

public:
	std::string filename_;
	std::vector<Ptr<Stream>> streams_;
	Ptr<OutputFormat> formatContext_;
};

}// namespace av
