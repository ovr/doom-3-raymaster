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

// No engine headers on purpose (the engine build force-includes the precompiled header anyway):
// the decoder also builds on its own, against FFmpeg only.

#ifdef USE_FFMPEG

#include "FFmpegDecoder.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

namespace {

constexpr int IO_BUFFER_SIZE = 64 * 1024;

std::string ErrorString( int err ) {
	char buffer[ AV_ERROR_MAX_STRING_SIZE ] = {};
	av_strerror( err, buffer, sizeof( buffer ) );
	return buffer;
}

int ReadPacket( void * opaque, uint8_t * buffer, int size ) {
	auto * source = static_cast< idFFmpegDecoder::Source * >( opaque );
	const int read = source->Read( buffer, size );
	return read > 0 ? read : AVERROR_EOF;
}

int64_t SeekPacket( void * opaque, int64_t offset, int whence ) {
	auto * source = static_cast< idFFmpegDecoder::Source * >( opaque );
	switch ( whence & ~AVSEEK_FORCE ) {
		case AVSEEK_SIZE:
			return source->Size();
		case SEEK_SET:
			break;
		case SEEK_CUR:
			offset += source->Tell();
			break;
		case SEEK_END:
			offset += source->Size();
			break;
		default:
			return AVERROR( EINVAL );
	}
	if ( offset < 0 || !source->Seek( offset ) ) {
		return AVERROR( EIO );
	}
	return offset;
}

AVCodecContext * OpenCodec( const AVCodecParameters * parms, std::string & error ) {
	const AVCodec * codec = avcodec_find_decoder( parms->codec_id );
	if ( codec == nullptr ) {
		error = std::string( "no decoder for " ) + avcodec_get_name( parms->codec_id );
		return nullptr;
	}
	AVCodecContext * context = avcodec_alloc_context3( codec );
	if ( context == nullptr ) {
		error = "out of memory";
		return nullptr;
	}
	int err = avcodec_parameters_to_context( context, parms );
	if ( err >= 0 ) {
		// the caller decodes on its own thread, don't spawn more
		context->thread_count = 1;
		err = avcodec_open2( context, codec, nullptr );
	}
	if ( err < 0 ) {
		error = std::string( "can't open the " ) + codec->name + " decoder: " + ErrorString( err );
		avcodec_free_context( &context );
		return nullptr;
	}
	return context;
}

#ifdef _WIN32
int DelayLoadFilter( unsigned long code ) {
	if ( code == VcppException( ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND ) ||
		 code == VcppException( ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND ) ) {
		return EXCEPTION_EXECUTE_HANDLER;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}	// namespace

/*
========================
idFFmpegDecoder::LoadLibraries

The first call into a delay-loaded DLL loads it, or raises a structured exception when it is
missing. No C++ objects in here, __try can't unwind them.
========================
*/
bool idFFmpegDecoder::LoadLibraries() {
#ifdef _WIN32
	__try {
		return avutil_version() != 0 && avcodec_version() != 0 && avformat_version() != 0;
	} __except ( DelayLoadFilter( GetExceptionCode() ) ) {
		return false;
	}
#else
	return true;
#endif
}

/*
========================
idFFmpegDecoder::VersionString
========================
*/
const char * idFFmpegDecoder::VersionString() {
	return av_version_info();
}

/*
========================
idFFmpegDecoder::~idFFmpegDecoder
========================
*/
idFFmpegDecoder::~idFFmpegDecoder() {
	Close();
}

/*
========================
idFFmpegDecoder::Fail
========================
*/
bool idFFmpegDecoder::Fail( const std::string & message ) {
	Close();
	error = message;
	return false;
}

/*
========================
idFFmpegDecoder::Open
========================
*/
bool idFFmpegDecoder::Open( std::unique_ptr<Source> newSource, const char * name ) {
	Close();
	error.clear();
	source = std::move( newSource );
	if ( !source ) {
		return Fail( "no file" );
	}

	auto * buffer = static_cast< uint8_t * >( av_malloc( IO_BUFFER_SIZE ) );
	if ( buffer == nullptr ) {
		return Fail( "out of memory" );
	}
	io = avio_alloc_context( buffer, IO_BUFFER_SIZE, 0, source.get(), ReadPacket, nullptr, SeekPacket );
	if ( io == nullptr ) {
		av_free( buffer );
		return Fail( "out of memory" );
	}

	format = avformat_alloc_context();
	if ( format == nullptr ) {
		return Fail( "out of memory" );
	}
	format->pb = io;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	// frees the context and NULLs format on failure, but leaves the custom io alone
	int err = avformat_open_input( &format, name, nullptr, nullptr );
	if ( err < 0 ) {
		return Fail( "unknown format: " + ErrorString( err ) );
	}
	// no avformat_find_stream_info: the Bink header has everything, and probing would decode
	// frames for every videoMap at material load

	videoStream = av_find_best_stream( format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0 );
	if ( videoStream < 0 ) {
		return Fail( "no video stream" );
	}
	const AVStream * video = format->streams[ videoStream ];
	if ( avcodec_find_decoder( video->codecpar->codec_id ) == nullptr ) {
		return Fail( std::string( "no decoder for " ) + avcodec_get_name( video->codecpar->codec_id ) );
	}
	width = video->codecpar->width;
	height = video->codecpar->height;
	if ( width <= 0 || height <= 0 ) {
		return Fail( "bad frame size" );
	}

	AVRational rate = video->avg_frame_rate;
	if ( rate.num <= 0 || rate.den <= 0 ) {
		rate = video->r_frame_rate;
	}
	if ( rate.num <= 0 || rate.den <= 0 ) {
		rate = av_inv_q( video->time_base );
	}
	if ( rate.num <= 0 || rate.den <= 0 ) {
		return Fail( "unknown frame rate" );
	}
	frameRateNum = rate.num;
	frameRateDen = rate.den;

	if ( video->nb_frames > 0 ) {
		numFrames = static_cast< int >( video->nb_frames );
	} else if ( video->duration > 0 ) {
		numFrames = static_cast< int >( av_rescale_q( video->duration, video->time_base, av_inv_q( rate ) ) );
	}

	// the first audio track, if there is one
	audioStream = av_find_best_stream( format, AVMEDIA_TYPE_AUDIO, -1, videoStream, nullptr, 0 );
	if ( audioStream >= 0 && avcodec_find_decoder( format->streams[ audioStream ]->codecpar->codec_id ) == nullptr ) {
		audioStream = -1;
	}
	for ( unsigned int i = 0; i < format->nb_streams; i++ ) {
		if ( static_cast< int >( i ) != videoStream && static_cast< int >( i ) != audioStream ) {
			format->streams[ i ]->discard = AVDISCARD_ALL;
		}
	}

	frameIndex = -1;
	return true;
}

/*
========================
idFFmpegDecoder::OpenCodecs

On the first frame: every videoMap material opens its video at load time, most are never shown,
and the Bink decoder allocates its block buffers (megabytes for the big ones) when it is opened.
========================
*/
bool idFFmpegDecoder::OpenCodecs() {
	if ( videoCodec != nullptr ) {
		return videoFrame != nullptr && audioFrame != nullptr && packet != nullptr;
	}
	videoCodec = OpenCodec( format->streams[ videoStream ]->codecpar, error );
	if ( videoCodec == nullptr ) {
		return false;
	}
	if ( audioStream >= 0 ) {
		// a broken sound track only loses the sound
		std::string audioError;
		audioCodec = OpenCodec( format->streams[ audioStream ]->codecpar, audioError );
		if ( audioCodec == nullptr ) {
			format->streams[ audioStream ]->discard = AVDISCARD_ALL;
			audioStream = -1;
		}
	}
	videoFrame = av_frame_alloc();
	audioFrame = av_frame_alloc();
	packet = av_packet_alloc();
	if ( videoFrame == nullptr || audioFrame == nullptr || packet == nullptr ) {
		error = "out of memory";
		return false;
	}
	return true;
}

/*
========================
idFFmpegDecoder::Close
========================
*/
void idFFmpegDecoder::Close() {
	av_frame_free( &videoFrame );
	av_frame_free( &audioFrame );
	av_packet_free( &packet );
	avcodec_free_context( &videoCodec );
	avcodec_free_context( &audioCodec );
	if ( format != nullptr ) {
		avformat_close_input( &format );
	}
	if ( io != nullptr ) {
		// FFmpeg may have replaced the buffer, free the current one
		av_freep( &io->buffer );
		avio_context_free( &io );
	}
	source.reset();

	videoStream = -1;
	audioStream = -1;
	width = 0;
	height = 0;
	frameRateNum = 0;
	frameRateDen = 1;
	numFrames = 0;
	demuxEnded = false;
	drained = false;
	frameIndex = -1;
	current = {};
}

/*
========================
idFFmpegDecoder::DecodeNextFrame
========================
*/
bool idFFmpegDecoder::DecodeNextFrame() {
	if ( !IsOpen() || !OpenCodecs() ) {
		return false;
	}
	for ( ;; ) {
		int err = avcodec_receive_frame( videoCodec, videoFrame );
		if ( err == 0 ) {
			if ( videoFrame->format != AV_PIX_FMT_YUV420P && videoFrame->format != AV_PIX_FMT_YUVA420P ) {
				const char * pixFmt = av_get_pix_fmt_name( static_cast< AVPixelFormat >( videoFrame->format ) );
				error = std::string( "unsupported pixel format " ) + ( pixFmt != nullptr ? pixFmt : "?" );
				return false;
			}
			for ( int i = 0; i < 3; i++ ) {
				current.planes[ i ] = videoFrame->data[ i ];
				current.pitches[ i ] = videoFrame->linesize[ i ];
			}
			current.width = videoFrame->width;
			current.height = videoFrame->height;
			frameIndex++;
			return true;
		}
		if ( err == AVERROR_EOF ) {
			return false;
		}
		if ( err != AVERROR( EAGAIN ) ) {
			error = "decoding failed: " + ErrorString( err );
			return false;
		}

		// the decoder wants more input
		if ( demuxEnded ) {
			if ( drained ) {
				return false;
			}
			avcodec_send_packet( videoCodec, nullptr );
			drained = true;
			continue;
		}
		err = av_read_frame( format, packet );
		if ( err < 0 ) {
			if ( err != AVERROR_EOF ) {
				error = "read failed: " + ErrorString( err );
			}
			demuxEnded = true;
			continue;
		}
		if ( packet->stream_index == videoStream ) {
			// a broken packet only loses its frame, keep going
			err = avcodec_send_packet( videoCodec, packet );
			if ( err < 0 ) {
				error = "bad video packet: " + ErrorString( err );
			}
		} else if ( packet->stream_index == audioStream ) {
			DecodeAudio( packet );
		}
		av_packet_unref( packet );
	}
}

/*
========================
idFFmpegDecoder::DecodeAudio
========================
*/
void idFFmpegDecoder::DecodeAudio( const AVPacket * audioPacket ) {
	if ( audioCodec == nullptr || !audioEnabled || !audioSink ) {
		return;
	}
	if ( avcodec_send_packet( audioCodec, audioPacket ) < 0 ) {
		return;
	}
	while ( avcodec_receive_frame( audioCodec, audioFrame ) == 0 ) {
		const int channels = audioFrame->ch_layout.nb_channels;
		const int samples = audioFrame->nb_samples;
		if ( channels <= 0 || samples <= 0 ) {
			continue;
		}
		audioSamples.resize( static_cast< size_t >( samples ) * channels );
		float * out = audioSamples.data();
		switch ( audioFrame->format ) {
			case AV_SAMPLE_FMT_FLTP:
				for ( int c = 0; c < channels; c++ ) {
					const auto * in = reinterpret_cast< const float * >( audioFrame->extended_data[ c ] );
					for ( int i = 0; i < samples; i++ ) {
						out[ i * channels + c ] = in[ i ];
					}
				}
				break;
			case AV_SAMPLE_FMT_FLT:
				memcpy( out, audioFrame->extended_data[ 0 ], audioSamples.size() * sizeof( float ) );
				break;
			case AV_SAMPLE_FMT_S16P:
				for ( int c = 0; c < channels; c++ ) {
					const auto * in = reinterpret_cast< const int16_t * >( audioFrame->extended_data[ c ] );
					for ( int i = 0; i < samples; i++ ) {
						out[ i * channels + c ] = in[ i ] * ( 1.0f / 32768.0f );
					}
				}
				break;
			case AV_SAMPLE_FMT_S16: {
				const auto * in = reinterpret_cast< const int16_t * >( audioFrame->extended_data[ 0 ] );
				for ( size_t i = 0; i < audioSamples.size(); i++ ) {
					out[ i ] = in[ i ] * ( 1.0f / 32768.0f );
				}
				break;
			}
			default:
				continue;
		}
		audioSink( out, samples, channels, audioFrame->sample_rate );
	}
}

/*
========================
idFFmpegDecoder::Rewind
========================
*/
bool idFFmpegDecoder::Rewind() {
	if ( !IsOpen() ) {
		return false;
	}
	// the Bink demuxer can only seek back to the first frame, which is all this needs
	int err = av_seek_frame( format, videoStream, 0, AVSEEK_FLAG_BACKWARD );
	if ( err < 0 ) {
		error = "can't rewind: " + ErrorString( err );
		return false;
	}
	if ( videoCodec != nullptr ) {
		avcodec_flush_buffers( videoCodec );
	}
	if ( audioCodec != nullptr ) {
		avcodec_flush_buffers( audioCodec );
	}
	demuxEnded = false;
	drained = false;
	frameIndex = -1;
	current = {};
	return true;
}

#endif // USE_FFMPEG
