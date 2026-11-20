// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "Inspector.h"

#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "../api/EditorEngineApi.h"

namespace {

constexpr int COLUMN_KEY = 0;
constexpr int COLUMN_VALUE = 1;
constexpr int KEY_TYPE_ROLE = Qt::UserRole;
constexpr int KEY_IN_MAP_ROLE = Qt::UserRole + 1;
constexpr int COLOR_ICON_SIZE = 14;
constexpr int FACE_COLUMN_FACE = 0;
constexpr int FACE_COLUMN_MATERIAL = 1;
constexpr int FACE_COLUMN_VALUES = 2;		// the first of five
constexpr int NUM_FACE_VALUES = 5;
constexpr double FACE_VALUE_RANGE = 65536.0;

// "r g b", each 0 .. 1; white when the text is something else
[[nodiscard]] QColor ColorFromValue( const QString & value ) {
	const QStringList parts = value.split( QLatin1Char( ' ' ), Qt::SkipEmptyParts );
	if ( parts.size() < 3 ) {
		return Qt::white;
	}
	return QColor::fromRgbF( qBound( 0.0f, parts[0].toFloat(), 1.0f ), qBound( 0.0f, parts[1].toFloat(), 1.0f ), qBound( 0.0f, parts[2].toFloat(), 1.0f ) );
}

[[nodiscard]] QString ValueFromColor( const QColor & color ) {
	return QStringLiteral( "%1 %2 %3" ).arg( color.redF(), 0, 'g', 4 ).arg( color.greenF(), 0, 'g', 4 ).arg( color.blueF(), 0, 'g', 4 );
}

[[nodiscard]] QIcon ColorIcon( const QColor & color ) {
	QPixmap pixmap( COLOR_ICON_SIZE, COLOR_ICON_SIZE );
	pixmap.fill( color );
	return QIcon( pixmap );
}

}	// namespace

Inspector::Inspector( idEditorEngine * editorEngine, QWidget * parent ) : QWidget( parent ), engine( editorEngine ) {
	title = new QLabel( this );
	title->setTextInteractionFlags( Qt::TextSelectableByMouse );

	tree = new QTreeWidget( this );
	tree->setHeaderLabels( { tr( "Key" ), tr( "Value" ) } );
	tree->setRootIsDecorated( false );
	tree->setUniformRowHeights( true );
	tree->setAlternatingRowColors( true );
	tree->header()->setSectionResizeMode( COLUMN_KEY, QHeaderView::ResizeToContents );
	tree->setEditTriggers( QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked );
	connect( tree, &QTreeWidget::itemChanged, this, &Inspector::ItemChanged );
	connect( tree, &QTreeWidget::itemDoubleClicked, this, &Inspector::ItemActivated );

	newKey = new QLineEdit( this );
	newKey->setPlaceholderText( tr( "New key" ) );
	newValue = new QLineEdit( this );
	newValue->setPlaceholderText( tr( "Value" ) );
	auto * const addButton = new QPushButton( tr( "Add" ), this );
	resetButton = new QPushButton( tr( "Reset" ), this );
	resetButton->setToolTip( tr( "Takes the selected key out of the map: the class default is back" ) );
	connect( addButton, &QPushButton::clicked, this, &Inspector::AddKey );
	connect( newValue, &QLineEdit::returnPressed, this, &Inspector::AddKey );
	connect( resetButton, &QPushButton::clicked, this, &Inspector::ResetKey );

	auto * const row = new QHBoxLayout;
	row->addWidget( newKey, 1 );
	row->addWidget( newValue, 2 );
	row->addWidget( addButton );
	row->addWidget( resetButton );

	auto * const entityPage = new QWidget( this );
	auto * const entityLayout = new QVBoxLayout( entityPage );
	entityLayout->setContentsMargins( 0, 0, 0, 0 );
	entityLayout->setSpacing( 2 );
	entityLayout->addWidget( tree, 1 );
	entityLayout->addLayout( row );

	pages = new QStackedWidget( this );
	pages->addWidget( entityPage );
	pages->addWidget( CreateBrushPage() );

	auto * const layout = new QVBoxLayout( this );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addWidget( title );
	layout->addWidget( pages, 1 );

	Refresh();
}

QWidget * Inspector::CreateBrushPage() {
	auto * const page = new QWidget( this );

	faceList = new QTreeWidget( page );
	faceList->setObjectName( QStringLiteral( "faceList" ) );
	faceList->setHeaderLabels( { tr( "Face" ), tr( "Material" ), tr( "Shift S" ), tr( "Shift T" ), tr( "Repeat S" ), tr( "Repeat T" ), tr( "Rotate" ) } );
	faceList->setRootIsDecorated( false );
	faceList->setUniformRowHeights( true );
	faceList->setAlternatingRowColors( true );
	faceList->header()->setSectionResizeMode( FACE_COLUMN_MATERIAL, QHeaderView::Stretch );
	faceList->setToolTip( tr( "A click on a row selects that face of the brush" ) );
	connect( faceList, &QTreeWidget::itemSelectionChanged, this, &Inspector::FaceRowSelected );

	faceMaterial = new QLineEdit( page );
	faceMaterial->setObjectName( QStringLiteral( "faceMaterialEdit" ) );
	faceMaterial->setPlaceholderText( tr( "textures/..." ) );
	connect( faceMaterial, &QLineEdit::returnPressed, this, [this] { ApplyMaterial( faceMaterial->text().trimmed() ); } );
	auto * const pick = new QPushButton( tr( "From Browser" ), page );
	pick->setObjectName( QStringLiteral( "faceMaterialPick" ) );
	pick->setToolTip( tr( "Puts the material selected in the Content Browser on the face" ) );
	connect( pick, &QPushButton::clicked, this, &Inspector::materialPickRequested );
	auto * const materialRow = new QHBoxLayout;
	materialRow->addWidget( faceMaterial, 1 );
	materialRow->addWidget( pick );

	const char * const names[NUM_FACE_VALUES] = { "faceShiftS", "faceShiftT", "faceRepeatS", "faceRepeatT", "faceRotate" };
	const QString labels[NUM_FACE_VALUES] = { tr( "Shift S (repeats)" ), tr( "Shift T (repeats)" ), tr( "Repeat S (units)" ), tr( "Repeat T (units)" ), tr( "Rotate (degrees)" ) };
	auto * const form = new QFormLayout;
	form->addRow( tr( "Material" ), materialRow );
	for ( int i = 0; i < NUM_FACE_VALUES; i++ ) {
		faceValues[i] = new QDoubleSpinBox( page );
		faceValues[i]->setObjectName( QString::fromLatin1( names[i] ) );
		faceValues[i]->setRange( -FACE_VALUE_RANGE, FACE_VALUE_RANGE );
		faceValues[i]->setDecimals( 3 );
		faceValues[i]->setSingleStep( ( i < 2 ) ? 0.125 : ( ( i < 4 ) ? 8.0 : 15.0 ) );
		connect( faceValues[i], &QDoubleSpinBox::editingFinished, this, &Inspector::ApplyTexture );
		form->addRow( labels[i], faceValues[i] );
	}

	auto * const hint = new QLabel( tr( "The values go to the selected face, or to every face when no face is selected." ), page );
	hint->setWordWrap( true );

	auto * const layout = new QVBoxLayout( page );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addWidget( faceList, 1 );
	layout->addLayout( form );
	layout->addWidget( hint );
	return page;
}

void Inspector::RefreshBrush() {
	struct context_t {
		QTreeWidget *	list;
		Inspector *		self;
		double			values[NUM_FACE_VALUES];
		QString			material;
		bool			haveValues;
		bool			isPatch;
		int				patchWidth;
		int				patchHeight;
	} context = { faceList, this, {}, {}, false, false, 0, 0 };

	refreshing = true;
	faceList->clear();
	engine->EnumBrushFaces( brushEntity, brushPrimitive, []( void * opaque, const editorFaceInfo_t & face ) {
		auto * const ctx = static_cast<context_t *>( opaque );
		const double values[NUM_FACE_VALUES] = { face.texture.shiftS, face.texture.shiftT, face.texture.repeatS, face.texture.repeatT, face.texture.rotate };
		auto * const item = new QTreeWidgetItem( ctx->list );
		item->setText( FACE_COLUMN_FACE, face.isPatch ? QObject::tr( "patch" ) : QString::number( face.face ) );
		item->setText( FACE_COLUMN_MATERIAL, QString::fromLocal8Bit( face.material ) );
		for ( int i = 0; i < NUM_FACE_VALUES && !face.isPatch; i++ ) {
			item->setText( FACE_COLUMN_VALUES + i, QString::number( values[i], 'g', 6 ) );
		}
		if ( face.isPatch ) {
			ctx->isPatch = true;
			ctx->patchWidth = face.patchWidth;
			ctx->patchHeight = face.patchHeight;
		} else if ( !face.visible ) {
			item->setForeground( FACE_COLUMN_FACE, ctx->list->palette().brush( QPalette::Disabled, QPalette::Text ) );
			item->setToolTip( FACE_COLUMN_FACE, QObject::tr( "The other sides cut this one away" ) );
		}
		// the fields show the selected face, or the first one
		if ( face.face == ctx->self->brushFace || !ctx->haveValues ) {
			ctx->haveValues = true;
			ctx->material = QString::fromLocal8Bit( face.material );
			for ( int i = 0; i < NUM_FACE_VALUES; i++ ) {
				ctx->values[i] = values[i];
			}
		}
	}, &context );

	if ( brushFace >= 0 && brushFace < faceList->topLevelItemCount() ) {
		faceList->setCurrentItem( faceList->topLevelItem( brushFace ) );
	}
	// for the UI script
	faceList->setProperty( "selectedFace", brushFace );
	if ( !faceMaterial->hasFocus() ) {
		faceMaterial->setText( context.material );
	}
	for ( int i = 0; i < NUM_FACE_VALUES; i++ ) {
		loadedValues[i] = context.values[i];
		if ( !faceValues[i]->hasFocus() ) {
			faceValues[i]->setValue( context.values[i] );
		}
		// a patch has its texture coordinates in its control points
		faceValues[i]->setEnabled( !context.isPatch );
	}
	brushIsPatch = context.isPatch;
	faceList->setProperty( "isPatch", context.isPatch );
	refreshing = false;

	if ( numBrushes > 1 ) {
		title->setText( tr( "%1 brushes or patches; the faces of the first" ).arg( numBrushes ) );
	} else if ( context.isPatch ) {
		title->setText( tr( "Patch %1 of entity %2, %3 x %4 control points. The material can be set; the points cannot be edited yet." )
			.arg( brushPrimitive ).arg( brushEntity ).arg( context.patchWidth ).arg( context.patchHeight ) );
	} else if ( brushFace >= 0 ) {
		title->setText( tr( "Brush %1 of entity %2, face %3" ).arg( brushPrimitive ).arg( brushEntity ).arg( brushFace ) );
	} else {
		title->setText( tr( "Brush %1 of entity %2" ).arg( brushPrimitive ).arg( brushEntity ) );
	}
}

void Inspector::FaceRowSelected() {
	const QTreeWidgetItem * const item = faceList->currentItem();
	if ( refreshing || item == nullptr || numBrushes != 1 || brushIsPatch ) {
		return;
	}
	engine->SelectBrush( brushEntity, brushPrimitive, item->text( FACE_COLUMN_FACE ).toInt(), false );
}

bool Inspector::ApplyMaterial( const QString & name ) {
	if ( numBrushes == 0 || name.isEmpty() ) {
		return false;
	}
	engine->SetFaceMaterial( name.toLocal8Bit().constData() );
	return true;
}

void Inspector::ApplyTexture() {
	if ( refreshing || numBrushes == 0 || brushIsPatch ) {
		return;
	}
	bool changed = false;
	for ( int i = 0; i < NUM_FACE_VALUES; i++ ) {
		changed |= ( qAbs( faceValues[i]->value() - loadedValues[i] ) > 0.0005 );
	}
	if ( !changed ) {
		return;
	}
	const editorFaceTexture_t texture = { static_cast<float>( faceValues[0]->value() ), static_cast<float>( faceValues[1]->value() ),
		static_cast<float>( faceValues[2]->value() ), static_cast<float>( faceValues[3]->value() ), static_cast<float>( faceValues[4]->value() ) };
	for ( int i = 0; i < NUM_FACE_VALUES; i++ ) {
		loadedValues[i] = faceValues[i]->value();
	}
	engine->SetFaceTexture( texture );
}

void Inspector::Refresh() {
	struct brushSelection_t {
		int		count = 0;
		int		entity = 0;
		int		primitive = -1;
		int		face = -1;
	} brushSelection;
	engine->EnumBrushSelection( []( void * context, int entityIndex, int primitive, int face ) {
		auto * const found = static_cast<brushSelection_t *>( context );
		if ( found->count++ == 0 ) {
			found->entity = entityIndex;
			found->primitive = primitive;
			found->face = face;
		}
	}, &brushSelection );
	numBrushes = brushSelection.count;
	brushEntity = brushSelection.entity;
	brushPrimitive = brushSelection.primitive;
	brushFace = brushSelection.face;
	if ( numBrushes > 0 ) {
		entity = 0;
		pages->setCurrentIndex( 1 );
		setEnabled( true );
		RefreshBrush();
		return;
	}
	pages->setCurrentIndex( 0 );

	QList<int> selection;
	engine->EnumSelection( []( void * context, int index ) {
		static_cast<QList<int> *>( context )->append( index );
	}, &selection );

	const QString currentKey = ( tree->currentItem() != nullptr ) ? tree->currentItem()->text( COLUMN_KEY ) : QString();
	entity = selection.isEmpty() ? 0 : selection.first();

	refreshing = true;
	tree->clear();
	struct context_t {
		QTreeWidget *	tree;
		QString			name;
		QString			classname;
	} context = { tree, {}, {} };

	if ( entity != 0 ) {
		engine->EnumEntityKeys( entity, []( void * opaque, const editorKeyInfo_t & info ) {
			auto * const ctx = static_cast<context_t *>( opaque );
			const QString key = QString::fromLocal8Bit( info.key );
			const QString value = QString::fromLocal8Bit( info.value );
			if ( key == QLatin1String( "name" ) ) {
				ctx->name = value;
			} else if ( key == QLatin1String( "classname" ) ) {
				ctx->classname = value;
			}

			auto * const item = new QTreeWidgetItem( ctx->tree );
			item->setText( COLUMN_KEY, key );
			item->setData( COLUMN_KEY, KEY_TYPE_ROLE, info.type );
			item->setData( COLUMN_KEY, KEY_IN_MAP_ROLE, info.inMap );
			item->setToolTip( COLUMN_KEY, QString::fromLocal8Bit( info.description ) );
			item->setToolTip( COLUMN_VALUE, QString::fromLocal8Bit( info.description ) );
			if ( info.inMap ) {
				QFont font = item->font( COLUMN_KEY );
				font.setBold( true );
				item->setFont( COLUMN_KEY, font );
			} else {
				item->setForeground( COLUMN_KEY, ctx->tree->palette().brush( QPalette::Disabled, QPalette::Text ) );
				item->setForeground( COLUMN_VALUE, ctx->tree->palette().brush( QPalette::Disabled, QPalette::Text ) );
			}

			if ( info.type == EDITOR_KEY_BOOL ) {
				item->setFlags( item->flags() | Qt::ItemIsUserCheckable );
				item->setCheckState( COLUMN_VALUE, ( value.toInt() != 0 ) ? Qt::Checked : Qt::Unchecked );
			} else {
				item->setFlags( item->flags() | Qt::ItemIsEditable );
				item->setText( COLUMN_VALUE, value );
				if ( info.type == EDITOR_KEY_COLOR ) {
					item->setIcon( COLUMN_VALUE, ColorIcon( ColorFromValue( value ) ) );
				}
			}
		}, &context );
	}

	for ( int i = 0; i < tree->topLevelItemCount(); i++ ) {
		if ( tree->topLevelItem( i )->text( COLUMN_KEY ) == currentKey ) {
			tree->setCurrentItem( tree->topLevelItem( i ) );
			break;
		}
	}
	refreshing = false;

	if ( entity == 0 ) {
		title->setText( tr( "Nothing is selected" ) );
	} else if ( selection.size() == 1 ) {
		title->setText( tr( "%1 (%2)" ).arg( context.name, context.classname ) );
	} else {
		title->setText( tr( "%1 (%2), the first of %3 selected" ).arg( context.name, context.classname ).arg( selection.size() ) );
	}
	setEnabled( entity != 0 );
}

void Inspector::ItemChanged( QTreeWidgetItem * item, int column ) {
	if ( refreshing || entity == 0 || column != COLUMN_VALUE ) {
		return;
	}
	const QByteArray key = item->text( COLUMN_KEY ).toLocal8Bit();
	QString value = item->text( COLUMN_VALUE );
	if ( item->data( COLUMN_KEY, KEY_TYPE_ROLE ).toInt() == EDITOR_KEY_BOOL ) {
		value = ( item->checkState( COLUMN_VALUE ) == Qt::Checked ) ? QStringLiteral( "1" ) : QStringLiteral( "0" );
	}
	// the engine answers with a signal that fills the list again, after this handler
	engine->SetEntityKey( entity, key.constData(), value.toLocal8Bit().constData() );
}

void Inspector::ItemActivated( QTreeWidgetItem * item, int column ) {
	if ( entity == 0 || column != COLUMN_KEY || item->data( COLUMN_KEY, KEY_TYPE_ROLE ).toInt() != EDITOR_KEY_COLOR ) {
		return;
	}
	const QColor color = QColorDialog::getColor( ColorFromValue( item->text( COLUMN_VALUE ) ), this, item->text( COLUMN_KEY ) );
	if ( color.isValid() ) {
		engine->SetEntityKey( entity, item->text( COLUMN_KEY ).toLocal8Bit().constData(), ValueFromColor( color ).toLocal8Bit().constData() );
	}
}

void Inspector::AddKey() {
	const QString key = newKey->text().trimmed();
	if ( entity == 0 || key.isEmpty() ) {
		return;
	}
	engine->SetEntityKey( entity, key.toLocal8Bit().constData(), newValue->text().toLocal8Bit().constData() );
	newKey->clear();
	newValue->clear();
}

void Inspector::ResetKey() {
	const QTreeWidgetItem * const item = tree->currentItem();
	if ( entity != 0 && item != nullptr && item->data( COLUMN_KEY, KEY_IN_MAP_ROLE ).toBool() ) {
		engine->SetEntityKey( entity, item->text( COLUMN_KEY ).toLocal8Bit().constData(), nullptr );
	}
}
