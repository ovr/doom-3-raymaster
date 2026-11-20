// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QStringList>
#include <QTimer>

class EngineClient;
class QWidget;

/*
	A test tool, not a feature: Doom3Editor.exe --ui-script <file> drives the Qt panels the way a
	person would, because a console command of the bridge cannot reach them. One command a line,
	'#' starts a comment; the objects are found by their objectName.

	  trigger <action>                      a menu or toolbar action
	  click <button>
	  set <widget> <value>                  a line edit (and Return), a combo box, a spin box, an input dialog
	  select <view> <row>                   the current row of a list or tree
	  drag <widget> <x1> <y1> <x2> <y2> [middle]   mouse events of Qt (not the real mouse) in a widget; 0.5+30 is
	                                        its middle and 30 pixels more; the same point twice is a click
	  accept <dialog> | reject <dialog>     a dialog that is open
	  expect <object> <property> <value>    the property as text equals the value
	  expectcontains <object> <property> <text>
	  expectitem <tree> <row> <column> <text>
	  waitfor <object> <property> <msec> <value>   until it equals (value*: starts with), or a failure after msec
	  wait <msec>
	  note <text>                           a line "NOTE <text>" in the log
	  rect <widget>                         "RECT <widget> x y w h": where it is in the window, in device pixels
	  engine <console command>
	  shot <file.png>                       QWidget::grab of the main window (the engine viewport stays blank)
	  quit

	A timer runs the lines, so they go on inside a dialog's own event loop and inside a build,
	whose progress pumps the events; a line that opens a dialog returns when it closes, the lines
	after it run meanwhile. The results go to <file>.log: a PASS or FAIL line for every
	check and "ui-script: <n> passed, <m> failed" at the end.
*/
class UiScript final : public QObject {
	Q_OBJECT

public:
	UiScript( QWidget * window, EngineClient * client, const QString & fileName, QObject * parent = nullptr );

	void				Start();

private:
	void				Step();
	void				Run( const QString & line );
	void				Check( bool passed, const QString & what );
	void				Log( const QString & text );
	[[nodiscard]] QObject *	Find( const QString & name ) const;

	QWidget *			window;
	EngineClient *		client;
	QString				fileName;
	QStringList			lines;
	qsizetype			at = 0;
	QTimer				timer;
	QElapsedTimer		clock;
	qint64				waitUntil = 0;
	// a waitfor that is not met yet
	QString				pendingObject;
	QString				pendingProperty;
	QString				pendingValue;
	qsizetype			pendingLine = 0;
	QFile				log;
	int					passed = 0;
	int					failed = 0;
	bool				finished = false;
};
