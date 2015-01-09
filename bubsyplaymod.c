#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <ao/ao.h>

#include "playptmod.h"

#define SAMPLE_RATE 44100 /* 22050 - test low-pass capability of blep synthesizer */

static int running;

void signal_handler(int sig)
{
	running = 0;
    signal(sig, signal_handler);
}

void fade_buffer(signed short *buffer, unsigned int count, int fade_start, int fade_length)
{
    unsigned int i;
    for (i = 0; i < count; i++)
    {
        if (fade_start < fade_length)
        {
            buffer[ i * 2 + 0 ] = (int64_t)((int64_t)buffer[ i * 2 + 0 ] * ( fade_length - fade_start )) / fade_length;
            buffer[ i * 2 + 1 ] = (int64_t)((int64_t)buffer[ i * 2 + 1 ] * ( fade_length - fade_start )) / fade_length;
            fade_start++;
        }
        else
        {
            buffer[ i * 2 + 0 ] = 0;
            buffer[ i * 2 + 1 ] = 0;
        }
    }
}

int main(int argc, const char* const* argv)
{
	void * player;
	ao_device * dev;
    ao_sample_format fmt = { 16, SAMPLE_RATE, 2, AO_FMT_NATIVE, 0 };

	signed short sample_buffer[2048 * 2];

	if (argc != 2)
	{
		fprintf(stderr, "Usage:\t%s <song.mod>\n", argv[0]);
		return 1;
	}

    player = playptmod_Create(SAMPLE_RATE);

    playptmod_Config( player, PTMOD_OPTION_CLAMP_PERIODS, 0 );

	if ( !playptmod_Load(player, argv[1]) ) return 1;

    playptmod_Play(player, 0);

	signal(SIGINT, signal_handler);

	ao_initialize();

	dev = ao_open_live( ao_default_driver_id(), &fmt, NULL );

	if ( dev )
	{
        int fade_start = 0, fade_length = SAMPLE_RATE * 10;
        unsigned int max_channels = 0;
        playptmod_info info;
        running = 1;
        while ( running && fade_start < fade_length )
		{
			playptmod_Render16(player, sample_buffer, 2048);
            if (playptmod_LoopCounter(player) >= 2)
            {
                fade_buffer( sample_buffer, 2048, fade_start, fade_length );
                fade_start += 2048;
            }
			ao_play( dev, (char*) sample_buffer, 2048 * 4 );
            playptmod_GetInfo(player, &info);
            fprintf(stderr, "\ro: %3u - p: %3u - r: %2u - c: %2u (%2u)", info.order, info.pattern, info.row, info.channelsPlaying, info.channelsPlaying > max_channels ? max_channels = info.channelsPlaying : max_channels);
		}
        fprintf(stderr, "\n");

		ao_close( dev );
	}

	ao_shutdown();

	playptmod_Stop(player);
	playptmod_Free(player);

	return 0;
}
