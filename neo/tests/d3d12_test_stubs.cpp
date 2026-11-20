// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../idlib/precompiled.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

/*
	Engine stubs for d3d12Tests: the D3D12 objects report through common, so it prints to stdout;
	Error and FatalError end the run with exit code 1. The other systems stay NULL.
*/

namespace {

[[noreturn]] void TestAbort( const char * what ) {
	printf( "d3d12Tests: %s is not part of the test engine\n", what );
	fflush( stdout );
	exit( 1 );
}

void PrintLine( const char * prefix, const char * fmt, va_list args ) {
	printf( "%s", prefix );
	vprintf( fmt, args );
	const size_t length = strlen( fmt );
	if ( length == 0 || fmt[length - 1] != '\n' ) {
		printf( "\n" );
	}
}

class idCommonTestStub final : public idCommon {
public:
	void				Init( int, const char * const *, const char * ) override {}
	void				Shutdown() override {}
	bool				IsShuttingDown() const override { return false; }
	void				CreateMainMenu() override {}
	void				Quit() override { exit( 0 ); }
	bool				IsInitialized() const override { return true; }
	bool				IsMapSpawned() const override { return false; }
	void				Frame() override {}
	void				UpdateScreen( bool ) override {}
	void				UpdateLevelLoadPacifier() override {}
	void				StartupVariable( const char * ) override {}

	void				BeginRedirect( char *, int, void ( * )( const char * ) ) override {}
	void				EndRedirect() override {}
	void				SetRefreshOnPrint( bool ) override {}
	void				Printf( VERIFY_FORMAT_STRING const char * fmt, ... ) override {
		va_list args;
		va_start( args, fmt );
		vprintf( fmt, args );
		va_end( args );
	}
	void				VPrintf( const char * fmt, va_list args ) override { vprintf( fmt, args ); }
	void				DPrintf( VERIFY_FORMAT_STRING const char * fmt, ... ) override {
		va_list args;
		va_start( args, fmt );
		vprintf( fmt, args );
		va_end( args );
	}
	void				Warning( VERIFY_FORMAT_STRING const char * fmt, ... ) override {
		va_list args;
		va_start( args, fmt );
		PrintLine( "WARNING: ", fmt, args );
		va_end( args );
	}
	void				DWarning( VERIFY_FORMAT_STRING const char * fmt, ... ) override {
		va_list args;
		va_start( args, fmt );
		PrintLine( "WARNING: ", fmt, args );
		va_end( args );
	}
	void				PrintWarnings() override {}
	void				ClearWarnings( const char * ) override {}
	void				Error( VERIFY_FORMAT_STRING const char * fmt, ... ) override {
		va_list args;
		va_start( args, fmt );
		PrintLine( "ERROR: ", fmt, args );
		va_end( args );
		fflush( stdout );
		exit( 1 );
	}
	void				FatalError( VERIFY_FORMAT_STRING const char * fmt, ... ) override {
		va_list args;
		va_start( args, fmt );
		PrintLine( "FATAL ERROR: ", fmt, args );
		va_end( args );
		fflush( stdout );
		exit( 1 );
	}

	const char *		KeysFromBinding( const char * ) override { return ""; }
	const char *		BindingFromKey( const char * ) override { return ""; }
	int					ButtonState( int ) override { return 0; }
	int					KeyState( int ) override { return 0; }
	bool				IsMultiplayer() override { return false; }
	bool				IsServer() override { return false; }
	bool				IsClient() override { return false; }
	bool				GetConsoleUsed() override { return false; }
	int					GetSnapRate() override { return 0; }

	void				NetReceiveReliable( int, int, idBitMsg & ) override {}
	void				NetReceiveSnapshot( idSnapShot & ) override {}
	void				NetReceiveUsercmds( int, idBitMsg & ) override {}
	bool				ProcessEvent( const sysEvent_t * ) override { return false; }
	bool				LoadGame( const char * ) override { return false; }
	bool				SaveGame( const char * ) override { return false; }
	idDemoFile *		ReadDemo() override { return nullptr; }
	idDemoFile *		WriteDemo() override { return nullptr; }

	idGame *			Game() override { return nullptr; }
	idRenderWorld *		RW() override { return nullptr; }
	idSoundWorld *		SW() override { return nullptr; }
	idSoundWorld *		MenuSW() override { return nullptr; }
	idSession *			Session() override { return nullptr; }
	idCommonDialog &	Dialog() override { TestAbort( "idCommon::Dialog" ); }

	void				OnSaveCompleted( idSaveLoadParms & ) override {}
	void				OnLoadCompleted( idSaveLoadParms & ) override {}
	void				OnLoadFilesCompleted( idSaveLoadParms & ) override {}
	void				OnEnumerationCompleted( idSaveLoadParms & ) override {}
	void				OnDeleteCompleted( idSaveLoadParms & ) override {}
	void				TriggerScreenWipe( const char *, bool ) override {}
	void				OnStartHosting( idMatchParameters & ) override {}
	int					GetGameFrame() override { return 0; }
	void				LaunchExternalTitle( int, int, const lobbyConnectInfo_t * const ) override {}

	void				InitializeMPMapsModes() override {}
	const idStrList &	GetModeList() const override { return modes; }
	const idStrList &	GetModeDisplayList() const override { return modes; }
	const idList<mpMap_t> &	GetMapList() const override { return maps; }
	void				ResetPlayerInput( int ) override {}
	bool				JapaneseCensorship() const override { return false; }
	void				QueueShowShell() override {}
	currentGame_t		GetCurrentGame() const override { return DOOM3_BFG; }
	void				SwitchToGame( currentGame_t ) override {}

private:
	idStrList			modes;
	idList<mpMap_t>		maps;
};

idCommonTestStub	commonTestStub;

}	// namespace

idCommon *		common = &commonTestStub;
idCVarSystem *	cvarSystem = nullptr;
idFileSystem *	fileSystem = nullptr;

// cvars constructed before the cvar system exists link themselves into this list
idCVar *		idCVar::staticVars = nullptr;

int Sys_Milliseconds() {
	static const DWORD start = timeGetTime();
	return static_cast<int>( timeGetTime() - start );
}

uint64 Sys_Microseconds() {
	static LARGE_INTEGER frequency = {};
	if ( frequency.QuadPart == 0 ) {
		QueryPerformanceFrequency( &frequency );
	}
	LARGE_INTEGER counter;
	QueryPerformanceCounter( &counter );
	return static_cast<uint64>( counter.QuadPart * 1000000 / frequency.QuadPart );
}

void Sys_CPUCount( int & numLogicalCPUCores, int & numPhysicalCPUCores, int & numCPUPackages ) {
	SYSTEM_INFO info;
	GetSystemInfo( &info );
	numLogicalCPUCores = static_cast<int>( info.dwNumberOfProcessors );
	numPhysicalCPUCores = static_cast<int>( info.dwNumberOfProcessors );
	numCPUPackages = 1;
}
