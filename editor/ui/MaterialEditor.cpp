// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "MaterialEditor.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTextCursor>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "../api/EditorEngineApi.h"
#include "../mtr/MtrText.h"
#include "EngineClient.h"
#include "ThumbnailCache.h"

namespace {

constexpr int		PREVIEW_SIZE = 192;
constexpr int		PARSE_DELAY_MSEC = 300;
// the ids of the engine's previews are shared with the thumbnails, which count from 1 << 20
constexpr int		FIRST_PREVIEW_ID = 1 << 19;
// what a key item of the list knows about its line: offsets into the text
const int			LINE_BEGIN_ROLE = Qt::UserRole;
const int			VALUE_BEGIN_ROLE = Qt::UserRole + 1;
const int			LINE_END_ROLE = Qt::UserRole + 2;
const int			KEY_LENGTH_ROLE = Qt::UserRole + 3;

// Colours comments, strings, numbers, braces and the key that starts a line.
class MtrHighlighter final : public QSyntaxHighlighter {
public:
	explicit MtrHighlighter( QTextDocument * document ) : QSyntaxHighlighter( document ) {
		comment.setForeground( QColor( 106, 153, 85 ) );
		string.setForeground( QColor( 206, 145, 120 ) );
		number.setForeground( QColor( 181, 206, 168 ) );
		key.setForeground( QColor( 86, 156, 214 ) );
		brace.setForeground( QColor( 255, 215, 0 ) );
	}

protected:
	void highlightBlock( const QString & line ) override {
		enum { IN_CODE = 0, IN_COMMENT = 1 };
		static const QRegularExpression numberPattern( QStringLiteral( "(?<![\\w./])-?\\d+(\\.\\d+)?(?![\\w/])" ) );

		int pos = 0;
		bool keySeen = false;
		int state = ( previousBlockState() == IN_COMMENT ) ? IN_COMMENT : IN_CODE;
		QList<QPair<int, int>> code;		// the spans that are neither comment nor string
		while ( pos < line.size() ) {
			if ( state == IN_COMMENT ) {
				const int close = line.indexOf( QLatin1String( "*/" ), pos );
				const int end = ( close < 0 ) ? line.size() : close + 2;
				setFormat( pos, end - pos, comment );
				state = ( close < 0 ) ? IN_COMMENT : IN_CODE;
				pos = end;
				continue;
			}
			const QChar c = line[pos];
			if ( c == QLatin1Char( '/' ) && pos + 1 < line.size() && line[pos + 1] == QLatin1Char( '/' ) ) {
				setFormat( pos, line.size() - pos, comment );
				break;
			}
			if ( c == QLatin1Char( '/' ) && pos + 1 < line.size() && line[pos + 1] == QLatin1Char( '*' ) ) {
				state = IN_COMMENT;
				continue;
			}
			if ( c == QLatin1Char( '"' ) ) {
				const int close = line.indexOf( QLatin1Char( '"' ), pos + 1 );
				const int end = ( close < 0 ) ? line.size() : close + 1;
				setFormat( pos, end - pos, string );
				pos = end;
				keySeen = true;
				continue;
			}
			if ( c == QLatin1Char( '{' ) || c == QLatin1Char( '}' ) ) {
				setFormat( pos, 1, brace );
				// a line starts again after a brace
				keySeen = false;
				pos++;
				continue;
			}
			if ( c.isSpace() ) {
				pos++;
				continue;
			}
			int end = pos;
			while ( end < line.size() && !line[end].isSpace() && line[end] != QLatin1Char( '{' ) && line[end] != QLatin1Char( '}' ) && line[end] != QLatin1Char( '"' )
					&& !( line[end] == QLatin1Char( '/' ) && end + 1 < line.size() && ( line[end + 1] == QLatin1Char( '/' ) || line[end + 1] == QLatin1Char( '*' ) ) ) ) {
				end++;
			}
			if ( !keySeen ) {
				setFormat( pos, end - pos, key );
				keySeen = true;
			} else {
				code.append( { pos, end - pos } );
			}
			pos = end;
		}
		for ( const auto & span : code ) {
			auto matches = numberPattern.globalMatch( line.mid( span.first, span.second ) );
			while ( matches.hasNext() ) {
				const QRegularExpressionMatch match = matches.next();
				setFormat( span.first + static_cast<int>( match.capturedStart() ), static_cast<int>( match.capturedLength() ), number );
			}
		}
		setCurrentBlockState( state );
	}

private:
	QTextCharFormat		comment;
	QTextCharFormat		string;
	QTextCharFormat		number;
	QTextCharFormat		key;
	QTextCharFormat		brace;
};

void AddLines( QTreeWidgetItem * parent, const std::string & source, const mtr::block_t & block ) {
	for ( const mtr::line_t & line : block.lines ) {
		auto * const item = new QTreeWidgetItem( parent );
		item->setText( 0, QString::fromLatin1( line.key.data(), static_cast<qsizetype>( line.key.size() ) ) );
		const std::string_view value = mtr::Value( source, line );
		item->setText( 1, QString::fromLatin1( value.data(), static_cast<qsizetype>( value.size() ) ) );
		item->setData( 0, LINE_BEGIN_ROLE, static_cast<int>( line.begin ) );
		item->setData( 0, VALUE_BEGIN_ROLE, static_cast<int>( line.valueBegin ) );
		item->setData( 0, LINE_END_ROLE, static_cast<int>( line.end ) );
		item->setData( 0, KEY_LENGTH_ROLE, static_cast<int>( line.key.size() ) );
		item->setFlags( item->flags() | Qt::ItemIsEditable );
	}
}

// "Stage 2: blend add", from the stage's own blend line
[[nodiscard]] QString StageTitle( int number, const std::string & source, const mtr::block_t & block ) {
	QString title = MaterialEditor::tr( "Stage %1" ).arg( number );
	if ( const mtr::line_t * const blend = mtr::FindLine( block, "blend" ) ) {
		const std::string_view value = mtr::Value( source, *blend );
		title += QStringLiteral( ": blend " ) + QString::fromLatin1( value.data(), static_cast<qsizetype>( value.size() ) );
	}
	return title;
}

void AddBlocks( QTreeWidgetItem * parent, const std::string & source, const mtr::block_t & block, bool stages ) {
	int number = 1;
	for ( const mtr::block_t & child : block.blocks ) {
		auto * const item = new QTreeWidgetItem( parent );
		item->setText( 0, stages ? StageTitle( number, source, child ) : MaterialEditor::tr( "Block %1" ).arg( number ) );
		item->setFirstColumnSpanned( true );
		QFont bold = item->font( 0 );
		bold.setBold( true );
		item->setFont( 0, bold );
		AddLines( item, source, child );
		AddBlocks( item, source, child, false );
		number++;
	}
}

}	// namespace

MaterialEditor::MaterialEditor( EngineClient * engineClient, ThumbnailCache * thumbnailCache, QWidget * parent )
	: QWidget( parent ), client( engineClient ), thumbnails( thumbnailCache ), previewId( FIRST_PREVIEW_ID ) {
	title = new QLabel( tr( "Double click a material in the Content Browser to edit it." ), this );
	title->setWordWrap( true );
	title->setTextInteractionFlags( Qt::TextSelectableByMouse );

	preview = new QLabel( this );
	preview->setFixedSize( PREVIEW_SIZE, PREVIEW_SIZE );
	preview->setAlignment( Qt::AlignCenter );
	preview->setFrameShape( QFrame::StyledPanel );

	applyButton = new QPushButton( tr( "Apply" ), this );
	applyButton->setToolTip( tr( "Shows the text in the viewport and the preview; no file changes (Ctrl+Enter)" ) );
	saveButton = new QPushButton( tr( "Save" ), this );
	saveButton->setToolTip( tr( "Applies and writes the material's .mtr file into the project folder (Ctrl+S)" ) );
	revertButton = new QPushButton( tr( "Revert" ), this );
	revertButton->setToolTip( tr( "Back to the text the material had when it was opened" ) );

	keys = new QTreeWidget( this );
	keys->setHeaderLabels( { tr( "Key" ), tr( "Value" ) } );
	keys->setRootIsDecorated( true );
	keys->setAlternatingRowColors( true );
	keys->setEditTriggers( QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed );
	keys->header()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );

	text = new QPlainTextEdit( this );
	text->setFont( QFontDatabase::systemFont( QFontDatabase::FixedFont ) );
	text->setLineWrapMode( QPlainTextEdit::NoWrap );
	text->setTabStopDistance( text->fontMetrics().horizontalAdvance( QLatin1Char( ' ' ) ) * 4 );
	new MtrHighlighter( text->document() );

	status = new QLabel( this );
	status->setWordWrap( true );

	auto * const buttons = new QVBoxLayout;
	buttons->addWidget( applyButton );
	buttons->addWidget( saveButton );
	buttons->addWidget( revertButton );
	buttons->addStretch( 1 );

	auto * const top = new QHBoxLayout;
	top->addWidget( preview );
	top->addLayout( buttons );
	top->addStretch( 1 );

	auto * const splitter = new QSplitter( Qt::Vertical, this );
	splitter->addWidget( keys );
	splitter->addWidget( text );
	splitter->setStretchFactor( 0, 1 );
	splitter->setStretchFactor( 1, 2 );

	auto * const layout = new QVBoxLayout( this );
	layout->setContentsMargins( 2, 2, 2, 2 );
	layout->setSpacing( 4 );
	layout->addWidget( title );
	layout->addLayout( top );
	layout->addWidget( splitter, 1 );
	layout->addWidget( status );

	parseTimer.setSingleShot( true );
	parseTimer.setInterval( PARSE_DELAY_MSEC );
	connect( &parseTimer, &QTimer::timeout, this, &MaterialEditor::RebuildKeys );
	connect( text, &QPlainTextEdit::textChanged, this, [this] {
		parseTimer.start();
		UpdateState();
	} );
	connect( keys, &QTreeWidget::itemChanged, this, &MaterialEditor::ValueEdited );
	connect( applyButton, &QPushButton::clicked, this, [this] { static_cast<void>( Apply() ); } );
	connect( saveButton, &QPushButton::clicked, this, &MaterialEditor::Save );
	connect( revertButton, &QPushButton::clicked, this, &MaterialEditor::Revert );
	connect( client, &EngineClient::previewReady, this, &MaterialEditor::PreviewReady );

	auto * const applyShortcut = new QShortcut( QKeySequence( Qt::CTRL | Qt::Key_Return ), this );
	applyShortcut->setContext( Qt::WidgetWithChildrenShortcut );
	connect( applyShortcut, &QShortcut::activated, this, [this] { static_cast<void>( Apply() ); } );
	auto * const saveShortcut = new QShortcut( QKeySequence::Save, this );
	saveShortcut->setContext( Qt::WidgetWithChildrenShortcut );
	connect( saveShortcut, &QShortcut::activated, this, &MaterialEditor::Save );

	UpdateState();
}

void MaterialEditor::Open( const QString & materialName ) {
	struct loaded_t {
		QByteArray		text;
		QString			fileName;
		bool			implicit = false;
		bool			valid = true;
	} loaded;
	if ( !client->Engine()->GetMaterial( materialName.toLocal8Bit().constData(), []( void * context, const editorMaterialInfo_t & material ) {
			loaded_t & target = *static_cast<loaded_t *>( context );
			target.text = material.text;
			target.fileName = QString::fromLocal8Bit( material.fileName );
			target.implicit = material.implicit;
			target.valid = material.valid;
		}, &loaded ) ) {
		status->setText( tr( "There is no material '%1'." ).arg( materialName ) );
		return;
	}

	name = materialName;
	fileName = loaded.fileName;
	implicit = loaded.implicit;
	crlf = loaded.text.contains( "\r\n" );
	unsaved = false;
	// Latin-1 both ways: every byte is one character, so offsets into the text are the engine's
	openedText = QString::fromLatin1( loaded.text ).replace( QLatin1String( "\r\n" ), QLatin1String( "\n" ) );
	appliedText = openedText;

	title->setText( implicit
		? tr( "<b>%1</b><br>an implicit material: the engine made it from the texture of that name, it has no file" ).arg( name )
		: tr( "<b>%1</b><br>%2" ).arg( name, fileName ) );
	text->setPlainText( openedText );
	parseTimer.stop();
	RebuildKeys();
	status->setText( loaded.valid ? QString() : tr( "The engine could not parse this material and shows the default one." ) );
	UpdateState();
	RequestPreview();
}

QByteArray MaterialEditor::EngineText() const {
	QString out = text->toPlainText();
	if ( crlf ) {
		out.replace( QLatin1Char( '\n' ), QLatin1String( "\r\n" ) );
	}
	return out.toLatin1();
}

void MaterialEditor::RebuildKeys() {
	if ( name.isEmpty() ) {
		return;
	}
	const std::string source = text->toPlainText().toLatin1().toStdString();
	mtr::decl_t decl;
	std::string error;
	if ( !mtr::ParseDecl( source, decl, error ) ) {
		// the old list stays, but its offsets are stale
		keys->setEnabled( false );
		status->setText( tr( "The text is not a declaration: %1." ).arg( QString::fromStdString( error ) ) );
		return;
	}

	fillingKeys = true;
	keys->clear();
	auto * const root = new QTreeWidgetItem( keys );
	root->setText( 0, tr( "Material" ) );
	root->setFirstColumnSpanned( true );
	QFont bold = root->font( 0 );
	bold.setBold( true );
	root->setFont( 0, bold );
	AddLines( root, source, decl.body );
	AddBlocks( keys->invisibleRootItem(), source, decl.body, true );
	keys->expandAll();
	keys->setEnabled( true );
	fillingKeys = false;

	if ( status->text().startsWith( tr( "The text is not a declaration" ).left( 20 ) ) ) {
		status->clear();
	}
}

void MaterialEditor::ValueEdited( QTreeWidgetItem * item, int column ) {
	if ( fillingKeys || column != 1 || !item->data( 0, LINE_END_ROLE ).isValid() ) {
		return;
	}
	// typing since the last parse moved the offsets: parse first, and the edit is lost this once
	if ( parseTimer.isActive() ) {
		parseTimer.stop();
		RebuildKeys();
		return;
	}
	const int lineBegin = item->data( 0, LINE_BEGIN_ROLE ).toInt();
	const int valueBegin = item->data( 0, VALUE_BEGIN_ROLE ).toInt();
	const int lineEnd = item->data( 0, LINE_END_ROLE ).toInt();
	const int keyLength = item->data( 0, KEY_LENGTH_ROLE ).toInt();
	const QString value = item->text( 1 ).trimmed();

	// through the cursor, so that it is one step of the text's own undo
	QTextCursor cursor( text->document() );
	if ( valueBegin < lineEnd ) {
		cursor.setPosition( value.isEmpty() ? lineBegin + keyLength : valueBegin );
		cursor.setPosition( lineEnd, QTextCursor::KeepAnchor );
		cursor.insertText( value );
	} else if ( !value.isEmpty() ) {
		cursor.setPosition( lineEnd );
		cursor.insertText( QLatin1Char( '\t' ) + value );
	}
	parseTimer.stop();
	RebuildKeys();
}

bool MaterialEditor::Apply() {
	if ( name.isEmpty() ) {
		return false;
	}
	const QByteArray engineText = EngineText();
	const bool ok = client->Engine()->ApplyMaterialText( name.toLocal8Bit().constData(), engineText.constData() );
	appliedText = text->toPlainText();
	unsaved = true;
	status->setText( ok ? tr( "Applied. The file is not changed until Save." )
						: tr( "The engine did not take the text: see the Output Log. It shows the default material until a text parses." ) );
	thumbnails->Invalidate( EDITOR_PREVIEW_MATERIAL, name );
	UpdateState();
	RequestPreview();
	return ok;
}

void MaterialEditor::Save() {
	if ( name.isEmpty() || implicit || !Apply() ) {
		return;
	}
	if ( client->Engine()->SaveMaterial( name.toLocal8Bit().constData() ) ) {
		unsaved = false;
		status->setText( tr( "Saved to %1 in the project folder." ).arg( fileName ) );
	} else {
		status->setText( tr( "Not saved: see the Output Log." ) );
	}
	UpdateState();
}

void MaterialEditor::Revert() {
	if ( name.isEmpty() ) {
		return;
	}
	text->setPlainText( openedText );
	parseTimer.stop();
	RebuildKeys();
	static_cast<void>( Apply() );
}

void MaterialEditor::RequestPreview() {
	previewId++;
	client->Engine()->RequestPreview( previewId, EDITOR_PREVIEW_MATERIAL, name.toLocal8Bit().constData(), PREVIEW_SIZE );
}

void MaterialEditor::PreviewReady( int id, const QImage & image ) {
	// only the newest: an older picture shows an older text
	if ( id != previewId ) {
		return;
	}
	if ( image.isNull() ) {
		preview->setText( tr( "no picture:\nthe engine stopped\nwith an error" ) );
	} else {
		QPixmap pixmap = QPixmap::fromImage( image );
		pixmap.setDevicePixelRatio( static_cast<qreal>( image.width() ) / PREVIEW_SIZE );
		preview->setPixmap( pixmap );
	}
}

void MaterialEditor::UpdateState() {
	const bool open = !name.isEmpty();
	const bool edited = open && text->toPlainText() != appliedText;
	applyButton->setEnabled( edited );
	saveButton->setEnabled( open && !implicit && ( edited || unsaved ) );
	revertButton->setEnabled( open && ( edited || appliedText != openedText ) );
	text->setEnabled( open );
}
