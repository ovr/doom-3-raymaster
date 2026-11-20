// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QMainWindow>

class ContentBrowser;
class EngineClient;
class Inspector;
class MaterialEditor;
class Outliner;
class Palette;
class OutputLog;
class OrthoView;
class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QProgressBar;
class QSpinBox;
class UiScript;
struct BuildResult;
class ViewportHost;
class idEditorEngine;

// The editor's window: the engine viewport in the middle, the panels docked around it.
class MainWindow final : public QMainWindow {
	Q_OBJECT

public:
	explicit MainWindow( idEditorEngine * engine, QWidget * parent = nullptr );

	// shows the window and starts the engine in it; a script file makes it a test run (UiScript.h)
	void				Start( const QString & scriptFile = QString() );

protected:
	void				closeEvent( QCloseEvent * event ) override;

private:
	void				CreateMenus();
	void				RestoreLayout();
	void				SaveLayout();
	void				ShowAbout();
	void				ChooseMap();
	void				NewMap();
	void				Deselect();
	void				Hollow();
	void				MakeEntity();
	void				ApplyMaterial( const QString & name );
	void				BuildProgress( const QString & stage, float fraction );
	void				BuildFinished( const BuildResult & result );
	void				CreateStatusBar();
	void				CreateOrthoDock();
	void				MapChanged();
	void				DocumentChanged();
	[[nodiscard]] bool	ConfirmDiscard();
	void				AssetActivated( int kind, const QString & name );
	void				EditMaterial( const QString & name );

	idEditorEngine *	engine;
	ViewportHost *		viewport;
	OutputLog *			outputLog;
	QDockWidget *		outputDock;
	Outliner *			outliner;
	QDockWidget *		outlinerDock;
	QAction *			closeMapAction = nullptr;
	QAction *			saveAction = nullptr;
	QAction *			undoAction = nullptr;
	QAction *			redoAction = nullptr;
	Inspector *			inspector = nullptr;
	QDockWidget *		inspectorDock = nullptr;
	QDockWidget *		paletteDock = nullptr;
	Palette *			palette = nullptr;
	ContentBrowser *	contentBrowser = nullptr;
	QDockWidget *		contentDock = nullptr;
	MaterialEditor *	materialEditor = nullptr;
	QDockWidget *		materialDock = nullptr;
	QAction *			playAction = nullptr;
	QAction *			stopAction = nullptr;
	bool				wasPlaying = false;
	QAction *			newMapAction = nullptr;
	QAction *			openMapAction = nullptr;
	QAction *			selectToolAction = nullptr;
	QAction *			boxToolAction = nullptr;
	QAction *			buildAction = nullptr;
	QAction *			buildFastAction = nullptr;
	QAction *			buildCancelAction = nullptr;
	QAction *			showLeakAction = nullptr;
	QAction *			goToLeakAction = nullptr;
	QList<QAction *>	editActions;			// off while the game plays or a build runs
	QList<QAction *>	brushActions;
	QComboBox *			gridCombo = nullptr;
	QComboBox *			angleCombo = nullptr;
	QAction *			showGridAction = nullptr;
	QSpinBox *			boxHeightSpin = nullptr;
	QLabel *			docStateLabel = nullptr;
	QLabel *			buildResultLabel = nullptr;
	QLabel *			buildStageLabel = nullptr;
	QProgressBar *		buildProgressBar = nullptr;
	OrthoView *			orthoView = nullptr;
	QDockWidget *		orthoDock = nullptr;
	UiScript *			script = nullptr;
	bool				closing = false;
	EngineClient *		client;
};
