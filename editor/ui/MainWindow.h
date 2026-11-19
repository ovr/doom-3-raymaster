// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QMainWindow>

class EngineClient;
class Outliner;
class OutputLog;
class QAction;
class QDockWidget;
class ViewportHost;
class idEditorEngine;

// The editor's window: the engine viewport in the middle, the panels docked around it.
class MainWindow final : public QMainWindow {
	Q_OBJECT

public:
	explicit MainWindow( idEditorEngine * engine, QWidget * parent = nullptr );

	// shows the window and starts the engine in it
	void				Start();

protected:
	void				closeEvent( QCloseEvent * event ) override;

private:
	void				CreateMenus();
	void				RestoreLayout();
	void				SaveLayout();
	void				ShowAbout();
	void				ChooseMap();
	void				MapChanged();
	void				DocumentChanged();
	[[nodiscard]] bool	ConfirmDiscard();

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
	bool				closing = false;
	EngineClient *		client;
};
