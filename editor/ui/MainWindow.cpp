// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>

#include "../api/EditorEngineApi.h"
#include "EngineClient.h"
#include "OutputLog.h"
#include "ViewportHost.h"

namespace {

// bump when the set of docks changes in a way an old saved layout would break
constexpr int LAYOUT_VERSION = 1;

[[nodiscard]] QSettings Settings() {
	return QSettings( QSettings::IniFormat, QSettings::UserScope, QStringLiteral( "Doom3Raymaster" ), QStringLiteral( "Doom3Editor" ) );
}

}	// namespace

MainWindow::MainWindow( idEditorEngine * engine_, QWidget * parent ) : QMainWindow( parent ), engine( engine_ ) {
	setWindowTitle( tr( "Doom 3 Editor" ) );
	setDockNestingEnabled( true );
	setCorner( Qt::BottomLeftCorner, Qt::LeftDockWidgetArea );
	setCorner( Qt::BottomRightCorner, Qt::RightDockWidgetArea );

	viewport = new ViewportHost( this );
	setCentralWidget( viewport );

	outputLog = new OutputLog( this );
	outputDock = new QDockWidget( tr( "Output Log" ), this );
	outputDock->setObjectName( QStringLiteral( "OutputLogDock" ) );
	outputDock->setWidget( outputLog );
	addDockWidget( Qt::BottomDockWidgetArea, outputDock );

	client = new EngineClient( engine, viewport, this );
	connect( client, &EngineClient::printed, outputLog, &OutputLog::Append );
	connect( client, &EngineClient::aboutToShutdown, this, &MainWindow::SaveLayout );
	connect( outputLog, &OutputLog::commandEntered, client, &EngineClient::Execute );

	CreateMenus();
	statusBar()->showMessage( tr( "Click the viewport to play in it. Shift+Esc gives the mouse and the keys back." ) );

	resize( 1600, 900 );
	RestoreLayout();
}

void MainWindow::CreateMenus() {
	QMenu * const fileMenu = menuBar()->addMenu( tr( "&File" ) );
	QAction * const exitAction = fileMenu->addAction( tr( "E&xit" ) );
	exitAction->setShortcut( QKeySequence( Qt::ALT | Qt::Key_F4 ) );
	connect( exitAction, &QAction::triggered, this, &QWidget::close );

	QMenu * const viewMenu = menuBar()->addMenu( tr( "&View" ) );
	viewMenu->addAction( outputDock->toggleViewAction() );
	viewMenu->addSeparator();
	QAction * const consoleAction = viewMenu->addAction( tr( "&Console Command" ) );
	consoleAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_QuoteLeft ) );
	connect( consoleAction, &QAction::triggered, this, [this] {
		outputDock->show();
		outputDock->raise();
		outputLog->FocusCommandLine();
	} );
	QAction * const focusAction = viewMenu->addAction( tr( "&Play in Viewport" ) );
	focusAction->setShortcut( QKeySequence( Qt::Key_F8 ) );
	connect( focusAction, &QAction::triggered, this, [this] { engine->FocusViewport(); } );

	QMenu * const helpMenu = menuBar()->addMenu( tr( "&Help" ) );
	connect( helpMenu->addAction( tr( "&About" ) ), &QAction::triggered, this, &MainWindow::ShowAbout );
	connect( helpMenu->addAction( tr( "About &Qt" ) ), &QAction::triggered, qApp, &QApplication::aboutQt );
}

void MainWindow::Start() {
	show();
	client->Start();
	viewport->Attach( engine );
}

void MainWindow::closeEvent( QCloseEvent * event ) {
	// the engine goes first, while its window still exists; the process ends there
	client->RequestQuit();
	event->ignore();
}

void MainWindow::RestoreLayout() {
	QSettings settings = Settings();
	if ( settings.value( "window/layoutVersion" ).toInt() != LAYOUT_VERSION ) {
		return;
	}
	restoreGeometry( settings.value( "window/geometry" ).toByteArray() );
	restoreState( settings.value( "window/state" ).toByteArray(), LAYOUT_VERSION );
}

void MainWindow::SaveLayout() {
	QSettings settings = Settings();
	settings.setValue( "window/layoutVersion", LAYOUT_VERSION );
	settings.setValue( "window/geometry", saveGeometry() );
	settings.setValue( "window/state", saveState( LAYOUT_VERSION ) );
	settings.sync();
}

void MainWindow::ShowAbout() {
	QMessageBox::about( this, tr( "About Doom 3 Editor" ), tr(
		"<b>Doom 3 Editor</b><br>"
		"The level and material editor of the DOOM 3 BFG Edition port.<br><br>"
		"The engine and the editor are free software under the GNU General Public License, version 3 or later.<br>"
		"The user interface uses Qt %1 under the GNU Lesser General Public License, version 3." ).arg( QString::fromLatin1( qVersion() ) ) );
}
