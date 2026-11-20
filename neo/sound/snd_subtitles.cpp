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

bool Subtitles_GetConversation( const idSoundWorld* world, const idSoundEmitter*& bodyEmitter, const idSoundEmitter*& headEmitter, bool& talking );

idCVar s_subtitles( "s_subtitles", "1", CVAR_SOUND | CVAR_BOOL | CVAR_ARCHIVE, "show subtitles for voice-over" );
idCVar s_subtitleScale( "s_subtitleScale", "0.34", CVAR_SOUND | CVAR_FLOAT | CVAR_ARCHIVE, "subtitle text scale (48 pt font units)" );
idCVar s_subtitleFont( "s_subtitleFont", "", CVAR_SOUND, "subtitle font override (empty uses the installed translation font)" );
idCVar s_subtitleFallbackFont( "s_subtitleFallbackFont", "Arial_Narrow", CVAR_SOUND, "font used when no localized subtitle font is available" );
idCVar s_subtitleDebug( "s_subtitleDebug", "0", CVAR_SOUND | CVAR_BOOL, "print voice-over samples that have no subtitle" );

namespace
{

struct subCue_t
{
	int		startMs;
	int		endMs;
	bool	presented;
	idStr	text;
};

struct activeSub_t
{
	idSoundWorldLocal*	world;
	const idSoundChannel* playback;
	int					emitterId;
	int					startTime;		// sound world time the sample started
	int					endTime;		// last cue end, in sound world time
	idStr				name;
	idList<subCue_t>	cues;
};

idList<activeSub_t> activeSubs;
const idSoundChannel* visiblePlayback = NULL;
int visibleCue = -1;

struct conversationVoice_t
{
	idSoundWorldLocal* world;
	int emitterId;
	idStr name;
};

idList<conversationVoice_t> conversationVoices;

bool WasConversationVoice( const activeSub_t& sub )
{
	for( const conversationVoice_t& voice : conversationVoices )
	{
		if( voice.world == sub.world && voice.emitterId == sub.emitterId && voice.name == sub.name )
		{
			return true;
		}
	}
	return false;
}

struct subtitleConversation_t
{
	idSoundWorldLocal*	world = NULL;
	const idSoundEmitter* bodyEmitter = NULL;
	const idSoundEmitter* headEmitter = NULL;
	bool				talking = false;
	int					firstStartTime = 0;
	int					lastStartTime = 0;

	bool Matches( const activeSub_t& sub ) const
	{
		return sub.world == world
			&& ( sub.playback->emitter == bodyEmitter || sub.playback->emitter == headEmitter )
			&& sub.startTime >= firstStartTime
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
		cue.presented = false;
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

void Subtitles_BeginConversation( idSoundWorld* world )
{
	if( world != NULL )
	{
		conversation = {};
		conversation.world = static_cast<idSoundWorldLocal*>( world );
		conversation.firstStartTime = conversation.world->GetSoundTime();
	}
}

/*
========================
Subtitles_SoundStarted

Called by idSoundEmitterLocal::StartSound once a channel has been set up.
========================
*/
void Subtitles_SoundStarted( idSoundWorldLocal* world, const idSoundChannel* channel, int length )
{
	if( !s_subtitles.GetBool() || length <= 0 || ( channel->parms.soundShaderFlags & ( SSF_LOOPING | SSF_NO_SUBTITLES ) ) != 0 )
	{
		return;
	}

	idStr name = channel->leadinSample->GetName();
	name.BackSlashesToSlashes();
	name.ToLower();
	if( name.Find( "sound/vo/" ) != 0 )
	{
		return;
	}
	name.StripFileExtension();
	// These scripted announcements restart one-shot sounds instead of looping them.
	if( name == "sound/vo/marscity/sec_hangar_trigger" || name.Find( "sound/vo/marscity/comp_announce" ) == 0 )
	{
		return;
	}

	// only subtitle what the player can actually hear
	const idSoundEmitterLocal* emitter = channel->emitter;
	const bool global = ( channel->parms.soundShaderFlags & SSF_GLOBAL ) != 0;
	const bool local = ( emitter->emitterId == world->listener.id );
	if( !global && !local && channel->parms.maxDistance > 0.0f )
	{
		const float distance = ( world->listener.pos - emitter->origin ).LengthFast() * DOOM_TO_METERS;
		if( distance >= channel->parms.maxDistance )
		{
			return;
		}
	}

	const idStr key = "#sub_" + name;
	const char* value = idLocalization::FindString( key.c_str() );
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
		if( activeSubs[i].world == world && activeSubs[i].playback->emitter == emitter && activeSubs[i].name == name )
		{
			Subtitles_SoundStopped( activeSubs[i].playback );
		}
	}

	activeSub_t& sub = activeSubs.Alloc();
	sub.world = world;
	sub.playback = channel;
	sub.emitterId = emitter->emitterId;
	sub.startTime = channel->startTime;
	sub.name = name;
	ParseCues( value, sub.cues );
	sub.endTime = sub.startTime;
	for( int i = 0; i < sub.cues.Num(); i++ )
	{
		sub.endTime = Max( sub.endTime, sub.startTime + Min( length, sub.cues[i].endMs ) );
	}
}

void Subtitles_SoundStopped( const idSoundChannel* channel )
{
	for( int i = activeSubs.Num() - 1; i >= 0; --i )
	{
		if( activeSubs[i].playback == channel )
		{
			activeSubs.RemoveIndex( i );
		}
	}
	if( visiblePlayback == channel )
	{
		visiblePlayback = NULL;
		visibleCue = -1;
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
	conversationVoices.Clear();
	visiblePlayback = NULL;
	visibleCue = -1;
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

	const idStr requested = s_subtitleFont.GetString();
	const char* localizedName = idLocalization::FindString( "#font_*" );
	const idStr localized = localizedName != NULL ? localizedName : "";
	static idStr lastRequest;
	static idStr lastLocalized;
	static idStr resolved;
	static int retryTime = 0;
	const int now = Sys_Milliseconds();
	if( requested != lastRequest || localized != lastLocalized || ( resolved.IsEmpty() && now >= retryTime ) )
	{
		lastRequest = requested;
		lastLocalized = localized;
		const idFont* loaded = requested.IsEmpty() ? NULL : renderSystem->RegisterFont( requested.c_str(), true );
		// Use the installed translation's physical font data independently of the GUI aliases.
		if( loaded == NULL || !loaded->HasGlyph( 0x6211 ) )
		{
			const idFont* chinese = renderSystem->RegisterFont( "chinese", true );
			if( chinese != NULL && chinese->HasGlyph( 0x6211 ) )
			{
				loaded = chinese;
			}
		}
		if( loaded == NULL && !localized.IsEmpty() && localized.Icmp( "chinese" ) != 0 && localized.Icmp( requested ) != 0 )
		{
			loaded = renderSystem->RegisterFont( localized.c_str(), true );
		}
		resolved = loaded != NULL ? loaded->GetName() : "";
		retryTime = now + 1000;
	}
	idFont* font = resolved.IsEmpty() ? NULL : renderSystem->RegisterFont( resolved.c_str(), true );
	if( font == NULL )
	{
		resolved.Clear();
		return renderSystem->RegisterFont( s_subtitleFallbackFont.GetString() );
	}
	return font;
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
	for( int i = activeSubs.Num() - 1; i >= 0; --i )
	{
		const activeSub_t& sub = activeSubs[i];
		if( sub.world != world || now >= sub.endTime || now < sub.startTime - 1000 )
		{
			Subtitles_SoundStopped( sub.playback );
		}
	}
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
			if( conversation.Matches( sub ) )
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
	activeSub_t* selected = NULL;
	int selectedCue = -1;
	int latestStart = 0;
	for( int i = 0; i < activeSubs.Num(); i++ )
	{
		activeSub_t& sub = activeSubs[i];
		if( !conversation.Matches( sub ) && ( onlySpeaker || WasConversationVoice( sub ) ) )
		{
			continue;
		}
		const int t = now - sub.startTime;
		for( int j = 0; j < sub.cues.Num(); j++ )
		{
			const subCue_t& cue = sub.cues[j];
			const int cueStart = sub.startTime + cue.startMs;
			if( t >= cue.startMs && t < cue.endMs && !cue.text.IsEmpty()
				&& ( !cue.presented || ( sub.playback == visiblePlayback && j == visibleCue ) )
				&& ( selected == NULL || cueStart >= latestStart ) )
			{
				selected = &sub;
				selectedCue = j;
				latestStart = cueStart;
			}
		}
	}
	if( selected == NULL )
	{
		visiblePlayback = NULL;
		visibleCue = -1;
		return;
	}

	if( font == NULL )
	{
		return;
	}
	subCue_t& cue = selected->cues[selectedCue];
	if( !cue.presented && selected->emitterId > 0 && conversation.Matches( *selected ) && !WasConversationVoice( *selected ) )
	{
		conversationVoice_t& voice = conversationVoices.Alloc();
		voice.world = selected->world;
		voice.emitterId = selected->emitterId;
		voice.name = selected->name;
	}
	cue.presented = true;
	visiblePlayback = selected->playback;
	visibleCue = selectedCue;
	const idStr& shown = cue.text;

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

	// Match SWF text blending and draw the overlay independently of scene depth.
	renderSystem->SetGLState( GLS_OVERRIDE | GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK
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
