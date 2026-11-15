/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __FFMPEGDECODER_H__
#define __FFMPEGDECODER_H__

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVIOContext;
struct AVPacket;

/*
================================================
Decodes the Bink videos into planar YUV 4:2:0 frames and interleaved float audio. No engine
dependency: the bytes come through a Source (idCinematic wraps an idFile, so .resources work).
Not thread safe, the owner serializes the calls.
================================================
*/
class idFFmpegDecoder {
public:
	class Source {
	public:
		virtual				~Source() = default;
		virtual int			Read( uint8_t * buffer, int size ) = 0;	// bytes read, 0 at the end
		virtual bool		Seek( int64_t offset ) = 0;				// from the start of the file
		virtual int64_t		Tell() const = 0;
		virtual int64_t		Size() const = 0;
	};

	// planes[ 0 ] is Y, planes[ 1 ] Cb (U), planes[ 2 ] Cr (V); the chroma planes are half the
	// size, rounded up. Valid until the next DecodeNextFrame, Rewind or Close.
	struct Frame {
		const uint8_t *		planes[ 3 ] = {};
		int					pitches[ 3 ] = {};
		int					width = 0;
		int					height = 0;
	};

	// samples holds numFrames * numChannels interleaved floats
	using AudioSink = std::function< void( const float * samples, int numFrames, int numChannels, int sampleRate ) >;

						idFFmpegDecoder() = default;
						~idFFmpegDecoder();
						idFFmpegDecoder( const idFFmpegDecoder & ) = delete;
	idFFmpegDecoder &	operator=( const idFFmpegDecoder & ) = delete;

	// true when the FFmpeg DLLs could be loaded; the engine delay-loads them, so this must be
	// checked before any other call
	[[nodiscard]] static bool			LoadLibraries();
	[[nodiscard]] static const char *	VersionString();

	// takes ownership of the source and reads the header; the name is only used for probing
	// and in messages. The decoders are opened by the first DecodeNextFrame.
	bool				Open( std::unique_ptr<Source> source, const char * name );
	void				Close();
	[[nodiscard]] bool	IsOpen() const { return format != nullptr; }
	[[nodiscard]] const std::string & GetError() const { return error; }

	[[nodiscard]] int	Width() const { return width; }
	[[nodiscard]] int	Height() const { return height; }
	[[nodiscard]] int	FrameRateNum() const { return frameRateNum; }
	[[nodiscard]] int	FrameRateDen() const { return frameRateDen; }
	// from the file header, 0 if unknown
	[[nodiscard]] int	NumFrames() const { return numFrames; }
	[[nodiscard]] bool	HasAudio() const { return audioStream >= 0; }

	// audio decoded on the way to the next video frame goes to the sink; without a sink, or
	// while disabled, the audio packets are skipped
	void				SetAudioSink( AudioSink sink ) { audioSink = std::move( sink ); }
	void				EnableAudio( bool enable ) { audioEnabled = enable; }

	// decodes the next video frame, false at the end of the stream or on an error (GetError)
	bool				DecodeNextFrame();
	// index of the current frame, 0 for the first one, -1 before the first DecodeNextFrame
	[[nodiscard]] int64_t		FrameIndex() const { return frameIndex; }
	[[nodiscard]] const Frame &	CurrentFrame() const { return current; }

	// back to the start, the next DecodeNextFrame returns frame 0
	bool				Rewind();

private:
	bool				OpenCodecs();
	void				DecodeAudio( const AVPacket * packet );
	bool				Fail( const std::string & message );

	std::unique_ptr<Source>	source;
	AVIOContext *		io = nullptr;
	AVFormatContext *	format = nullptr;
	AVCodecContext *	videoCodec = nullptr;
	AVCodecContext *	audioCodec = nullptr;
	AVFrame *			videoFrame = nullptr;
	AVFrame *			audioFrame = nullptr;
	AVPacket *			packet = nullptr;
	int					videoStream = -1;
	int					audioStream = -1;

	int					width = 0;
	int					height = 0;
	int					frameRateNum = 0;
	int					frameRateDen = 1;
	int					numFrames = 0;

	bool				demuxEnded = false;		// no more packets, the decoder is being drained
	bool				drained = false;		// the flush packet has been sent
	int64_t				frameIndex = -1;
	Frame				current;

	AudioSink			audioSink;
	bool				audioEnabled = true;
	std::vector<float>	audioSamples;

	std::string			error;
};

#endif /* !__FFMPEGDECODER_H__ */
