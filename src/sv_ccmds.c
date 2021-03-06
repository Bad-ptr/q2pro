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

#include "sv_local.h"

/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/

/*
====================
SV_SetMaster_f

Specify a list of master servers
====================
*/
static void SV_SetMaster_f( void ) {
    netadr_t adr;
    int     i, total;
    char    *s;
    master_t *m, *n;
    size_t len;

#if USE_CLIENT
    // only dedicated servers send heartbeats
    if( !dedicated->integer ) {
        Com_Printf( "Only dedicated servers use masters.\n" );
        return;
    }
#endif

    // free old masters
    FOR_EACH_MASTER_SAFE( m, n ) {
        Z_Free( m );
    }

    List_Init( &sv_masterlist );

    total = 0;
    for( i = 1; i < Cmd_Argc(); i++ ) {
        if( total == MAX_MASTERS ) {
            Com_Printf( "Too many masters.\n" );
            break;
        }

        s = Cmd_Argv( i );
        if( !NET_StringToAdr( s, &adr, PORT_MASTER ) ) {
            Com_Printf( "Bad master address: %s\n", s );
            continue;
        }

        FOR_EACH_MASTER( m ) {
            if( NET_IsEqualBaseAdr( &m->adr, &adr ) ) {
                Com_Printf( "Ignoring duplicate master at %s.\n", NET_AdrToString( &adr ) );
                goto out;
            }
        }

        Com_Printf( "Master server at %s.\n", NET_AdrToString( &adr ) );
        len = strlen( s );
        m = Z_Malloc( sizeof( *m ) + len );
        memcpy( m->name, s, len + 1 );
        m->adr = adr;
        m->last_ack = 0;
        m->last_resolved = time( NULL );
        List_Append( &sv_masterlist, &m->entry );
        total++;
out:;
    }

    if( total ) {
        // make sure the server is listed public
        Cvar_Set( "public", "1" );

        svs.last_heartbeat = svs.realtime - HEARTBEAT_SECONDS*1000;
    }
}

static void SV_ListMasters_f( void ) {
    master_t *m;
    char buf[8], *adr;
    int i;

    if( LIST_EMPTY( &sv_masterlist ) ) {
        Com_Printf( "There are no masters.\n" );
        return;
    }

    Com_Printf( "num hostname              lastmsg address\n"
                "--- --------------------- ------- ---------------------\n" );
    i = 0;
    FOR_EACH_MASTER( m ) {
        if( !svs.initialized ) {
            strcpy( buf, "down" );
        } else if( !m->last_ack ) {
            strcpy( buf, "never" );
        } else {
            Q_snprintf( buf, sizeof( buf ), "%u", svs.realtime - m->last_ack );
        }
        adr = m->adr.port ? NET_AdrToString( &m->adr ) : "error";
        Com_Printf( "%3d %-21.21s %7s %-21s\n", ++i, m->name, buf, adr );
    }
}

client_t *SV_GetPlayer( const char *s, qboolean partial ) {
    client_t    *other, *match;
    int         i, count;

    if( !s[0] ) {
        return NULL;
    }

    // numeric values are just slot numbers
    if( COM_IsUint( s ) ) {
        i = atoi( s );
        if( i < 0 || i >= sv_maxclients->integer ) {
            Com_Printf( "Bad client slot number: %d\n", i );
            return NULL;
        }

        other = &svs.client_pool[i];
        if( other->state <= cs_zombie ) {
            Com_Printf( "Client slot %d is not active.\n", i );
            return NULL;
        }
        return other;
    }

    // check for exact name match
    FOR_EACH_CLIENT( other ) {
        if( other->state <= cs_zombie ) {
            continue;
        }
        if( !strcmp( other->name, s ) ) {
            return other;
        }
    }

    if( !partial ) {
        Com_Printf( "Userid '%s' is not on the server.\n", s );
        return NULL;
    }

    // check for partial, case insensitive name match
    match = NULL;
    count = 0;
    FOR_EACH_CLIENT( other ) {
        if( other->state <= cs_zombie ) {
            continue;
        }
        if( !Q_stricmp( other->name, s ) ) {
            return other; // exact match
        }
        if( Q_stristr( other->name, s ) ) {
            match = other; // partial match
            count++;
        }
    }

    if( !match ) {
        Com_Printf( "No clients matching '%s' found.\n", s );
        return NULL;
    }

    if( count > 1 ) {
        Com_Printf( "'%s' matches multiple clients.\n", s );
        return NULL;
    }

    return match;
}

static void SV_Player_g( genctx_t *ctx ) {
    client_t *cl;

    if( !svs.initialized ) {
        return;
    }
    
    FOR_EACH_CLIENT( cl ) {
        if( cl->state <= cs_zombie ) {
            continue;
        }
        if( !Prompt_AddMatch( ctx, cl->name ) ) {
            break;
        }
    }
}

static void SV_SetPlayer_c( genctx_t *ctx, int argnum ) {
    if( argnum == 1 ) {
        SV_Player_g( ctx );
    }
}

/*
==================
SV_SetPlayer

Sets sv_client and sv_player to the player with idnum Cmd_Argv(1)
==================
*/
static qboolean SV_SetPlayer( void ) {
    client_t    *cl;

    cl = SV_GetPlayer( Cmd_Argv( 1 ), !!sv_enhanced_setplayer->integer );
    if( !cl ) {
        return qfalse;
    }

    sv_client = cl;
    sv_player = sv_client->edict;
    return qtrue;
}

//=========================================================

/*
======================
SV_Map

  the full syntax is:

  map [*]<map>$<startspot>+<nextserver>

command from the console or progs.
Map can also be a.cin, .pcx, or .dm2 file
Nextserver is used to allow a cinematic to play, then proceed to
another level:

    map tram.cin+jail_e3
======================
*/
static void SV_Map( int argnum, qboolean restart ) {
    char    mapcmd[MAX_QPATH];
    char    expanded[MAX_QPATH];
    char    *s, *ch, *spawnpoint;
    cm_t    cm;
    qerror_t ret;
    size_t  len;

    // save the mapcmd
    len = Cmd_ArgvBuffer( argnum, mapcmd, sizeof( mapcmd ) );
    if( len >= sizeof( mapcmd ) ) {
        Com_Printf( "Refusing to process oversize level string.\n" );
        return;
    }

    s = mapcmd;

    // if there is a + in the map, set nextserver to the remainder
    // we go directly to nextserver as we don't support cinematics
    ch = strchr( s, '+' );
    if( ch ) {
        s = ch + 1;
    }

    // skip the end-of-unit flag if necessary
    if( *s == '*' ) {
        s++;
    }

    // if there is a $, use the remainder as a spawnpoint
    ch = strchr( s, '$' );
    if( ch ) {
        *ch = 0;
        spawnpoint = ch + 1;
    } else {
        spawnpoint = mapcmd + len;
    }

    // now expand and try to load the map
    len = Q_concat( expanded, sizeof( expanded ), "maps/", s, ".bsp", NULL );
    if( len >= sizeof( expanded ) ) {
        ret = Q_ERR_NAMETOOLONG;
    } else {
        ret = CM_LoadMap( &cm, expanded );
    }

    if( ret ) {
        Com_Printf( "Couldn't load %s: %s\n", expanded, Q_ErrorString( ret ) );
        return;
    }

    // any error will drop from this point
    if( sv.state != ss_game || restart ) {
        SV_InitGame( MVD_SPAWN_DISABLED );  // the game is just starting
    }

    SV_SpawnServer( &cm, s, spawnpoint );
}

/*
==================
SV_DemoMap_f

Puts the server in demo mode on a specific map/cinematic
==================
*/
static void SV_DemoMap_f( void ) {
    Com_Printf( "'%s' command is no longer supported.\n", Cmd_Argv( 0 ) );
#if USE_CLIENT
    Com_Printf( "To play a client demo, use 'demo' command instead.\n" );
#endif
#if USE_MVD_CLIENT
    Com_Printf( "To play a MVD, use 'mvdplay' command.\n" );
#endif
}

/*
==================
SV_GameMap_f

Saves the state of the map just being exited and goes to a new map.

If the initial character of the map string is '*', the next map is
in a new unit, so the current savegame directory is cleared of
map files.

Example:

*inter.cin+jail

Clears the archived maps, plays the inter.cin cinematic, then
goes to map jail.bsp.
==================
*/
static void SV_GameMap_f( void ) {
    if( Cmd_Argc() != 2 ) {
        Com_Printf( "Usage: %s <mapname>\n", Cmd_Argv( 0 ) );
        return;
    }

#if !USE_CLIENT
    // admin option to reload the game DLL or entire server
    if( sv_recycle->integer > 0 ) {
        if( sv_recycle->integer > 1 ) {
            Com_Quit( NULL, ERR_RECONNECT );
        }
        SV_Map( 1, qtrue );
        return;
    }
#endif
    
    SV_Map( 1, qfalse );
}

/*
==================
SV_Map_f

Goes directly to a given map without any savegame archiving.
For development work
==================
*/
static void SV_Map_f( void ) {
    if( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <mapname>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( sv.state == ss_game &&
        sv_allow_map->integer != 1 &&
#if !USE_CLIENT
        sv_recycle->integer == 0 &&
#endif
        Cvar_CountLatchedVars() == 0 &&
        strcmp( Cmd_Argv( 2 ), "force" ) != 0 )
    {
        if( sv_allow_map->integer == 0 ) {
            static qboolean warned;

            Com_Printf(
                "Using '%s' command will cause full server restart, "
                "which is likely not what you want. Use 'gamemap' "
                "command for changing maps.\n", Cmd_Argv( 0 ) );
            if( !warned ) {
                Com_Printf(
                    "(You can set 'sv_allow_map' to 1 "
                    "if you wish to disable this check.)\n" );
                warned = qtrue;
            }
            return;
        }

        SV_Map( 1, qfalse );
        return;
    }

    SV_Map( 1, qtrue );
}

static void SV_Map_c( genctx_t *ctx, int argnum ) {
    if( argnum == 1 ) {
        FS_File_g( "maps", ".bsp", FS_SEARCH_STRIPEXT, ctx );
    }
}

static void SV_DumpEnts_f( void ) {
    bsp_t *c = sv.cm.cache;
    char buffer[MAX_OSPATH];

    if( !c || !c->entitystring ) {
        Com_Printf( "No map loaded.\n" );
        return;
    }

    if( Cmd_Argc() != 2 ) {
        Com_Printf( "Usage: %s <filename>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( FS_EasyWriteFile( buffer, sizeof( buffer ), FS_MODE_WRITE,
        "maps/", Cmd_Argv( 1 ), ".ent", c->entitystring, c->numentitychars ) )
    {
        Com_Printf( "Dumped entity string to %s\n", buffer );
    }
}

//===============================================================

/*
==================
SV_Kick_f

Kick a user off of the server
==================
*/
static void SV_Kick_f( void ) {
    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }

    if( Cmd_Argc() != 2 ) {
        Com_Printf( "Usage: %s <userid>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( !SV_SetPlayer() )
        return;

    SV_DropClient( sv_client, "kicked" );
    sv_client->lastmessage = svs.realtime;    // min case there is a funny zombie

    // optionally ban their IP address
    if( !strcmp( Cmd_Argv( 0 ), "kickban" ) ) {
        netadr_t *addr = &sv_client->netchan->remote_address;
        if( addr->type == NA_IP ) {
            addrmatch_t *match = Z_Malloc( sizeof( *match ) );
            match->addr.u32 = addr->ip.u32;
            match->mask = 0xffffffffU;
            match->hits = 0;
            match->time = 0;
            match->comment[0] = 0;
            List_Append( &sv_banlist, &match->entry );
        }
    }

    sv_client = NULL;
    sv_player = NULL;
}

static void dump_clients( void ) {
    client_t    *client;

    Com_Printf(
"num score ping name            lastmsg address                rate pr fps\n"
"--- ----- ---- --------------- ------- --------------------- ----- -- ---\n" );
    FOR_EACH_CLIENT( client ) {
        Com_Printf( "%3i %5i ", client->number,
            client->edict->client->ps.stats[STAT_FRAGS] );

        switch( client->state ) {
        case cs_zombie:
            Com_Printf( "ZMBI " );
            break;
        case cs_assigned:
            Com_Printf( "ASGN " );
            break;
        case cs_connected:
        case cs_primed:
            if( client->download ) {
                Com_Printf( "DNLD " );
            } else if( client->http_download ) {
                Com_Printf( "HTTP " );
            } else if( client->state == cs_connected ) {
                Com_Printf( "CNCT " );
            } else {
                Com_Printf( "PRIM " );
            }
            break;
        default:
            Com_Printf( "%4i ", client->ping < 9999 ? client->ping : 9999 );
            break;
        }

        Com_Printf( "%-15.15s ", client->name );
        Com_Printf( "%7u ", svs.realtime - client->lastmessage );
        Com_Printf( "%-21s ", NET_AdrToString(
            &client->netchan->remote_address ) );
        Com_Printf( "%5"PRIz" ", client->rate );
        Com_Printf( "%2i ", client->protocol );
        Com_Printf( "%3i ", client->fps );
        Com_Printf( "\n" );
    }
}

static void dump_versions( void ) {
    client_t    *client;

    Com_Printf(
"num name            version\n"
"--- --------------- -----------------------------------------\n" );

    FOR_EACH_CLIENT( client ) {
        Com_Printf( "%3i %-15.15s %-40.40s\n",
            client->number, client->name,
            client->versionString ? client->versionString : "-" );
    }
}

static void dump_downloads( void ) {
    client_t    *client;
    int         size, percent;
    char        *name;

    Com_Printf(
"num name            download                                 size    done\n"
"--- --------------- ---------------------------------------- ------- ----\n" );

    FOR_EACH_CLIENT( client ) {
        if( client->download ) {
            name = client->downloadname;
            size = client->downloadsize;
            if( !size )
                size = 1;
            percent = client->downloadcount*100/size;
        } else if( client->http_download ) {
            name = "<HTTP download>";
            size = percent = 0;
        } else {
            continue;
        }
        Com_Printf( "%3i %-15.15s %-40.40s %-7d %3d%%\n",
            client->number, client->name, name, size, percent );
    }
}

static void dump_time( void ) {
    client_t    *client;
    char        buffer[MAX_QPATH];
    time_t      clock = time( NULL );

    Com_Printf(
"num name            time\n"
"--- --------------- --------\n" );

    FOR_EACH_CLIENT( client ) {
        Com_TimeDiff( buffer, sizeof( buffer ),
            &client->connect_time, clock );
        Com_Printf( "%3i %-15.15s %s\n",
            client->number, client->name, buffer );
    }
}

static void dump_lag( void ) {
    client_t    *cl;

#ifdef USE_PACKETDUP
#define PD1 " dup"
#define PD2 " ---"
#define PD3 " %3d"
#else
#define PD1
#define PD2
#define PD3
#endif

    Com_Printf(
"num name            PLs2c PLc2s Rmin Ravg Rmax"PD1"\n"
"--- --------------- ----- ----- ---- ---- ----"PD2"\n" );

    FOR_EACH_CLIENT( cl ) {
        Com_Printf( "%3i %-15.15s %5.2f %5.2f %4d %4d %4d"PD3"\n",
            cl->number, cl->name, PL_S2C( cl ), PL_C2S( cl ), 
            cl->min_ping, AVG_PING( cl ), cl->max_ping
#ifdef USE_PACKETDUP
            , cl->numpackets - 1
#endif
            );
    }
}

static void dump_protocols( void ) {
    client_t    *cl;

    Com_Printf(
"num name            major minor msglen zlib chan\n"
"--- --------------- ----- ----- ------ ---- ----\n" );

    FOR_EACH_CLIENT( cl ) {
        Com_Printf( "%3i %-15.15s %5d %5d %6"PRIz"  %s  %s\n",
            cl->number, cl->name, cl->protocol, cl->version,
            cl->netchan->maxpacketlen,
            cl->has_zlib ? "yes" : "no ",
            cl->netchan->type ? "new" : "old" );
    }
}

static void dump_settings( void ) {
    client_t    *cl;
    char        opt[8];

    Com_Printf(
"num name            proto options upd fps\n"
"--- --------------- ----- ------- --- ---\n" );

    opt[6] = ' ';
    opt[7] = 0;
    FOR_EACH_CLIENT( cl ) {
        opt[0] = cl->settings[CLS_NOGUN]          ? 'G' : ' ';
        opt[1] = cl->settings[CLS_NOBLEND]        ? 'B' : ' ';
        opt[2] = cl->settings[CLS_RECORDING]      ? 'R' : ' ';
        opt[3] = cl->settings[CLS_NOGIBS]         ? 'I' : ' ';
        opt[4] = cl->settings[CLS_NOFOOTSTEPS]    ? 'F' : ' ';
        opt[5] = cl->settings[CLS_NOPREDICT]      ? 'P' : ' ';
        Com_Printf( "%3i %-15.15s %5d %s %3d %3d\n",
            cl->number, cl->name, cl->protocol, opt,
            cl->settings[CLS_PLAYERUPDATES], cl->settings[CLS_FPS] );
    }
}

/*
================
SV_Status_f
================
*/
static void SV_Status_f( void ) {
    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }

    if( sv.name[0] ) {
        Com_Printf( "Current map: %s\n\n", sv.name );
    }

    if( LIST_EMPTY( &sv_clientlist ) ) {
        Com_Printf( "No UDP clients.\n" );
    } else {
        if( Cmd_Argc() > 1 ) {
            char *w = Cmd_Argv( 1 );
            switch( *w ) {
                case 't': dump_time(); break;
                case 'd': dump_downloads(); break;
                case 'l': dump_lag(); break;
                case 'p': dump_protocols(); break;
                case 's': dump_settings(); break;
                default: dump_versions(); break;
            }
        } else {
            dump_clients();
        }
    }
    Com_Printf( "\n" );

#if USE_MVD_SERVER
    SV_MvdStatus_f();
#endif
}

/*
==================
SV_ConSay_f
==================
*/
static void SV_ConSay_f( void ) {
    client_t *client;
    char *s;

    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }

    if( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <raw text>\n", Cmd_Argv( 0 ) );
        return;
    }

    s = Cmd_RawArgs();
    FOR_EACH_CLIENT( client ) {
        if( client->state != cs_spawned )
            continue;
        SV_ClientPrintf( client, PRINT_CHAT, "console: %s\n", s );
    }

    if( Com_IsDedicated() ) {
        Com_LPrintf( PRINT_TALK, "console: %s\n", s );
    }
}


/*
==================
SV_Heartbeat_f
==================
*/
static void SV_Heartbeat_f( void ) {
    svs.last_heartbeat = svs.realtime - HEARTBEAT_SECONDS*1000;
}


/*
===========
SV_Serverinfo_f

  Examine or change the serverinfo string
===========
*/
static void SV_Serverinfo_f( void ) {
    char serverinfo[MAX_INFO_STRING];

    Cvar_BitInfo( serverinfo, CVAR_SERVERINFO );

    Com_Printf( "Server info settings:\n" );
    Info_Print( serverinfo );
}

void SV_PrintMiscInfo( void ) {
    char buffer[MAX_QPATH];

    Com_Printf( "version              %s\n",
        sv_client->versionString ? sv_client->versionString : "-" );
    Com_Printf( "protocol (maj/min)   %d/%d\n",
        sv_client->protocol, sv_client->version );
    Com_Printf( "maxmsglen            %"PRIz"\n", sv_client->netchan->maxpacketlen );
    Com_Printf( "zlib support         %s\n", sv_client->has_zlib ? "yes" : "no" );
    Com_Printf( "netchan type         %s\n", sv_client->netchan->type ? "new" : "old" );
    Com_Printf( "ping                 %d\n", sv_client->ping );
    Com_Printf( "movement fps         %d\n", sv_client->fps );
#if USE_FPS
    Com_Printf( "update rate          %d\n", sv_client->settings[CLS_FPS] );
#endif
    Com_Printf( "RTT (min/avg/max)    %d/%d/%d ms\n",
        sv_client->min_ping, AVG_PING( sv_client ), sv_client->max_ping );
    Com_Printf( "PL server to client  %.2f%% (approx)\n", PL_S2C( sv_client ) );
    Com_Printf( "PL client to server  %.2f%%\n", PL_C2S( sv_client ) );
#ifdef USE_PACKETDUP
    Com_Printf( "packetdup            %d\n", sv_client->numpackets - 1 );
#endif
    Com_TimeDiff( buffer, sizeof( buffer ),
        &sv_client->connect_time, time( NULL ) );
    Com_Printf( "connection time      %s\n", buffer );
}

/*
===========
SV_DumpUser_f

Examine all a users info strings
===========
*/
static void SV_DumpUser_f( void ) {
    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }

    if( Cmd_Argc() != 2 ) {
        Com_Printf( "Usage: %s <userid>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( !SV_SetPlayer() )
        return;

    Com_Printf( "\nuserinfo\n" );
    Com_Printf( "--------\n" );
    Info_Print( sv_client->userinfo );

    Com_Printf( "\nmiscinfo\n" );
    Com_Printf( "--------\n" );
    SV_PrintMiscInfo();

    sv_client = NULL;
    sv_player = NULL;
}

/*
==================
SV_Stuff_f

Stuff raw command string to the client.
==================
*/
static void SV_Stuff_f( void ) {
    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }

    if( Cmd_Argc() < 3 ) {
        Com_Printf( "Usage: %s <userid> <raw text>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( !SV_SetPlayer() )
        return;

    MSG_WriteByte( svc_stufftext );
    MSG_WriteString( Cmd_RawArgsFrom( 2 ) );
    SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );

    sv_client = NULL;
    sv_player = NULL;
}

/*
==================
SV_Stuff_f

Stuff raw command string to all clients.
==================
*/
static void SV_Stuffall_f( void ) {
    client_t *client;

    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }

    if( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <raw text>\n", Cmd_Argv( 0 ) );
        return;
    }

    MSG_WriteByte( svc_stufftext );
    MSG_WriteString( Cmd_RawArgsFrom( 1 ) );

    FOR_EACH_CLIENT( client ) {
        SV_ClientAddMessage( client, MSG_RELIABLE );
    }

    SZ_Clear( &msg_write );

}

static void SV_PickClient_f( void ) {
    char *s;
    netadr_t address;

    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }
    if( sv_maxclients->integer == 1 ) {
        Com_Printf( "Single player server running.\n" );
        return;
    }

    if ( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <address>\n", Cmd_Argv( 0 ) );
        return;
    }

    s = Cmd_Argv( 1 );
    if ( !NET_StringToAdr( s, &address, 0 ) ) {
        Com_Printf( "Bad client address: %s\n", s );
        return;
    }
    if ( address.port == 0 ) {
        Com_Printf( "Please specify client port explicitly.\n" );
        return;
    }

    OOB_PRINT( NS_SERVER, &address, "passive_connect\n" );
}


/*
===============
SV_KillServer_f

Kick everyone off, possibly in preparation for a new game
===============
*/
static void SV_KillServer_f( void ) {
    if( !svs.initialized ) {
        Com_Printf( "No server running.\n" );
        return;
    }

    SV_Shutdown( "Server was killed.\n", ERR_DISCONNECT );
}

/*
===============
SV_ServerCommand_f

Let the game dll handle a command
===============
*/
static void SV_ServerCommand_f( void ) {
    if( !ge ) {
        Com_Printf( "No game loaded.\n" );
        return;
    }

    ge->ServerCommand();
}

// ( ip & mask ) == ( addr & mask )
// bits = 32 --> mask = 255.255.255.255
// bits = 24 --> mask = 255.255.255.0

static qboolean parse_mask( const char *s, uint32_t *addr, uint32_t *mask ) {
    netadr_t address;
    char *p;
    int bits;

    p = strchr( s, '/' );
    if( p ) {
        *p++ = 0;
        if( *p == 0 ) {
            Com_Printf( "Please specify a mask after '/'.\n" );
            return qfalse;
        }
        bits = atoi( p );
        if( bits < 1 || bits > 32 ) {
            Com_Printf( "Bad mask: %d bits\n", bits );
            return qfalse;
        }
    } else {
        bits = 32;
    }

    if( !NET_StringToAdr( s, &address, 0 ) ) {
        Com_Printf( "Bad address: %s\n", s );
        return qfalse;
    }

    *addr = address.ip.u32;
    *mask = BigLong( ~( ( 1 << ( 32 - bits ) ) - 1 ) );
    return qtrue;
}

static size_t format_mask( addrmatch_t *match, char *buf, size_t size ) {
    uint8_t *ip = match->addr.u8;
    uint32_t mask = BigLong( match->mask );
    int i;

    for( i = 0; i < 32; i++ ) {
        if( mask & ( 1 << i ) ) {
            break;
        }
    }

    return Q_snprintf( buf, size, "%d.%d.%d.%d/%d",
        ip[0], ip[1], ip[2], ip[3], 32 - i );
}

void SV_AddMatch_f( list_t *list ) {
    char *s, buf[32];
    addrmatch_t *match;
    uint32_t addr, mask;
    size_t len;

    if( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <address[/mask]> [comment]\n", Cmd_Argv( 0 ) );
        return;
    }

    s = Cmd_Argv( 1 );
    if( !parse_mask( s, &addr, &mask ) ) {
        return;
    }

    LIST_FOR_EACH( addrmatch_t, match, list, entry ) {
        if( match->addr.u32 == addr && match->mask == mask ) {
            format_mask( match, buf, sizeof( buf ) );
            Com_Printf( "Entry %s already exists.\n", buf );
            return;
        }
    }

    s = Cmd_ArgsFrom( 2 );
    len = strlen( s );
    match = Z_Malloc( sizeof( *match ) + len );
    match->addr.u32 = addr;
    match->mask = mask;
    match->hits = 0;
    match->time = 0;
    memcpy( match->comment, s, len + 1 );
    List_Append( list, &match->entry );
}

void SV_DelMatch_f( list_t *list ) {
    char *s;
    addrmatch_t *match, *next;
    uint32_t addr, mask;
    int i;

    if( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <address[/mask]|id|all>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( LIST_EMPTY( list ) ) {
        Com_Printf( "Address list is empty.\n" );
        return;
    }

    s = Cmd_Argv( 1 );
    if( !strcmp( s, "all" ) ) {
        LIST_FOR_EACH_SAFE( addrmatch_t, match, next, list, entry ) {
            Z_Free( match );
        }
        List_Init( list );
        return;
    }

    // numeric values are just slot numbers
    if( COM_IsUint( s ) ) {
        i = atoi( s );
        if( i < 1 ) {
            Com_Printf( "Bad index: %d\n", i );
            return;
        }
        match = LIST_INDEX( addrmatch_t, i - 1, list, entry );
        if( match ) {
            goto remove;
        }
        Com_Printf( "No such index: %d\n", i );
        return;
    }

    if( !parse_mask( s, &addr, &mask ) ) {
        return;
    }

    LIST_FOR_EACH( addrmatch_t, match, list, entry ) {
        if( match->addr.u32 == addr && match->mask == mask ) {
remove:
            List_Remove( &match->entry );
            Z_Free( match );
            return;
        }
    }
    Com_Printf( "No such entry: %s\n", s );
}

void SV_ListMatches_f( list_t *list ) {
    addrmatch_t *match;
    char last[32];
    char addr[32];
    int count;

    if( LIST_EMPTY( list ) ) {
        Com_Printf( "Address list is empty.\n" );
        return;
    }

    Com_Printf( "id address/mask       hits last hit     comment\n"
                "-- ------------------ ---- ------------ -------\n" );
    count = 1;
    LIST_FOR_EACH( addrmatch_t, match, list, entry ) {
        format_mask( match, addr, sizeof( addr ) );
        if( !match->time ) {
            strcpy( last, "never" );
        } else {
            strftime( last, sizeof( last ), "%d %b %H:%M",
                localtime( &match->time ) );
        }
        Com_Printf( "%-2d %-18s %-4u %-12s %s\n", count, addr,
            match->hits, last, match->comment );
        count++;
    }
}

static void SV_AddBan_f( void ) {
    SV_AddMatch_f( &sv_banlist );
}
static void SV_DelBan_f( void ) {
    SV_DelMatch_f( &sv_banlist );
}
static void SV_ListBans_f( void ) {
    SV_ListMatches_f( &sv_banlist );
}

static void SV_AddBlackHole_f( void ) {
    SV_AddMatch_f( &sv_blacklist );
}
static void SV_DelBlackHole_f( void ) {
    SV_DelMatch_f( &sv_blacklist );
}
static void SV_ListBlackHoles_f( void ) {
    SV_ListMatches_f( &sv_blacklist );
}

static list_t *SV_FindStuffList( void ) {
    char *s = Cmd_Argv( 1 );

    if( !strcmp( s, "connect" ) ) {
        return &sv_cmdlist_connect;
    }
    if( !strcmp( s, "begin" ) ) {
        return &sv_cmdlist_begin;
    }
    Com_Printf( "Unknown stuffcmd list: %s\n", s );
    return NULL;
}

static void SV_AddStuffCmd_f( void ) {
    char *s;
    list_t *list;
    stuffcmd_t *stuff;
    int len;

    if( Cmd_Argc() < 3 ) {
        Com_Printf( "Usage: %s <list> <command>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( ( list = SV_FindStuffList() ) == NULL ) {
        return;
    }

    s = Cmd_ArgsFrom( 2 );
    len = strlen( s );
    stuff = Z_Malloc( sizeof( *stuff ) + len );
    stuff->len = len;
    memcpy( stuff->string, s, len + 1 );
    List_Append( list, &stuff->entry );
}

static void SV_DelStuffCmd_f( void ) {
    list_t *list;
    stuffcmd_t *stuff, *next;
    char *s;
    int i;

    if( Cmd_Argc() < 3 ) {
        Com_Printf( "Usage: %s <list> <id|all>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( ( list = SV_FindStuffList() ) == NULL ) {
        return;
    }

    if( LIST_EMPTY( list ) ) {
        Com_Printf( "No stuffcmds registered.\n" );
        return;
    }

    s = Cmd_Argv( 2 );
    if( !strcmp( s, "all" ) ) {
        LIST_FOR_EACH_SAFE( stuffcmd_t, stuff, next, list, entry ) {
            Z_Free( stuff );
        }
        List_Init( list );
        return;
    }
    i = atoi( s );
    if( i < 1 ) {
        Com_Printf( "Bad stuffcmd index: %d\n", i );
        return;
    }
    stuff = LIST_INDEX( stuffcmd_t, i - 1, list, entry );
    if( !stuff ) {
        Com_Printf( "No such stuffcmd index: %d\n", i );
        return;
    }

    List_Remove( &stuff->entry );
    Z_Free( stuff );
}

static void SV_ListStuffCmds_f( void ) {
    list_t *list;
    stuffcmd_t *stuff;
    int count;

    if( Cmd_Argc() != 2 ) {
        Com_Printf( "Usage: %s <list>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( ( list = SV_FindStuffList() ) == NULL ) {
        return;
    }

    if( LIST_EMPTY( list ) ) {
        Com_Printf( "No stuffcmds registered.\n" );
        return;
    }

    Com_Printf( "id command\n"
                "-- -------\n" );
    count = 1;
    LIST_FOR_EACH( stuffcmd_t, stuff, list, entry ) {
        Com_Printf( "%-2d %s\n", count, stuff->string );
        count++;
    }
}

static void SV_StuffCmd_c( genctx_t *ctx, int argnum ) {
    if( argnum == 1 ) {
        Prompt_AddMatch( ctx, "connect" );
        Prompt_AddMatch( ctx, "begin" );
    }
}

static const char filteractions[FA_MAX][8] = {
    "ignore", "print", "stuff", "kick"
};

static void SV_AddFilterCmd_f( void ) {
    char *s, *comment;
    filtercmd_t *filter;
    filteraction_t action;
    size_t len;

    if( Cmd_Argc() < 2 ) {
usage:
        Com_Printf( "Usage: %s <command> [ignore|print|stuff|kick] [comment]\n", Cmd_Argv( 0 ) );
        return;
    }

    if( Cmd_Argc() > 2 ) {
        s = Cmd_Argv( 2 );
        for( action = 0; action < FA_MAX; action++ ) {
            if( !strcmp( s, filteractions[action] ) ) {
                break;
            }
        }
        if( action == FA_MAX ) {
            goto usage;
        }
        comment = Cmd_ArgsFrom( 3 );
    } else {
        action = FA_IGNORE;
        comment = NULL;
    }


    s = Cmd_Argv( 1 );
    LIST_FOR_EACH( filtercmd_t, filter, &sv_filterlist, entry ) {
        if( !Q_stricmp( filter->string, s ) ) {
            Com_Printf( "Filtercmd already exists: %s\n", s );
            return;
        }
    }
    len = strlen( s );
    filter = Z_Malloc( sizeof( *filter ) + len );
    memcpy( filter->string, s, len + 1 );
    filter->action = action;
    filter->comment = Z_CopyString( comment );
    List_Append( &sv_filterlist, &filter->entry );
}

static void SV_AddFilterCmd_c( genctx_t *ctx, int argnum ) {
    filteraction_t action;

    if( argnum == 2 ) {
        for( action = 0; action < FA_MAX; action++ ) {
            Prompt_AddMatch( ctx, filteractions[action] );
        }
    }
}

static void SV_DelFilterCmd_f( void ) {
    filtercmd_t *filter, *next;
    char *s;
    int i;

    if( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: %s <id|cmd|all>\n", Cmd_Argv( 0 ) );
        return;
    }

    if( LIST_EMPTY( &sv_filterlist ) ) {
        Com_Printf( "No filtercmds registered.\n" );
        return;
    }

    s = Cmd_Argv( 1 );
    if( !strcmp( s, "all" ) ) {
        LIST_FOR_EACH_SAFE( filtercmd_t, filter, next, &sv_filterlist, entry ) {
            Z_Free( filter->comment );
            Z_Free( filter );
        }
        List_Init( &sv_filterlist );
        return;
    }
    if( COM_IsUint( s ) ) {
        i = atoi( s );
        if( i < 1 ) {
            Com_Printf( "Bad filtercmd index: %d\n", i );
            return;
        }
        filter = LIST_INDEX( filtercmd_t, i - 1, &sv_filterlist, entry );
        if( !filter ) {
            Com_Printf( "No such filtercmd index: %d\n", i );
            return;
        }
    } else {
        LIST_FOR_EACH( filtercmd_t, filter, &sv_filterlist, entry ) {
            if( !Q_stricmp( filter->string, s ) ) {
                goto remove;
            }
        }
        Com_Printf( "No such filtercmd string: %s\n", s );
        return;
    }

remove:
    List_Remove( &filter->entry );
    Z_Free( filter->comment );
    Z_Free( filter );
}

static void SV_DelFilterCmd_c( genctx_t *ctx, int argnum ) {
    filtercmd_t *filter;

    if( argnum == 1 ) {
        if( LIST_EMPTY( &sv_filterlist ) ) {
            return;
        }
        ctx->ignorecase = qtrue;
        Prompt_AddMatch( ctx, "all" );
        LIST_FOR_EACH( filtercmd_t, filter, &sv_filterlist, entry ) {
            if( !Prompt_AddMatch( ctx, filter->string ) ) {
                break;
            }
        }
    }
}

static void SV_ListFilterCmds_f( void ) {
    filtercmd_t *filter;
    int count;

    if( LIST_EMPTY( &sv_filterlist ) ) {
        Com_Printf( "No filtercmds registered.\n" );
        return;
    }

    Com_Printf( "id command          action comment\n"
                "-- ---------------- ------ -------\n" );
    count = 1;
    LIST_FOR_EACH( filtercmd_t, filter, &sv_filterlist, entry ) {
        Com_Printf( "%-2d %-16s %-6s %s\n", count,
            filter->string, filteractions[filter->action],
            filter->comment ? filter->comment : "" );
        count++;
    }
}

#if USE_MVD_CLIENT || USE_MVD_SERVER

const cmd_option_t o_record[] = {
    { "h", "help", "display this message" },
    { "z", "compress", "compress file with gzip" },
    { NULL }
};

static void SV_Record_c( genctx_t *ctx, int argnum ) {
#if USE_MVD_CLIENT
    // TODO
    if( argnum == 1 ) {
        MVD_File_g( ctx );
    }
#endif
}

static void SV_Record_f( void ) {
#if USE_MVD_CLIENT
    if( sv.state == ss_broadcast ) {
        MVD_StreamedRecord_f();
        return;
    }
#endif
#if USE_MVD_SERVER
    SV_MvdRecord_f();
#endif
}

static void SV_Stop_f( void ) {
#if USE_MVD_CLIENT
    if( sv.state == ss_broadcast ) {
        MVD_StreamedStop_f();
        return;
    }
#endif
#if USE_MVD_SERVER
    SV_MvdStop_f();
#endif
}

#endif

//===========================================================

static const cmdreg_t c_server[] = {
    { "heartbeat", SV_Heartbeat_f },
    { "kick", SV_Kick_f, SV_SetPlayer_c },
    { "kickban", SV_Kick_f, SV_SetPlayer_c },
    { "status", SV_Status_f },
    { "serverinfo", SV_Serverinfo_f },
    { "dumpuser", SV_DumpUser_f, SV_SetPlayer_c },
    { "stuff", SV_Stuff_f, SV_SetPlayer_c },
    { "stuffall", SV_Stuffall_f },
    { "map", SV_Map_f, SV_Map_c },
    { "demomap", SV_DemoMap_f },
    { "gamemap", SV_GameMap_f, SV_Map_c },
    { "dumpents", SV_DumpEnts_f },
    { "setmaster", SV_SetMaster_f },
    { "listmasters", SV_ListMasters_f },
    { "killserver", SV_KillServer_f },
    { "sv", SV_ServerCommand_f },
    { "pickclient", SV_PickClient_f },
    { "addban", SV_AddBan_f },
    { "delban", SV_DelBan_f },
    { "listbans", SV_ListBans_f },
    { "addblackhole", SV_AddBlackHole_f },
    { "delblackhole", SV_DelBlackHole_f },
    { "listblackholes", SV_ListBlackHoles_f },
    { "addstuffcmd", SV_AddStuffCmd_f, SV_StuffCmd_c },
    { "delstuffcmd", SV_DelStuffCmd_f, SV_StuffCmd_c },
    { "liststuffcmds", SV_ListStuffCmds_f, SV_StuffCmd_c },
    { "addfiltercmd", SV_AddFilterCmd_f, SV_AddFilterCmd_c },
    { "delfiltercmd", SV_DelFilterCmd_f, SV_DelFilterCmd_c },
    { "listfiltercmds", SV_ListFilterCmds_f },
#if USE_CLIENT
    { "savegame", SV_Savegame_f },
    { "loadgame", SV_Loadgame_f },
#endif
#if USE_MVD_CLIENT || USE_MVD_SERVER
    { "mvdrecord", SV_Record_f, SV_Record_c },
    { "mvdstop", SV_Stop_f },
#endif

    { NULL }
};


/*
==================
SV_InitOperatorCommands
==================
*/
void SV_InitOperatorCommands( void ) {
    Cmd_Register( c_server );

    if ( Com_IsDedicated() )
        Cmd_AddCommand( "say", SV_ConSay_f );
}
 
