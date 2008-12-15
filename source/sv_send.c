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
// sv_send.c

#include "sv_local.h"

/*
=============================================================================

MISC

=============================================================================
*/

char sv_outputbuf[SV_OUTPUTBUF_LENGTH];

void SV_FlushRedirect( int redirected, char *outputbuf, size_t len ) {
	byte    buffer[MAX_PACKETLEN_DEFAULT];

	if( redirected == RD_PACKET ) {
	    memcpy( buffer, "\xff\xff\xff\xffprint\n", 10 );
        memcpy( buffer + 10, outputbuf, len );
	    NET_SendPacket( NS_SERVER, &net_from, len + 10, buffer );
	} else if( redirected == RD_CLIENT ) {
		MSG_WriteByte( svc_print );
		MSG_WriteByte( PRINT_HIGH );
		MSG_WriteData( outputbuf, len );
        MSG_WriteByte( 0 );
        //Sys_Printf("redirect: %d bytes: %s", outputbuf);
		SV_ClientAddMessage( sv_client, MSG_RELIABLE|MSG_CLEAR );
	}
}

/*
=======================
SV_RateDrop

Returns qtrue if the client is over its current
bandwidth estimation and should not be sent another packet
=======================
*/
static qboolean SV_RateDrop( client_t *client ) {
	size_t	total;
	int		i;

	// never drop over the loopback
	if( !client->rate ) {
		return qfalse;
	}

	total = 0;
	for( i = 0; i < RATE_MESSAGES; i++ ) {
		total += client->message_size[i];
	}

	if( total > client->rate ) {
		client->surpressCount++;
		client->message_size[sv.framenum % RATE_MESSAGES] = 0;
		return qtrue;
	}

	return qfalse;
}

void SV_CalcSendTime( client_t *client, size_t size ) {
	// never drop over the loopback
	if( !client->rate ) {
        client->send_time = svs.realtime;
        client->send_delta = 0;
		return;
	}

	client->message_size[sv.framenum % RATE_MESSAGES] = size;

    client->send_time = svs.realtime;
	client->send_delta = size * 1000 / client->rate;
}

/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/


/*
=================
SV_ClientPrintf

Sends text across to be displayed if the level passes.
NOT archived in MVD stream.
=================
*/
void SV_ClientPrintf( client_t *client, int level, const char *fmt, ... ) {
	va_list		argptr;
	char		string[MAX_STRING_CHARS];
    size_t      len;
	
	if( level < client->messagelevel )
		return;
	
	va_start( argptr, fmt );
	len = Q_vsnprintf( string, sizeof( string ), fmt, argptr );
	va_end( argptr );

    if( len >= sizeof( string ) ) {
        Com_WPrintf( "%s: overflow\n", __func__ );
        return;
    }

	MSG_WriteByte( svc_print );
	MSG_WriteByte( level );
	MSG_WriteData( string, len + 1 );

	SV_ClientAddMessage( client, MSG_RELIABLE|MSG_CLEAR );
}

/*
=================
SV_BroadcastPrintf

Sends text to all active clients, including MVD clients.
NOT archived in MVD stream.
=================
*/
void SV_BroadcastPrintf( int level, const char *fmt, ... ) {
	va_list		argptr;
	char		string[MAX_STRING_CHARS];
	client_t	*client;
	size_t		len;

	va_start( argptr, fmt );
	len = Q_vsnprintf( string, sizeof( string ), fmt, argptr );
	va_end( argptr );

    if( len >= sizeof( string ) ) {
        Com_WPrintf( "%s: overflow\n", __func__ );
        return;
    }

	MSG_WriteByte( svc_print );
	MSG_WriteByte( level );
	MSG_WriteData( string, len + 1 );

    FOR_EACH_CLIENT( client ) {
		if( client->state != cs_spawned )
			continue;
		if( level < client->messagelevel )
			continue;
		SV_ClientAddMessage( client, MSG_RELIABLE );
	}

	SZ_Clear( &msg_write );
}

void SV_ClientCommand( client_t *client, const char *fmt, ... ) {
	va_list		argptr;
	char		string[MAX_STRING_CHARS];
    size_t      len;
	
	va_start( argptr, fmt );
	len = Q_vsnprintf( string, sizeof( string ), fmt, argptr );
	va_end( argptr );

    if( len >= sizeof( string ) ) {
        Com_WPrintf( "%s: overflow\n", __func__ );
        return;
    }

	MSG_WriteByte( svc_stufftext );
	MSG_WriteData( string, len + 1 );

	SV_ClientAddMessage( client, MSG_RELIABLE|MSG_CLEAR );
}

/*
=================
SV_BroadcastCommand

Sends command to all active clients.
NOT archived in MVD stream.
=================
*/
void SV_BroadcastCommand( const char *fmt, ... ) {
	va_list		argptr;
	char		string[MAX_STRING_CHARS];
	client_t	*client;
    size_t      len;
	
	va_start( argptr, fmt );
	len = Q_vsnprintf( string, sizeof( string ), fmt, argptr );
	va_end( argptr );

    if( len >= sizeof( string ) ) { 
        Com_WPrintf( "%s: overflow\n", __func__ );
        return;
    }

	MSG_WriteByte( svc_stufftext );
	MSG_WriteData( string, len + 1 );

    FOR_EACH_CLIENT( client ) {
    	SV_ClientAddMessage( client, MSG_RELIABLE );
	}

	SZ_Clear( &msg_write );
}


/*
=================
SV_Multicast

Sends the contents of the write buffer to a subset of the clients,
then clears the write buffer.

Archived in MVD stream.

MULTICAST_ALL	same as broadcast (origin can be NULL)
MULTICAST_PVS	send to clients potentially visible from org
MULTICAST_PHS	send to clients potentially hearable from org
=================
*/
void SV_Multicast( vec3_t origin, multicast_t to ) {
	client_t	*client;
	byte		mask[MAX_MAP_VIS];
	mleaf_t		*leaf1, *leaf2;
    int         leafnum;
	int			flags;
	vec3_t		org;

	flags = 0;

	switch( to ) {
	case MULTICAST_ALL_R:
		flags |= MSG_RELIABLE;	
        // intentional fallthrough
	case MULTICAST_ALL:
        leaf1 = NULL;
        leafnum = 0;
		break;
	case MULTICAST_PHS_R:
		flags |= MSG_RELIABLE;
        // intentional fallthrough
	case MULTICAST_PHS:
		leaf1 = CM_PointLeaf( &sv.cm, origin );
        leafnum = leaf1 - sv.cm.cache->leafs;
		BSP_ClusterVis( sv.cm.cache, mask, leaf1->cluster, DVIS_PHS );
		break;
	case MULTICAST_PVS_R:
		flags |= MSG_RELIABLE;
        // intentional fallthrough
	case MULTICAST_PVS:
		leaf1 = CM_PointLeaf( &sv.cm, origin );
        leafnum = leaf1 - sv.cm.cache->leafs;
		BSP_ClusterVis( sv.cm.cache, mask, leaf1->cluster, DVIS_PVS );
		break;
	default:
		Com_Error( ERR_DROP, "SV_Multicast: bad to: %i", to );
	}

	// send the data to all relevent clients
    FOR_EACH_CLIENT( client ) {
		if( client->state < cs_primed ) {
			continue;
		}
		// do not send unreliables to connecting clients
		if( !( flags & MSG_RELIABLE ) && ( client->state != cs_spawned ||
            client->download || ( client->flags & CF_NODATA ) ) )
        {
			continue; 
		}

		if( leaf1 ) {
			// find the client's PVS
#if 0
	        player_state_t *ps = &client->edict->client->ps;
			VectorMA( ps->viewoffset, 0.125f, ps->pmove.origin, org );
#else
            // FIXME: for some strange reason, game code assumes the server
            // uses entity origin for PVS/PHS culling, not the view origin
			VectorCopy( client->edict->s.origin, org );
#endif
			leaf2 = CM_PointLeaf( &sv.cm, org );
			if( !CM_AreasConnected( &sv.cm, leaf1->area, leaf2->area ) ) {
				continue;
            }
			if( !Q_IsBitSet( mask, leaf2->cluster ) ) {
				continue;
			}
		}

		SV_ClientAddMessage( client, flags );
	}

#if USE_MVD_SERVER
    // add to MVD datagram
	SV_MvdMulticast( leafnum, to );
#endif

	// clear the buffer 
	SZ_Clear( &msg_write );
}



/*
=======================
SV_ClientAddMessage

Adds contents of the current write buffer to client's message list.
Does NOT clean the buffer for multicast delivery purpose,
unless told otherwise.
=======================
*/
void SV_ClientAddMessage( client_t *client, int flags ) {
#if USE_CLIENT
    if( sv_debug_send->integer > 1 ) {
        Com_Printf( S_COLOR_BLUE"%s: add%c: %"PRIz"\n", client->name,
            ( flags & MSG_RELIABLE ) ? 'r' : 'u', msg_write.cursize );
    }
#endif

	if( !msg_write.cursize ) {
		return;
	}

//    if( client->state > cs_zombie ) {
        client->AddMessage( client, msg_write.data, msg_write.cursize,
            ( flags & MSG_RELIABLE ) ? qtrue : qfalse );
  //  }

	if( flags & MSG_CLEAR ) {
		SZ_Clear( &msg_write );
	}
}

static inline void SV_PacketizedRemove( client_t *client, message_packet_t *msg ) {
	List_Remove( &msg->entry );

	if( msg->cursize > MSG_TRESHOLD ) {
		Z_Free( msg );
    } else {
        List_Insert( &client->msg_free, &msg->entry );
    }
}

#define FOR_EACH_MSG_SAFE( list )     LIST_FOR_EACH_SAFE( message_packet_t, msg, next, list, entry )

void SV_PacketizedClear( client_t *client ) {
	message_packet_t	*msg, *next;

    FOR_EACH_MSG_SAFE( &client->msg_used[0] ) {
		SV_PacketizedRemove( client, msg );
	}
    FOR_EACH_MSG_SAFE( &client->msg_used[1] ) {
		SV_PacketizedRemove( client, msg );
	}
    client->msg_bytes = 0;
}

message_packet_t *SV_PacketizedAdd( client_t *client, byte *data,
							  size_t len, qboolean reliable )
{
	message_packet_t	*msg;

	if( len > MSG_TRESHOLD ) {
		if( len > MAX_MSGLEN ) {
			Com_Error( ERR_FATAL, "%s: oversize packet", __func__ );
		}
		msg = SV_Malloc( sizeof( *msg ) + len - MSG_TRESHOLD );
	} else {
        if( LIST_EMPTY( &client->msg_free ) ) {
            Com_WPrintf( "Out of message slots for %s!\n", client->name );
            if( reliable ) {
                SV_PacketizedClear( client );
                SV_DropClient( client, "no slot for reliable message" );
            }
            return NULL;
        }
        msg = LIST_FIRST( message_packet_t, &client->msg_free, entry );
    	List_Remove( &msg->entry );
	}

	memcpy( msg->data, data, len );
	msg->cursize = ( uint16_t )len;

    List_Append( &client->msg_used[reliable], &msg->entry );
    if( !reliable ) {
        client->msg_bytes += len;
    }

    return msg;
}

static void emit_snd( client_t *client, message_packet_t *msg ) {
    entity_state_t *state;
    client_frame_t *frame;
    int flags, entnum;
    int i, j;

    entnum = msg->sendchan >> 3;
    flags = msg->flags;

    // check if position needs to be explicitly sent
    if( !( flags & SND_POS ) ) {
	    frame = &client->frames[sv.framenum & UPDATE_MASK];

        for( i = 0; i < frame->numEntities; i++ ) {
            j = ( frame->firstEntity + i ) % svs.numEntityStates;
            state = &svs.entityStates[j];
            if( state->number == entnum ) {
                break;
            }
        }
        if( i == frame->numEntities ) {
#if USE_CLIENT
            if( sv_debug_send->integer ) {
                Com_Printf( S_COLOR_BLUE "Forcing position on entity %d for %s\n",
                    entnum, client->name );
            }
#endif
            flags |= SND_POS;   // entity is not present in frame
        }
    }

    MSG_WriteByte( svc_sound );
    MSG_WriteByte( flags );
    MSG_WriteByte( msg->index );

    if( flags & SND_VOLUME )
        MSG_WriteByte( msg->volume );
    if( flags & SND_ATTENUATION )
        MSG_WriteByte( msg->attenuation );
    if( flags & SND_OFFSET )
        MSG_WriteByte( msg->timeofs );

    MSG_WriteShort( msg->sendchan );

    if( flags & SND_POS ) {
        for( i = 0; i < 3; i++ ) {
            MSG_WriteShort( msg->pos[i] );
        }
    }
}

static inline void write_snd( client_t *client, message_packet_t *msg, size_t maxsize ) {
    // if this msg fits, write it
    if( msg_write.cursize + MAX_SOUND_PACKET <= maxsize ) {
        emit_snd( client, msg );
    }
	List_Remove( &msg->entry );
    List_Insert( &client->msg_free, &msg->entry );
}

static inline void write_msg( client_t *client, message_packet_t *msg, size_t maxsize ) {
    // if this msg fits, write it
    if( msg_write.cursize + msg->cursize <= maxsize ) {
        MSG_WriteData( msg->data, msg->cursize );
    }
	SV_PacketizedRemove( client, msg );
}

static inline void emit_messages( client_t *client, size_t maxsize ) {
	message_packet_t	*msg, *next;

    FOR_EACH_MSG_SAFE( &client->msg_used[0] ) {
        if( msg->cursize ) {
            write_msg( client, msg, maxsize );
        } else {
            write_snd( client, msg, maxsize );
        }
    }
}

/*
===============================================================================

FRAME UPDATES - DEFAULT, R1Q2 AND Q2PRO CLIENTS (OLD NETCHAN)

===============================================================================
*/


void SV_ClientAddMessage_Old( client_t *client, byte *data,
							  size_t len, qboolean reliable )
{
	if( len > client->netchan->maxpacketlen ) {
		if( reliable ) {
			SV_DropClient( client, "oversize reliable message" );
		} else {
            Com_DPrintf( "Dumped oversize unreliable for %s\n", client->name );
        }
		return;
	}

    SV_PacketizedAdd( client, data, len, reliable );
}

/*
=======================
SV_ClientWriteReliableMessages_Old

This should be the only place data is
ever written to client->netchan.message
=======================
*/
void SV_ClientWriteReliableMessages_Old( client_t *client, size_t maxsize ) {
	message_packet_t	*msg, *next;
    int count;

	if( client->netchan->reliable_length ) {
#if USE_CLIENT
		if( sv_debug_send->integer > 1 ) {
			Com_Printf( S_COLOR_BLUE"%s: unacked\n", client->name );
		}
#endif
		return;	// there is still outgoing reliable message pending
	}

	// find at least one reliable message to send
    count = 0;
    FOR_EACH_MSG_SAFE( &client->msg_used[1] ) {
		// stop if this msg doesn't fit (reliables must be delivered in order)
		if( client->netchan->message.cursize + msg->cursize > maxsize ) {
			if( !count ) {
				Com_WPrintf( "Overflow on the first reliable message "
					"for %s (should not happen).\n", client->name );
			}
			break;
		}

#if USE_CLIENT
		if( sv_debug_send->integer > 1 ) {
			Com_Printf( S_COLOR_BLUE"%s: wrt%d: %d\n",
                client->name, count, msg->cursize );
		}
#endif

		SZ_Write( &client->netchan->message, msg->data, msg->cursize );
		SV_PacketizedRemove( client, msg );
        count++;
	}
}

static void repack_messages( client_t *client, size_t maxsize ) {
	message_packet_t	*msg, *next;

	if( msg_write.cursize + 4 > maxsize ) {
        return;
    }

    // temp entities first
    FOR_EACH_MSG_SAFE( &client->msg_used[0] ) {
        if( !msg->cursize || msg->data[0] != svc_temp_entity ) {
            continue;
        }
        // ignore some low-priority effects, these checks come from r1q2
        if( msg->data[1] == TE_BLOOD || msg->data[1] == TE_SPLASH ||
            msg->data[1] == TE_GUNSHOT || msg->data[1] == TE_BULLET_SPARKS ||
            msg->data[1] == TE_SHOTGUN )
        {
            continue;
        }
        write_msg( client, msg, maxsize );
    }

	if( msg_write.cursize + 4 > maxsize ) {
        return;
    }

    // then entity sounds
    FOR_EACH_MSG_SAFE( &client->msg_used[0] ) {
        if( !msg->cursize ) {
            write_snd( client, msg, maxsize );
        }
    }

	if( msg_write.cursize + 4 > maxsize ) {
        return;
    }

	// then positioned sounds
    FOR_EACH_MSG_SAFE( &client->msg_used[0] ) {
        if( msg->cursize && msg->data[0] == svc_sound ) {
            write_msg( client, msg, maxsize );
        }
    }

    if( msg_write.cursize + 4 > maxsize ) {
        return;
    }

    // then everything else left
    FOR_EACH_MSG_SAFE( &client->msg_used[0] ) {
        if( msg->cursize ) {
            write_msg( client, msg, maxsize );
        }
    }
}

/*
=======================
SV_ClientWriteDatagram_Old
=======================
*/
void SV_ClientWriteDatagram_Old( client_t *client ) {
	message_packet_t *msg;
	size_t maxsize, cursize;

	maxsize = client->netchan->maxpacketlen;
	if( client->netchan->reliable_length ) {
		// there is still unacked reliable message pending
		maxsize -= client->netchan->reliable_length;
	} else {
		// find at least one reliable message to send
		// and make sure to reserve space for it
        if( !LIST_EMPTY( &client->msg_used[1] ) ) {
            msg = LIST_FIRST( message_packet_t, &client->msg_used[1], entry );
            maxsize -= msg->cursize;
        }
	}

	// send over all the relevant entity_state_t
	// and the player_state_t
	client->WriteFrame( client );
	if( msg_write.cursize > maxsize ) {
#if USE_CLIENT
		if( sv_debug_send->integer ) {
			Com_Printf( S_COLOR_BLUE"Frame overflowed for %s: %"PRIz" > %"PRIz"\n",
                client->name, msg_write.cursize, maxsize );
		}
#endif
		SZ_Clear( &msg_write );
	}

	// now write unreliable messages
	// it is necessary for this to be after the WriteFrame
	// so that entity references will be current
    if( msg_write.cursize + client->msg_bytes > maxsize ) {
        // throw out some low priority effects
        repack_messages( client, maxsize );
    } else {
        // all messages fit, write them in order
        emit_messages( client, maxsize );
    }

    // write at least one reliable message
	SV_ClientWriteReliableMessages_Old( client,
        client->netchan->maxpacketlen - msg_write.cursize );

	// send the datagram
	cursize = client->netchan->Transmit( client->netchan,
        msg_write.cursize, msg_write.data, client->numpackets );

	// record the size for rate estimation
	SV_CalcSendTime( client, cursize );

	SZ_Clear( &msg_write );
}

/*
===============================================================================

FRAME UPDATES - Q2PRO CLIENTS (NEW NETCHAN)

===============================================================================
*/

void SV_ClientAddMessage_New( client_t *client, byte *data,
							  size_t len, qboolean reliable )
{
    if( reliable ) {
	    SZ_Write( &client->netchan->message, data, len );
    } else {
        SV_PacketizedAdd( client, data, len, qfalse );
    }
}

void SV_ClientWriteDatagram_New( client_t *client ) {
	size_t cursize;

	// send over all the relevant entity_state_t
	// and the player_state_t
	client->WriteFrame( client );

	if( msg_write.overflowed ) {
		// should never really happen
		Com_WPrintf( "Frame overflowed for %s\n", client->name );
		SZ_Clear( &msg_write );
	}

	// now write unreliable messages
	// for this client out to the message
	// it is necessary for this to be after the WriteFrame
	// so that entity references will be current
    if( msg_write.cursize + client->msg_bytes > msg_write.maxsize ) {
		Com_WPrintf( "Dumping datagram for %s\n", client->name );
    } else {
        emit_messages( client, msg_write.maxsize );
    }

#if USE_CLIENT
	if( sv_pad_packets->integer ) {
        size_t pad = msg_write.cursize + sv_pad_packets->integer;

        if( pad > msg_write.maxsize ) {
            pad = msg_write.maxsize;
        }
		for( ; pad > 0; pad-- ) {
		    MSG_WriteByte( svc_nop );
	    }
    }
#endif

	// send the datagram
	cursize = client->netchan->Transmit( client->netchan,
        msg_write.cursize, msg_write.data, client->numpackets );

	// record the size for rate estimation
	SV_CalcSendTime( client, cursize );

	// clear the write buffer
	SZ_Clear( &msg_write );
}


/*
===============================================================================

COMMON STUFF

===============================================================================
*/

static void finish_frame( client_t *client ) {
	message_packet_t	*msg, *next;

    FOR_EACH_MSG_SAFE( &client->msg_used[0] ) {
		SV_PacketizedRemove( client, msg );
	}
    client->msg_bytes = 0;
}

/*
=======================
SV_SendClientMessages

Called each game frame, sends svc_frame messages to spawned clients only.
Clients in earlier connection state are handled in SV_SendAsyncPackets.
=======================
*/
void SV_SendClientMessages( void ) {
	client_t	*client;
    size_t      cursize;

	// send a message to each connected client
    FOR_EACH_CLIENT( client ) {
		if( client->state != cs_spawned || client->download || ( client->flags & CF_NODATA ) )
            goto finish;

		// if the reliable message overflowed,
		// drop the client (should never happen)
		if( client->netchan->message.overflowed ) {
			SZ_Clear( &client->netchan->message );
			SV_DropClient( client, "overflowed" );
		}

		// don't overrun bandwidth
		if( SV_RateDrop( client ) ) {
#if USE_CLIENT
			if( sv_debug_send->integer ) {
				Com_Printf( "Frame %d surpressed for %s\n",
                    sv.framenum, client->name );
			}
#endif
			client->surpressCount++;
		} else {
            // don't write any frame data until all fragments are sent
            if( client->netchan->fragment_pending ) {
                cursize = client->netchan->TransmitNextFragment( client->netchan );
                SV_CalcSendTime( client, cursize );
            } else {
                // build the new frame and write it
                SV_BuildClientFrame( client );
                client->WriteDatagram( client );
            }
        }

finish:
	    // clear all unreliable messages still left
        finish_frame( client );
	}

}

