/*****************************************************************************
 * ifo.c: Dummy ifo demux to enable opening DVDs rips by double cliking on VIDEO_TS.IFO
 *****************************************************************************
 * Copyright (C) 2007 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Antoine Cellerier <dionoea @t videolan d.t org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_access.h>
#include <assert.h>

#include "playlist.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int ReadDVD( stream_t *, input_item_node_t * );
static int ReadDVD_VR( stream_t *, input_item_node_t * );

static const char *StreamLocation( const stream_t *s )
{
    return s->psz_filepath ? s->psz_filepath : s->psz_url;
}

/*****************************************************************************
 * Import_IFO: main import function
 *****************************************************************************/
int Import_IFO( vlc_object_t *p_this )
{
    stream_t *p_stream = (stream_t *)p_this;

    CHECK_FILE(p_stream);

    if( !stream_HasExtension( p_stream, ".IFO" ) )
        return VLC_EGENERIC;

    const char *psz_location = StreamLocation( p_stream );
    if( psz_location == NULL )
        return VLC_EGENERIC;

    size_t len = strlen( psz_location );

    const char *psz_file = psz_location + len - strlen( "VIDEO_TS.IFO" );
    /* Valid filenames are :
     *  - VIDEO_TS.IFO
     *  - VTS_XX_X.IFO where X are digits
     */
    if( len > strlen( "VIDEO_TS.IFO" )
        && ( !strcasecmp( psz_file, "VIDEO_TS.IFO" )
        || (!strncasecmp( psz_file, "VTS_", 4 )
        && !strcasecmp( psz_file + strlen( "VTS_00_0" ) , ".IFO" ) ) ) )
    {
        const uint8_t *p_peek;
        ssize_t i_peek = vlc_stream_Peek( p_stream->s, &p_peek, 8 );

        if( i_peek != 8 || memcmp( p_peek, "DVDVIDEO", 8 ) )
            return VLC_EGENERIC;

        p_stream->pf_readdir = ReadDVD;
    }
    /* Valid filename for DVD-VR is VR_MANGR.IFO */
    else if( len >= 12 && !strcmp( &psz_location[len-12], "VR_MANGR.IFO" ) )
    {
        const uint8_t *p_peek;
        ssize_t i_peek = vlc_stream_Peek( p_stream->s, &p_peek, 8 );

        if( i_peek != 8 || memcmp( p_peek, "DVD_RTR_", 8 ) )
            return VLC_EGENERIC;

        p_stream->pf_readdir = ReadDVD_VR;
    }
    else
        return VLC_EGENERIC;

    p_stream->pf_control = access_vaDirectoryControlHelper;

    return VLC_SUCCESS;
}

static int ReadDVD( stream_t *p_stream, input_item_node_t *node )
{
    char *psz_url, *psz_dir;
    const char *psz_location = StreamLocation(p_stream);

    psz_dir = strrchr( psz_location, '/' );
    if( psz_dir != NULL )
       psz_dir[1] = '\0';

    if( asprintf( &psz_url, "dvd://%s", psz_location ) == -1 )
        return 0;

    input_item_t *p_input = input_item_New( psz_url, psz_url );
    input_item_node_AppendItem( node, p_input );
    input_item_Release( p_input );

    free( psz_url );

    return VLC_SUCCESS;
}

static int ReadDVD_VR( stream_t *p_stream, input_item_node_t *node )
{
    const char *psz_location = StreamLocation(p_stream);

    size_t len = strlen( psz_location );
    char *psz_url = malloc( len + 1 );

    if( unlikely( psz_url == NULL ) )
        return 0;
    assert( len >= 12 );
    len -= 12;
    memcpy( psz_url, psz_location, len );
    memcpy( psz_url + len, "VR_MOVIE.VRO", 13 );

    input_item_t *p_input = input_item_New( psz_url, psz_url );
    input_item_node_AppendItem( node, p_input );
    input_item_Release( p_input );

    free( psz_url );

    return VLC_SUCCESS;
}
