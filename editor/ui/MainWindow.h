// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QMainWindow>

class EngineClient;
class OutputLog;
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

	idEditorEngine *	engine;
	ViewportHost *		viewport;
	OutputLog *			outputLog;
	QDockWidget *		outputDock;
	EngineClient *		client;
};
