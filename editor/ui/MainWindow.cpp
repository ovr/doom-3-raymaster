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
#include <QHBoxLayout>
#include <QInputDialog>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QVBoxLayout>

#include "../api/EditorEngineApi.h"
#include "ContentBrowser.h"
#include "EngineClient.h"
#include "Inspector.h"
#include "MaterialEditor.h"
#include "OrthoView.h"
#include "UiScript.h"
#include "Outliner.h"
#include "Palette.h"
#include "OutputLog.h"
#include "ViewportHost.h"

namespace {

// bump when the set of docks changes in a way an old saved layout would break
constexpr int LAYOUT_VERSION = 8;
constexpr int ORTHO_DOCK_WIDTH = 420;
constexpr float GRID_SIZES[] = { 0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f };
constexpr float ANGLE_SNAPS[] = { 1.0f, 5.0f, 15.0f, 45.0f, 90.0f };
constexpr int DEFAULT_ANGLE_SNAP = 2;
constexpr int STATUS_MESSAGE_MSEC = 6000;
constexpr int BOX_HEIGHT_MAX = 4096;
constexpr float HOLLOW_DEFAULT = 8.0f;			// the wall's thickness when the grid is off
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

	contentBrowser = new ContentBrowser( client, this );
	contentDock = new QDockWidget( tr( "Content Browser" ), this );
	contentDock->setObjectName( QStringLiteral( "ContentBrowserDock" ) );
	contentDock->setWidget( contentBrowser );
	addDockWidget( Qt::BottomDockWidgetArea, contentDock );
	tabifyDockWidget( outputDock, contentDock );
	contentDock->raise();
	connect( contentBrowser, &ContentBrowser::activated, this, &MainWindow::AssetActivated );

	materialEditor = new MaterialEditor( client, contentBrowser->Thumbnails(), this );
	materialDock = new QDockWidget( tr( "Material Editor" ), this );
	materialDock->setObjectName( QStringLiteral( "MaterialEditorDock" ) );
	materialDock->setWidget( materialEditor );
	addDockWidget( Qt::RightDockWidgetArea, materialDock );
	tabifyDockWidget( inspectorDock, materialDock );
	inspectorDock->raise();
	connect( client, &EngineClient::editMaterialRequested, this, &MainWindow::EditMaterial );
	CreateOrthoDock();
	CreateStatusBar();
	connect( client, &EngineClient::selectionChanged, orthoView, &OrthoView::Reload );
	connect( client, &EngineClient::entitiesChanged, orthoView, &OrthoView::Reload );
	connect( client, &EngineClient::buildProgress, this, &MainWindow::BuildProgress );
	connect( client, &EngineClient::buildFinished, this, &MainWindow::BuildFinished );
	connect( client, &EngineClient::statusText, this, [this]( const QString & text ) { statusBar()->showMessage( text, STATUS_MESSAGE_MSEC ); } );
	connect( orthoView, &OrthoView::statusText, this, [this]( const QString & text ) { statusBar()->showMessage( text, STATUS_MESSAGE_MSEC ); } );
	connect( contentBrowser, &ContentBrowser::applyMaterialRequested, this, &MainWindow::ApplyMaterial );
	connect( inspector, &Inspector::materialPickRequested, this, [this] {
		const QString name = contentBrowser->CurrentMaterial();
		if ( name.isEmpty() ) {
			statusBar()->showMessage( tr( "Select a material in the Content Browser first." ), STATUS_MESSAGE_MSEC );
		} else {
			ApplyMaterial( name );
		}
	} );
	connect( client, &EngineClient::mapChanged, this, &MainWindow::MapChanged );
	connect( client, &EngineClient::entitiesChanged, outliner, &Outliner::Reload );
	connect( client, &EngineClient::selectionChanged, outliner, &Outliner::SyncSelection );
	// queued: an edit made in the inspector must not rebuild its list under its own handler
	connect( client, &EngineClient::selectionChanged, inspector, &Inspector::Refresh, Qt::QueuedConnection );
	connect( client, &EngineClient::documentChanged, inspector, &Inspector::Refresh, Qt::QueuedConnection );
	connect( client, &EngineClient::documentChanged, this, &MainWindow::DocumentChanged );
	connect( client, &EngineClient::documentChanged, outliner, &Outliner::UpdateBrushCounts );
	connect( client, &EngineClient::printed, outputLog, &OutputLog::Append );
	connect( client, &EngineClient::aboutToShutdown, this, &MainWindow::SaveLayout );
	connect( outputLog, &OutputLog::commandEntered, client, &EngineClient::Execute );

	CreateMenus();
	MapChanged();

	resize( 1600, 900 );
	RestoreLayout();
}

void MainWindow::CreateOrthoDock() {
	orthoView = new OrthoView( engine, this );
	orthoView->setObjectName( QStringLiteral( "orthoView" ) );
	auto * const kind = new QComboBox( this );
	kind->setObjectName( QStringLiteral( "orthoKind" ) );
	kind->addItems( { tr( "Top (XY)" ), tr( "Front (XZ)" ), tr( "Side (YZ)" ) } );
	connect( kind, &QComboBox::currentIndexChanged, orthoView, &OrthoView::SetKind );
	auto * const centerButton = new QPushButton( tr( "To Camera" ), this );
	centerButton->setObjectName( QStringLiteral( "orthoCenter" ) );
	centerButton->setToolTip( tr( "Puts the middle of the 2D view where the camera is" ) );
	connect( centerButton, &QPushButton::clicked, orthoView, &OrthoView::CenterOnCamera );

	auto * const top = new QHBoxLayout;
	top->setContentsMargins( 0, 0, 0, 0 );
	top->addWidget( kind );
	top->addWidget( centerButton );
	top->addStretch( 1 );
	auto * const panel = new QWidget( this );
	auto * const layout = new QVBoxLayout( panel );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addLayout( top );
	layout->addWidget( orthoView, 1 );

	orthoDock = new QDockWidget( tr( "2D View" ), this );
	orthoDock->setObjectName( QStringLiteral( "OrthoViewDock" ) );
	orthoDock->setWidget( panel );
	addDockWidget( Qt::LeftDockWidgetArea, orthoDock );
	// the 2D view gets most of the left side
	resizeDocks( { paletteDock, orthoDock }, { 1, 2 }, Qt::Vertical );
	resizeDocks( { orthoDock }, { ORTHO_DOCK_WIDTH }, Qt::Horizontal );
}

void MainWindow::CreateStatusBar() {
	buildStageLabel = new QLabel( this );
	buildStageLabel->setObjectName( QStringLiteral( "buildStage" ) );
	buildProgressBar = new QProgressBar( this );
	buildProgressBar->setObjectName( QStringLiteral( "buildProgress" ) );
	buildProgressBar->setRange( 0, 100 );
	buildProgressBar->setMaximumWidth( 180 );
	buildProgressBar->setTextVisible( false );
	buildResultLabel = new QLabel( this );
	buildResultLabel->setObjectName( QStringLiteral( "buildResult" ) );
	docStateLabel = new QLabel( this );
	docStateLabel->setObjectName( QStringLiteral( "docState" ) );
	docStateLabel->setToolTip( tr( "Compiled: the viewport shows the world the game loads. Uncompiled: it shows the brushes as they are; Build (F7) compiles them." ) );
	for ( QWidget * const widget : std::initializer_list<QWidget *>{ buildStageLabel, buildProgressBar, buildResultLabel, docStateLabel } ) {
		statusBar()->addPermanentWidget( widget );
	}
	buildStageLabel->hide();
	buildProgressBar->hide();
}

void MainWindow::CreateMenus() {
	QMenu * const fileMenu = menuBar()->addMenu( tr( "&File" ) );
	newMapAction = fileMenu->addAction( tr( "&New Map..." ) );
	newMapAction->setObjectName( QStringLiteral( "actionNewMap" ) );
	newMapAction->setShortcut( QKeySequence::New );
	connect( newMapAction, &QAction::triggered, this, &MainWindow::NewMap );
	openMapAction = fileMenu->addAction( tr( "&Open Map..." ) );
	openMapAction->setObjectName( QStringLiteral( "actionOpenMap" ) );
	openMapAction->setShortcut( QKeySequence::Open );
	connect( openMapAction, &QAction::triggered, this, &MainWindow::ChooseMap );
	saveAction = fileMenu->addAction( tr( "&Save Map" ) );
	saveAction->setObjectName( QStringLiteral( "actionSaveMap" ) );
	saveAction->setShortcut( QKeySequence::Save );
	connect( saveAction, &QAction::triggered, this, [this] { engine->SaveMap(); } );
	closeMapAction = fileMenu->addAction( tr( "&Close Map" ) );
	closeMapAction->setObjectName( QStringLiteral( "actionCloseMap" ) );
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
	undoAction->setObjectName( QStringLiteral( "actionUndo" ) );
	undoAction->setShortcut( QKeySequence::Undo );
	connect( undoAction, &QAction::triggered, this, [this] { engine->Undo(); } );
	redoAction = editMenu->addAction( tr( "&Redo" ) );
	redoAction->setObjectName( QStringLiteral( "actionRedo" ) );
	redoAction->setShortcuts( { QKeySequence( Qt::CTRL | Qt::Key_Y ), QKeySequence( Qt::CTRL | Qt::SHIFT | Qt::Key_Z ) } );
	connect( redoAction, &QAction::triggered, this, [this] { engine->Redo(); } );
	editMenu->addSeparator();
	QAction * const duplicateAction = editMenu->addAction( tr( "D&uplicate" ) );
	duplicateAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_D ) );
	connect( duplicateAction, &QAction::triggered, this, [this] { engine->DuplicateSelection(); } );
	QAction * const deleteAction = editMenu->addAction( tr( "De&lete" ) );
	deleteAction->setShortcut( QKeySequence::Delete );
	connect( deleteAction, &QAction::triggered, this, [this] { engine->DeleteSelection(); } );
	QAction * const deselectAction = editMenu->addAction( tr( "&Deselect" ) );
	deselectAction->setShortcut( QKeySequence( Qt::Key_Escape ) );
	deselectAction->setToolTip( tr( "The box tool goes back to selecting; a face to its brush; a selection to nothing (Esc)" ) );
	connect( deselectAction, &QAction::triggered, this, &MainWindow::Deselect );
	duplicateAction->setObjectName( QStringLiteral( "actionDuplicate" ) );
	deleteAction->setObjectName( QStringLiteral( "actionDelete" ) );
	deselectAction->setObjectName( QStringLiteral( "actionDeselect" ) );
	editActions << duplicateAction << deleteAction;

	// what the left button does in the viewport: Q selects, B draws boxes
	QToolBar * const tools = addToolBar( tr( "Tools" ) );
	tools->setObjectName( QStringLiteral( "ToolsBar" ) );
	auto * const toolGroup = new QActionGroup( this );
	selectToolAction = tools->addAction( tr( "Select" ) );
	selectToolAction->setObjectName( QStringLiteral( "actionToolSelect" ) );
	selectToolAction->setShortcut( QKeySequence( Qt::Key_Q ) );
	selectToolAction->setToolTip( tr( "Click selects, the gizmo moves or turns the selection (Q)" ) );
	boxToolAction = tools->addAction( tr( "Box" ) );
	boxToolAction->setObjectName( QStringLiteral( "actionToolBox" ) );
	boxToolAction->setShortcut( QKeySequence( Qt::Key_B ) );
	boxToolAction->setToolTip( tr( "Drag a rectangle on a face or on the grid plane: it becomes a brush (B)" ) );
	for ( QAction * const action : { selectToolAction, boxToolAction } ) {
		action->setCheckable( true );
		toolGroup->addAction( action );
	}
	selectToolAction->setChecked( true );
	connect( selectToolAction, &QAction::triggered, this, [this] { engine->SetTool( EDITOR_TOOL_SELECT ); } );
	connect( boxToolAction, &QAction::triggered, this, [this] { engine->SetTool( EDITOR_TOOL_BOX ); } );
	tools->addSeparator();

	// the gizmo and the grid, as in other editors: W moves, E rotates
	auto * const gizmoGroup = new QActionGroup( this );
	QAction * const moveAction = tools->addAction( tr( "Move" ) );
	moveAction->setObjectName( QStringLiteral( "actionGizmoMove" ) );
	moveAction->setShortcut( QKeySequence( Qt::Key_W ) );
	moveAction->setToolTip( tr( "Move the selection along an axis (W)" ) );
	QAction * const rotateAction = tools->addAction( tr( "Rotate" ) );
	rotateAction->setObjectName( QStringLiteral( "actionGizmoRotate" ) );
	rotateAction->setShortcut( QKeySequence( Qt::Key_E ) );
	rotateAction->setToolTip( tr( "Turn the selection around an axis, in steps of the angle: entities, brushes and patches (E)" ) );
	for ( QAction * const action : { moveAction, rotateAction } ) {
		action->setCheckable( true );
		gizmoGroup->addAction( action );
	}
	moveAction->setChecked( true );
	connect( moveAction, &QAction::triggered, this, [this] { engine->SetGizmo( EDITOR_GIZMO_MOVE ); } );
	connect( rotateAction, &QAction::triggered, this, [this] { engine->SetGizmo( EDITOR_GIZMO_ROTATE ); } );
	editMenu->addSeparator();
	editMenu->addAction( selectToolAction );
	editMenu->addAction( boxToolAction );
	editMenu->addAction( moveAction );
	editMenu->addAction( rotateAction );

	tools->addSeparator();
	tools->addWidget( new QLabel( tr( " Grid " ), tools ) );
	gridCombo = new QComboBox( tools );
	gridCombo->setObjectName( QStringLiteral( "gridCombo" ) );
	for ( const float size : GRID_SIZES ) {
		gridCombo->addItem( ( size == 0.0f ) ? tr( "Off" ) : QString::number( size ), size );
	}
	gridCombo->setCurrentIndex( DEFAULT_GRID );
	gridCombo->setToolTip( tr( "The step of a move and of a new brush, in map units" ) );
	connect( gridCombo, &QComboBox::currentIndexChanged, this, [this] { engine->SetGridSize( gridCombo->currentData().toFloat() ); } );
	tools->addWidget( gridCombo );
	tools->addWidget( new QLabel( tr( " Angle " ), tools ) );
	angleCombo = new QComboBox( tools );
	angleCombo->setObjectName( QStringLiteral( "angleCombo" ) );
	for ( const float degrees : ANGLE_SNAPS ) {
		angleCombo->addItem( QString::number( degrees ), degrees );
	}
	angleCombo->setCurrentIndex( DEFAULT_ANGLE_SNAP );
	angleCombo->setToolTip( tr( "The step of a turn with the rotate gizmo, in degrees" ) );
	connect( angleCombo, &QComboBox::currentIndexChanged, this, [this] { engine->SetAngleSnap( angleCombo->currentData().toFloat() ); } );
	tools->addWidget( angleCombo );
	tools->addWidget( new QLabel( tr( " Box height " ), tools ) );
	boxHeightSpin = new QSpinBox( tools );
	boxHeightSpin->setObjectName( QStringLiteral( "boxHeightSpin" ) );
	boxHeightSpin->setRange( 0, BOX_HEIGHT_MAX );
	boxHeightSpin->setSingleStep( 8 );
	boxHeightSpin->setSpecialValueText( tr( "Auto" ) );
	boxHeightSpin->setToolTip( tr( "How high the box tool makes a brush; Auto is eight steps of the grid" ) );
	connect( boxHeightSpin, &QSpinBox::editingFinished, this, [this] { engine->SetBoxHeight( static_cast<float>( boxHeightSpin->value() ) ); } );
	tools->addWidget( boxHeightSpin );

	QMenu * const brushMenu = menuBar()->addMenu( tr( "&Brush" ) );
	QAction * const hollowAction = brushMenu->addAction( tr( "&Hollow..." ) );
	hollowAction->setObjectName( QStringLiteral( "actionHollow" ) );
	hollowAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_H ) );
	hollowAction->setToolTip( tr( "Every selected brush becomes one wall for each of its faces (Ctrl+H)" ) );
	connect( hollowAction, &QAction::triggered, this, &MainWindow::Hollow );
	QAction * const makeEntityAction = brushMenu->addAction( tr( "Make &Entity..." ) );
	makeEntityAction->setObjectName( QStringLiteral( "actionMakeEntity" ) );
	makeEntityAction->setToolTip( tr( "The selected brushes of the world become a func_static, a door or another brush entity" ) );
	connect( makeEntityAction, &QAction::triggered, this, &MainWindow::MakeEntity );
	QAction * const moveToWorldAction = brushMenu->addAction( tr( "Move To &World" ) );
	moveToWorldAction->setObjectName( QStringLiteral( "actionMoveToWorld" ) );
	moveToWorldAction->setToolTip( tr( "The brushes of the selected brush entities go back to the world; the entities are removed" ) );
	connect( moveToWorldAction, &QAction::triggered, this, [this] { engine->MoveBrushesToWorld(); } );
	brushActions << hollowAction << makeEntityAction << moveToWorldAction;
	tools->addSeparator();
	tools->addAction( hollowAction );

	QMenu * const buildMenu = menuBar()->addMenu( tr( "B&uild" ) );
	buildAction = buildMenu->addAction( tr( "&Build" ) );
	buildAction->setObjectName( QStringLiteral( "actionBuild" ) );
	buildAction->setShortcut( QKeySequence( Qt::Key_F7 ) );
	buildAction->setToolTip( tr( "Saves the map and compiles it: the world and, with monsters in it, their navigation (F7)" ) );
	connect( buildAction, &QAction::triggered, this, [this] { engine->Build( false ); } );
	buildFastAction = buildMenu->addAction( tr( "Build &Fast" ) );
	buildFastAction->setObjectName( QStringLiteral( "actionBuildFast" ) );
	buildFastAction->setShortcut( QKeySequence( Qt::SHIFT | Qt::Key_F7 ) );
	buildFastAction->setToolTip( tr( "The world only: monsters will not find their way (Shift+F7)" ) );
	connect( buildFastAction, &QAction::triggered, this, [this] { engine->Build( true ); } );
	buildCancelAction = buildMenu->addAction( tr( "&Cancel Build" ) );
	buildCancelAction->setObjectName( QStringLiteral( "actionBuildCancel" ) );
	connect( buildCancelAction, &QAction::triggered, this, [this] { engine->CancelBuild(); } );
	buildMenu->addSeparator();
	showLeakAction = buildMenu->addAction( tr( "Show &Leak" ) );
	showLeakAction->setObjectName( QStringLiteral( "actionShowLeak" ) );
	showLeakAction->setCheckable( true );
	showLeakAction->setToolTip( tr( "The red line of the last build that leaked: from an entity to the hole" ) );
	connect( showLeakAction, &QAction::triggered, this, [this]( bool checked ) { engine->ShowLeak( checked ); } );
	goToLeakAction = buildMenu->addAction( tr( "&Go To Leak" ) );
	goToLeakAction->setObjectName( QStringLiteral( "actionGoToLeak" ) );
	connect( goToLeakAction, &QAction::triggered, this, [this] { engine->GoToLeak(); } );
	tools->addSeparator();
	tools->addAction( buildAction );
	tools->addAction( buildCancelAction );

	QMenu * const viewMenu = menuBar()->addMenu( tr( "&View" ) );
	viewMenu->addAction( tools->toggleViewAction() );
	viewMenu->addAction( orthoDock->toggleViewAction() );
	showGridAction = viewMenu->addAction( tr( "&Grid" ) );
	showGridAction->setObjectName( QStringLiteral( "actionShowGrid" ) );
	showGridAction->setCheckable( true );
	showGridAction->setChecked( true );
	showGridAction->setShortcut( QKeySequence( Qt::Key_G ) );
	showGridAction->setToolTip( tr( "The grid of the 3D viewport, on the plane z = 0 or the one the box tool drew on last (G)" ) );
	connect( showGridAction, &QAction::triggered, this, [this]( bool checked ) { engine->ShowGrid( checked ); } );
	viewMenu->addAction( showLeakAction );
	viewMenu->addAction( outlinerDock->toggleViewAction() );
	viewMenu->addAction( inspectorDock->toggleViewAction() );
	viewMenu->addAction( paletteDock->toggleViewAction() );
	viewMenu->addAction( contentDock->toggleViewAction() );
	viewMenu->addAction( materialDock->toggleViewAction() );
	viewMenu->addAction( outputDock->toggleViewAction() );
	viewMenu->addSeparator();
	QAction * const consoleAction = viewMenu->addAction( tr( "&Console Command" ) );
	consoleAction->setShortcut( QKeySequence( Qt::CTRL | Qt::Key_QuoteLeft ) );
	connect( consoleAction, &QAction::triggered, this, [this] {
		outputDock->show();
		outputDock->raise();
		outputLog->FocusCommandLine();
	} );

	QMenu * const playMenu = menuBar()->addMenu( tr( "&Play" ) );
	playAction = playMenu->addAction( tr( "&Play" ) );
	playAction->setObjectName( QStringLiteral( "actionPlay" ) );
	playAction->setShortcut( QKeySequence( Qt::Key_F5 ) );
	playAction->setToolTip( tr( "Saves the map, builds it when brushes changed, and starts the game on it in the viewport (F5)" ) );
	connect( playAction, &QAction::triggered, this, [this] { engine->Play(); } );
	stopAction = playMenu->addAction( tr( "&Stop" ) );
	stopAction->setObjectName( QStringLiteral( "actionStop" ) );
	stopAction->setToolTip( tr( "Ends the game; the map is edited again (Shift+Esc in the game)" ) );
	connect( stopAction, &QAction::triggered, this, [this] { engine->StopPlay(); } );
	tools->addSeparator();
	tools->addAction( playAction );
	tools->addAction( stopAction );

	QMenu * const helpMenu = menuBar()->addMenu( tr( "&Help" ) );
	connect( helpMenu->addAction( tr( "&About" ) ), &QAction::triggered, this, &MainWindow::ShowAbout );
	connect( helpMenu->addAction( tr( "About &Qt" ) ), &QAction::triggered, qApp, &QApplication::aboutQt );
}

void MainWindow::Start( const QString & scriptFile ) {
	show();
	client->Start();
	viewport->Attach( engine );
	contentBrowser->Load();
	palette->Load();
	if ( !scriptFile.isEmpty() ) {
		script = new UiScript( this, client, scriptFile, this );
		script->Start();
	}
}

void MainWindow::NewMap() {
	if ( !ConfirmDiscard() ) {
		return;
	}
	QInputDialog dialog( this );
	dialog.setObjectName( QStringLiteral( "newMapDialog" ) );
	dialog.setWindowTitle( tr( "New Map" ) );
	dialog.setLabelText( tr( "The map's name, as the game's map command takes it (maps/<name>.map):" ) );
	dialog.setTextValue( QStringLiteral( "editor/untitled" ) );
	if ( dialog.exec() != QDialog::Accepted ) {
		return;
	}
	QString name = dialog.textValue().trimmed().toLower();
	name.replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) );
	if ( name.startsWith( QLatin1String( "maps/" ) ) ) {
		name.remove( 0, 5 );
	}
	if ( name.endsWith( QLatin1String( ".map" ) ) ) {
		name.chop( 4 );
	}
	static const QRegularExpression valid( QStringLiteral( "^[a-z0-9_]+(/[a-z0-9_]+)*$" ) );
	if ( !valid.match( name ).hasMatch() ) {
		QMessageBox::warning( this, tr( "New Map" ), tr( "A map's name is made of letters, digits, _ and / for folders: editor/room1" ) );
		return;
	}
	const QByteArray text = name.toLocal8Bit();
	if ( engine->MapExists( text.constData() ) ) {
		QMessageBox box( QMessageBox::Question, tr( "New Map" ),
			tr( "maps/%1.map exists already. Start an empty map of that name? The file is replaced when the new map is saved." ).arg( name ),
			QMessageBox::Yes | QMessageBox::Cancel, this );
		box.setObjectName( QStringLiteral( "newMapOverwrite" ) );
		box.setDefaultButton( QMessageBox::Cancel );
		// accept() of the test script counts as Yes
		const int answer = box.exec();
		if ( answer != QMessageBox::Yes && answer != QDialog::Accepted ) {
			return;
		}
	}
	engine->NewMap( text.constData() );
}

void MainWindow::Deselect() {
	editorDocumentState_t state = {};
	engine->GetDocumentState( state );
	if ( state.tool == EDITOR_TOOL_BOX ) {
		engine->SetTool( EDITOR_TOOL_SELECT );
		return;
	}
	struct selection_t {
		int		count = 0;
		int		entity = 0;
		int		primitive = 0;
		int		face = -1;
	} selection;
	engine->EnumBrushSelection( []( void * context, int entityIndex, int primitive, int face ) {
		auto * const found = static_cast<selection_t *>( context );
		found->count++;
		found->entity = entityIndex;
		found->primitive = primitive;
		found->face = face;
	}, &selection );
	if ( selection.count == 1 && selection.face != -1 ) {
		engine->SelectBrush( selection.entity, selection.primitive, -1, false );
	} else {
		engine->SelectEntity( 0, false );
	}
}

void MainWindow::Hollow() {
	editorDocumentState_t state = {};
	engine->GetDocumentState( state );
	QInputDialog dialog( this );
	dialog.setObjectName( QStringLiteral( "hollowDialog" ) );
	dialog.setWindowTitle( tr( "Hollow" ) );
	dialog.setLabelText( tr( "The thickness of the walls, in map units:" ) );
	dialog.setInputMode( QInputDialog::DoubleInput );
	dialog.setDoubleRange( 1.0, 1024.0 );
	dialog.setDoubleDecimals( 0 );
	dialog.setDoubleValue( ( state.gridSize > 0.0f ) ? state.gridSize : HOLLOW_DEFAULT );
	if ( dialog.exec() == QDialog::Accepted ) {
		engine->HollowBrushes( static_cast<float>( dialog.doubleValue() ) );
	}
}

void MainWindow::MakeEntity() {
	QStringList classes;
	engine->EnumBrushEntityClasses( []( void * context, const char * classname ) {
		static_cast<QStringList *>( context )->append( QString::fromLocal8Bit( classname ) );
	}, &classes );
	QInputDialog dialog( this );
	dialog.setObjectName( QStringLiteral( "makeEntityDialog" ) );
	dialog.setWindowTitle( tr( "Make Entity" ) );
	dialog.setLabelText( tr( "The class the selected brushes become:" ) );
	dialog.setComboBoxItems( classes );
	dialog.setComboBoxEditable( true );
	if ( dialog.exec() == QDialog::Accepted && !dialog.textValue().trimmed().isEmpty() ) {
		engine->MakeBrushEntity( dialog.textValue().trimmed().toLocal8Bit().constData() );
	}
}

void MainWindow::ApplyMaterial( const QString & name ) {
	if ( !inspector->ApplyMaterial( name ) ) {
		statusBar()->showMessage( tr( "Select a brush or a face first: the material goes on it." ), STATUS_MESSAGE_MSEC );
	}
}

void MainWindow::BuildProgress( const QString & stage, float fraction ) {
	buildStageLabel->setText( tr( "Building, %1" ).arg( stage ) );
	buildProgressBar->setValue( qBound( 0, qRound( fraction * 100.0f ), 100 ) );
	buildStageLabel->show();
	buildProgressBar->show();
}

void MainWindow::BuildFinished( const BuildResult & result ) {
	buildStageLabel->hide();
	buildProgressBar->hide();
	QString text;
	switch ( result.status ) {
		case EDITOR_BUILD_OK:
			text = tr( "Build OK: %1 areas, %2 portals, %3 triangles, %4 AAS files, %5 s" )
				.arg( result.areas ).arg( result.portals ).arg( result.triangles ).arg( result.aasFiles ).arg( result.seconds, 0, 'f', 1 );
			break;
		case EDITOR_BUILD_LEAKED:
			text = tr( "Build LEAKED: %1 points of the leak line" ).arg( result.leakPoints );
			break;
		case EDITOR_BUILD_CANCELLED:
			text = tr( "Build cancelled" );
			break;
		default:
			text = tr( "Build FAILED: %1" ).arg( result.error );
			break;
	}
	buildResultLabel->setText( text );
	// for the UI script: a build that Play started shows only here
	buildResultLabel->setProperty( "builds", buildResultLabel->property( "builds" ).toInt() + 1 );
	outputLog->Append( text + QLatin1Char( '\n' ) );
	if ( result.status != EDITOR_BUILD_LEAKED && result.status != EDITOR_BUILD_FAILED ) {
		return;
	}
	// after the frame the build ran in: a dialog inside it would hold the engine
	const bool leaked = ( result.status == EDITOR_BUILD_LEAKED );
	const QString error = result.error;
	QTimer::singleShot( 0, this, [this, leaked, error] {
		QMessageBox box( QMessageBox::Warning, tr( "Build" ), leaked
			? tr( "The map leaks: an entity can see the void outside, so nothing was compiled.\n\nThe red line in the viewport leads from the entity to the hole. Close the hole and build again." )
			: tr( "The build failed:\n\n%1" ).arg( error ), QMessageBox::Ok, this );
		box.setObjectName( QStringLiteral( "buildMessage" ) );
		QPushButton * const goButton = leaked ? box.addButton( tr( "Go To Leak" ), QMessageBox::ActionRole ) : nullptr;
		box.exec();
		if ( goButton != nullptr && box.clickedButton() == goButton ) {
			engine->GoToLeak();
		}
	} );
}

void MainWindow::EditMaterial( const QString & name ) {
	materialDock->show();
	materialDock->raise();
	materialEditor->Open( name );
}

void MainWindow::AssetActivated( int kind, const QString & name ) {
	const QByteArray text = name.toLocal8Bit();
	const bool mapOpen = engine->CurrentMap()[0] != '\0';
	switch ( kind ) {
		case EDITOR_ASSET_ENTITY_CLASS:
		case EDITOR_ASSET_MODEL:
			if ( !mapOpen ) {
				statusBar()->showMessage( tr( "Open a map to put things into it." ) );
			} else if ( kind == EDITOR_ASSET_MODEL ) {
				engine->CreateEntity( "func_static", text.constData() );
			} else {
				engine->CreateEntity( text.constData(), nullptr );
			}
			break;
		case EDITOR_ASSET_MAP:
			if ( ConfirmDiscard() ) {
				engine->OpenMap( text.constData() );
			}
			break;
		case EDITOR_ASSET_SOUND:
			engine->PlaySound( text.constData() );
			break;
		case EDITOR_ASSET_MATERIAL:
			EditMaterial( name );
			break;
		default:
			break;
	}
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

	// what the document is to the game: the title and the status bar say it
	const bool busy = state.playing || state.building;
	const bool open = !map.isEmpty();
	QString stateText = tr( "No map" );
	QString titleNote;
	if ( state.building ) {
		stateText = tr( "Building" );
		titleNote = tr( " [building]" );
	} else if ( state.playing ) {
		stateText = tr( "Playing" );
		titleNote = tr( " [playing]" );
	} else if ( open ) {
		stateText = state.geometryDirty ? tr( "Uncompiled" ) : tr( "Compiled" );
		titleNote = state.geometryDirty ? tr( " [uncompiled]" ) : QString();
	}
	docStateLabel->setText( stateText );
	setWindowTitle( open ? tr( "%1%2%3 - Doom 3 Editor" ).arg( map, state.dirty ? QStringLiteral( "*" ) : QString(), titleNote ) : tr( "Doom 3 Editor" ) );
	saveAction->setEnabled( open && !busy );
	playAction->setEnabled( open && !busy );
	stopAction->setEnabled( state.playing );
	newMapAction->setEnabled( !busy );
	openMapAction->setEnabled( !busy );
	closeMapAction->setEnabled( open && !busy );
	buildAction->setEnabled( open && !busy );
	buildFastAction->setEnabled( open && !busy );
	buildCancelAction->setEnabled( state.building );
	showLeakAction->setEnabled( state.leakPoints > 0 );
	showLeakAction->setChecked( state.leakPoints > 0 && state.leakShown );
	showGridAction->setChecked( state.gridShown );
	if ( const int angleIndex = angleCombo->findData( state.angleSnap ); angleIndex != -1 && angleIndex != angleCombo->currentIndex() ) {
		const QSignalBlocker blocker( angleCombo );
		angleCombo->setCurrentIndex( angleIndex );
	}
	goToLeakAction->setEnabled( state.leakPoints > 1 && !busy );
	for ( QAction * const action : editActions + brushActions ) {
		action->setEnabled( open && !busy );
	}
	( ( state.tool == EDITOR_TOOL_BOX ) ? boxToolAction : selectToolAction )->setChecked( true );
	if ( const int gridIndex = gridCombo->findData( state.gridSize ); gridIndex != -1 && gridIndex != gridCombo->currentIndex() ) {
		const QSignalBlocker blocker( gridCombo );
		gridCombo->setCurrentIndex( gridIndex );
	}
	orthoView->Reload();
	// the game or the compiler has the map: nothing of it can be edited, and the engine has no frame for a picture
	for ( QDockWidget * const dock : { outlinerDock, inspectorDock, paletteDock, contentDock, materialDock, orthoDock } ) {
		dock->widget()->setEnabled( !busy );
	}
	if ( state.playing ) {
		statusBar()->showMessage( tr( "Playing %1: Shift+Esc stops the game" ).arg( map ) );
	} else if ( wasPlaying ) {
		statusBar()->clearMessage();
	}
	wasPlaying = state.playing;
	undoAction->setEnabled( state.canUndo && !busy );
	undoAction->setText( undoLabel.isEmpty() ? tr( "&Undo" ) : tr( "&Undo %1" ).arg( undoLabel ) );
	redoAction->setEnabled( state.canRedo && !busy );
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
	outliner->Reload();
	inspector->Refresh();
	DocumentChanged();
	orthoView->CenterOnCamera();
	buildResultLabel->clear();
	statusBar()->showMessage( map.isEmpty()
		? tr( "File > New Map or Open Map, or a double click on a map in the Content Browser, starts editing." )
		: tr( "Click selects (Ctrl adds, Shift takes a face), drag the gizmo to move. B: box tool, drag to draw a brush. Hold the right mouse button to look; W A S D fly, Q / E down and up, Shift faster." ) );
}

void MainWindow::ShowAbout() {
	QMessageBox::about( this, tr( "About Doom 3 Editor" ), tr(
		"<b>Doom 3 Editor</b><br>"
		"The level and material editor of the DOOM 3 BFG Edition port.<br><br>"
		"The engine and the editor are free software under the GNU General Public License, version 3 or later.<br>"
		"The user interface uses Qt %1 under the GNU Lesser General Public License, version 3." ).arg( QString::fromLatin1( qVersion() ) ) );
}
