// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "Inspector.h"

#include <QColorDialog>
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

	auto * const layout = new QVBoxLayout( this );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addWidget( title );
	layout->addWidget( tree, 1 );
	layout->addLayout( row );

	Refresh();
}

void Inspector::Refresh() {
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
