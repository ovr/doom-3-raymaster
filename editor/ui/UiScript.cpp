// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "UiScript.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVariant>
#include <QWidget>

#include "EngineClient.h"

namespace {

constexpr int STEP_INTERVAL_MSEC = 20;

// the first word of text, which loses it
[[nodiscard]] QString TakeWord( QString & text ) {
	text = text.trimmed();
	const qsizetype space = text.indexOf( QLatin1Char( ' ' ) );
	const QString word = ( space == -1 ) ? text : text.left( space );
	text = ( space == -1 ) ? QString() : text.mid( space + 1 ).trimmed();
	return word;
}

}	// namespace

UiScript::UiScript( QWidget * window_, EngineClient * client_, const QString & fileName_, QObject * parent )
	: QObject( parent ), window( window_ ), client( client_ ), fileName( fileName_ ) {
	timer.setInterval( STEP_INTERVAL_MSEC );
	connect( &timer, &QTimer::timeout, this, &UiScript::Step );
}

void UiScript::Start() {
	log.setFileName( fileName + QStringLiteral( ".log" ) );
	log.open( QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text );
	QFile file( fileName );
	if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
		Check( false, QStringLiteral( "cannot read %1" ).arg( fileName ) );
		Run( QStringLiteral( "quit" ) );
		return;
	}
	lines = QString::fromUtf8( file.readAll() ).split( QLatin1Char( '\n' ) );
	clock.start();
	timer.start();
}

void UiScript::Log( const QString & text ) {
	log.write( text.toUtf8() );
	log.write( "\n" );
	log.flush();
}

void UiScript::Check( bool ok, const QString & what ) {
	( ok ? passed : failed )++;
	Log( QStringLiteral( "%1 line %2: %3" ).arg( ok ? QStringLiteral( "PASS" ) : QStringLiteral( "FAIL" ) ).arg( at ).arg( what ) );
}

QObject * UiScript::Find( const QString & name ) const {
	if ( name == QLatin1String( "window" ) ) {
		return window;
	}
	if ( QObject * const child = window->findChild<QObject *>( name ) ) {
		return child;
	}
	// a dialog and what is in it
	for ( QWidget * const widget : QApplication::allWidgets() ) {
		if ( widget->objectName() == name ) {
			return widget;
		}
	}
	return nullptr;
}

void UiScript::Step() {
	if ( finished || clock.elapsed() < waitUntil ) {
		return;
	}
	if ( !pendingObject.isEmpty() ) {
		const QObject * const object = Find( pendingObject );
		const QString value = ( object != nullptr ) ? object->property( pendingProperty.toLatin1().constData() ).toString() : QString();
		// a value that ends with * is the start of the text
		const bool met = pendingValue.endsWith( QLatin1Char( '*' ) ) ? value.startsWith( pendingValue.chopped( 1 ) ) : ( value == pendingValue );
		if ( !met ) {
			return;
		}
		Check( true, QStringLiteral( "waitfor %1 %2 = %3" ).arg( pendingObject, pendingProperty, value ) );
		pendingObject.clear();
	}
	while ( at < lines.size() ) {
		// counted first: a command that opens a dialog returns only when it closes
		const QString line = lines[at++].trimmed();
		if ( !line.isEmpty() && !line.startsWith( QLatin1Char( '#' ) ) ) {
			// not from this timer: it does not fire again while a dialog the line opened is up
			QTimer::singleShot( 0, this, [this, line] { Run( line ); } );
			return;
		}
	}
	Run( QStringLiteral( "quit" ) );
}

void UiScript::Run( const QString & line ) {
	QString rest = line;
	const QString command = TakeWord( rest );

	if ( command == QLatin1String( "wait" ) ) {
		waitUntil = clock.elapsed() + rest.toInt();
		return;
	}
	if ( command == QLatin1String( "note" ) ) {
		Log( QStringLiteral( "NOTE %1" ).arg( rest ) );
		return;
	}
	if ( command == QLatin1String( "engine" ) ) {
		client->Execute( rest );
		return;
	}
	if ( command == QLatin1String( "shot" ) ) {
		const QString path = QFileInfo( fileName ).absoluteDir().filePath( rest );
		Check( window->grab().save( path ), QStringLiteral( "shot %1" ).arg( path ) );
		return;
	}
	if ( command == QLatin1String( "quit" ) ) {
		finished = true;
		timer.stop();
		Log( QStringLiteral( "ui-script: %1 passed, %2 failed" ).arg( passed ).arg( failed ) );
		log.close();
		client->RequestQuit();
		return;
	}

	const QString name = TakeWord( rest );
	QObject * const object = Find( name );
	if ( object == nullptr ) {
		Check( false, QStringLiteral( "%1: there is no object '%2'" ).arg( command, name ) );
		return;
	}

	if ( command == QLatin1String( "rect" ) ) {
		// in device pixels of the main window's client area, for a test that moves the real mouse
		const auto * const widget = qobject_cast<QWidget *>( object );
		if ( widget == nullptr ) {
			Check( false, QStringLiteral( "rect %1: not a widget" ).arg( name ) );
			return;
		}
		const QPointF corner = QPointF( widget->mapTo( window, QPoint( 0, 0 ) ) ) * window->devicePixelRatioF();
		const QSizeF size = QSizeF( widget->size() ) * window->devicePixelRatioF();
		Log( QStringLiteral( "RECT %1 %2 %3 %4 %5" ).arg( name ).arg( qRound( corner.x() ) ).arg( qRound( corner.y() ) ).arg( qRound( size.width() ) ).arg( qRound( size.height() ) ) );
	} else if ( command == QLatin1String( "drag" ) ) {
		auto * const widget = qobject_cast<QWidget *>( object );
		if ( widget == nullptr ) {
			Check( false, QStringLiteral( "drag %1: not a widget" ).arg( name ) );
			return;
		}
		// 0.5+30: the middle and 30 pixels more
		const auto coordinate = [&rest]( int size ) {
			const QString word = TakeWord( rest );
			const qsizetype sign = qMax( word.indexOf( QLatin1Char( '+' ), 1 ), word.indexOf( QLatin1Char( '-' ), 1 ) );
			return ( sign == -1 ) ? word.toDouble() * size : word.left( sign ).toDouble() * size + word.mid( sign ).toDouble();
		};
		const double x1 = coordinate( widget->width() );
		const double y1 = coordinate( widget->height() );
		const double x2 = coordinate( widget->width() );
		const double y2 = coordinate( widget->height() );
		const Qt::MouseButton button = ( rest == QLatin1String( "middle" ) ) ? Qt::MiddleButton : Qt::LeftButton;
		const auto send = [widget]( QEvent::Type type, const QPointF & at, Qt::MouseButton changed, Qt::MouseButtons down ) {
			QMouseEvent event( type, at, widget->mapToGlobal( at ), changed, down, Qt::NoModifier );
			QApplication::sendEvent( widget, &event );
		};
		const QPointF from( x1, y1 );
		const QPointF to( x2, y2 );
		send( QEvent::MouseButtonPress, from, button, button );
		if ( from != to ) {
			send( QEvent::MouseMove, ( from + to ) * 0.5, Qt::NoButton, button );
			send( QEvent::MouseMove, to, Qt::NoButton, button );
		}
		send( QEvent::MouseButtonRelease, to, button, Qt::NoButton );
	} else if ( command == QLatin1String( "trigger" ) ) {
		auto * const action = qobject_cast<QAction *>( object );
		if ( action == nullptr || !action->isEnabled() ) {
			Check( false, QStringLiteral( "trigger %1: %2" ).arg( name, ( action == nullptr ) ? QStringLiteral( "not an action" ) : QStringLiteral( "the action is disabled" ) ) );
			return;
		}
		action->trigger();
	} else if ( command == QLatin1String( "click" ) ) {
		auto * const button = qobject_cast<QAbstractButton *>( object );
		if ( button == nullptr || !button->isEnabled() ) {
			Check( false, QStringLiteral( "click %1: not a button that is enabled" ).arg( name ) );
			return;
		}
		button->click();
	} else if ( command == QLatin1String( "set" ) ) {
		if ( auto * const input = qobject_cast<QInputDialog *>( object ) ) {
			if ( input->inputMode() == QInputDialog::DoubleInput ) {
				input->setDoubleValue( rest.toDouble() );
			} else {
				input->setTextValue( rest );
			}
		} else if ( auto * const lineEdit = qobject_cast<QLineEdit *>( object ) ) {
			lineEdit->setText( rest );
			emit lineEdit->returnPressed();
		} else if ( auto * const combo = qobject_cast<QComboBox *>( object ) ) {
			const int index = combo->findText( rest );
			if ( index == -1 ) {
				Check( false, QStringLiteral( "set %1: no item '%2'" ).arg( name, rest ) );
				return;
			}
			combo->setCurrentIndex( index );
		} else if ( auto * const spin = qobject_cast<QSpinBox *>( object ) ) {
			spin->setValue( rest.toInt() );
			emit spin->editingFinished();
		} else if ( auto * const doubleSpin = qobject_cast<QDoubleSpinBox *>( object ) ) {
			doubleSpin->setValue( rest.toDouble() );
			emit doubleSpin->editingFinished();
		} else {
			Check( false, QStringLiteral( "set %1: a widget of a kind the script cannot set" ).arg( name ) );
		}
	} else if ( command == QLatin1String( "select" ) ) {
		auto * const view = qobject_cast<QAbstractItemView *>( object );
		const QModelIndex index = ( view != nullptr && view->model() != nullptr ) ? view->model()->index( rest.toInt(), 0 ) : QModelIndex();
		if ( !index.isValid() ) {
			Check( false, QStringLiteral( "select %1 %2: no such row" ).arg( name, rest ) );
			return;
		}
		view->setCurrentIndex( index );
	} else if ( command == QLatin1String( "accept" ) || command == QLatin1String( "reject" ) ) {
		auto * const dialog = qobject_cast<QDialog *>( object );
		if ( dialog == nullptr || !dialog->isVisible() ) {
			Check( false, QStringLiteral( "%1 %2: not a dialog that is open" ).arg( command, name ) );
			return;
		}
		if ( command == QLatin1String( "accept" ) ) {
			dialog->accept();
		} else {
			dialog->reject();
		}
	} else if ( command == QLatin1String( "expect" ) || command == QLatin1String( "expectcontains" ) ) {
		const QString property = TakeWord( rest );
		const QString value = object->property( property.toLatin1().constData() ).toString();
		const bool ok = ( command == QLatin1String( "expect" ) ) ? ( value == rest ) : value.contains( rest );
		Check( ok, QStringLiteral( "%1 %2 %3 '%4'%5" ).arg( command, name, property, rest, ok ? QString() : QStringLiteral( ", it is '%1'" ).arg( value.right( 400 ) ) ) );
	} else if ( command == QLatin1String( "expectitem" ) ) {
		const int row = TakeWord( rest ).toInt();
		const int column = TakeWord( rest ).toInt();
		const auto * const tree = qobject_cast<QTreeWidget *>( object );
		const QTreeWidgetItem * const item = ( tree != nullptr ) ? tree->topLevelItem( row ) : nullptr;
		const QString value = ( item != nullptr ) ? item->text( column ) : QStringLiteral( "<no such row>" );
		Check( value == rest, QStringLiteral( "expectitem %1 %2 %3 '%4'%5" ).arg( name ).arg( row ).arg( column ).arg( rest, ( value == rest ) ? QString() : QStringLiteral( ", it is '%1'" ).arg( value ) ) );
	} else if ( command == QLatin1String( "waitfor" ) ) {
		pendingProperty = TakeWord( rest );
		const int timeout = TakeWord( rest ).toInt();
		pendingObject = name;
		pendingValue = rest;
		// when the time is over the check fails and the script goes on
		const qsizetype waitLine = at;
		pendingLine = at;
		QTimer::singleShot( timeout, this, [this, waitLine] {
			if ( !pendingObject.isEmpty() && pendingLine == waitLine ) {
				const QObject * const waited = Find( pendingObject );
				Check( false, QStringLiteral( "waitfor %1 %2 '%3' timed out, it is '%4'" ).arg( pendingObject, pendingProperty, pendingValue,
					( waited != nullptr ) ? waited->property( pendingProperty.toLatin1().constData() ).toString() : QString() ) );
				pendingObject.clear();
			}
		} );
	} else {
		Check( false, QStringLiteral( "unknown command '%1'" ).arg( command ) );
	}
}
