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
#include "../../idlib/precompiled.h"
#include "../snd_local.h"
#include "../../../doomclassic/doom/i_sound.h"

#include <mmdeviceapi.h>

idCVar s_showLevelMeter( "s_showLevelMeter", "0", CVAR_BOOL|CVAR_ARCHIVE, "Show VU meter" );
idCVar s_meterTopTime( "s_meterTopTime", "1000", CVAR_INTEGER|CVAR_ARCHIVE, "How long (in milliseconds) peaks are displayed on the VU meter" );
idCVar s_meterPosition( "s_meterPosition", "100 100 20 200", CVAR_ARCHIVE, "VU meter location (x y w h)" );
idCVar s_device( "s_device", "-1", CVAR_INTEGER|CVAR_ARCHIVE, "Which audio device to use (listDevices to list, -1 for default)" );
idCVar s_showPerfData( "s_showPerfData", "0", CVAR_BOOL, "Show XAudio2 Performance data" );
extern idCVar s_volume_dB;

/*
========================
idSoundHardware_XAudio2::idSoundHardware_XAudio2
========================
*/
idSoundHardware_XAudio2::idSoundHardware_XAudio2() {
	pXAudio2 = NULL;
	pMasterVoice = NULL;
	pSubmixVoice = NULL;

	vuMeterRMS = NULL;
	vuMeterPeak = NULL;

	outputChannels = 0;
	channelMask = 0;

	voices.SetNum( 0 );
	zombieVoices.SetNum( 0 );
	freeVoices.SetNum( 0 );

	lastResetTime = 0;
}

/*
================================================
Audio device enumeration

XAudio2 2.8+ no longer enumerates devices itself (GetDeviceCount / GetDeviceDetails
were removed), so the render endpoints are listed through the MMDevice API and the
mastering voice is created with the endpoint ID.
================================================
*/

// PKEY_Device_FriendlyName / PKEY_AudioEngine_DeviceFormat, declared locally so no
// INITGUID / functiondiscoverykeys_devpkey.h is needed.
static const PROPERTYKEY D3_PKEY_Device_FriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const PROPERTYKEY D3_PKEY_AudioEngine_DeviceFormat = { { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };

static const int AUDIO_DEVICE_ID_LENGTH = 256;

static const ERole		audioDeviceRoles[] = { eConsole, eMultimedia, eCommunications };
static const char *		audioDeviceRoleNames[] = { "Console Device", "Multimedia Device", "Communications Device" };
static const int		NUM_AUDIO_DEVICE_ROLES = sizeof( audioDeviceRoles ) / sizeof( audioDeviceRoles[0] );

struct audioDevice_t {
	idStr		name;
	wchar_t		id[ AUDIO_DEVICE_ID_LENGTH ];
	int			channels;
	int			sampleRate;
	DWORD		channelMask;
	int			roles;			// bit mask of audioDeviceRoles indices
};

/*
========================
GetDefaultAudioDeviceId
========================
*/
static void GetDefaultAudioDeviceId( IMMDeviceEnumerator * enumerator, ERole role, wchar_t * id, size_t idSize ) {
	id[0] = L'\0';
	IMMDevice * device = NULL;
	if ( SUCCEEDED( enumerator->GetDefaultAudioEndpoint( eRender, role, &device ) ) ) {
		LPWSTR deviceId = NULL;
		if ( SUCCEEDED( device->GetId( &deviceId ) ) ) {
			wcsncpy_s( id, idSize, deviceId, _TRUNCATE );
			CoTaskMemFree( deviceId );
		}
		device->Release();
	}
}

/*
========================
EnumerateAudioDevices

Lists the active render endpoints. Returns false if the MMDevice API is unavailable.
========================
*/
static bool EnumerateAudioDevices( idList< audioDevice_t > & devices ) {
	devices.Clear();

	IMMDeviceEnumerator * enumerator = NULL;
	if ( FAILED( CoCreateInstance( __uuidof( MMDeviceEnumerator ), NULL, CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ), (void **)&enumerator ) ) ) {
		return false;
	}

	wchar_t defaultIds[ NUM_AUDIO_DEVICE_ROLES ][ AUDIO_DEVICE_ID_LENGTH ];
	for ( int r = 0; r < NUM_AUDIO_DEVICE_ROLES; r++ ) {
		GetDefaultAudioDeviceId( enumerator, audioDeviceRoles[r], defaultIds[r], AUDIO_DEVICE_ID_LENGTH );
	}

	IMMDeviceCollection * collection = NULL;
	if ( FAILED( enumerator->EnumAudioEndpoints( eRender, DEVICE_STATE_ACTIVE, &collection ) ) ) {
		enumerator->Release();
		return false;
	}

	UINT deviceCount = 0;
	if ( FAILED( collection->GetCount( &deviceCount ) ) ) {
		deviceCount = 0;
	}

	for ( UINT i = 0; i < deviceCount; i++ ) {
		IMMDevice * device = NULL;
		if ( FAILED( collection->Item( i, &device ) ) ) {
			continue;
		}

		LPWSTR deviceId = NULL;
		if ( FAILED( device->GetId( &deviceId ) ) ) {
			device->Release();
			continue;
		}

		audioDevice_t & info = devices.Alloc();
		info.name = "Unknown Device";
		wcsncpy_s( info.id, AUDIO_DEVICE_ID_LENGTH, deviceId, _TRUNCATE );
		info.channels = 0;
		info.sampleRate = 0;
		info.channelMask = 0;
		info.roles = 0;
		CoTaskMemFree( deviceId );

		for ( int r = 0; r < NUM_AUDIO_DEVICE_ROLES; r++ ) {
			if ( wcscmp( info.id, defaultIds[r] ) == 0 ) {
				info.roles |= BIT( r );
			}
		}

		IPropertyStore * properties = NULL;
		if ( SUCCEEDED( device->OpenPropertyStore( STGM_READ, &properties ) ) ) {
			PROPVARIANT value;

			PropVariantInit( &value );
			if ( SUCCEEDED( properties->GetValue( D3_PKEY_Device_FriendlyName, &value ) ) && value.vt == VT_LPWSTR && value.pwszVal != NULL ) {
				char mbcsDisplayName[ 256 ];
				wcstombs( mbcsDisplayName, value.pwszVal, sizeof( mbcsDisplayName ) );
				mbcsDisplayName[ sizeof( mbcsDisplayName ) - 1 ] = '\0';
				info.name = mbcsDisplayName;
			}
			PropVariantClear( &value );

			PropVariantInit( &value );
			if ( SUCCEEDED( properties->GetValue( D3_PKEY_AudioEngine_DeviceFormat, &value ) ) && value.vt == VT_BLOB && value.blob.cbSize >= sizeof( WAVEFORMATEX ) ) {
				const WAVEFORMATEX * format = (const WAVEFORMATEX *)value.blob.pBlobData;
				info.channels = format->nChannels;
				info.sampleRate = format->nSamplesPerSec;
				if ( format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && value.blob.cbSize >= sizeof( WAVEFORMATEXTENSIBLE ) ) {
					info.channelMask = ( (const WAVEFORMATEXTENSIBLE *)format )->dwChannelMask;
				}
			}
			PropVariantClear( &value );

			properties->Release();
		}
		device->Release();
	}

	collection->Release();
	enumerator->Release();
	return true;
}

/*
========================
PrintAudioDevice
========================
*/
static void PrintAudioDevice( int index, const audioDevice_t & device ) {
	idStaticList< const char *, NUM_AUDIO_DEVICE_ROLES > roles;
	for ( int r = 0; r < NUM_AUDIO_DEVICE_ROLES; r++ ) {
		if ( device.roles & BIT( r ) ) {
			roles.Append( audioDeviceRoleNames[r] );
		}
	}
	idStaticList< const char *, 11 > channelNames;
	if ( device.channelMask & SPEAKER_FRONT_LEFT ) {
		channelNames.Append( "Front Left" );
	}
	if ( device.channelMask & SPEAKER_FRONT_RIGHT ) {
		channelNames.Append( "Front Right" );
	}
	if ( device.channelMask & SPEAKER_FRONT_CENTER ) {
		channelNames.Append( "Front Center" );
	}
	if ( device.channelMask & SPEAKER_LOW_FREQUENCY ) {
		channelNames.Append( "Low Frequency" );
	}
	if ( device.channelMask & SPEAKER_BACK_LEFT ) {
		channelNames.Append( "Back Left" );
	}
	if ( device.channelMask & SPEAKER_BACK_RIGHT ) {
		channelNames.Append( "Back Right" );
	}
	if ( device.channelMask & SPEAKER_FRONT_LEFT_OF_CENTER ) {
		channelNames.Append( "Front Left of Center" );
	}
	if ( device.channelMask & SPEAKER_FRONT_RIGHT_OF_CENTER ) {
		channelNames.Append( "Front Right of Center" );
	}
	if ( device.channelMask & SPEAKER_BACK_CENTER ) {
		channelNames.Append( "Back Center" );
	}
	if ( device.channelMask & SPEAKER_SIDE_LEFT ) {
		channelNames.Append( "Side Left" );
	}
	if ( device.channelMask & SPEAKER_SIDE_RIGHT ) {
		channelNames.Append( "Side Right" );
	}
	idLib::Printf( "%3d: %s\n", index, device.name.c_str() );
	idLib::Printf( "     %d channels, %d Hz\n", device.channels, device.sampleRate );
	if ( device.channelMask != 0 && channelNames.Num() != device.channels ) {
		idLib::Printf( S_COLOR_YELLOW "WARNING: " S_COLOR_RED "Mismatch between # of channels and channel mask\n" );
	}
	if ( channelNames.Num() == 1 ) {
		idLib::Printf( "     %s\n", channelNames[0] );
	} else if ( channelNames.Num() == 2 ) {
		idLib::Printf( "     %s and %s\n", channelNames[0], channelNames[1] );
	} else if ( channelNames.Num() > 2 ) {
		idLib::Printf( "     %s", channelNames[0] );
		for ( int i = 1; i < channelNames.Num() - 1; i++ ) {
			idLib::Printf( ", %s", channelNames[i] );
		}
		idLib::Printf( ", and %s\n", channelNames[channelNames.Num() - 1] );
	}
	if ( roles.Num() == 1 ) {
		idLib::Printf( "     Default %s\n", roles[0] );
	} else if ( roles.Num() == 2 ) {
		idLib::Printf( "     Default %s and %s\n", roles[0], roles[1] );
	} else if ( roles.Num() > 2 ) {
		idLib::Printf( "     Default %s", roles[0] );
		for ( int i = 1; i < roles.Num() - 1; i++ ) {
			idLib::Printf( ", %s", roles[i] );
		}
		idLib::Printf( ", and %s\n", roles[roles.Num() - 1] );
	}
}

void listDevices_f( const idCmdArgs & args ) {
	idList< audioDevice_t > devices;
	if ( !EnumerateAudioDevices( devices ) || devices.Num() == 0 ) {
		idLib::Warning( "No audio devices found" );
		return;
	}
	for ( int i = 0; i < devices.Num(); i++ ) {
		PrintAudioDevice( i, devices[i] );
	}
}

/*
========================
idSoundHardware_XAudio2::Init
========================
*/
void idSoundHardware_XAudio2::Init() {

	cmdSystem->AddCommand( "listDevices", listDevices_f, 0, "Lists the connected sound devices", NULL );

	DWORD xAudioCreateFlags = 0;
#ifdef _DEBUG
	xAudioCreateFlags |= XAUDIO2_DEBUG_ENGINE;
#endif

	XAUDIO2_PROCESSOR xAudioProcessor = XAUDIO2_DEFAULT_PROCESSOR;

	if ( FAILED( XAudio2Create( &pXAudio2, xAudioCreateFlags, xAudioProcessor ) ) ) {
		if ( xAudioCreateFlags & XAUDIO2_DEBUG_ENGINE ) {
			// in case the debug engine isn't installed
			xAudioCreateFlags &= ~XAUDIO2_DEBUG_ENGINE;
			if ( FAILED( XAudio2Create( &pXAudio2, xAudioCreateFlags, xAudioProcessor ) ) ) {		
				idLib::FatalError( "Failed to create XAudio2 engine." );
				return;
			}
		} else {
			idLib::FatalError( "Failed to create XAudio2 engine." );
			return;
		}
	}
#ifdef _DEBUG
	XAUDIO2_DEBUG_CONFIGURATION debugConfiguration = { 0 };
	debugConfiguration.TraceMask = XAUDIO2_LOG_WARNINGS;
	debugConfiguration.BreakMask = XAUDIO2_LOG_ERRORS;
	pXAudio2->SetDebugConfiguration( &debugConfiguration );
#endif

	// Register the sound engine callback
	pXAudio2->RegisterForCallbacks( &soundEngineCallback );
	soundEngineCallback.hardware = this;

	idList< audioDevice_t > devices;
	if ( !EnumerateAudioDevices( devices ) || devices.Num() == 0 ) {
		idLib::Warning( "No audio devices found" );
		pXAudio2->Release();
		pXAudio2 = NULL;
		return;
	}

	for ( int i = 0; i < devices.Num(); i++ ) {
		PrintAudioDevice( i, devices[i] );
	}

	// -1 (or an out of range index) uses the system default device
	const wchar_t * deviceId = NULL;
	int preferredDevice = s_device.GetInteger();
	if ( preferredDevice >= 0 && preferredDevice < devices.Num() ) {
		deviceId = devices[ preferredDevice ].id;
		idLib::Printf( "Using device %d\n", preferredDevice );
	} else {
		idLib::Printf( "Using default device\n" );
	}

	DWORD outputSampleRate = 44100; // Max( (DWORD)XAUDIO2FX_REVERB_MIN_FRAMERATE, Min( (DWORD)XAUDIO2FX_REVERB_MAX_FRAMERATE, deviceDetails.OutputFormat.Format.nSamplesPerSec ) );

	if ( FAILED( pXAudio2->CreateMasteringVoice( &pMasterVoice, XAUDIO2_DEFAULT_CHANNELS, outputSampleRate, 0, deviceId, NULL, AudioCategory_GameEffects ) ) ) {
		idLib::Warning( "Failed to create master voice" );
		pXAudio2->Release();
		pXAudio2 = NULL;
		return;
	}
	pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ) );

	XAUDIO2_VOICE_DETAILS masterVoiceDetails;
	pMasterVoice->GetVoiceDetails( &masterVoiceDetails );
	DWORD masterChannelMask = 0;
	if ( FAILED( pMasterVoice->GetChannelMask( &masterChannelMask ) ) ) {
		masterChannelMask = 0;
	}

	outputChannels = masterVoiceDetails.InputChannels;
	channelMask = masterChannelMask;

	idSoundVoice::InitSurround( outputChannels, channelMask );

	// ---------------------
	// Initialize the Doom classic sound system.
	// ---------------------
	I_InitSoundHardware( outputChannels, channelMask );

	// ---------------------
	// Create VU Meter Effect
	// ---------------------
	IUnknown * vuMeter = NULL;
	XAudio2CreateVolumeMeter( &vuMeter, 0 );

	XAUDIO2_EFFECT_DESCRIPTOR descriptor;
	descriptor.InitialState = true;
	descriptor.OutputChannels = outputChannels;
	descriptor.pEffect = vuMeter;

	XAUDIO2_EFFECT_CHAIN chain;
	chain.EffectCount = 1;
	chain.pEffectDescriptors = &descriptor;

	pMasterVoice->SetEffectChain( &chain );

	vuMeter->Release();

	// ---------------------
	// Create VU Meter Graph
	// ---------------------

	vuMeterRMS = console->CreateGraph( outputChannels );
	vuMeterPeak = console->CreateGraph( outputChannels );
	vuMeterRMS->Enable( false );
	vuMeterPeak->Enable( false );

	memset( vuMeterPeakTimes, 0, sizeof( vuMeterPeakTimes ) );

	vuMeterPeak->SetFillMode( idDebugGraph::GRAPH_LINE );
	vuMeterPeak->SetBackgroundColor( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );

	vuMeterRMS->AddGridLine( 0.500f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	vuMeterRMS->AddGridLine( 0.250f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	vuMeterRMS->AddGridLine( 0.125f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );

	const char * channelNames[] = { "L", "R", "C", "S", "Lb", "Rb", "Lf", "Rf", "Cb", "Ls", "Rs" };
	for ( int i = 0, ci = 0; ci < sizeof( channelNames ) / sizeof( channelNames[0] ); ci++ ) {
		if ( ( channelMask & BIT( ci ) ) == 0 ) {
			continue;
		}
		vuMeterRMS->SetLabel( i, channelNames[ ci ] );
		i++;
	}

	// ---------------------
	// Create submix buffer
	// ---------------------
	if ( FAILED( pXAudio2->CreateSubmixVoice( &pSubmixVoice, 1, outputSampleRate, 0, 0, NULL, NULL ) ) ) {
		idLib::FatalError( "Failed to create submix voice" );
	}

	// XAudio doesn't really impose a maximum number of voices
	voices.SetNum( voices.Max() );
	freeVoices.SetNum( voices.Max() );
	zombieVoices.SetNum( 0 );
	for ( int i = 0; i < voices.Num(); i++ ) {
		freeVoices[i] = &voices[i];
	}
}

/*
========================
idSoundHardware_XAudio2::Shutdown
========================
*/
void idSoundHardware_XAudio2::Shutdown() {
	for ( int i = 0; i < voices.Num(); i++ ) {
		voices[ i ].DestroyInternal();
	}
	voices.Clear();
	freeVoices.Clear();
	zombieVoices.Clear();

	// ---------------------
	// Shutdown the Doom classic sound system.
	// ---------------------
	I_ShutdownSoundHardware();

	if ( pXAudio2 != NULL ) {
		// Unregister the sound engine callback
		pXAudio2->UnregisterForCallbacks( &soundEngineCallback );
	}

	if ( pSubmixVoice != NULL ) {
		pSubmixVoice->DestroyVoice();
		pSubmixVoice = NULL;
	}
	if ( pMasterVoice != NULL ) {
		// release the vu meter effect
		pMasterVoice->SetEffectChain( NULL );
		pMasterVoice->DestroyVoice();
		pMasterVoice = NULL;
	}
	if ( pXAudio2 != NULL ) {
		XAUDIO2_PERFORMANCE_DATA perfData;
		pXAudio2->GetPerformanceData( &perfData );
		idLib::Printf( "Final pXAudio2 performanceData: Voices: %d/%d CPU: %.2f%% Mem: %dkb\n", perfData.ActiveSourceVoiceCount, perfData.TotalSourceVoiceCount, perfData.AudioCyclesSinceLastQuery / (float)perfData.TotalCyclesSinceLastQuery, perfData.MemoryUsageInBytes / 1024 );
		pXAudio2->Release();
		pXAudio2 = NULL;
	}
	if ( vuMeterRMS != NULL ) {
		console->DestroyGraph( vuMeterRMS );
		vuMeterRMS = NULL;
	}
	if ( vuMeterPeak != NULL ) {
		console->DestroyGraph( vuMeterPeak );
		vuMeterPeak = NULL;
	}
}

/*
========================
idSoundHardware_XAudio2::AllocateVoice
========================
*/
idSoundVoice * idSoundHardware_XAudio2::AllocateVoice( const idSoundSample * leadinSample, const idSoundSample * loopingSample ) {
	if ( leadinSample == NULL ) {
		return NULL;
	}
	if ( loopingSample != NULL ) {
		if ( ( leadinSample->format.basic.formatTag != loopingSample->format.basic.formatTag ) || ( leadinSample->format.basic.numChannels != loopingSample->format.basic.numChannels ) ) {
			idLib::Warning( "Leadin/looping format mismatch: %s & %s", leadinSample->GetName(), loopingSample->GetName() );
			loopingSample = NULL;
		}
	}

	// Try to find a free voice that matches the format
	// But fallback to the last free voice if none match the format
	idSoundVoice * voice = NULL;
	for ( int i = 0; i < freeVoices.Num(); i++ ) {
		if ( freeVoices[i]->IsPlaying() ) {
			continue;
		}
		voice = (idSoundVoice *)freeVoices[i];
		if ( voice->CompatibleFormat( (idSoundSample_XAudio2*)leadinSample ) ) {
			break;
		}
	}
	if ( voice != NULL ) {
		voice->Create( leadinSample, loopingSample );
		freeVoices.Remove( voice );
		return voice;
	}
	
	return NULL;
}

/*
========================
idSoundHardware_XAudio2::FreeVoice
========================
*/
void idSoundHardware_XAudio2::FreeVoice( idSoundVoice * voice ) {
	voice->Stop();

	// Stop() is asyncronous, so we won't flush bufferes until the
	// voice on the zombie channel actually returns !IsPlaying() 
	zombieVoices.Append( voice );
}

/*
========================
idSoundHardware_XAudio2::Update
========================
*/
void idSoundHardware_XAudio2::Update() {
	if ( pXAudio2 == NULL ) {
		int nowTime = Sys_Milliseconds();
		if ( lastResetTime + 1000 < nowTime ) {
			lastResetTime = nowTime;
			Init();
		}
		return;
	}
	if ( soundSystem->IsMuted() ) {
		pMasterVoice->SetVolume( 0.0f, OPERATION_SET );
	} else {
		pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ), OPERATION_SET );
	}

	pXAudio2->CommitChanges( XAUDIO2_COMMIT_ALL );

	// IXAudio2SourceVoice::Stop() has been called for every sound on the
	// zombie list, but it is documented as asyncronous, so we have to wait
	// until it actually reports that it is no longer playing.
	for ( int i = 0; i < zombieVoices.Num(); i++ ) {
		zombieVoices[i]->FlushSourceBuffers();
		if ( !zombieVoices[i]->IsPlaying() ) {
			freeVoices.Append( zombieVoices[i] );
			zombieVoices.RemoveIndexFast( i );
			i--;
		} else {
			static int playingZombies;
			playingZombies++;
		}
	}

	if ( s_showPerfData.GetBool() ) {
		XAUDIO2_PERFORMANCE_DATA perfData;
		pXAudio2->GetPerformanceData( &perfData );
		idLib::Printf( "Voices: %d/%d CPU: %.2f%% Mem: %dkb\n", perfData.ActiveSourceVoiceCount, perfData.TotalSourceVoiceCount, perfData.AudioCyclesSinceLastQuery / (float)perfData.TotalCyclesSinceLastQuery, perfData.MemoryUsageInBytes / 1024 );
	}

	if ( vuMeterRMS == NULL ) {
		// Init probably hasn't been called yet
		return;
	}

	vuMeterRMS->Enable( s_showLevelMeter.GetBool() );
	vuMeterPeak->Enable( s_showLevelMeter.GetBool() );

	if ( !s_showLevelMeter.GetBool() ) {
		pMasterVoice->DisableEffect( 0 );
		return;
	} else {
		pMasterVoice->EnableEffect( 0 );
	}

	float peakLevels[ 8 ];
	float rmsLevels[ 8 ];

	XAUDIO2FX_VOLUMEMETER_LEVELS levels;
	levels.ChannelCount = outputChannels;
	levels.pPeakLevels = peakLevels;
	levels.pRMSLevels = rmsLevels;

	if ( levels.ChannelCount > 8 ) {
		levels.ChannelCount = 8;
	}

	pMasterVoice->GetEffectParameters( 0, &levels, sizeof( levels ) );

	int currentTime = Sys_Milliseconds();
	for ( int i = 0; i < outputChannels; i++ ) {
		if ( vuMeterPeakTimes[i] < currentTime ) {
			vuMeterPeak->SetValue( i, vuMeterPeak->GetValue( i ) * 0.9f, colorRed );
		}
	}

	float width = 20.0f;
	float height = 200.0f;
	float left = 100.0f;
	float top = 100.0f;

	sscanf( s_meterPosition.GetString(), "%f %f %f %f", &left, &top, &width, &height );

	vuMeterRMS->SetPosition( left, top, width * levels.ChannelCount, height );
	vuMeterPeak->SetPosition( left, top, width * levels.ChannelCount, height );

	for ( uint32 i = 0; i < levels.ChannelCount; i++ ) {
		vuMeterRMS->SetValue( i, rmsLevels[ i ], idVec4( 0.5f, 1.0f, 0.0f, 1.00f ) );
		if ( peakLevels[ i ] >= vuMeterPeak->GetValue( i ) ) {
			vuMeterPeak->SetValue( i, peakLevels[ i ], colorRed );
			vuMeterPeakTimes[i] = currentTime + s_meterTopTime.GetInteger();
		}
	}
}


/*
================================================
idSoundEngineCallback
================================================
*/

/*
========================
idSoundEngineCallback::OnCriticalError
========================
*/
void idSoundEngineCallback::OnCriticalError( HRESULT Error ) {
	soundSystemLocal.SetNeedsRestart();
}
