/*
===========================================================================

Voice-over subtitles for RBDOOM-3-BFG (doom3bfg-cht).

When a sample under sound/vo/ starts playing within earshot of the listener,
look up "#sub_<sample path without extension>" in the language dictionary and
show its cues at the bottom of the screen, timed against the sound world clock
(so pausing the game pauses the subtitles).

Cue format in the .lang value, one cue per line ("\n" escape in the file):
	<start seconds>|<end seconds>|<text>

This file is part of RBDOOM-3-BFG and is distributed under the GNU GPL v3,
like the rest of the engine source.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "snd_local.h"
#include "../renderer/GLState.h"

bool Subtitles_GetConversation( const idSoundWorld* world, int& bodyEmitter, int& headEmitter, bool& talking );

idCVar s_subtitles( "s_subtitles", "1", CVAR_SOUND | CVAR_BOOL | CVAR_ARCHIVE, "show subtitles for voice-over" );
idCVar s_subtitleScale( "s_subtitleScale", "0.34", CVAR_SOUND | CVAR_FLOAT | CVAR_ARCHIVE, "subtitle text scale (48 pt font units)" );
idCVar s_subtitleFont( "s_subtitleFont", "", CVAR_SOUND, "subtitle font override (empty uses the localized UI font)" );
idCVar s_subtitleFallbackFont( "s_subtitleFallbackFont", "Arial_Narrow", CVAR_SOUND, "font used when no localized subtitle font is available" );
idCVar s_subtitleDebug( "s_subtitleDebug", "0", CVAR_SOUND | CVAR_BOOL, "print voice-over samples that have no subtitle" );

namespace
{

struct subCue_t
{
	int		startMs;
	int		endMs;
	idStr	text;
};

struct activeSub_t
{
	idSoundWorldLocal*	world;
	int					emitterIndex;
	int					startTime;		// sound world time the sample started
	int					endTime;		// last cue end, in sound world time
	idStr				name;
	idList<subCue_t>	cues;
};

idList<activeSub_t> activeSubs;

struct subtitleConversation_t
{
	idSoundWorldLocal*	world = NULL;
	int					bodyEmitter = -1;
	int					headEmitter = -1;
	bool				talking = false;
	int					lastStartTime = 0;

	bool Matches( const activeSub_t& sub ) const
	{
		return sub.world == world
			&& ( sub.emitterIndex == bodyEmitter || sub.emitterIndex == headEmitter )
			&& ( talking || sub.startTime <= lastStartTime );
	}
};

subtitleConversation_t conversation;

void ParseCues( const char* value, idList<subCue_t>& cues )
{
	idStr all = value;
	int pos = 0;
	while( pos < all.Length() )
	{
		int nl = all.Find( '\n', pos );
		if( nl < 0 )
		{
			nl = all.Length();
		}
		idStr line = all.Mid( pos, nl - pos );
		pos = nl + 1;

		int bar1 = line.Find( '|' );
		if( bar1 < 0 )
		{
			continue;
		}
		int bar2 = line.Find( '|', bar1 + 1 );
		if( bar2 < 0 )
		{
			continue;
		}
		subCue_t& cue = cues.Alloc();
		cue.startMs = idMath::Ftoi( atof( line.Left( bar1 ).c_str() ) * 1000.0f );
		cue.endMs = idMath::Ftoi( atof( line.Mid( bar1 + 1, bar2 - bar1 - 1 ).c_str() ) * 1000.0f );
		cue.text = line.Right( line.Length() - bar2 - 1 );
	}
}

// draws one line of UTF-8 text with its baseline at y, returns nothing
void DrawLine( const idFont* font, float scale, const idStr& text, float x, float y )
{
	int i = 0;
	while( i < text.Length() )
	{
		uint32 c = idStr::UTF8Char( text.c_str(), i );
		if( c == 0 )
		{
			break;
		}
		scaledGlyphInfo_t g;
		font->GetScaledGlyph( scale, c, g );
		if( g.material != NULL && g.width > 0.0f )
		{
			renderSystem->DrawStretchPic( x + g.left, y - g.top, g.width, g.height, g.s1, g.t1, g.s2, g.t2, g.material );
		}
		x += g.xSkip;
	}
}

float LineWidth( const idFont* font, float scale, const idStr& text )
{
	float w = 0.0f;
	int i = 0;
	while( i < text.Length() )
	{
		uint32 c = idStr::UTF8Char( text.c_str(), i );
		if( c == 0 )
		{
			break;
		}
		w += font->GetGlyphWidth( scale, c );
	}
	return w;
}

// break a cue into lines no wider than maxWidth; CJK text can break anywhere,
// latin words are kept together when possible
void WrapText( const idFont* font, float scale, const idStr& text, float maxWidth, idStrList& lines )
{
	idStr line;
	float lineW = 0.0f;
	int lastSpaceLen = -1;
	int i = 0;
	while( i < text.Length() )
	{
		int charStart = i;
		uint32 c = idStr::UTF8Char( text.c_str(), i );
		if( c == 0 )
		{
			break;
		}
		float cw = font->GetGlyphWidth( scale, c );
		if( lineW + cw > maxWidth && line.Length() > 0 )
		{
			if( c < 0x80 && lastSpaceLen > 0 )
			{
				// break at the last space of a latin run
				idStr rest = line.Right( line.Length() - lastSpaceLen - 1 );
				line.CapLength( lastSpaceLen );
				lines.Append( line );
				line = rest;
			}
			else
			{
				lines.Append( line );
				line.Empty();
			}
			lineW = LineWidth( font, scale, line );
			lastSpaceLen = -1;
			if( c == ' ' && line.Length() == 0 )
			{
				continue;
			}
		}
		if( c == ' ' )
		{
			lastSpaceLen = line.Length();
		}
		line.Append( text.c_str() + charStart, i - charStart );
		lineW += cw;
	}
	if( line.Length() > 0 )
	{
		lines.Append( line );
	}
}

}	// namespace

/*
========================
Subtitles_SoundStarted

Called by idSoundEmitterLocal::StartSound once a channel has been set up.
========================
*/
void Subtitles_SoundStarted( idSoundWorldLocal* world, const idVec3& origin, int emitterId, int emitterIndex, const soundShaderParms_t& parms, const char* sampleName, int startTime )
{
	if( !s_subtitles.GetBool() || world == NULL || sampleName == NULL )
	{
		return;
	}

	idStr name = sampleName;
	name.BackSlashesToSlashes();
	name.ToLower();
	if( name.Find( "sound/vo/" ) != 0 )
	{
		return;
	}
	name.StripFileExtension();

	// only subtitle what the player can actually hear
	const bool global = ( parms.soundShaderFlags & SSF_GLOBAL ) != 0;
	const bool local = ( emitterId == world->listener.id );
	if( !global && !local && parms.maxDistance > 0.0f )
	{
		const float distance = ( world->listener.pos - origin ).LengthFast() * DOOM_TO_METERS;
		if( distance >= parms.maxDistance )
		{
			return;
		}
	}

	const char* value = idLocalization::FindString( va( "#sub_%s", name.c_str() ) );
	if( value == NULL || value[0] == '\0' )
	{
		if( s_subtitleDebug.GetBool() )
		{
			idLib::Printf( "subtitle missing: %s\n", name.c_str() );
		}
		return;
	}

	// the same line restarting replaces the old one
	for( int i = activeSubs.Num() - 1; i >= 0; i-- )
	{
		if( activeSubs[i].world == world && activeSubs[i].emitterIndex == emitterIndex && activeSubs[i].name == name )
		{
			activeSubs.RemoveIndex( i );
		}
	}

	activeSub_t& sub = activeSubs.Alloc();
	sub.world = world;
	sub.emitterIndex = emitterIndex;
	sub.startTime = startTime;
	sub.name = name;
	ParseCues( value, sub.cues );
	sub.endTime = startTime;
	for( int i = 0; i < sub.cues.Num(); i++ )
	{
		sub.endTime = Max( sub.endTime, startTime + sub.cues[i].endMs );
	}
}

/*
========================
Subtitles_Clear
========================
*/
void Subtitles_Clear()
{
	activeSubs.Clear();
	conversation = {};
}

/*
========================
Subtitles_PrepareFont

Run on the main thread before the game/draw job starts.
========================
*/
idFont* Subtitles_PrepareFont()
{
	if( !s_subtitles.GetBool() )
	{
		return NULL;
	}

	const char* requestedFont = s_subtitleFont.GetString();
	const char* localizedFont = idLocalization::FindString( "#font_*" );
	const char* fallbackFont = localizedFont != NULL && localizedFont[0] != '\0'
		? localizedFont : s_subtitleFallbackFont.GetString();
	if( requestedFont[0] == '\0' || idStr::Icmp( requestedFont, fallbackFont ) == 0 )
	{
		// Use the same registered font as the UI, even if a file probe would fail.
		return renderSystem->RegisterFont( fallbackFont );
	}

	static idStr checkedFont;
	static bool fontExists = false;
	static int retryTime = 0;
	const int now = Sys_Milliseconds();
	if( checkedFont != requestedFont || ( !fontExists && now >= retryTime ) )
	{
		checkedFont = requestedFont;
		const idStr path = "newfonts/" + checkedFont + "/48.dat";
		fontExists = fileSystem->ReadFile( path.c_str(), NULL ) > 0;
		retryTime = now + 1000;
	}
	return renderSystem->RegisterFont( fontExists ? checkedFont.c_str() : fallbackFont );
}

/*
========================
Subtitles_Draw

Called from idCommonLocal::Draw after the game view.
========================
*/
void Subtitles_Draw( const idFont* font )
{
	idSoundWorldLocal* world = soundSystemLocal.currentSoundWorld;
	if( !s_subtitles.GetBool() || world == NULL )
	{
		Subtitles_Clear();
		return;
	}

	const int now = world->GetSoundTime();
	if( conversation.world != world )
	{
		conversation = {};
	}
	const bool wasTalking = conversation.talking;
	conversation.talking = false;
	if( Subtitles_GetConversation( world, conversation.bodyEmitter, conversation.headEmitter, conversation.talking ) )
	{
		conversation.world = world;
		if( !conversation.talking )
		{
			conversation.lastStartTime = now;
		}
	}
	else if( wasTalking )
	{
		conversation.lastStartTime = now;
	}

	// Keep the speaker selected through the last cue, including gaps between cues.
	bool onlySpeaker = conversation.talking;
	if( conversation.world != NULL && !onlySpeaker )
	{
		for( const activeSub_t& sub : activeSubs )
		{
			if( conversation.Matches( sub ) && now < sub.endTime && now >= sub.startTime - 1000 )
			{
				onlySpeaker = true;
				break;
			}
		}
		if( !onlySpeaker )
		{
			conversation = {};
		}
	}

	// Choose the active cue that started most recently in sound-world time.
	idStr shown;
	int latestStart = 0;
	for( int i = 0; i < activeSubs.Num(); i++ )
	{
		activeSub_t& sub = activeSubs[i];
		if( sub.world != world || now > sub.endTime || now < sub.startTime - 1000 )
		{
			activeSubs.RemoveIndex( i );
			i--;
			continue;
		}
		if( onlySpeaker && !conversation.Matches( sub ) )
		{
			continue;
		}
		const int t = now - sub.startTime;
		for( int j = 0; j < sub.cues.Num(); j++ )
		{
			const subCue_t& cue = sub.cues[j];
			const int cueStart = sub.startTime + cue.startMs;
			if( t >= cue.startMs && t < cue.endMs && !cue.text.IsEmpty()
				&& ( shown.IsEmpty() || cueStart >= latestStart ) )
			{
				shown = cue.text;
				latestStart = cueStart;
			}
		}
	}
	if( shown.IsEmpty() )
	{
		return;
	}

	if( font == NULL )
	{
		return;
	}

	const idMaterial* whiteMaterial = declManager->FindMaterial( "_white" );
	const float scale = s_subtitleScale.GetFloat();
	const float virtW = renderSystem->GetVirtualWidth();
	const float virtH = renderSystem->GetVirtualHeight();
	const float maxWidth = virtW * 0.8f;
	const float lineHeight = font->GetLineHeight( scale ) * 1.05f;
	const float ascender = font->GetAscender( scale );
	const float pad = 4.0f;

	idStrList lines;
	WrapText( font, scale, shown, maxWidth, lines );

	// Match the SWF text blend state for the font atlas.
	renderSystem->SetGLState( GLS_OVERRIDE | GLS_DEPTHFUNC_LESS | GLS_DEPTHMASK
		| GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	float y = virtH * 0.88f - ( lines.Num() - 1 ) * lineHeight;
	for( int i = 0; i < lines.Num(); i++ )
	{
		const float w = LineWidth( font, scale, lines[i] );
		const float x = ( virtW - w ) * 0.5f;

		renderSystem->SetColor4( 0.0f, 0.0f, 0.0f, 0.55f );
		renderSystem->DrawStretchPic( x - pad, y - ascender - pad * 0.5f, w + pad * 2.0f, lineHeight + pad * 0.5f, 0, 0, 1, 1, whiteMaterial );

		// 1 px drop shadow, then the text
		renderSystem->SetColor4( 0.0f, 0.0f, 0.0f, 1.0f );
		DrawLine( font, scale, lines[i], x + 1.0f, y + 1.0f );
		renderSystem->SetColor4( 1.0f, 1.0f, 1.0f, 1.0f );
		DrawLine( font, scale, lines[i], x, y );

		y += lineHeight;
	}
	renderSystem->SetColor4( 1.0f, 1.0f, 1.0f, 1.0f );
	renderSystem->SetGLState( 0 );
}
