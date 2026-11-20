// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

#include "../api/EditorEngineApi.h"

class QWidget;

/*
	The UI's side of the engine: it starts the engine inside the viewport widget, runs one engine
	frame per pass of the Qt event loop and turns the engine's callbacks into signals.

	  Qt event loop ── frameTimer ── Tick() ── engine->Frame() ──┬─ OnPrint()     → printed()
	                                    │                        └─ PumpEvents()  → processEvents()
	                                    └─ quit asked: engine->Shutdown(), the process ends
*/
class EngineClient final : public QObject, public idEditorHost {
	Q_OBJECT

public:
	EngineClient( idEditorEngine * engine, QWidget * viewport, QObject * parent = nullptr );

	// starts the engine; the viewport's window must be shown
	void				Start();
	void				RequestQuit() { quitRequested = true; }
	void				Execute( const QString & command );
	[[nodiscard]] idEditorEngine *	Engine() const { return engine; }

	// idEditorHost
	void *				ViewportParent() override;
	void				GetViewportSize( int & width, int & height ) override;
	void				PumpEvents() override;
	void				OnQuitRequested() override { quitRequested = true; }
	void				OnFatalError( const char * text ) override;
	void				OnPrint( const char * text ) override;
	void				OnMapChanged() override { emit mapChanged(); }
	void				OnEntitiesChanged() override { emit entitiesChanged(); }
	void				OnSelectionChanged() override { emit selectionChanged(); }
	void				OnDocumentChanged() override { emit documentChanged(); }
	void				OnPreviewReady( int id, int width, int height, const unsigned char * rgb ) override;

signals:
	void				printed( const QString & text );
	void				mapChanged();
	void				entitiesChanged();
	void				selectionChanged();
	void				documentChanged();
	// a null image: the engine could not draw the picture
	void				previewReady( int id, const QImage & image );
	// the last thing before the engine and the process end
	void				aboutToShutdown();

private:
	void				Tick();
	void				FlushPrints();

	idEditorEngine *	engine;
	QWidget *			viewport;
	QTimer				frameTimer;
	QElapsedTimer		sincePump;
	QString				pendingPrints;
	bool				quitRequested = false;
	bool				inPump = false;
};
