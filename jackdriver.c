/* AG: This is a trimmed-down version of the Jack MIDI driver pilfered from
   Spencer Jackson's osc2midi program, cf. https://github.com/ssj71/OSC2MIDI. */

/*-
 * Copyright (c) 2014 Spencer Jackson <ssjackson71@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sysexits.h>
#include <errno.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>
#include <jack/session.h>
#include "jackdriver.h"


typedef struct _MidiMessage
{
    jack_nframes_t	time;
    int		len;	/* Length of MIDI message, in bytes. */
    uint8_t	data[3];
} MidiMessage;

#define RINGBUFFER_SIZE		256*sizeof(MidiMessage)

/* Will emit a warning if time between jack callbacks is longer than this. */
#define MAX_TIME_BETWEEN_CALLBACKS	0.1

/* Will emit a warning if execution of jack callback takes longer than this. */
#define MAX_PROCESSING_TIME	0.01


///////////////////////////////////////////////
//These functions operate in the JACK RT Thread
///////////////////////////////////////////////

double
get_time(void)
{
    double seconds;
    int ret;
    struct timeval tv;

    ret = gettimeofday(&tv, NULL);

    if (ret)
    {
        perror("gettimeofday");
        exit(EX_OSERR);
    }

    seconds = tv.tv_sec + tv.tv_usec / 1000000.0;

    return (seconds);
}

double
get_delta_time(void)
{
    static double previously = -1.0;
    double now;
    double delta;

    now = get_time();

    if (previously == -1.0)
    {
        previously = now;

        return (0);
    }

    delta = now - previously;
    previously = now;

    assert(delta >= 0.0);

    return (delta);
}


double
nframes_to_ms(jack_client_t* jack_client,jack_nframes_t nframes)
{
    jack_nframes_t sr;

    sr = jack_get_sample_rate(jack_client);

    assert(sr > 0);

    return ((nframes * 1000.0) / (double)sr);
}

void
queue_message(jack_ringbuffer_t* ringbuffer, MidiMessage *ev)
{
    int written;

    if (jack_ringbuffer_write_space(ringbuffer) < sizeof(*ev))
    {
        printf("Not enough space in the ringbuffer, MIDI LOST.");
        return;
    }

    written = jack_ringbuffer_write(ringbuffer, (char *)ev, sizeof(*ev));

    if (written != sizeof(*ev))
        printf("jack_ringbuffer_write failed, MIDI LOST.");
}

void
process_midi_output(JACK_SEQ* seq,jack_nframes_t nframes)
{
    int read, t;
    uint8_t *buffer;
    void *port_buffer;
    jack_nframes_t last_frame_time;
    MidiMessage ev;

    last_frame_time = jack_last_frame_time(seq->jack_client);

    port_buffer = jack_port_get_buffer(seq->output_port, nframes);
    if (port_buffer == NULL)
    {
        printf("jack_port_get_buffer failed, cannot send anything.");
        return;
    }

#ifdef JACK_MIDI_NEEDS_NFRAMES
    jack_midi_clear_buffer(port_buffer, nframes);
#else
    jack_midi_clear_buffer(port_buffer);
#endif

    while (jack_ringbuffer_read_space(seq->ringbuffer_out))
    {
        read = jack_ringbuffer_peek(seq->ringbuffer_out, (char *)&ev, sizeof(ev));

        if (read != sizeof(ev))
        {
            //warn_from_jack_thread_context("Short read from the ringbuffer, possible note loss.");
            jack_ringbuffer_read_advance(seq->ringbuffer_out, read);
            continue;
        }

        t = ev.time + nframes - last_frame_time;

        /* If computed time is too much into the future, we'll need
           to send it later. */
        if (t >= (int)nframes)
            break;

        /* If computed time is < 0, we missed a cycle because of xrun. */
        if (t < 0)
            t = 0;

        jack_ringbuffer_read_advance(seq->ringbuffer_out, sizeof(ev));

#ifdef JACK_MIDI_NEEDS_NFRAMES
        buffer = jack_midi_event_reserve(port_buffer, t, ev.len, nframes);
#else
        buffer = jack_midi_event_reserve(port_buffer, t, ev.len);
#endif

        if (buffer == NULL)
        {
            //warn_from_jack_thread_context("jack_midi_event_reserve failed, NOTE LOST.");
            break;
        }

        memcpy(buffer, ev.data, ev.len);
    }
}

int
process_callback(jack_nframes_t nframes, void *seqq)
{
    JACK_SEQ* seq = (JACK_SEQ*)seqq;
#ifdef MEASURE_TIME
    if (get_delta_time() > MAX_TIME_BETWEEN_CALLBACKS)
        printf("Had to wait too long for JACK callback; scheduling problem?");
#endif

    process_midi_output( seq,nframes );

#ifdef MEASURE_TIME
    if (get_delta_time() > MAX_PROCESSING_TIME)
        printf("Processing took too long; scheduling problem?");
#endif

    return (0);
}

///////////////////////////////////////////////
//these functions are executed in other threads
///////////////////////////////////////////////
void queue_midi(void* seqq, uint8_t msg[])
{
    MidiMessage ev;
    JACK_SEQ* seq = (JACK_SEQ*)seqq;
    ev.len = 3;

    // At least with JackOSX, Jack will transmit the bytes verbatim, so make
    // sure that we look at the status byte and trim the message accordingly,
    // in order not to transmit any invalid MIDI data.
    switch (msg[0] & 0xf0)
    {
    case 0x80:
    case 0x90:
    case 0xa0:
    case 0xb0:
    case 0xe0:
        break; // 2 data bytes
    case 0xc0:
    case 0xd0:
        ev.len = 2; // 1 data byte
        break;
    case 0xf0: // system message
        switch (msg[0])
        {
        case 0xf2:
            break; // 2 data bytes
        case 0xf1:
        case 0xf3:
            ev.len = 2; // 1 data byte
            break;
        case 0xf6:
        case 0xf8:
        case 0xf9:
        case 0xfa:
        case 0xfb:
        case 0xfc:
        case 0xfe:
        case 0xff:
            ev.len = 1; // no data byte
            break;
        default:
            // ignore unknown (most likely sysex)
            return;
        }
        break;
    default:
        return; // not a valid MIDI message, bail out
    }

    ev.data[0] = msg[0];
    ev.data[1] = msg[1];
    ev.data[2] = msg[2];

    ev.time = jack_frame_time(seq->jack_client);
    queue_message(seq->ringbuffer_out,&ev);
}

// support for Jack shutdown and session management

#include <signal.h>

void
shutdown_callback()
{
  // We signal the main thread here, which may be blocked in a read
  // operation. The signal will interrupt the read operation so that the main
  // thread can do a proper shutdown.
  kill(getpid(), SIGINT);
}

char *jack_command_line = "shuttlepro";

void
session_callback(jack_session_event_t *event, void *seqq)
{
  JACK_SEQ* seq = (JACK_SEQ*)seqq;

  event->command_line = strdup(jack_command_line);
  jack_session_reply(seq->jack_client, event);

  if (event->type == JackSessionSaveAndQuit)
    kill(getpid(), SIGINT);

  jack_session_event_free (event);
}

////////////////////////////////
//this is run in the main thread
////////////////////////////////
int
init_jack(JACK_SEQ* seq, uint8_t verbose)
{
    int err;
    char *client_name = seq->client_name?seq->client_name:"shuttlepro";
    jack_status_t status;

    if(verbose)printf("opening client...\n");
    seq->jack_client = jack_client_open(client_name, JackNullOption, &status);

    if (seq->jack_client == NULL)
    {
        printf("Could not connect to the JACK server; run jackd first?\n");
        return 0;
    }

    if (verbose && (status & JackServerStarted)) {
      printf("JACK server started\n");
    }
    if (verbose && (status & JackNameNotUnique)) {
      client_name = jack_get_client_name(seq->jack_client);
      printf("JACK client name changed to: %s\n", client_name);
    }

    if(verbose)printf("assigning shutdown callback...\n");
    jack_on_shutdown(seq->jack_client, shutdown_callback, (void*)seq);

    if(verbose)printf("assigning session callback...\n");
    jack_set_session_callback(seq->jack_client, session_callback, (void*)seq);

    if(verbose)printf("assigning process callback...\n");
    err = jack_set_process_callback(seq->jack_client, process_callback, (void*)seq);
    if (err)
    {
        printf("Could not register JACK process callback.\n");
        return 0;
    }

    if(verbose)printf("initializing JACK output: \ncreating ringbuffer...\n");
    seq->ringbuffer_out = jack_ringbuffer_create(RINGBUFFER_SIZE);

    if (seq->ringbuffer_out == NULL)
    {
        printf("Cannot create JACK ringbuffer.\n");
        return 0;
    }

    jack_ringbuffer_mlock(seq->ringbuffer_out);

    seq->output_port = jack_port_register(seq->jack_client, "midi_out",
					  JACK_DEFAULT_MIDI_TYPE,
					  JackPortIsOutput, 0);

    if (seq->output_port == NULL)
    {
        printf("Could not register JACK port.\n");
        return 0;
    }

    if (jack_activate(seq->jack_client))
    {
        printf("Cannot activate JACK client.\n");
        return 0;
    }
    return 1;
}

void close_jack(JACK_SEQ* seq)
{
    jack_ringbuffer_free(seq->ringbuffer_out);
}
