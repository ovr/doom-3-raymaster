// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "Palette.h"

#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include "../api/EditorEngineApi.h"

Palette::Palette( idEditorEngine * editorEngine, QWidget * parent ) : QWidget( parent ), engine( editorEngine ) {
	filter = new QLineEdit( this );
	filter->setPlaceholderText( tr( "Filter classes: light, monster_, item_ ..." ) );
	filter->setClearButtonEnabled( true );

	list = new QListWidget( this );
	list->setUniformItemSizes( true );
	list->setToolTip( tr( "Double click makes the entity in front of the camera" ) );

	auto * const layout = new QVBoxLayout( this );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addWidget( filter );
	layout->addWidget( list, 1 );

	connect( filter, &QLineEdit::textChanged, this, &Palette::ApplyFilter );
	connect( list, &QListWidget::itemDoubleClicked, this, [this]( QListWidgetItem * item ) {
		engine->CreateEntity( item->text().toLocal8Bit().constData() );
	} );
}

void Palette::Load() {
	if ( list->count() > 0 ) {
		return;
	}
	QStringList names;
	engine->EnumEntityClasses( []( void * context, const char * classname ) {
		static_cast<QStringList *>( context )->append( QString::fromLocal8Bit( classname ) );
	}, &names );
	names.sort( Qt::CaseInsensitive );
	list->addItems( names );
	ApplyFilter( filter->text() );
}

void Palette::ApplyFilter( const QString & text ) {
	for ( int i = 0; i < list->count(); i++ ) {
		QListWidgetItem * const item = list->item( i );
		item->setHidden( !text.isEmpty() && !item->text().contains( text, Qt::CaseInsensitive ) );
	}
}
