/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "quakedef.h"

/* The PAPP sound shim implements CD tracks using MP3 files in id1/music/. */
extern void papp_music_play(int track, int looping);
extern void papp_music_stop(void);
extern void papp_music_pause(void);
extern void papp_music_resume(void);

void CDAudio_Play(byte track, qboolean looping)
{
	papp_music_play((int)track, looping != 0);
}


void CDAudio_Stop(void)
{
	papp_music_stop();
}


void CDAudio_Pause(void)
{
	papp_music_pause();
}


void CDAudio_Resume(void)
{
	papp_music_resume();
}


void CDAudio_Update(void)
{
}


int CDAudio_Init(void)
{
	return 0;
}


void CDAudio_Shutdown(void)
{
}
