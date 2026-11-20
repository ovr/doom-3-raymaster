// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "ThumbnailCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>

#include "../api/EditorEngineApi.h"
#include "EngineClient.h"

namespace {

constexpr int		THUMBNAIL_SIZE = 128;
constexpr int		MAX_IN_FLIGHT = 2;
constexpr int		MAX_WANTED = 256;
constexpr int		MEMORY_BUDGET_KB = 48 * 1024;
// the ids of the engine's previews are shared with the other users of RequestPreview
constexpr int		FIRST_ID = 1 << 20;

[[nodiscard]] QString Key( int kind, const QString & name ) {
	return QString::number( kind ) + QLatin1Char( ':' ) + name;
}

[[nodiscard]] int CostKb( const QPixmap & pixmap ) {
	return qMax( 1, pixmap.width() * pixmap.height() * 4 / 1024 );
}

}	// namespace

ThumbnailCache::ThumbnailCache( EngineClient * engineClient, QObject * parent ) : QObject( parent ), client( engineClient ), memory( MEMORY_BUDGET_KB ), nextId( FIRST_ID ) {
	folder = QStandardPaths::writableLocation( QStandardPaths::CacheLocation ) + QStringLiteral( "/thumbnails" );
	connect( client, &EngineClient::previewReady, this, &ThumbnailCache::Received );
}

QString ThumbnailCache::FilePath( int kind, const QString & name ) const {
	const QByteArray hash = QCryptographicHash::hash( name.toLower().toUtf8(), QCryptographicHash::Sha1 ).toHex();
	return QStringLiteral( "%1/%2/%3.png" ).arg( folder ).arg( kind ).arg( QString::fromLatin1( hash ) );
}

QPixmap ThumbnailCache::Get( int kind, const QString & name ) {
	const QString key = Key( kind, name );
	if ( const QPixmap * const cached = memory.object( key ) ) {
		return *cached;
	}
	QPixmap fromDisk;
	if ( fromDisk.load( FilePath( kind, name ) ) ) {
		memory.insert( key, new QPixmap( fromDisk ), CostKb( fromDisk ) );
		return fromDisk;
	}

	for ( const request_t & flying : std::as_const( inFlight ) ) {
		if ( flying.kind == kind && flying.name == name ) {
			return QPixmap();
		}
	}
	// asked again: it becomes the newest
	for ( int i = 0; i < wanted.size(); i++ ) {
		if ( wanted[i].kind == kind && wanted[i].name == name ) {
			wanted.move( i, wanted.size() - 1 );
			return QPixmap();
		}
	}
	wanted.append( { kind, name } );
	if ( wanted.size() > MAX_WANTED ) {
		wanted.removeFirst();
	}
	Feed();
	return QPixmap();
}

void ThumbnailCache::DropWanted() {
	wanted.clear();
}

void ThumbnailCache::Invalidate( int kind, const QString & name ) {
	memory.remove( Key( kind, name ) );
	QFile::remove( FilePath( kind, name ) );
}

void ThumbnailCache::Feed() {
	while ( inFlight.size() < MAX_IN_FLIGHT && !wanted.isEmpty() ) {
		const request_t request = wanted.takeLast();
		const int id = nextId++;
		inFlight.insert( id, request );
		client->Engine()->RequestPreview( id, request.kind, request.name.toLocal8Bit().constData(), THUMBNAIL_SIZE );
	}
}

void ThumbnailCache::Received( int id, const QImage & image ) {
	const auto found = inFlight.constFind( id );
	if ( found == inFlight.constEnd() ) {
		return;
	}
	const request_t request = *found;
	inFlight.erase( found );

	if ( image.isNull() ) {
		// in memory only: the asset may be fixed by the next start
		QPixmap failed( THUMBNAIL_SIZE, THUMBNAIL_SIZE );
		failed.fill( QColor( 70, 24, 24 ) );
		memory.insert( Key( request.kind, request.name ), new QPixmap( failed ), CostKb( failed ) );
	} else {
		const QPixmap pixmap = QPixmap::fromImage( image );
		memory.insert( Key( request.kind, request.name ), new QPixmap( pixmap ), CostKb( pixmap ) );
		const QString path = FilePath( request.kind, request.name );
		QDir().mkpath( QFileInfo( path ).path() );
		image.save( path, "PNG" );
	}

	emit ready( request.kind, request.name );
	Feed();
}
