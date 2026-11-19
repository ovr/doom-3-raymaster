// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "Outliner.h"

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "../api/EditorEngineApi.h"

namespace {

enum Column { COLUMN_NAME, COLUMN_CLASS, NUM_COLUMNS };
constexpr int ENTITY_INDEX_ROLE = Qt::UserRole;

}	// namespace

Outliner::Outliner( idEditorEngine * engine_, QWidget * parent ) : QWidget( parent ), engine( engine_ ) {
	filter = new QLineEdit( this );
	filter->setPlaceholderText( tr( "Filter by name or class" ) );
	filter->setClearButtonEnabled( true );
	connect( filter, &QLineEdit::textChanged, this, &Outliner::ApplyFilter );

	tree = new QTreeWidget( this );
	tree->setColumnCount( NUM_COLUMNS );
	tree->setHeaderLabels( { tr( "Name" ), tr( "Class" ) } );
	tree->setRootIsDecorated( false );
	tree->setUniformRowHeights( true );
	tree->setAlternatingRowColors( true );
	tree->setSortingEnabled( true );
	tree->sortByColumn( COLUMN_NAME, Qt::AscendingOrder );
	tree->header()->setSectionResizeMode( COLUMN_NAME, QHeaderView::Stretch );
	connect( tree, &QTreeWidget::itemActivated, this, &Outliner::Activate );

	count = new QLabel( this );

	auto * const layout = new QVBoxLayout( this );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addWidget( filter );
	layout->addWidget( tree );
	layout->addWidget( count );

	Reload();
}

void Outliner::Reload() {
	// sorting while inserting would sort once per item
	tree->setSortingEnabled( false );
	tree->clear();

	QList<QTreeWidgetItem *> items;
	engine->EnumEntities( []( void * context, const editorEntityInfo_t & entity ) {
		const QString classname = QString::fromLocal8Bit( entity.classname );
		QString name = QString::fromLocal8Bit( entity.name );
		if ( name.isEmpty() ) {
			name = QStringLiteral( "(%1 %2)" ).arg( classname ).arg( entity.index );
		}
		auto * const item = new QTreeWidgetItem( { name, classname } );
		item->setData( COLUMN_NAME, ENTITY_INDEX_ROLE, entity.index );
		static_cast<QList<QTreeWidgetItem *> *>( context )->append( item );
	}, &items );
	tree->addTopLevelItems( items );

	tree->setSortingEnabled( true );
	ApplyFilter( filter->text() );
}

void Outliner::ApplyFilter( const QString & text ) {
	int shown = 0;
	const int total = tree->topLevelItemCount();
	for ( int i = 0; i < total; i++ ) {
		QTreeWidgetItem * const item = tree->topLevelItem( i );
		const bool match = text.isEmpty()
			|| item->text( COLUMN_NAME ).contains( text, Qt::CaseInsensitive )
			|| item->text( COLUMN_CLASS ).contains( text, Qt::CaseInsensitive );
		item->setHidden( !match );
		shown += match ? 1 : 0;
	}
	count->setText( ( shown == total ) ? tr( "%1 entities" ).arg( total ) : tr( "%1 of %2 entities" ).arg( shown ).arg( total ) );
}

void Outliner::Activate( QTreeWidgetItem * item ) {
	engine->LookAtEntity( item->data( COLUMN_NAME, ENTITY_INDEX_ROLE ).toInt() );
}
