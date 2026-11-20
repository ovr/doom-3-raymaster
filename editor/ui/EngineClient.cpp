// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "EngineClient.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QWidget>

namespace {

// inside a frame the windows are served this often: a normal frame is shorter and skips it
constexpr qint64 PUMP_INTERVAL_MSEC = 30;

}	// namespace

EngineClient::EngineClient( idEditorEngine * engine_, QWidget * viewport_, QObject * parent )
	: QObject( parent ), engine( engine_ ), viewport( viewport_ ) {
	// interval 0: one frame per pass of the event loop, the engine's own pacing limits the rate
	frameTimer.setInterval( 0 );
	connect( &frameTimer, &QTimer::timeout, this, &EngineClient::Tick );
}

void EngineClient::Start() {
	sincePump.start();
	engine->Init( this );
	FlushPrints();
	frameTimer.start();
}

void EngineClient::Execute( const QString & command ) {
	engine->ExecuteCommand( command.toLocal8Bit().constData() );
}

void EngineClient::Tick() {
	// the events dispatched inside a frame come here again
	if ( inPump ) {
		return;
	}
	if ( quitRequested ) {
		frameTimer.stop();
		emit aboutToShutdown();
		engine->Shutdown();
		return;
	}
	sincePump.restart();
	engine->Frame();
	FlushPrints();
}

void EngineClient::FlushPrints() {
	if ( !pendingPrints.isEmpty() ) {
		emit printed( pendingPrints );
		pendingPrints.clear();
	}
}

void * EngineClient::ViewportParent() {
	return reinterpret_cast<void *>( viewport->winId() );
}

void EngineClient::GetViewportSize( int & width, int & height ) {
	const qreal scale = viewport->devicePixelRatioF();
	width = qRound( viewport->width() * scale );
	height = qRound( viewport->height() * scale );
}

void EngineClient::OnPreviewReady( int id, int width, int height, const unsigned char * rgb ) {
	if ( rgb == nullptr || width <= 0 ) {
		emit previewReady( id, QImage() );
		return;
	}
	// a deep copy: the pixels are the engine's
	emit previewReady( id, QImage( rgb, width, height, width * 3, QImage::Format_RGB888 ).copy() );
}

void EngineClient::OnBuildFinished( const editorBuildResult_t & result ) {
	BuildResult copy;
	copy.status = result.status;
	copy.error = QString::fromLocal8Bit( result.error );
	copy.areas = result.areas;
	copy.portals = result.portals;
	copy.triangles = result.triangles;
	copy.aasFiles = result.aasFiles;
	copy.aasAreas = result.aasAreas;
	copy.leakPoints = result.leakPoints;
	copy.seconds = result.seconds;
	emit buildFinished( copy );
}

void EngineClient::PumpEvents() {
	if ( inPump || sincePump.elapsed() < PUMP_INTERVAL_MSEC ) {
		return;
	}
	sincePump.restart();
	inPump = true;
	FlushPrints();
	QCoreApplication::processEvents();
	inPump = false;
}

void EngineClient::OnFatalError( const char * text ) {
	frameTimer.stop();
	inPump = true;
	FlushPrints();
	QMessageBox::critical( viewport->window(), tr( "Doom 3 Editor: fatal error" ), QString::fromLocal8Bit( text ) );
}

void EngineClient::OnPrint( const char * text ) {
	pendingPrints += QString::fromLocal8Bit( text );
}
