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

#pragma hdrstop
#include "../idlib/precompiled.h"


extern idCVar s_noSound;

#include "tr_local.h"

#ifdef USE_FFMPEG
#include "FFmpegDecoder.h"

#include <deque>
#include <memory>
#include <vector>

#include <xaudio2.h>
#endif

/*
===============================================================================

	The Bink videos (videoMap materials, the loading video, the PDA video discs, the
	shell's Mars) are decoded with FFmpeg when the build has it (USE_FFMPEG) and its DLLs
	are found at startup. Otherwise idCinematic stays the stub of the GPL release: no
	frames, so the videos are black.

	Time base: ImageForTime( 0 ) means "now" (Sys_Milliseconds), which is what the 2D GUI
	views pass (their renderView.time is 0) and what ResetTime( Sys_Milliseconds() ) callers
	expect; world surfaces pass the view's game time.

	Threading: ImageForTime runs in the backend on the main thread, where the GL uploads
	are legal. Materials, and with them cinematics, can be created, reset and freed on the
	game thread, so every entry point locks the cinematic, and nothing but ImageForTime
	(and ShutdownCinematic) touches GL.

===============================================================================
*/

#ifdef USE_FFMPEG

namespace {

// the delay-loaded FFmpeg DLLs were found by InitCinematic
bool					ffmpegAvailable = false;

// how long one ImageForTime may decode to catch up after it was not called for a while;
// it always decodes at least one frame
constexpr uint64		MAX_CATCHUP_USEC = 8000;

// audio is only decoded within this many frames of the frame to show, so the sound of
// frames that are skipped to catch up doesn't play late
constexpr int64			AUDIO_MAX_LAG_FRAMES = 2;

// asked for a frame this little behind the current one, the cinematic keeps the current frame
// instead of decoding again from the start
constexpr int64			REWIND_TOLERANCE_FRAMES = 2;

/*
================================================
Y, Cr and Cb textures of a cinematic (bink shader units 0, 1, 2), outside the image manager.
A cinematic can be freed on any thread but deleting a texture needs the GL context, so freed
sets go to a pool for the next cinematic.
================================================
*/
struct cinematicImages_t {
	idImage *	y = nullptr;
	idImage *	cr = nullptr;
	idImage *	cb = nullptr;
};

idSysMutex						imagePoolMutex;
idList<cinematicImages_t>		imagePool;			// free sets
idList<idImage *>				allImages;			// every image ever made, for ShutdownCinematic
int								numImageSets = 0;

/*
========================
PlaneMatches / UploadPlane
========================
*/
bool PlaneMatches( const idImage * image, int width, int height ) {
	return image->IsLoaded() && image->GetOpts().format == FMT_LUM8 &&
		image->GetUploadWidth() == width && image->GetUploadHeight() == height;
}

void UploadPlane( idImage * image, const uint8 * data, int pitch, int width, int height ) {
	if ( !PlaneMatches( image, width, height ) ) {
		idImageOpts opts;
		opts.format = FMT_LUM8;
		opts.colorFormat = CFM_DEFAULT;
		opts.width = width;
		opts.height = height;
		opts.numLevels = 1;
		// videoMaps are always clamped
		image->AllocImage( opts, TF_LINEAR, TR_CLAMP );
	}
	// the row length is in pixels, which are bytes here
	image->SubImageUpload( 0, 0, width, height, data, pitch );
}

/*
================================================
idCinematicFile reads the video through the engine file system, so a file inside a .resources
container works as well as a loose one.
================================================
*/
class idCinematicFile final : public idFFmpegDecoder::Source {
public:
	explicit			idCinematicFile( idFile * f ) : file( f ) {}
						~idCinematicFile() override { fileSystem->CloseFile( file ); }

	int					Read( uint8 * buffer, int size ) override { return file->Read( buffer, size ); }
	bool				Seek( int64 offset ) override {
		return offset >= 0 && offset <= INT_MAX && file->Seek( static_cast<long>( offset ), FS_SEEK_SET ) == 0;
	}
	int64				Tell() const override { return file->Tell(); }
	int64				Size() const override { return file->Length(); }

private:
	idFile *			file;
};

/*
================================================
The sound track of a video (only video/loadvideo.bik has one) on its own XAudio2 voice into
the mastering voice, as the Bink SDK did, so s_volume_dB and muting apply. Called with the
cinematic locked.
================================================
*/
class idCinematicAudio {
public:
						idCinematicAudio() = default;
						~idCinematicAudio() { Stop(); }
						idCinematicAudio( const idCinematicAudio & ) = delete;
	idCinematicAudio &	operator=( const idCinematicAudio & ) = delete;

	void				Submit( const float * samples, int numFrames, int numChannels, int sampleRate );
	// cuts the sound and frees the voice
	void				Stop();

private:
	void				Reclaim();

	IXAudio2 *			engine = nullptr;	// the engine the voice was made on
	IXAudio2SourceVoice * voice = nullptr;
	int					channels = 0;
	int					rate = 0;
	// XAudio2 reads the submitted buffers in place until they are played
	std::deque< std::vector<float> > queued;
};

/*
========================
idCinematicAudio::Submit
========================
*/
void idCinematicAudio::Submit( const float * samples, int numFrames, int numChannels, int sampleRate ) {
	auto * xa2 = static_cast<IXAudio2 *>( soundSystem->GetIXAudio2() );
	if ( xa2 == nullptr || s_noSound.GetBool() || numFrames <= 0 || numChannels <= 0 || numChannels > XAUDIO2_MAX_AUDIO_CHANNELS ) {
		Stop();
		return;
	}
	if ( voice != nullptr && ( engine != xa2 || channels != numChannels || rate != sampleRate ) ) {
		Stop();
	}
	if ( voice == nullptr ) {
		WAVEFORMATEX format = {};
		format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		format.nChannels = static_cast<WORD>( numChannels );
		format.nSamplesPerSec = static_cast<DWORD>( sampleRate );
		format.wBitsPerSample = 32;
		format.nBlockAlign = static_cast<WORD>( numChannels * sizeof( float ) );
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
		if ( FAILED( xa2->CreateSourceVoice( &voice, &format ) ) ) {
			voice = nullptr;
			return;
		}
		engine = xa2;
		channels = numChannels;
		rate = sampleRate;
		voice->Start();
	}

	Reclaim();
	if ( queued.size() >= XAUDIO2_MAX_QUEUED_BUFFERS - 1 ) {
		return;		// nobody is listening (the video stalled), drop it
	}
	queued.emplace_back( samples, samples + static_cast<size_t>( numFrames ) * numChannels );
	const std::vector<float> & buffer = queued.back();

	XAUDIO2_BUFFER xbuffer = {};
	xbuffer.AudioBytes = static_cast<UINT32>( buffer.size() * sizeof( float ) );
	xbuffer.pAudioData = reinterpret_cast<const BYTE *>( buffer.data() );
	if ( FAILED( voice->SubmitSourceBuffer( &xbuffer ) ) ) {
		queued.pop_back();
	}
}

/*
========================
idCinematicAudio::Reclaim

Frees the buffers the voice has finished with; they finish in order.
========================
*/
void idCinematicAudio::Reclaim() {
	XAUDIO2_VOICE_STATE state = {};
	voice->GetState( &state, XAUDIO2_VOICE_NOSAMPLESPLAYED );
	while ( queued.size() > state.BuffersQueued ) {
		queued.pop_front();
	}
}

/*
========================
idCinematicAudio::Stop
========================
*/
void idCinematicAudio::Stop() {
	if ( voice != nullptr ) {
		// a voice dies with its engine: when the sound system was restarted or shut down,
		// there is nothing left to destroy
		if ( engine == static_cast<IXAudio2 *>( soundSystem->GetIXAudio2() ) ) {
			voice->DestroyVoice();		// synchronous, the buffers are free after it
		}
		voice = nullptr;
		engine = nullptr;
	}
	queued.clear();
}

/*
================================================
idCinematicFFmpeg
================================================
*/
class idCinematicFFmpeg final : public idCinematic {
public:
						idCinematicFFmpeg() = default;
						~idCinematicFFmpeg() override;

	bool				InitFromFile( const char * qpath, bool looping ) override;
	int					AnimationLength() override;
	cinData_t			ImageForTime( int milliseconds ) override;
	void				Close() override;
	void				ResetTime( int time ) override;
	int					GetStartTime() override;
	void				ExportToTGA( bool skipExisting = true ) override;
	float				GetFrameRate() const override;

private:
	void				CloseLocked();
	[[nodiscard]] int64	FrameForTime( int time ) const;
	[[nodiscard]] int64	TimeForFrames( int64 frames ) const;
	bool				DecodeTo( int64 frame );
	bool				AcquireImages();
	void				ReleaseImages();

	mutable idSysMutex	mutex;

	idStr				fileName;
	std::unique_ptr<idFFmpegDecoder> decoder;
	idCinematicAudio	audio;
	cinematicImages_t	images;
	int64				uploadedFrame = -1;		// frame in the images

	bool				looping = false;
	int					status = FMV_EOF;
	int					startTime = -1;			// -1: starts at the first ImageForTime
	int					numFrames = 0;
	int					frameRateNum = 30;
	int					frameRateDen = 1;
	bool				warned = false;			// a decoding error was reported
};

/*
========================
CinematicPath

The materials name the original Doom 3 RoQ files; the BFG Edition has the same videos as Bink.
A bare file name is looked up in video/.
========================
*/
idStr CinematicPath( const char * qpath ) {
	idStr path = qpath;
	path.BackSlashesToSlashes();
	if ( path.Find( '/' ) < 0 ) {
		path = idStr( "video/" ) + path;
	}
	path.SetFileExtension( ".bik" );
	return path;
}

/*
========================
idCinematicFFmpeg::~idCinematicFFmpeg
========================
*/
idCinematicFFmpeg::~idCinematicFFmpeg() {
	Close();
}

/*
========================
idCinematicFFmpeg::InitFromFile
========================
*/
bool idCinematicFFmpeg::InitFromFile( const char * qpath, bool amLooping ) {
	idScopedCriticalSection lock( mutex );
	CloseLocked();

	if ( qpath == nullptr || qpath[ 0 ] == '\0' ) {
		return false;
	}
	fileName = CinematicPath( qpath );
	looping = amLooping;

	idFile * file = fileSystem->OpenFileRead( fileName );
	if ( file == nullptr ) {
		// plenty of the materials name videos that never shipped
		common->DPrintf( "idCinematic: %s not found\n", fileName.c_str() );
		return false;
	}
	auto newDecoder = std::make_unique<idFFmpegDecoder>();
	if ( !newDecoder->Open( std::make_unique<idCinematicFile>( file ), fileName ) ) {
		common->Warning( "idCinematic: can't play %s: %s", fileName.c_str(), newDecoder->GetError().c_str() );
		return false;
	}
	if ( newDecoder->NumFrames() <= 0 ) {
		common->Warning( "idCinematic: %s has no frames", fileName.c_str() );
		return false;
	}
	decoder = std::move( newDecoder );
	numFrames = decoder->NumFrames();
	frameRateNum = decoder->FrameRateNum();
	frameRateDen = decoder->FrameRateDen();
	if ( decoder->HasAudio() ) {
		decoder->SetAudioSink( [this]( const float * samples, int frames, int channels, int rate ) {
			audio.Submit( samples, frames, channels, rate );
		} );
	}
	status = FMV_PLAY;
	startTime = -1;
	uploadedFrame = -1;
	warned = false;
	return true;
}

/*
========================
idCinematicFFmpeg::FrameForTime
========================
*/
int64 idCinematicFFmpeg::FrameForTime( int time ) const {
	const int64 elapsed = static_cast<int64>( time ) - startTime;
	if ( elapsed <= 0 ) {
		return 0;
	}
	return elapsed * frameRateNum / ( static_cast<int64>( frameRateDen ) * 1000 );
}

/*
========================
idCinematicFFmpeg::TimeForFrames

Milliseconds the given number of frames take, rounded up so that
FrameForTime( startTime + TimeForFrames( n ) ) is n.
========================
*/
int64 idCinematicFFmpeg::TimeForFrames( int64 frames ) const {
	return ( frames * frameRateDen * 1000 + frameRateNum - 1 ) / frameRateNum;
}

/*
========================
idCinematicFFmpeg::DecodeTo

Decodes forward to the given frame, within MAX_CATCHUP_USEC once one frame is done. False when
the stream ended first.
========================
*/
bool idCinematicFFmpeg::DecodeTo( int64 frame ) {
	const uint64 deadline = Sys_Microseconds() + MAX_CATCHUP_USEC;
	while ( decoder->FrameIndex() < frame ) {
		decoder->EnableAudio( frame - decoder->FrameIndex() <= AUDIO_MAX_LAG_FRAMES );
		if ( !decoder->DecodeNextFrame() ) {
			if ( !decoder->GetError().empty() && !warned ) {
				common->Warning( "idCinematic: %s: %s", fileName.c_str(), decoder->GetError().c_str() );
				warned = true;
			}
			// the header promised more frames than there are
			const int64 decoded = decoder->FrameIndex() + 1;
			if ( decoded > 0 && decoded < numFrames ) {
				numFrames = static_cast<int>( decoded );
			}
			return false;
		}
		if ( Sys_Microseconds() > deadline ) {
			break;
		}
	}
	return true;
}

/*
========================
idCinematicFFmpeg::ImageForTime
========================
*/
cinData_t idCinematicFFmpeg::ImageForTime( int thisTime ) {
	cinData_t cin = {};

	idScopedCriticalSection lock( mutex );

	if ( !decoder || status == FMV_EOF || status == FMV_IDLE || !R_IsInitialized() ) {
		return cin;
	}
	if ( thisTime <= 0 ) {
		thisTime = Sys_Milliseconds();
	}
	if ( startTime == -1 ) {
		startTime = thisTime;
	}

	bool lastFrame = false;		// a non looping video shows its last frame
	bool wrapped = false;		// a looping one came around
	int64 frame = FrameForTime( thisTime );
	if ( frame >= numFrames ) {
		if ( looping ) {
			startTime += static_cast<int>( TimeForFrames( frame / numFrames * numFrames ) );
			frame = Min( FrameForTime( thisTime ), static_cast<int64>( numFrames - 1 ) );
			wrapped = true;
		} else {
			frame = numFrames - 1;
			lastFrame = true;
		}
	}

	const int64 current = decoder->FrameIndex();
	if ( frame < current ) {
		if ( !wrapped && frame + REWIND_TOLERANCE_FRAMES >= current ) {
			// only a little behind (views on different clocks): keep the frame there is
			frame = current;
		} else if ( !decoder->Rewind() ) {
			// back in time: ResetTime, or the loop came around
			common->Warning( "idCinematic: %s: %s", fileName.c_str(), decoder->GetError().c_str() );
			status = FMV_EOF;
			return cin;
		}
	} else if ( looping && current >= 0 && frame - current > frameRateNum / frameRateDen ) {
		// a looping video that was out of sight for a while continues where it was instead of
		// fast forwarding (non looping ones catch up, their sound may be a separate shader)
		frame = current + 1;
		startTime = thisTime - static_cast<int>( TimeForFrames( frame ) );
		if ( frame >= numFrames ) {
			frame = 0;
			startTime = thisTime;
			decoder->Rewind();
		}
	}

	if ( !DecodeTo( frame ) ) {
		if ( decoder->FrameIndex() < 0 ) {
			status = FMV_EOF;		// not a single frame
			return cin;
		}
		// the stream ended early; a looping video comes around on the next call, numFrames
		// is the real count now
		lastFrame = !looping;
	} else if ( lastFrame && decoder->FrameIndex() < frame ) {
		lastFrame = false;			// still catching up to it
	}

	if ( !AcquireImages() ) {
		return cin;
	}
	const idFFmpegDecoder::Frame & f = decoder->CurrentFrame();
	const int chromaWidth = ( f.width + 1 ) / 2;
	const int chromaHeight = ( f.height + 1 ) / 2;
	if ( uploadedFrame != decoder->FrameIndex() || !PlaneMatches( images.y, f.width, f.height ) ||
		 !PlaneMatches( images.cr, chromaWidth, chromaHeight ) || !PlaneMatches( images.cb, chromaWidth, chromaHeight ) ) {
		UploadPlane( images.y, f.planes[ 0 ], f.pitches[ 0 ], f.width, f.height );
		UploadPlane( images.cb, f.planes[ 1 ], f.pitches[ 1 ], chromaWidth, chromaHeight );
		UploadPlane( images.cr, f.planes[ 2 ], f.pitches[ 2 ], chromaWidth, chromaHeight );
		// an upload may reach the texture through a slot behind the back of the latch: the next Bind must really bind
		renderBackend->ForgetTextureBindings();
		uploadedFrame = decoder->FrameIndex();
	}

	if ( lastFrame ) {
		// shows the last frame now, black from the next call on (until ResetTime)
		status = FMV_IDLE;
	}

	cin.imageWidth = f.width;
	cin.imageHeight = f.height;
	cin.imageY = images.y;
	cin.imageCr = images.cr;
	cin.imageCb = images.cb;
	cin.status = lastFrame ? FMV_EOF : FMV_PLAY;
	return cin;
}

/*
========================
idCinematicFFmpeg::AcquireImages

Main thread with the GL context.
========================
*/
bool idCinematicFFmpeg::AcquireImages() {
	if ( images.y != nullptr ) {
		return true;
	}
	idScopedCriticalSection lock( imagePoolMutex );
	if ( imagePool.Num() > 0 ) {
		images = imagePool[ imagePool.Num() - 1 ];
		imagePool.RemoveIndex( imagePool.Num() - 1 );
		uploadedFrame = -1;
		return true;
	}
	const int set = numImageSets++;
	images.y = globalImages->AllocStandaloneImage( va( "_cinematic%iY", set ) );
	images.cr = globalImages->AllocStandaloneImage( va( "_cinematic%iCr", set ) );
	images.cb = globalImages->AllocStandaloneImage( va( "_cinematic%iCb", set ) );
	allImages.Append( images.y );
	allImages.Append( images.cr );
	allImages.Append( images.cb );
	uploadedFrame = -1;
	return true;
}

/*
========================
idCinematicFFmpeg::ReleaseImages

Any thread: no GL here, the images go back to the pool with their textures.
========================
*/
void idCinematicFFmpeg::ReleaseImages() {
	if ( images.y == nullptr ) {
		return;
	}
	idScopedCriticalSection lock( imagePoolMutex );
	imagePool.Append( images );
	images = {};
	uploadedFrame = -1;
}

/*
========================
idCinematicFFmpeg::AnimationLength
========================
*/
int idCinematicFFmpeg::AnimationLength() {
	idScopedCriticalSection lock( mutex );
	if ( !decoder ) {
		return 0;
	}
	return static_cast<int>( TimeForFrames( numFrames ) );
}

/*
========================
idCinematicFFmpeg::GetFrameRate
========================
*/
float idCinematicFFmpeg::GetFrameRate() const {
	idScopedCriticalSection lock( mutex );
	return static_cast<float>( frameRateNum ) / static_cast<float>( frameRateDen );
}

/*
========================
idCinematicFFmpeg::GetStartTime
========================
*/
int idCinematicFFmpeg::GetStartTime() {
	idScopedCriticalSection lock( mutex );
	return startTime;
}

/*
========================
idCinematicFFmpeg::ResetTime
========================
*/
void idCinematicFFmpeg::ResetTime( int time ) {
	idScopedCriticalSection lock( mutex );
	startTime = time;
	status = decoder ? FMV_PLAY : FMV_EOF;
	// the next ImageForTime rewinds; the old sound stops now
	audio.Stop();
}

/*
========================
idCinematicFFmpeg::Close
========================
*/
void idCinematicFFmpeg::Close() {
	idScopedCriticalSection lock( mutex );
	CloseLocked();
}

/*
========================
idCinematicFFmpeg::CloseLocked
========================
*/
void idCinematicFFmpeg::CloseLocked() {
	audio.Stop();
	decoder.reset();
	ReleaseImages();
	fileName.Clear();
	status = FMV_EOF;
	startTime = -1;
	numFrames = 0;
	frameRateNum = 30;
	frameRateDen = 1;
}

/*
========================
idCinematicFFmpeg::ExportToTGA

Writes every frame to <video path without extension>/frameNNNNN.tga in fs_savepath, converted
to RGB like the bink shaders do.
========================
*/
void idCinematicFFmpeg::ExportToTGA( bool skipExisting ) {
	idScopedCriticalSection lock( mutex );
	if ( !decoder || !decoder->Rewind() ) {
		return;
	}
	audio.Stop();
	decoder->EnableAudio( false );

	idStr baseName = fileName;
	baseName.StripFileExtension();

	std::vector<byte> rgba;
	int written = 0;
	while ( decoder->DecodeNextFrame() ) {
		const idStr tgaName = va( "%s/frame%05i.tga", baseName.c_str(), static_cast<int>( decoder->FrameIndex() ) );
		if ( skipExisting && fileSystem->ReadFile( tgaName, nullptr, nullptr ) > 0 ) {
			continue;
		}
		const idFFmpegDecoder::Frame & f = decoder->CurrentFrame();
		rgba.resize( static_cast<size_t>( f.width ) * f.height * 4 );
		for ( int y = 0; y < f.height; y++ ) {
			const uint8 * lumaRow = f.planes[ 0 ] + y * f.pitches[ 0 ];
			const uint8 * cbRow = f.planes[ 1 ] + ( y / 2 ) * f.pitches[ 1 ];
			const uint8 * crRow = f.planes[ 2 ] + ( y / 2 ) * f.pitches[ 2 ];
			byte * out = &rgba[ static_cast<size_t>( y ) * f.width * 4 ];
			for ( int x = 0; x < f.width; x++, out += 4 ) {
				// same constants as base/shaders/bink.hlsl
				const float luma = 1.164123535f * ( lumaRow[ x ] / 255.0f );
				const float cr = crRow[ x / 2 ] / 255.0f;
				const float cb = cbRow[ x / 2 ] / 255.0f;
				const float r = luma + 1.595794678f * cr - 0.87065506f;
				const float g = luma - 0.813476563f * cr - 0.391448975f * cb + 0.529705048f;
				const float b = luma + 2.017822266f * cb - 1.081668854f;
				out[ 0 ] = static_cast<byte>( idMath::Ftoi( idMath::ClampFloat( 0.0f, 1.0f, r ) * 255.0f + 0.5f ) );
				out[ 1 ] = static_cast<byte>( idMath::Ftoi( idMath::ClampFloat( 0.0f, 1.0f, g ) * 255.0f + 0.5f ) );
				out[ 2 ] = static_cast<byte>( idMath::Ftoi( idMath::ClampFloat( 0.0f, 1.0f, b ) * 255.0f + 0.5f ) );
				out[ 3 ] = 255;
			}
		}
		R_WriteTGA( tgaName, rgba.data(), f.width, f.height );
		written++;
	}
	common->Printf( "%s: wrote %i of %i frames to %s/\n", fileName.c_str(), written, static_cast<int>( decoder->FrameIndex() + 1 ), baseName.c_str() );

	// the next ImageForTime decodes from the start again
	decoder->Rewind();
	decoder->EnableAudio( true );
}

}	// namespace

#endif // USE_FFMPEG

//===========================================

/*
==============
idCinematic::InitCinematic
==============
*/
void idCinematic::InitCinematic() {
#ifdef USE_FFMPEG
	ffmpegAvailable = idFFmpegDecoder::LoadLibraries();
	if ( ffmpegAvailable ) {
		common->Printf( "Bink videos: FFmpeg %s\n", idFFmpegDecoder::VersionString() );
	} else {
		common->Printf( "Bink videos disabled: the FFmpeg DLLs (avformat, avcodec, avutil) are not next to the executable\n" );
	}
#else
	common->Printf( "Bink videos disabled: built without FFmpeg\n" );
#endif
}

/*
==============
idCinematic::ShutdownCinematic

Called by the render system shutdown while the GL context is still there.
==============
*/
void idCinematic::ShutdownCinematic() {
#ifdef USE_FFMPEG
	idScopedCriticalSection lock( imagePoolMutex );
	if ( R_IsInitialized() ) {
		for ( int i = 0; i < allImages.Num(); i++ ) {
			allImages[ i ]->PurgeImage();
		}
	}
	// the pooled sets are unused; the others still belong to live cinematics, which keep
	// working (their textures are made again) if the renderer comes back
	for ( int i = 0; i < imagePool.Num(); i++ ) {
		const cinematicImages_t & set = imagePool[ i ];
		for ( idImage * image : { set.y, set.cr, set.cb } ) {
			allImages.Remove( image );
			delete image;
		}
	}
	imagePool.Clear();
#endif
}

/*
==============
idCinematic::Alloc
==============
*/
idCinematic * idCinematic::Alloc() {
#ifdef USE_FFMPEG
	if ( ffmpegAvailable ) {
		return new ( TAG_CINEMATIC ) idCinematicFFmpeg;
	}
#endif
	return new ( TAG_CINEMATIC ) idCinematic;
}

/*
==============
idCinematic::~idCinematic
==============
*/
idCinematic::~idCinematic( ) {
	Close();
}

/*
==============
idCinematic::InitFromFile
==============
*/
bool idCinematic::InitFromFile( const char *qpath, bool looping ) {
	return false;
}

/*
==============
idCinematic::AnimationLength
==============
*/
int idCinematic::AnimationLength() {
	return 0;
}

/*
==============
idCinematic::GetStartTime
==============
*/
int idCinematic::GetStartTime() {
	return -1;
}

/*
==============
idCinematic::ResetTime
==============
*/
void idCinematic::ResetTime(int milliseconds) {
}

/*
==============
idCinematic::ImageForTime
==============
*/
cinData_t idCinematic::ImageForTime( int milliseconds ) {
	cinData_t c;
	memset( &c, 0, sizeof( c ) );
	return c;
}

/*
==============
idCinematic::ExportToTGA
==============
*/
void idCinematic::ExportToTGA( bool skipExisting ) {
}

/*
==============
idCinematic::GetFrameRate
==============
*/
float idCinematic::GetFrameRate() const {
	return 30.0f;
}

/*
==============
idCinematic::Close
==============
*/
void idCinematic::Close() {
}

/*
==============
idSndWindow::InitFromFile
==============
*/
bool idSndWindow::InitFromFile( const char *qpath, bool looping ) {
	idStr fname = qpath;

	fname.ToLower();
	if ( !fname.Icmp( "waveform" ) ) {
		showWaveform = true;
	} else {
		showWaveform = false;
	}
	return true;
}

/*
==============
idSndWindow::ImageForTime
==============
*/
cinData_t idSndWindow::ImageForTime( int milliseconds ) {
	return soundSystem->ImageForTime( milliseconds, showWaveform );
}

/*
==============
idSndWindow::AnimationLength
==============
*/
int idSndWindow::AnimationLength() {
	return -1;
}
