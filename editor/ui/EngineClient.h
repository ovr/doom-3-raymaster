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

// how a build ended: editorBuildResult_t with a text of its own
struct BuildResult {
	int					status = 0;			// editorBuildStatus_t
	QString				error;
	int					areas = 0;
	int					portals = 0;
	int					triangles = 0;
	int					aasFiles = 0;
	int					aasAreas = 0;
	int					leakPoints = 0;
	float				seconds = 0.0f;
};

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
	void				OnBuildProgress( const char * stage, float fraction ) override { emit buildProgress( QString::fromLocal8Bit( stage ), fraction ); }
	void				OnBuildFinished( const editorBuildResult_t & result ) override;
	void				OnStatus( const char * text ) override { emit statusText( QString::fromLocal8Bit( text ) ); }
	void				OnEditMaterial( const char * name ) override { emit editMaterialRequested( QString::fromLocal8Bit( name ) ); }

signals:
	void				printed( const QString & text );
	void				mapChanged();
	void				entitiesChanged();
	void				selectionChanged();
	void				documentChanged();
	void				editMaterialRequested( const QString & name );
	// inside a build, which blocks the frame: these arrive from the engine's pumping
	void				buildProgress( const QString & stage, float fraction );
	void				buildFinished( const BuildResult & result );
	void				statusText( const QString & text );
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
