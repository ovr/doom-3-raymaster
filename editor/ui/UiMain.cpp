// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPalette>
#include <QStyleFactory>

#include "../api/EditorEngineApi.h"
#include "MainWindow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// Qt looks for its plugins next to the exe, but they are deployed next to this module
[[nodiscard]] QString ModuleDirectory() {
	HMODULE module = nullptr;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>( &ModuleDirectory ), &module );
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW( module, path, MAX_PATH );
	return QFileInfo( QString::fromWCharArray( path ) ).absolutePath();
}

void ApplyDarkTheme( QApplication & app ) {
	app.setStyle( QStyleFactory::create( QStringLiteral( "Fusion" ) ) );

	const QColor window( 37, 37, 38 );
	const QColor base( 30, 30, 30 );
	const QColor raised( 51, 51, 55 );
	const QColor text( 220, 220, 220 );
	const QColor dimmed( 128, 128, 128 );
	const QColor accent( 14, 99, 156 );

	QPalette palette;
	palette.setColor( QPalette::Window, window );
	palette.setColor( QPalette::WindowText, text );
	palette.setColor( QPalette::Base, base );
	palette.setColor( QPalette::AlternateBase, window );
	palette.setColor( QPalette::ToolTipBase, raised );
	palette.setColor( QPalette::ToolTipText, text );
	palette.setColor( QPalette::Text, text );
	palette.setColor( QPalette::PlaceholderText, dimmed );
	palette.setColor( QPalette::Button, raised );
	palette.setColor( QPalette::ButtonText, text );
	palette.setColor( QPalette::BrightText, Qt::white );
	palette.setColor( QPalette::Link, QColor( 86, 156, 214 ) );
	palette.setColor( QPalette::Highlight, accent );
	palette.setColor( QPalette::HighlightedText, Qt::white );
	palette.setColor( QPalette::Light, raised.lighter( 130 ) );
	palette.setColor( QPalette::Midlight, raised.lighter( 115 ) );
	palette.setColor( QPalette::Mid, raised.darker( 130 ) );
	palette.setColor( QPalette::Dark, base.darker( 130 ) );
	palette.setColor( QPalette::Shadow, Qt::black );
	for ( const QPalette::ColorRole role : { QPalette::WindowText, QPalette::Text, QPalette::ButtonText } ) {
		palette.setColor( QPalette::Disabled, role, dimmed );
	}
	app.setPalette( palette );
}

}	// namespace

extern "C" __declspec( dllexport ) int EditorUiMain( idEditorEngine * engine, int argc, char ** argv ) {
	if ( engine == nullptr || engine->ApiVersion() != EDITOR_API_VERSION ) {
		return 2;
	}

	QCoreApplication::addLibraryPath( ModuleDirectory() );
	QApplication app( argc, argv );
	QApplication::setApplicationName( QStringLiteral( "Doom3Editor" ) );
	QApplication::setOrganizationName( QStringLiteral( "Doom3Raymaster" ) );
	ApplyDarkTheme( app );

	MainWindow window( engine );
	window.Start();

	// the engine ends the process when the window closes, so exec() returns only on a failure
	return ( QApplication::exec() == 0 ) ? 1 : 3;
}
