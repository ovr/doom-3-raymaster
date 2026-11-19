// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QLabel>
#include <QToolBar>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QVBoxLayout>

#include "../api/EditorEngineApi.h"
#include "EngineClient.h"
#include "Inspector.h"
#include "Outliner.h"
#include "Palette.h"
#include "OutputLog.h"
#include "ViewportHost.h"

namespace {

// bump when the set of docks changes in a way an old saved layout would break
constexpr int LAYOUT_VERSION = 4;
constexpr float GRID_SIZES[] = { 0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
constexpr int DEFAULT_GRID = 4;		// index into GRID_SIZES
constexpr int MAP_NAME_ROLE = Qt::UserRole;

[[nodiscard]] QSettings Settings() {
	return QSettings( QSettings::IniFormat, QSettings::UserScope, QStringLiteral( "Doom3Raymaster" ), QStringLiteral( "Doom3Editor" ) );
}

// the maps the game data declares; "" when the dialog was cancelled
[[nodiscard]] QString AskForMap( QWidget * parent, idEditorEngine * engine ) {
	QDialog dialog( parent );
	dialog.setWindowTitle( QObject::tr( "Open Map" ) );
	dialog.resize( 560, 640 );

	auto * const filter = new QLineEdit( &dialog );
	filter->setPlaceholderText( QObject::tr( "Filter" ) );
	filter->setClearButtonEnabled( true );
	auto * const list = new QListWidget( &dialog );
	auto * const buttons = new QDialogButtonBox( QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog );

	auto * const layout = new QVBoxLayout( &dialog );
	layout->addWidget( filter );
	layout->addWidget( list );
	layout->addWidget( buttons );

	engine->EnumMaps( []( void * context, const editorMapInfo_t & map ) {
		const QString name = QString::fromLocal8Bit( map.name );
		const QString title = QString::fromLocal8Bit( map.title );
		auto * const item = new QListWidgetItem( title.isEmpty() || title == name ? name : QStringLiteral( "%1  (%2)" ).arg( name, title ) );
		item->setData( MAP_NAME_ROLE, name );
		static_cast<QListWidget *>( context )->addItem( item );
	}, list );
	list->sortItems();
	list->setCurrentRow( 0 );

	QObject::connect( filter, &QLineEdit::textChanged, list, [list]( const QString & text ) {
		for ( int i = 0; i < list->count(); i++ ) {
			list->item( i )->setHidden( !list->item( i )->text().contains( text, Qt::CaseInsensitive ) );
		}
	} );
	QObject::connect( list, &QListWidget::itemActivated, &dialog, &QDialog::accept );
	QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );

	if ( dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr || list->currentItem()->isHidden() ) {
		return QString();
	}
	return list->currentItem()->data( MAP_NAME_ROLE ).toString();
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

	outliner = new Outliner( engine, this );
	outlinerDock = new QDockWidget( tr( "Outliner" ), this );
	outlinerDock->setObjectName( QStringLiteral( "OutlinerDock" ) );
	outlinerDock->setWidget( outliner );
	addDockWidget( Qt::RightDockWidgetArea, outlinerDock );

	inspector = new Inspector( engine, this );
	inspectorDock = new QDockWidget( tr( "Inspector" ), this );
	inspectorDock->setObjectName( QStringLiteral( "InspectorDock" ) );
	inspectorDock->setWidget( inspector );
	addDockWidget( Qt::RightDockWidgetArea, inspectorDock );

	palette = new Palette( engine, this );
	paletteDock = new QDockWidget( tr( "Entity Classes" ), this );
	paletteDock->setObjectName( QStringLiteral( "PaletteDock" ) );
	paletteDock->setWidget( palette );
	addDockWidget( Qt::LeftDockWidgetArea, paletteDock );

	client = new EngineClient( engine, viewport, this );
	connect( client, &EngineClient::mapChanged, this, &MainWindow::MapChanged );
	connect( client, &EngineClient::entitiesChanged, outliner, &Outliner::Reload );
	connect( client, &EngineClient::selectionChanged, outliner, &Outliner::SyncSelection );
	// queued: an edit made in the inspector must not rebuild its list under its own handler
	connect( client, &EngineClient::selectionChanged, inspector, &Inspector::Refresh, Qt::QueuedConnection );
	connect( client, &EngineClient::documentChanged, inspector, &Inspector::Refresh, Qt::QueuedConnection );
	connect( client, &EngineClient::documentChanged, this, &MainWindow::DocumentChanged );
	connect( client, &EngineClient::printed, outputLog, &OutputLog::Append );
	connect( client, &EngineClient::aboutToShutdown, this, &MainWindow::SaveLayout );
	connect( outputLog, &OutputLog::commandEntered, client, &EngineClient::Execute );

	CreateMenus();
	MapChanged();

	resize( 1600, 900 );
	RestoreLayout();
}

void MainWindow::CreateMenus() {
	QMenu * const fileMenu = menuBar()->addMenu( tr( "&File" ) );
	QAction * const openMapAction = fileMenu->addAction( tr( "&Open Map..." ) );
	openMapAction->setShortcut( QKeySequence::Open );
	connect( openMapAction, &QAction::triggered, this, &MainWindow::ChooseMap );
	saveAction = fileMenu->addAction( tr( "&Save Map" ) );
	saveAction->setShortcut( QKeySequence::Save );
	connect( saveAction, &QAction::triggered, this, [this] { engine->SaveMap(); } );
	closeMapAction = fileMenu->addAction( tr( "&Close Map" ) );
	connect( closeMapAction, &QAction::triggered, this, [this] {
		if ( ConfirmDiscard() ) {
			engine->CloseMap();
		}
	} );
	fileMenu->addSeparator();
	QAction * const exitAction = fileMenu->addAction( tr( "E&xit" ) );
	exitAction->setShortcut( QKeySequence( Qt::ALT | Qt::Key_F4 ) );
	connect( exitAction, &QAction::triggered, this, &QWidget::close );

	QMenu * const editMenu = menuBar()->addMenu( tr( "&Edit" ) );
	undoAction = editMenu->addAction( tr( "&Undo" ) );
	undoAction->setShortcut( QKeySequence::Undo );
	connect( undoAction, &QAction::triggered, this, [this] { engine->Undo(); } );
	redoAction = editMenu->addAction( tr( "&Redo" ) );
	redoAction->setShortcuts( { QKeySequence( Qt::CTRL | Qt::Key_Y ), QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Z ) } );
	connect( redoAction, &QAction::triggered, this, [this] { engine->Redo(); } );
	editMenu->addSeparator();
	QAction * const duplicateAction = editMenu->addAction( tr( "D&uplicate" ) );
	duplicateAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_D ) );
	connect( duplicateAction, &QAction::triggered, this, [this] { engine->DuplicateSelection(); } );
	QAction * const deleteAction = editMenu->addAction( tr( "De&lete" ) );
	deleteAction->setShortcut( QKeySequence::Delete );
	connect( deleteAction, &QAction::triggered, this, [this] { engine->DeleteSelection(); } );
	QAction * const deselectAction = editMenu->addAction( tr( "&Deselect All" ) );
	deselectAction->setShortcut( QKeySequence( Qt::Key_Escape ) );
	connect( deselectAction, &QAction::triggered, this, [this] { engine->SelectEntity( 0, false ); } );

	// the gizmo and the grid, as in other editors: W moves, E rotates
	QToolBar * const tools = addToolBar( tr( "Tools" ) );
	tools->setObjectName( QStringLiteral( "ToolsBar" ) );
	auto * const gizmoGroup = new QActionGroup( this );
	QAction * const moveAction = tools->addAction( tr( "Move" ) );
	moveAction->setShortcut( QKeySequence( Qt::Key_W ) );
	moveAction->setToolTip( tr( "Move the selection along an axis (W)" ) );
	QAction * const rotateAction = tools->addAction( tr( "Rotate" ) );
	rotateAction->setShortcut( QKeySequence( Qt::Key_E ) );
	rotateAction->setToolTip( tr( "Turn the selection around an axis, in steps of 15 degrees (E)" ) );
	for ( QAction * const action : { moveAction, rotateAction } ) {
		action->setCheckable( true );
		gizmoGroup->addAction( action );
	}
	moveAction->setChecked( true );
	connect( moveAction, &QAction::triggered, this, [this] { engine->SetGizmo( EDITOR_GIZMO_MOVE ); } );
	connect( rotateAction, &QAction::triggered, this, [this] { engine->SetGizmo( EDITOR_GIZMO_ROTATE ); } );
	editMenu->addSeparator();
	editMenu->addAction( moveAction );
	editMenu->addAction( rotateAction );

	tools->addSeparator();
	tools->addWidget( new QLabel( tr( " Grid " ), tools ) );
	auto * const grid = new QComboBox( tools );
	for ( const float size : GRID_SIZES ) {
		grid->addItem( ( size == 0.0f ) ? tr( "Off" ) : QString::number( size ), size );
	}
	grid->setCurrentIndex( DEFAULT_GRID );
	grid->setToolTip( tr( "The step of a move, in map units" ) );
	connect( grid, &QComboBox::currentIndexChanged, this, [this, grid] { engine->SetGridSize( grid->currentData().toFloat() ); } );
	tools->addWidget( grid );

	QMenu * const viewMenu = menuBar()->addMenu( tr( "&View" ) );
	viewMenu->addAction( tools->toggleViewAction() );
	viewMenu->addAction( outlinerDock->toggleViewAction() );
	viewMenu->addAction( inspectorDock->toggleViewAction() );
	viewMenu->addAction( paletteDock->toggleViewAction() );
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
	event->ignore();
	if ( closing || ConfirmDiscard() ) {
		closing = true;
		client->RequestQuit();
	}
}

bool MainWindow::ConfirmDiscard() {
	editorDocumentState_t state = {};
	engine->GetDocumentState( state );
	if ( !state.dirty ) {
		return true;
	}
	const QString map = QString::fromLocal8Bit( engine->CurrentMap() );
	return QMessageBox::question( this, tr( "Unsaved Changes" ), tr( "%1 has changes that are not saved. Discard them?" ).arg( map ),
		QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel ) == QMessageBox::Discard;
}

void MainWindow::DocumentChanged() {
	editorDocumentState_t state = {};
	engine->GetDocumentState( state );
	const QString map = QString::fromLocal8Bit( engine->CurrentMap() );
	const QString undoLabel = QString::fromLocal8Bit( state.undoLabel );
	const QString redoLabel = QString::fromLocal8Bit( state.redoLabel );

	setWindowTitle( map.isEmpty() ? tr( "Doom 3 Editor" ) : tr( "%1%2 - Doom 3 Editor" ).arg( map, state.dirty ? QStringLiteral( "*" ) : QString() ) );
	saveAction->setEnabled( !map.isEmpty() );
	undoAction->setEnabled( state.canUndo );
	undoAction->setText( undoLabel.isEmpty() ? tr( "&Undo" ) : tr( "&Undo %1" ).arg( undoLabel ) );
	redoAction->setEnabled( state.canRedo );
	redoAction->setText( redoLabel.isEmpty() ? tr( "&Redo" ) : tr( "&Redo %1" ).arg( redoLabel ) );
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

void MainWindow::ChooseMap() {
	if ( !ConfirmDiscard() ) {
		return;
	}
	const QString name = AskForMap( this, engine );
	if ( !name.isEmpty() ) {
		engine->OpenMap( name.toLocal8Bit().constData() );
	}
}

void MainWindow::MapChanged() {
	const QString map = QString::fromLocal8Bit( engine->CurrentMap() );
	closeMapAction->setEnabled( !map.isEmpty() );
	outliner->Reload();
	if ( !map.isEmpty() ) {
		palette->Load();
	}
	inspector->Refresh();
	DocumentChanged();
	statusBar()->showMessage( map.isEmpty()
		? tr( "File > Open Map starts editing. Without a map: click the viewport to play in it, Shift+Esc gives the mouse and the keys back." )
		: tr( "Click selects (Ctrl adds), drag the gizmo to move or turn. Hold the right mouse button to look; W A S D fly, Q / E down and up, Shift faster." ) );
}

void MainWindow::ShowAbout() {
	QMessageBox::about( this, tr( "About Doom 3 Editor" ), tr(
		"<b>Doom 3 Editor</b><br>"
		"The level and material editor of the DOOM 3 BFG Edition port.<br><br>"
		"The engine and the editor are free software under the GNU General Public License, version 3 or later.<br>"
		"The user interface uses Qt %1 under the GNU Lesser General Public License, version 3." ).arg( QString::fromLatin1( qVersion() ) ) );
}
