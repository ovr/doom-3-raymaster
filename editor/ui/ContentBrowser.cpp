// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "ContentBrowser.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QVBoxLayout>

#include "../api/EditorEngineApi.h"
#include "EngineClient.h"
#include "ThumbnailCache.h"

namespace {

constexpr int		ICON_SIZE = 96;
constexpr int		CELL_WIDTH = 132;
constexpr int		CELL_HEIGHT = ICON_SIZE + 48;
constexpr int		NO_PREVIEW = -1;
const int			FULL_NAME_ROLE = Qt::UserRole;

// the editorPreviewKind_t of an asset kind's pictures
[[nodiscard]] int PreviewKind( int assetKind ) {
	switch ( assetKind ) {
		case EDITOR_ASSET_MATERIAL:		return EDITOR_PREVIEW_MATERIAL;
		case EDITOR_ASSET_MODEL:		return EDITOR_PREVIEW_MODEL;
		case EDITOR_ASSET_ENTITY_CLASS:	return EDITOR_PREVIEW_ENTITY_CLASS;
		default:						return NO_PREVIEW;
	}
}

// an icon of the style on the picture's background, for the kinds nothing can be drawn of
[[nodiscard]] QPixmap StandardPicture( QStyle::StandardPixmap which ) {
	QPixmap picture( ICON_SIZE, ICON_SIZE );
	picture.fill( QColor( 28, 28, 28 ) );
	QPainter painter( &picture );
	const QPixmap icon = QApplication::style()->standardIcon( which ).pixmap( ICON_SIZE / 2, ICON_SIZE / 2 );
	painter.drawPixmap( ( ICON_SIZE - icon.width() ) / 2, ( ICON_SIZE - icon.height() ) / 2, icon );
	return picture;
}

}	// namespace

// The names of one kind and the rows the filter leaves. The pictures come from the cache when a
// row is painted, so only what is seen is ever made.
class AssetListModel final : public QAbstractListModel {
public:
	AssetListModel( ThumbnailCache * cache, QObject * parent ) : QAbstractListModel( parent ), thumbnails( cache ) {
		placeholder = QPixmap( ICON_SIZE, ICON_SIZE );
		placeholder.fill( QColor( 28, 28, 28 ) );
		soundPicture = StandardPicture( QStyle::SP_MediaVolume );
		mapPicture = StandardPicture( QStyle::SP_FileDialogContentsView );
		connect( thumbnails, &ThumbnailCache::ready, this, [this]( int previewKind, const QString & name ) {
			const auto row = rowOf.constFind( name );
			if ( previewKind == PreviewKind( kind ) && row != rowOf.constEnd() ) {
				const QModelIndex changed = index( *row );
				emit dataChanged( changed, changed, { Qt::DecorationRole } );
			}
		} );
	}

	void SetNames( int assetKind, const QStringList & sortedNames ) {
		beginResetModel();
		kind = assetKind;
		names = sortedNames;
		Filter();
		endResetModel();
	}

	void SetFilter( const QString & text ) {
		beginResetModel();
		filter = text;
		Filter();
		endResetModel();
	}

	[[nodiscard]] int Kind() const { return kind; }
	[[nodiscard]] int Total() const { return static_cast<int>( names.size() ); }

	int rowCount( const QModelIndex & parent = QModelIndex() ) const override {
		return parent.isValid() ? 0 : static_cast<int>( shown.size() );
	}

	QVariant data( const QModelIndex & index, int role ) const override {
		if ( !index.isValid() || index.row() >= shown.size() ) {
			return QVariant();
		}
		const QString & name = names[shown[index.row()]];
		switch ( role ) {
			case Qt::DisplayRole:
				// the last part: the folders of a name do not fit under a picture
				return name.mid( name.lastIndexOf( QLatin1Char( '/' ) ) + 1 );
			case Qt::ToolTipRole:
			case FULL_NAME_ROLE:
				return name;
			case Qt::DecorationRole: {
				const int previewKind = PreviewKind( kind );
				if ( previewKind == NO_PREVIEW ) {
					return ( kind == EDITOR_ASSET_SOUND ) ? soundPicture : mapPicture;
				}
				const QPixmap picture = thumbnails->Get( previewKind, name );
				return picture.isNull() ? placeholder : picture;
			}
			default:
				return QVariant();
		}
	}

private:
	void Filter() {
		shown.clear();
		rowOf.clear();
		for ( int i = 0; i < names.size(); i++ ) {
			if ( filter.isEmpty() || names[i].contains( filter, Qt::CaseInsensitive ) ) {
				rowOf.insert( names[i], static_cast<int>( shown.size() ) );
				shown.append( i );
			}
		}
	}

	ThumbnailCache *	thumbnails;
	int					kind = EDITOR_ASSET_MATERIAL;
	QStringList			names;
	QString				filter;
	QList<int>			shown;			// indexes into names
	QHash<QString, int>	rowOf;			// name -> row of shown
	QPixmap				placeholder;
	QPixmap				soundPicture;
	QPixmap				mapPicture;
};

ContentBrowser::ContentBrowser( EngineClient * engineClient, QWidget * parent ) : QWidget( parent ), client( engineClient ) {
	thumbnails = new ThumbnailCache( client, this );
	model = new AssetListModel( thumbnails, this );

	kinds = new QComboBox( this );
	kinds->addItem( tr( "Materials" ), EDITOR_ASSET_MATERIAL );
	kinds->addItem( tr( "Models" ), EDITOR_ASSET_MODEL );
	kinds->addItem( tr( "Entity classes" ), EDITOR_ASSET_ENTITY_CLASS );
	kinds->addItem( tr( "Sounds" ), EDITOR_ASSET_SOUND );
	kinds->addItem( tr( "Maps" ), EDITOR_ASSET_MAP );

	filter = new QLineEdit( this );
	filter->setPlaceholderText( tr( "Filter by a part of the name: base_wall, monster_, door ..." ) );
	filter->setClearButtonEnabled( true );

	count = new QLabel( this );

	view = new QListView( this );
	view->setModel( model );
	view->setViewMode( QListView::IconMode );
	view->setResizeMode( QListView::Adjust );
	view->setMovement( QListView::Static );
	view->setLayoutMode( QListView::Batched );
	view->setUniformItemSizes( true );
	view->setIconSize( QSize( ICON_SIZE, ICON_SIZE ) );
	view->setGridSize( QSize( CELL_WIDTH, CELL_HEIGHT ) );
	view->setWordWrap( false );
	view->setTextElideMode( Qt::ElideMiddle );
	view->setContextMenuPolicy( Qt::CustomContextMenu );
	view->setToolTip( tr( "Double click: a material is edited, a class or a model is put in front of the camera, a map is opened, a sound is played" ) );

	auto * const top = new QHBoxLayout;
	top->setContentsMargins( 0, 0, 0, 0 );
	top->addWidget( kinds );
	top->addWidget( filter, 1 );
	top->addWidget( count );

	auto * const layout = new QVBoxLayout( this );
	layout->setContentsMargins( 0, 0, 0, 0 );
	layout->setSpacing( 2 );
	layout->addLayout( top );
	layout->addWidget( view, 1 );

	connect( kinds, &QComboBox::currentIndexChanged, this, [this] { ShowKind( kinds->currentData().toInt() ); } );
	connect( filter, &QLineEdit::textChanged, this, &ContentBrowser::ApplyFilter );
	connect( view, &QListView::doubleClicked, this, [this]( const QModelIndex & index ) {
		emit activated( model->Kind(), index.data( FULL_NAME_ROLE ).toString() );
	} );
	connect( view, &QListView::customContextMenuRequested, this, &ContentBrowser::ShowMenu );
}

void ContentBrowser::Load() {
	if ( !loaded ) {
		loaded = true;
		ShowKind( kinds->currentData().toInt() );
	}
}

void ContentBrowser::ShowKind( int kind ) {
	if ( !loaded ) {
		return;
	}
	QStringList names;
	client->Engine()->EnumAssets( kind, []( void * context, const char * name ) {
		static_cast<QStringList *>( context )->append( QString::fromLocal8Bit( name ) );
	}, &names );
	names.sort( Qt::CaseInsensitive );

	thumbnails->DropWanted();
	model->SetNames( kind, names );
	ApplyFilter();
}

void ContentBrowser::ApplyFilter() {
	thumbnails->DropWanted();
	model->SetFilter( filter->text() );
	view->scrollToTop();
	count->setText( tr( "%1 of %2" ).arg( model->rowCount() ).arg( model->Total() ) );
}

void ContentBrowser::ShowMenu( const QPoint & position ) {
	const QModelIndex index = view->indexAt( position );
	if ( !index.isValid() ) {
		return;
	}
	const QString name = index.data( FULL_NAME_ROLE ).toString();
	QMenu menu( this );
	connect( menu.addAction( tr( "Copy Name" ) ), &QAction::triggered, this, [name] { QApplication::clipboard()->setText( name ); } );
	if ( PreviewKind( model->Kind() ) != NO_PREVIEW ) {
		connect( menu.addAction( tr( "Make the Picture Again" ) ), &QAction::triggered, this, [this, name, index] {
			thumbnails->Invalidate( PreviewKind( model->Kind() ), name );
			view->update( index );
		} );
	}
	menu.exec( view->viewport()->mapToGlobal( position ) );
}
