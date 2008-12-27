/*****************************************************************************
 * thread.c : Playlist management functions
 *****************************************************************************
 * Copyright © 1999-2008 the VideoLAN team
 * $Id$
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *          Clément Stenac <zorglub@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_input.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include "stream_output/stream_output.h"
#include "playlist_internal.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void *Thread   ( vlc_object_t * );

/*****************************************************************************
 * Main functions for the global thread
 *****************************************************************************/

/**
 * Create the main playlist threads.
 * Additionally to the playlist, this thread controls :
 *    - Statistics
 *    - VLM
 * \param p_parent
 * \return an object with a started thread
 */
void playlist_Activate( playlist_t *p_playlist )
{
    /* */
    playlist_private_t *p_sys = pl_priv(p_playlist);

    /* Fetcher */
    p_sys->p_fetcher = playlist_fetcher_New( p_playlist );
    if( !p_sys->p_fetcher )
        msg_Err( p_playlist, "cannot create playlist fetcher" );

    /* Preparse */
    p_sys->p_preparser = playlist_preparser_New( p_playlist, p_sys->p_fetcher );
    if( !p_sys->p_preparser )
        msg_Err( p_playlist, "cannot create playlist preparser" );

    /* Start the playlist thread */
    if( vlc_thread_create( p_playlist, "playlist", Thread,
                           VLC_THREAD_PRIORITY_LOW, false ) )
    {
        msg_Err( p_playlist, "cannot spawn playlist thread" );
    }
    msg_Dbg( p_playlist, "Activated" );
}

void playlist_Deactivate( playlist_t *p_playlist )
{
    /* */
    playlist_private_t *p_sys = pl_priv(p_playlist);

    msg_Dbg( p_playlist, "Deactivate" );

    vlc_object_kill( p_playlist );
    vlc_thread_join( p_playlist );
    assert( !p_sys->p_input );

    PL_LOCK;
    playlist_preparser_t *p_preparser = p_sys->p_preparser;
    playlist_fetcher_t *p_fetcher = p_sys->p_fetcher;

    p_sys->p_preparser = NULL;
    p_sys->p_fetcher = NULL;
    PL_UNLOCK;

    if( p_preparser )
        playlist_preparser_Delete( p_preparser );
    if( p_fetcher )
        playlist_fetcher_Delete( p_fetcher );

    /* release input ressources */
    if( p_sys->p_input_ressource )
        input_ressource_Delete( p_sys->p_input_ressource );
    p_sys->p_input_ressource = NULL;

    /* */
    playlist_MLDump( p_playlist );

    PL_LOCK;

    /* Release the current node */
    set_current_status_node( p_playlist, NULL );

    /* Release the current item */
    set_current_status_item( p_playlist, NULL );

    FOREACH_ARRAY( playlist_item_t *p_del, p_playlist->all_items )
        free( p_del->pp_children );
        vlc_gc_decref( p_del->p_input );
        free( p_del );
    FOREACH_END();
    ARRAY_RESET( p_playlist->all_items );
    FOREACH_ARRAY( playlist_item_t *p_del, p_sys->items_to_delete )
        free( p_del->pp_children );
        vlc_gc_decref( p_del->p_input );
        free( p_del );
    FOREACH_END();
    ARRAY_RESET( p_sys->items_to_delete );

    ARRAY_RESET( p_playlist->items );
    ARRAY_RESET( p_playlist->current );

    PL_UNLOCK;

    msg_Dbg( p_playlist, "Deactivated" );
}

/* */

/* Input Callback */
static int InputEvent( vlc_object_t *p_this, char const *psz_cmd,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    VLC_UNUSED(p_this); VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval);
    playlist_t *p_playlist = p_data;

    if( newval.i_int != INPUT_EVENT_STATE &&
        newval.i_int != INPUT_EVENT_DEAD )
        return VLC_SUCCESS;

    PL_LOCK;

    vlc_object_signal_unlocked( p_playlist );

    PL_UNLOCK;
    return VLC_SUCCESS;
}

static void UpdateActivity( playlist_t *p_playlist, int i_delta )
{
    PL_ASSERT_LOCKED;

    const int i_activity = var_GetInteger( p_playlist, "activity" ) ;
    var_SetInteger( p_playlist, "activity", i_activity + i_delta );
}

/**
 * Synchronise the current index of the playlist
 * to match the index of the current item.
 *
 * \param p_playlist the playlist structure
 * \param p_cur the current playlist item
 * \return nothing
 */
static void ResyncCurrentIndex( playlist_t *p_playlist, playlist_item_t *p_cur )
{
    PL_ASSERT_LOCKED;

    PL_DEBUG( "resyncing on %s", PLI_NAME( p_cur ) );
    /* Simply resync index */
    int i;
    p_playlist->i_current_index = -1;
    for( i = 0 ; i< p_playlist->current.i_size; i++ )
    {
        if( ARRAY_VAL( p_playlist->current, i ) == p_cur )
        {
            p_playlist->i_current_index = i;
            break;
        }
    }
    PL_DEBUG( "%s is at %i", PLI_NAME( p_cur ), p_playlist->i_current_index );
}

static void ResetCurrentlyPlaying( playlist_t *p_playlist,
                                   playlist_item_t *p_cur )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);

    stats_TimerStart( p_playlist, "Items array build",
                      STATS_TIMER_PLAYLIST_BUILD );
    PL_DEBUG( "rebuilding array of current - root %s",
              PLI_NAME( p_sys->status.p_node ) );
    ARRAY_RESET( p_playlist->current );
    p_playlist->i_current_index = -1;
    for( playlist_item_t *p_next = NULL; ; )
    {
        /** FIXME: this is *slow* */
        p_next = playlist_GetNextLeaf( p_playlist,
                                       p_sys->status.p_node,
                                       p_next, true, false );
        if( !p_next )
            break;

        if( p_next == p_cur )
            p_playlist->i_current_index = p_playlist->current.i_size;
        ARRAY_APPEND( p_playlist->current, p_next);
    }
    PL_DEBUG("rebuild done - %i items, index %i", p_playlist->current.i_size,
                                                  p_playlist->i_current_index);

    if( var_GetBool( p_playlist, "random" ) )
    {
        /* Shuffle the array */
        srand( (unsigned int)mdate() );
        for( int j = p_playlist->current.i_size - 1; j > 0; j-- )
        {
            int i = rand() % (j+1); /* between 0 and j */
            playlist_item_t *p_tmp;
            /* swap the two items */
            p_tmp = ARRAY_VAL(p_playlist->current, i);
            ARRAY_VAL(p_playlist->current,i) = ARRAY_VAL(p_playlist->current,j);
            ARRAY_VAL(p_playlist->current,j) = p_tmp;
        }
    }
    p_sys->b_reset_currently_playing = false;
    stats_TimerStop( p_playlist, STATS_TIMER_PLAYLIST_BUILD );
}


/**
 * Start the input for an item
 *
 * \param p_playlist the playlist object
 * \param p_item the item to play
 * \return nothing
 */
static int PlayItem( playlist_t *p_playlist, playlist_item_t *p_item )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);
    input_item_t *p_input = p_item->p_input;

    PL_ASSERT_LOCKED;

    msg_Dbg( p_playlist, "creating new input thread" );

    p_input->i_nb_played++;
    set_current_status_item( p_playlist, p_item );

    p_sys->status.i_status = PLAYLIST_RUNNING;

    UpdateActivity( p_playlist, DEFAULT_INPUT_ACTIVITY );

    assert( p_sys->p_input == NULL );

    input_thread_t *p_input_thread =
        input_CreateThreadExtended( p_playlist, p_input, NULL, p_sys->p_input_ressource );

    if( p_input_thread )
    {
        p_sys->p_input = p_input_thread;

        var_AddCallback( p_input_thread, "intf-event", InputEvent, p_playlist );
    }

    p_sys->p_input_ressource = NULL;

    char *psz_uri = input_item_GetURI( p_item->p_input );
    if( psz_uri && ( !strncmp( psz_uri, "directory:", 10 ) ||
                     !strncmp( psz_uri, "vlc:", 4 ) ) )
    {
        free( psz_uri );
        return VLC_SUCCESS;
    }
    free( psz_uri );

    /* TODO store art policy in playlist private data */
    if( var_GetInteger( p_playlist, "album-art" ) == ALBUM_ART_WHEN_PLAYED )
    {
        bool b_has_art;

        char *psz_arturl, *psz_name;
        psz_arturl = input_item_GetArtURL( p_input );
        psz_name = input_item_GetName( p_input );

        /* p_input->p_meta should not be null after a successfull CreateThread */
        b_has_art = !EMPTY_STR( psz_arturl );

        if( !b_has_art || strncmp( psz_arturl, "attachment://", 13 ) )
        {
            PL_DEBUG( "requesting art for %s", psz_name );
            playlist_AskForArtEnqueue( p_playlist, p_input, pl_Locked );
        }
        free( psz_arturl );
        free( psz_name );
    }

    PL_UNLOCK;
    var_SetInteger( p_playlist, "playlist-current", p_input->i_id );
    PL_LOCK;

    return VLC_SUCCESS;
}

/**
 * Compute the next playlist item depending on
 * the playlist course mode (forward, backward, random, view,...).
 *
 * \param p_playlist the playlist object
 * \return nothing
 */
static playlist_item_t *NextItem( playlist_t *p_playlist )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);
    playlist_item_t *p_new = NULL;

    /* Handle quickly a few special cases */
    /* No items to play */
    if( p_playlist->items.i_size == 0 )
    {
        msg_Info( p_playlist, "playlist is empty" );
        return NULL;
    }

    /* Start the real work */
    if( p_sys->request.b_request )
    {
        p_new = p_sys->request.p_item;
        int i_skip = p_sys->request.i_skip;
        PL_DEBUG( "processing request item %s node %s skip %i",
                        PLI_NAME( p_sys->request.p_item ),
                        PLI_NAME( p_sys->request.p_node ), i_skip );

        if( p_sys->request.p_node &&
            p_sys->request.p_node != get_current_status_node( p_playlist ) )
        {

            set_current_status_node( p_playlist, p_sys->request.p_node );
            p_sys->request.p_node = NULL;
            p_sys->b_reset_currently_playing = true;
        }

        /* If we are asked for a node, go to it's first child */
        if( i_skip == 0 && ( p_new == NULL || p_new->i_children != -1 ) )
        {
            i_skip++;
            if( p_new != NULL )
            {
                p_new = playlist_GetNextLeaf( p_playlist, p_new, NULL, true, false );
                for( int i = 0; i < p_playlist->current.i_size; i++ )
                {
                    if( p_new == ARRAY_VAL( p_playlist->current, i ) )
                    {
                        p_playlist->i_current_index = i;
                        i_skip = 0;
                    }
                }
            }
        }

        if( p_sys->b_reset_currently_playing )
            /* A bit too bad to reset twice ... */
            ResetCurrentlyPlaying( p_playlist, p_new );
        else if( p_new )
            ResyncCurrentIndex( p_playlist, p_new );
        else
            p_playlist->i_current_index = -1;

        if( p_playlist->current.i_size && (i_skip > 0) )
        {
            if( p_playlist->i_current_index < -1 )
                p_playlist->i_current_index = -1;
            for( int i = i_skip; i > 0 ; i-- )
            {
                p_playlist->i_current_index++;
                if( p_playlist->i_current_index >= p_playlist->current.i_size )
                {
                    PL_DEBUG( "looping - restarting at beginning of node" );
                    p_playlist->i_current_index = 0;
                }
            }
            p_new = ARRAY_VAL( p_playlist->current,
                               p_playlist->i_current_index );
        }
        else if( p_playlist->current.i_size && (i_skip < 0) )
        {
            for( int i = i_skip; i < 0 ; i++ )
            {
                p_playlist->i_current_index--;
                if( p_playlist->i_current_index <= -1 )
                {
                    PL_DEBUG( "looping - restarting at end of node" );
                    p_playlist->i_current_index = p_playlist->current.i_size-1;
                }
            }
            p_new = ARRAY_VAL( p_playlist->current,
                               p_playlist->i_current_index );
        }
        /* Clear the request */
        p_sys->request.b_request = false;
    }
    /* "Automatic" item change ( next ) */
    else
    {
        bool b_loop = var_GetBool( p_playlist, "loop" );
        bool b_repeat = var_GetBool( p_playlist, "repeat" );
        bool b_playstop = var_GetBool( p_playlist, "play-and-stop" );

        /* Repeat and play/stop */
        if( b_repeat && get_current_status_item( p_playlist ) )
        {
            msg_Dbg( p_playlist,"repeating item" );
            return get_current_status_item( p_playlist );
        }
        if( b_playstop )
        {
            msg_Dbg( p_playlist,"stopping (play and stop)" );
            return NULL;
        }

        /* */
        if( get_current_status_item( p_playlist ) )
        {
            playlist_item_t *p_parent = get_current_status_item( p_playlist );
            while( p_parent )
            {
                if( p_parent->i_flags & PLAYLIST_SKIP_FLAG )
                {
                    msg_Dbg( p_playlist, "blocking item, stopping") ;
                    return NULL;
                }
                p_parent = p_parent->p_parent;
            }
        }

        PL_DEBUG( "changing item without a request (current %i/%i)",
                  p_playlist->i_current_index, p_playlist->current.i_size );
        /* Cant go to next from current item */
        if( get_current_status_item( p_playlist ) &&
            get_current_status_item( p_playlist )->i_flags & PLAYLIST_SKIP_FLAG )
            return NULL;

        if( p_sys->b_reset_currently_playing )
            ResetCurrentlyPlaying( p_playlist,
                                   get_current_status_item( p_playlist ) );

        p_playlist->i_current_index++;
        assert( p_playlist->i_current_index <= p_playlist->current.i_size );
        if( p_playlist->i_current_index == p_playlist->current.i_size )
        {
            if( !b_loop || p_playlist->current.i_size == 0 )
                return NULL;
            p_playlist->i_current_index = 0;
        }
        PL_DEBUG( "using item %i", p_playlist->i_current_index );
        if ( p_playlist->current.i_size == 0 )
            return NULL;

        p_new = ARRAY_VAL( p_playlist->current, p_playlist->i_current_index );
        /* The new item can't be autoselected  */
        if( p_new != NULL && p_new->i_flags & PLAYLIST_SKIP_FLAG )
            return NULL;
    }
    return p_new;
}

static int LoopInput( playlist_t *p_playlist )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);
    input_thread_t *p_input = p_sys->p_input;

    if( !p_input )
        return VLC_EGENERIC;

    if( ( p_sys->request.b_request || !vlc_object_alive( p_playlist ) ) && !p_input->b_die )
    {
        PL_DEBUG( "incoming request - stopping current input" );
        input_StopThread( p_input );
    }

    /* This input is dead. Remove it ! */
    if( p_input->b_dead )
    {
        PL_DEBUG( "dead input" );

        assert( p_sys->p_input_ressource == NULL );

        p_sys->p_input_ressource = input_DetachRessource( p_input );
        if( !var_CreateGetBool( p_input, "sout-keep" ) )
            input_ressource_TerminateSout( p_sys->p_input_ressource );

        /* The DelCallback must be issued without playlist lock
         * It is not a problem as we return VLC_EGENERIC */
        PL_UNLOCK;
        var_DelCallback( p_input, "intf-event", InputEvent, p_playlist );
        PL_LOCK;

        p_sys->p_input = NULL;
        vlc_thread_join( p_input );
        vlc_object_release( p_input );

        UpdateActivity( p_playlist, -DEFAULT_INPUT_ACTIVITY );

        return VLC_EGENERIC;
    }
    /* This input is dying, let it do */
    else if( p_input->b_die )
    {
        PL_DEBUG( "dying input" );
    }
    /* This input has finished, ask it to die ! */
    else if( p_input->b_error || p_input->b_eof )
    {
        PL_DEBUG( "finished input" );
        input_StopThread( p_input );
    }
    return VLC_SUCCESS;
}

static void LoopRequest( playlist_t *p_playlist )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);
    assert( !p_sys->p_input );

    /* No input. Several cases
     *  - No request, running status -> start new item
     *  - No request, stopped status -> collect garbage
     *  - Request, running requested -> start new item
     *  - Request, stopped requested -> collect garbage
    */
    const int i_status = p_sys->request.b_request ?
                         p_sys->request.i_status : p_sys->status.i_status;

    if( i_status == PLAYLIST_STOPPED )
    {
        p_sys->status.i_status = PLAYLIST_STOPPED;

        if( p_sys->p_input_ressource )
            input_ressource_TerminateVout( p_sys->p_input_ressource );

        if( vlc_object_alive( p_playlist ) )
            vlc_object_wait( p_playlist );
        return;
    }

    playlist_item_t *p_item = NextItem( p_playlist );
    if( p_item )
    {
        msg_Dbg( p_playlist, "starting new item" );
        PlayItem( p_playlist, p_item );
        return;
    }

    msg_Dbg( p_playlist, "nothing to play" );
    p_sys->status.i_status = PLAYLIST_STOPPED;

    if( var_GetBool( p_playlist, "play-and-exit" ) )
    {
        msg_Info( p_playlist, "end of playlist, exiting" );
        vlc_object_kill( p_playlist->p_libvlc );
    }
}

/**
 * Run the main control thread itself
 */
static void *Thread ( vlc_object_t *p_this )
{
    playlist_t *p_playlist = (playlist_t*)p_this;
    playlist_private_t *p_sys = pl_priv(p_playlist);
    int canc = vlc_savecancel();

    vlc_object_lock( p_playlist );
    while( vlc_object_alive( p_playlist ) || p_sys->p_input )
    {
        /* FIXME: what's that ! */
        if( p_sys->b_reset_currently_playing &&
            mdate() - p_sys->last_rebuild_date > 30000 ) // 30 ms
        {
            ResetCurrentlyPlaying( p_playlist,
                                   get_current_status_item( p_playlist ) );
            p_sys->last_rebuild_date = mdate();
        }

        /* If there is an input, check that it doesn't need to die. */
        while( !LoopInput( p_playlist ) )
            vlc_object_wait( p_playlist );

        LoopRequest( p_playlist );
    }
    vlc_object_unlock( p_playlist );

    vlc_restorecancel (canc);
    return NULL;
}

