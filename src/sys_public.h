/*
Copyright (C) 1997-2001 Id Software, Inc.

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

typedef struct {
    void    *base;
    size_t  maxsize;
    size_t  cursize;
    size_t  mapped;
} mempool_t;

// loads the dll and returns entry pointer
void    *Sys_LoadLibrary( const char *path, const char *sym, void **handle );
void    Sys_FreeLibrary( void *handle );
void    *Sys_GetProcAddress( void *handle, const char *sym );

unsigned    Sys_Milliseconds( void );
void    Sys_Sleep( int msec );

void    Hunk_Begin( mempool_t *pool, size_t maxsize );
void    *Hunk_Alloc( mempool_t *pool, size_t size );
void    Hunk_End( mempool_t *pool );
void    Hunk_Free( mempool_t *pool );

void    Sys_Init( void );
void    Sys_AddDefaultConfig( void );

#if USE_SYSCON
void    Sys_RunConsole( void );
void    Sys_ConsoleOutput( const char *string );
void    Sys_SetConsoleTitle( const char *title );
void    Sys_SetConsoleColor( color_index_t color );
void    Sys_Printf( const char *fmt, ... ) q_printf( 1, 2 );
#endif

void    Sys_Error( const char *error, ... ) q_noreturn q_printf( 1, 2 );
void    Sys_Quit( void ) q_noreturn;

void    Sys_ListFiles_r( const char *path, const char *filter,
            unsigned flags, size_t baselen, int *count_p, void **files, int depth );

struct  file_info_s;
qerror_t Sys_GetPathInfo( const char *path, struct file_info_s *info );
qerror_t Sys_GetFileInfo( FILE *fp, struct file_info_s *info );

void    Sys_DebugBreak( void );

#if USE_AC_CLIENT
qboolean Sys_GetAntiCheatAPI( void );
#endif

extern cvar_t   *sys_basedir;
extern cvar_t   *sys_libdir;
extern cvar_t   *sys_homedir;
extern cvar_t   *sys_forcegamelib;
#if USE_SYSCON && !(defined _WIN32)
extern cvar_t   *sys_console;
#endif

