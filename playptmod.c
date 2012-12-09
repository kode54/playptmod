/*
** - --=playptmod v0.64 - 8bitbubsy 2010-2012=-- -
** This is the portable library version, which requires a separate
** audio output method, to be used in a production zip/rar whatever.
**
** Thanks to mukunda for learning me how to code a .MOD player
** some years back!
**
** Thanks to ad_/aciddose/adejr for the BLEP and LED filter
** routines.
**
** Note: There's a lot of weird behavior in the coding to
** "emulate" the weird stuff ProTracker on the Amiga does.
** If you see something fishy in the code, it's probably
** supposed to be like that. Please don't change it, you're
** literally asking for hours of hard debugging if you do.
**
** HOW DO I USE THIS FILE?
** Make a new file called main.c, and put this on top:
**
** #include <stdio.h>
**
** void * playptmod_Create(int soundFrequency);
** int playptmod_Load(void *, const char *filename);
** void playptmod_Play(void *);
** void playptmod_Render(void *, signed short *, int);
** void playptmod_Free(void *);
**
** void main(void)
** {
**		int app_running = 1;
**
**		void *p = playptmod_Create(44100);
**		playptmod_Load(p, "hello.mod");
**		playptmod_Play(p);
**
**		while (app_running)
**		{
**      signed short samples[1024];
**
**			if (someone_pressed_a_key())
**			{
**				app_running = 0;
**			}
**
**      playptmod_Render(p, samples, 512);
**      // output samples to system here
**
**			// Make sure to delay a bit here
**		}
**		
**		playptmod_Free(p);
**
**		return 0;
** }
**
**
** You can also integrate it as a resource in the EXE,
** and use some Win32 API functions to copy the MOD
** to memory and get a pointer to it. Then you call
** playptmod_LoadMem instead.
** Perfect for those cracktros!
**
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "playptmod.h"

#define HI_NYBBLE(x) ((x) >> 4)
#define LO_NYBBLE(x) ((x) & 0x0F)
#define CLAMP(x, low, high)  (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define LERP(I, F) (I[0] + F * (I[1] - I[0]))
#define DENORMAL_OFFSET 1E-10f
#define USE_HIGHPASS 1
#define USE_BLEP 1
#define ZC 8
#define OS 5
#define SP 5
#define NS (ZC * OS / SP)
#define RNS 7
#ifdef FM_PI
#undef FM_PI
#endif
#define FM_PI 3.1415926f
#define PT_MIN_PERIOD 113
#define PT_MAX_PERIOD 856
#define PAULA_CHANNELS 32
#define MOD_ROWS 64
#define MOD_SAMPLES 31

enum
{
  FORMAT_MK,     // ProTracker 1.x
  FORMAT_MK2,    // ProTracker 2.x (if tune has >64 patterns)
  FORMAT_FLT4,   // StarTrekker
  FORMAT_FLT8,
  FORMAT_NCHN,   // FastTracker II (only 1-9 channel MODs)
  FORMAT_NNCH,   // FastTracker II (10-32 channel MODs)
  FORMAT_16CN,   // FastTracker II (16 channel MODs)
  FORMAT_32CN,   // FastTracker II (32 channel MODs)
  FORMAT_STK,    // The Ultimate SoundTracker (15 samples)
  FORMAT_NT,     // NoiseTracker 1.0

  FORMAT_MTM,    // MultiTracker

  FORMAT_UNKNOWN
};

enum
{
  FLAG_NOTE = 1,
  FLAG_SAMPLE = 2,
  FLAG_NEWSAMPLE = 4,
  AFLAG_START = 1,
  AFLAG_DELAY = 2,
  AFLAG_NEW_SAMPLE = 4
};

enum
{
  soundBufferSize = 2048 * 4
};

typedef struct modnote
{
  unsigned char sample, command, param;
  short period;
} modnote_t;

typedef struct
{
  unsigned char order_count, pattern_count, row_count, restart_pos, order[128], vol[PAULA_CHANNELS], pan[PAULA_CHANNELS], tempo, ticks, format, channel_count;
} MODULE_HEADER;

typedef struct
{
  unsigned char volume;
  signed char finetune;
  unsigned int loop_start, loop_length, length;
  unsigned int offset;
  unsigned char attribute;
} MODULE_SAMPLE;

typedef struct
{
  unsigned char seqchannel, flags, aflags, tmp_aflags, finetune;
  unsigned char tremoloctrl, tremolospeed, tremolodepth;
  unsigned char vibratoctrl, vibratospeed, vibratodepth;
  unsigned char glissandoctrl, glissandospeed;
  unsigned char invloop_delay, invloop_speed;
  unsigned char sample, command, param;
  signed char pattern_loop_row, pattern_loop_times, volume;
  signed short period, tperiod;	
  unsigned int tremolopos, vibratopos, offset, sample_offset_temp, invloop_offset;
  int no_note, bug_offset_not_added;
} mod_channel;

typedef struct
{
  unsigned char module_loaded, MAX_PATTERNS;
  signed char *sample_data, *original_sample_data;
  int total_sample_size;
  MODULE_HEADER head;
  MODULE_SAMPLE samples[256];
  modnote_t *patterns[256];
  mod_channel channels[PAULA_CHANNELS];
} MODULE;

typedef struct paula_filter_state
{
  float led[4], high[2];
} Filter;

typedef struct paula_filter_coefficients
{
  float led, led_fb, high;
} FilterC;

typedef struct voice_data
{
  const signed char *data, *new_data;
  signed int vol, pan_l, pan_r;
  int len, loop_len, loop_end;
  int index, new_len, new_loop_len, new_loop_end, swap_sample_flag;
  int step, new_step;
  float rate, frac;
} Voice;

typedef struct blep_data
{
  signed int index;
  signed int last_value;
  signed int samples_left;
  float buffer[RNS + 1];
} blep;

typedef struct player_data
{
  unsigned char mod_speed, mod_bpm, mod_tick, mod_pattern, mod_order, mod_start_order, aflags;
  unsigned char PBreakPosition, PattDelayTime, PattDelayTime2;
  signed char *mixerBuffer, mod_row, avolume; /* must be signed */
  signed short aperiod;
  int moduleLoaded, modulePlaying, tempoTimerVal, use_led_filter;
  int soundFrequency, PosJumpAssert, PBreakFlag;
  unsigned int loop_counter;
  signed int mod_samplecounter;
  unsigned int mod_samplespertick;
  float vsync_block_length, vsync_samples_left, *mixer_buffer_l, *mixer_buffer_r, *pt_period_freq_tab, *pt_extended_period_freq_tab;
  unsigned char *pt_tab_vibsine;
  int minPeriod, maxPeriod, calculatedMinPeriod, calculatedMaxPeriod;
  int vsync_timing;
  Voice v[PAULA_CHANNELS];
  Filter filter;
  FilterC filter_c;
  blep b[PAULA_CHANNELS], b_vol[PAULA_CHANNELS];
  MODULE *source;
} player;

static const unsigned int blepdata[48] =
{
  0x3f7fe1f1, 0x3f7fd548, 0x3f7fd6a3, 0x3f7fd4e3, 0x3f7fad85, 0x3f7f2152, 0x3f7dbfae, 0x3f7accdf,
  0x3f752f1e, 0x3f6b7384, 0x3f5bfbcb, 0x3f455cf2, 0x3f26e524, 0x3f0128c4, 0x3eacc7dc, 0x3e29e86b,
  0x3c1c1d29, 0xbde4bbe6, 0xbe3aae04, 0xbe48dedd, 0xbe22ad7e, 0xbdb2309a, 0xbb82b620, 0x3d881411,
  0x3ddadbf3, 0x3de2c81d, 0x3daaa01f, 0x3d1e769a, 0xbbc116d7, 0xbd1402e8, 0xbd38a069, 0xbd0c53bb,
  0xbc3ffb8c, 0x3c465fd2, 0x3cea5764, 0x3d0a51d6, 0x3ceae2d5, 0x3c92ac5a, 0x3be4cbf7, 0x00000000,
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
};

static const unsigned char pt_tab_invloop[16] =
{
  0x00, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0D, 0x0F, 0x13, 0x16, 0x1A, 0x20, 0x2B, 0x40, 0x80
};

static const short rawPeriodTable[592] =
{
  856,808,762,720,678,640,604,570,538,508,480,453,
  428,404,381,360,339,320,302,285,269,254,240,226,
  214,202,190,180,170,160,151,143,135,127,120,113,0,
  850,802,757,715,674,637,601,567,535,505,477,450,
  425,401,379,357,337,318,300,284,268,253,239,225,
  213,201,189,179,169,159,150,142,134,126,119,113,0,
  844,796,752,709,670,632,597,563,532,502,474,447,
  422,398,376,355,335,316,298,282,266,251,237,224,
  211,199,188,177,167,158,149,141,133,125,118,112,0,
  838,791,746,704,665,628,592,559,528,498,470,444,
  419,395,373,352,332,314,296,280,264,249,235,222,
  209,198,187,176,166,157,148,140,132,125,118,111,0,
  832,785,741,699,660,623,588,555,524,495,467,441,
  416,392,370,350,330,312,294,278,262,247,233,220,
  208,196,185,175,165,156,147,139,131,124,117,110,0,
  826,779,736,694,655,619,584,551,520,491,463,437,
  413,390,368,347,328,309,292,276,260,245,232,219,
  206,195,184,174,164,155,146,138,130,123,116,109,0,
  820,774,730,689,651,614,580,547,516,487,460,434,
  410,387,365,345,325,307,290,274,258,244,230,217,
  205,193,183,172,163,154,145,137,129,122,115,109,0,
  814,768,725,684,646,610,575,543,513,484,457,431,
  407,384,363,342,323,305,288,272,256,242,228,216,
  204,192,181,171,161,152,144,136,128,121,114,108,0,
  907,856,808,762,720,678,640,604,570,538,508,480,
  453,428,404,381,360,339,320,302,285,269,254,240,
  226,214,202,190,180,170,160,151,143,135,127,120,0,
  900,850,802,757,715,675,636,601,567,535,505,477,
  450,425,401,379,357,337,318,300,284,268,253,238,
  225,212,200,189,179,169,159,150,142,134,126,119,0,
  894,844,796,752,709,670,632,597,563,532,502,474,
  447,422,398,376,355,335,316,298,282,266,251,237,
  223,211,199,188,177,167,158,149,141,133,125,118,0,
  887,838,791,746,704,665,628,592,559,528,498,470,
  444,419,395,373,352,332,314,296,280,264,249,235,
  222,209,198,187,176,166,157,148,140,132,125,118,0,
  881,832,785,741,699,660,623,588,555,524,494,467,
  441,416,392,370,350,330,312,294,278,262,247,233,
  220,208,196,185,175,165,156,147,139,131,123,117,0,
  875,826,779,736,694,655,619,584,551,520,491,463,
  437,413,390,368,347,328,309,292,276,260,245,232,
  219,206,195,184,174,164,155,146,138,130,123,116,0,
  868,820,774,730,689,651,614,580,547,516,487,460,
  434,410,387,365,345,325,307,290,274,258,244,230,
  217,205,193,183,172,163,154,145,137,129,122,115,0,
  862,814,768,725,684,646,610,575,543,513,484,457,
  431,407,384,363,342,323,305,288,272,256,242,228,
  216,203,192,181,171,161,152,144,136,128,121,114,0
};

static short extendedRawPeriodTable[16 * 85];

static const short npertab[84] = {
    /* Octaves 6 -> 0 */
    /* C    C#     D    D#     E     F    F#     G    G#     A    A#     B */
    0x6b0,0x650,0x5f4,0x5a0,0x54c,0x500,0x4b8,0x474,0x434,0x3f8,0x3c0,0x38a,
    0x358,0x328,0x2fa,0x2d0,0x2a6,0x280,0x25c,0x23a,0x21a,0x1fc,0x1e0,0x1c5,
    0x1ac,0x194,0x17d,0x168,0x153,0x140,0x12e,0x11d,0x10d,0x0fe,0x0f0,0x0e2,
    0x0d6,0x0ca,0x0be,0x0b4,0x0aa,0x0a0,0x097,0x08f,0x087,0x07f,0x078,0x071,
    0x06b,0x065,0x05f,0x05a,0x055,0x050,0x04b,0x047,0x043,0x03f,0x03c,0x038,
    0x035,0x032,0x02f,0x02d,0x02a,0x028,0x025,0x023,0x021,0x01f,0x01e,0x01c,
    0x01b,0x019,0x018,0x016,0x015,0x014,0x013,0x012,0x011,0x010,0x00f,0x00e
};

static const short finetune[16]={
    8363,8413,8463,8529,8581,8651,8723,8757,
    7895,7941,7985,8046,8107,8169,8232,8280
};

void freetables(player *p)
{
  free(p->pt_tab_vibsine);
  free(p->pt_period_freq_tab);
  free(p->pt_extended_period_freq_tab);
}

void maketables(player *p, int sound_frequency)
{
  int i, j;
  int minPeriod, maxPeriod;

  p->tempoTimerVal = (sound_frequency * 125) / 50;

  p->pt_tab_vibsine = (unsigned char *)malloc(32);
  for (i = 0; i < 32; i++)
    p->pt_tab_vibsine[i] = (unsigned char)floorf(sinf((float)i * FM_PI / 32.0f) * 255.0f);

  p->pt_period_freq_tab = (float *)malloc(sizeof (float) * 908);
  for (i = 108; i <= 907; i++) // 0..107 will never be looked up, junk is OK
    p->pt_period_freq_tab[i] = (3546895.0f / (float)i) / (float)sound_frequency;

  for (j = 0; j < 16; j++)
    for (i = 0; i < 85; i++)
      extendedRawPeriodTable[(j * 85) + i] = i == 84 ? 0 : npertab[i] * 8363 / finetune[j];

  p->calculatedMaxPeriod = maxPeriod = extendedRawPeriodTable[8 * 85];
  p->calculatedMinPeriod = minPeriod = extendedRawPeriodTable[7 * 85 + 83];

  p->pt_extended_period_freq_tab = (float *)malloc(sizeof (float) * (maxPeriod + 1));
  for (i = minPeriod; i <= maxPeriod; i++)
    p->pt_extended_period_freq_tab[i] = (3546895.0f / (float)i) / (float)sound_frequency;
}

static inline int period2note(player *p, int finetune, int period)
{
  int i;
  if (p->minPeriod == PT_MIN_PERIOD)
  {
    for (i = 0; i < 36; i++)
    {
      if (rawPeriodTable[(finetune * 37) + i] <= period)
      break;
    }
  }
  else
  {
    for (i = 0; i < 84; i++)
    {
      if (extendedRawPeriodTable[(finetune * 85) + i] <= period)
        break;
    }
  }

  return i;
}

static float calculate_rc_coefficient(const float sample_rate, const float cutoff_freq)
{
  if (cutoff_freq >= (sample_rate / 2.0f))
    return 1.0f;

  return 2.0f * FM_PI * cutoff_freq / sample_rate;
}

typedef struct
{
  unsigned long length, remain;
  const unsigned char *t_buf;
  const unsigned char *buf;
} BUF;

static BUF *bufopen(const unsigned char *bufToCopy, unsigned int bufferSize)
{
  BUF *b = (BUF *)malloc(sizeof (BUF));

  b->t_buf = bufToCopy;
  b->buf = &bufToCopy[0];

  b->length = bufferSize;
  b->remain = bufferSize;

  return b;
}

static void bufclose(BUF *_SrcBuf)
{
  free(_SrcBuf);
}

static void bufread(void *_DstBuf, size_t _ElementSize, size_t _Count, BUF *_SrcBuf)
{
  _Count *= _ElementSize;
  if (_Count > _SrcBuf->remain) _Count = _SrcBuf->remain;
  _SrcBuf->remain -= _Count;
  memcpy(_DstBuf, _SrcBuf->buf, _Count);
  _SrcBuf->buf += _Count;
}

static void bufseek(BUF *_SrcBuf, long _Offset, int _Origin)
{
  if (_SrcBuf->buf != NULL)
  {
    switch (_Origin)
    {
      case SEEK_SET: _SrcBuf->buf = _SrcBuf->t_buf + _Offset; break;
      case SEEK_CUR: _SrcBuf->buf += _Offset; break;
      default: break;
    }
    _Offset = _SrcBuf->buf - _SrcBuf->t_buf;
    _SrcBuf->remain = _Offset > _SrcBuf->length ? 0 : _SrcBuf->length - _Offset;
  }
}

static void blep_add(blep *b, float offset, float amplitude)
{
  const float *src;
  signed int i;
  signed int n = NS;
  float f;

  if (offset > 0.999999999f) // weird bugfix, TODO: Inspect
    return;

  i = (signed int)(offset * SP);
  src = (const float *)blepdata + i + SP;
  f = offset * SP - i;

  i = b->index;
  while (n--)
  {
    b->buffer[i] += amplitude * LERP(src, f);
    src += SP;
    i++;
    i &= RNS;
  }

  b->samples_left = NS;
}

static float blep_run(blep *b)
{
  const float output = b->buffer[b->index];

  b->buffer[b->index] = 0.0f;
  b->index++;
  b->index &= RNS;
  b->samples_left--;

  return output;
}

static void mixer_change_ch_src(player *p, int ch, const signed char *src, int len, int loop_start, int loop_len, int step)
{
  if (src != NULL)
  {
    p->v[ch].swap_sample_flag = 1;

    p->v[ch].new_data = src;
    p->v[ch].new_len = len;
    p->v[ch].new_loop_len = loop_len;
    p->v[ch].new_loop_end = loop_len + loop_start;
    p->v[ch].new_step = step;
  }
}

static void mixer_set_ch_src(player *p, int ch, const signed char *src, int len, int loop_start, int loop_len, int offset, int step)
{
  if (src != NULL)
  {
    p->v[ch].swap_sample_flag = 0;
    p->v[ch].data = src;
    p->v[ch].len = len;
    p->v[ch].index = offset;
    p->v[ch].frac = 0.0f;
    p->v[ch].loop_end = loop_start + loop_len;
    p->v[ch].loop_len = loop_len;
    p->v[ch].step = step;

    if (p->v[ch].index > 0)
    {
      if (p->v[ch].loop_len >= 4)
      {
        if (p->v[ch].index >= p->v[ch].loop_end)
          p->v[ch].index = 0;
      }
      else if (p->v[ch].index >= p->v[ch].len)
      {
        p->v[ch].data = NULL;
      }
    }
  }
}

static void mixer_set_ch_pan(player *p, int ch, int pan)
{
  p->v[ch].pan_l = 255 - pan;
  p->v[ch].pan_r = pan;
}

static void mixer_set_ch_vol(player *p, int ch, int vol)
{
  p->v[ch].vol = vol;
}

static void mixer_cut_channels(player *p)
{
  int i;

  memset(p->v, 0, sizeof (p->v));
  memset(p->b, 0, sizeof (p->b));
  memset(p->b_vol, 0, sizeof (p->b_vol));
  memset(&p->filter, 0, sizeof (p->filter));

  p->vsync_samples_left = 0.0f;

  if (p->source)
    for (i = 0; i < PAULA_CHANNELS; i++)
    {
      mixer_set_ch_vol(p, i, p->source->head.vol[i]);
      mixer_set_ch_pan(p, i, p->source->head.pan[i]);
    }
  else
    for (i = 0; i < PAULA_CHANNELS; i++)
    {
      mixer_set_ch_vol(p, i, 64);
      mixer_set_ch_pan(p, i, (i + 1) & 2 ? 192 : 64);
    }
}

static void mixer_set_ch_freq(player *p, int ch, float rate)
{
  p->v[ch].rate = rate;
}

static void mixer_output_audio(player *p, signed short *target, int samples_to_mix)
{
  int i, j;
  int step;
  signed short *out;

  memset(p->mixer_buffer_l, 0, sizeof (float) * samples_to_mix);
  memset(p->mixer_buffer_r, 0, sizeof (float) * samples_to_mix);

  for (i = 0; i < p->source->head.channel_count; i++)
  {
    j = 0;

    if (p->v[i].data)
    {
      step = p->v[i].step;
      for (j = 0; j < samples_to_mix;)
      {
        float offset = p->v[i].frac / (p->v[i].rate ? p->v[i].rate : 1.0f);

        while (offset >= 0.0f && j < samples_to_mix)
        {
          signed short t_s = (p->v[i].data ? (step == 2 ? (p->v[i].data[p->v[i].index] + p->v[i].data[p->v[i].index + 1] * 0x100) : p->v[i].data[p->v[i].index] * 0x100) : 0);
          signed int t_v = (p->v[i].data ? p->v[i].vol : 0);
          float t_vol = (float)t_v;
          float t_smp = (float)t_s;
          float t_offset = offset - floor(offset);
          signed int i_smp;

          offset -= 1.0f;

          if (t_s != p->b[i].last_value)
          {
            float delta = (float)(p->b[i].last_value - t_s);
            p->b[i].last_value = t_s;
            blep_add(&p->b[i], t_offset, delta);
          }

          if (t_v != p->b_vol[i].last_value)
          {
            float delta = (float)(p->b_vol[i].last_value - t_v);
            p->b_vol[i].last_value = t_v;
            blep_add(&p->b_vol[i], 0, delta);
          }

          if (p->b_vol[i].samples_left)
            t_vol += blep_run(&p->b_vol[i]);

          if (p->b[i].samples_left)
            t_smp += blep_run(&p->b[i]);

          t_smp *= t_vol;
          i_smp = (signed int)t_smp;

          p->mixer_buffer_l[j] += i_smp * p->v[i].pan_l;
          p->mixer_buffer_r[j] += i_smp * p->v[i].pan_r;

          j++;

          if (p->v[i].data)
          {
            p->v[i].frac += p->v[i].rate;

            if (p->v[i].frac >= 1.0f)
            {
              p->v[i].index += step;
              p->v[i].frac -= 1.0f;

              if (p->v[i].loop_len >= 4 * step)
              {
                if (p->v[i].index >= p->v[i].loop_end)
                {
                  if (p->v[i].swap_sample_flag)
                  {
                    p->v[i].swap_sample_flag = 0;

                    if (p->v[i].new_loop_len <= 2 * step)
                    {
                      p->v[i].data = NULL;

                      continue;
                    }

                    p->v[i].data = p->v[i].new_data;
                    p->v[i].len = p->v[i].new_len;
                    p->v[i].loop_end = p->v[i].new_loop_end;
                    p->v[i].loop_len = p->v[i].new_loop_len;
                    step = p->v[i].step = p->v[i].new_step;

                    p->v[i].index = p->v[i].loop_end - p->v[i].loop_len;
                  }
                  else
                  {
                    p->v[i].index -= p->v[i].loop_len;
                  }
                }
              }
              else if (p->v[i].index >= p->v[i].len)
              {
                if (p->v[i].swap_sample_flag)
                {
                  p->v[i].swap_sample_flag = 0;

                  p->v[i].data = p->v[i].new_data;
                  p->v[i].len = p->v[i].new_len;
                  p->v[i].loop_end = p->v[i].new_loop_end;
                  p->v[i].loop_len = p->v[i].new_loop_len;
                  step = p->v[i].step = p->v[i].new_step;
                }
                else
                {
                  p->v[i].data = NULL;
                }
              }
            }
          }
        }
      }
    }
    else if (p->v[i].swap_sample_flag)
    {
      p->v[i].swap_sample_flag = 0;

      p->v[i].data = p->v[i].new_data;
      p->v[i].len = p->v[i].new_len;
      p->v[i].loop_end = p->v[i].new_loop_end;
      p->v[i].loop_len = p->v[i].new_loop_len;
      p->v[i].step = p->v[i].new_step;
    }

    if ((j < samples_to_mix) && (p->v[i].data == NULL) && (p->b[i].samples_left || p->b_vol[i].samples_left))
    {
      for (; j < samples_to_mix; j++)
      {
        float t_vol = (float)p->b_vol[i].last_value;
        float t_smp = (float)p->b[i].last_value;
        signed int i_smp;

        if (p->b_vol[i].samples_left)
          t_vol += blep_run(&p->b_vol[i]);

        if (p->b[i].samples_left)
          t_smp += blep_run(&p->b[i]);

        t_smp *= t_vol;
        i_smp = (signed int)t_smp;

        p->mixer_buffer_l[j] += i_smp * p->v[i].pan_l;
        p->mixer_buffer_r[j] += i_smp * p->v[i].pan_r;
      }
    }
  }

  out = target;

  {
    static const float downscale = 1.0f / (96.0f * 256.0f);

    for (i = 0; i < samples_to_mix; i++)
    {
      float L = p->mixer_buffer_l[i];
      float R = p->mixer_buffer_r[i];

      if (p->use_led_filter)
      {
        p->filter.led[0] += p->filter_c.led * (L - p->filter.led[0])
          + p->filter_c.led_fb * (p->filter.led[0] - p->filter.led[1]) + DENORMAL_OFFSET;
        p->filter.led[1] += p->filter_c.led * (p->filter.led[0] - p->filter.led[1]) + DENORMAL_OFFSET;

        p->filter.led[2] += p->filter_c.led * (R - p->filter.led[2])
          + p->filter_c.led_fb * (p->filter.led[2] - p->filter.led[3]) + DENORMAL_OFFSET;
        p->filter.led[3] += p->filter_c.led * (p->filter.led[2] - p->filter.led[3]) + DENORMAL_OFFSET;

        L = p->filter.led[1];
        R = p->filter.led[3];
      }

      L -= p->filter.high[0];
      R -= p->filter.high[1];

      p->filter.high[0] += p->filter_c.high * L + DENORMAL_OFFSET;
      p->filter.high[1] += p->filter_c.high * R + DENORMAL_OFFSET;

      *out++ = (signed int)CLAMP(L * downscale, -32768, 32767);
      *out++ = (signed int)CLAMP(R * downscale, -32768, 32767);
    }
  }
}

static unsigned short file_get_word_bigendian(BUF *in)
{
  unsigned char bytes[2];
  bufread(bytes, 1, 2, in);

  return (bytes[0] << 8) | bytes[1];
}

static unsigned short file_get_word_littleendian(BUF *in)
{
  unsigned char bytes[2];
  bufread(bytes, 1, 2, in);

  return (bytes[1] << 8) | bytes[0];
}

static unsigned int file_get_dword_littleendian(BUF *in)
{
  unsigned char bytes[4];
  bufread(bytes, 1, 4, in);

  return (bytes[3] << 24) | (bytes[2] << 16) | (bytes[1] << 8) | bytes[0];
}

static int playptmod_LoadMTM(player *p, BUF *fModule)
{
  int i, j, k;
  unsigned int track_count, comment_length;
  unsigned char sample_count;
  unsigned long tracks_offset, sequences_offset, comment_offset;

  unsigned int total_sample_size = 0, sample_offset = 0;

  modnote_t *note = NULL;

  bufseek(fModule, 24, SEEK_SET);

  track_count = file_get_word_littleendian(fModule);
  bufread(&p->source->head.pattern_count, 1, 1, fModule); p->source->head.pattern_count++;
  bufread(&p->source->head.order_count, 1, 1, fModule); p->source->head.order_count++;
  comment_length = file_get_word_littleendian(fModule);
  bufread(&sample_count, 1, 1, fModule);
  bufseek(fModule, 1, SEEK_CUR);
  bufread(&p->source->head.row_count, 1, 1, fModule);
  bufread(&p->source->head.channel_count, 1, 1, fModule);

  if (!track_count || !sample_count || !p->source->head.row_count || p->source->head.row_count > 64 || !p->source->head.channel_count || p->source->head.channel_count > 32)
    return 0;

  bufread(&p->source->head.pan, 1, 32, fModule);

  for (i = 0; i < 32; i++)
  {
    if (p->source->head.pan[i] <= 15)
    {
      p->source->head.pan[i] -= (p->source->head.pan[i] & 8) / 8;
      p->source->head.pan[i] = (((int)p->source->head.pan[i]) * 255) / 14;
      p->source->head.vol[i] = 64;
    }
    else
    {
      p->source->head.pan[i] = 128;
      p->source->head.vol[i] = 0;
    }
  }

  for (i = 0; i < sample_count; i++)
  {
      bufseek(fModule, 22, SEEK_CUR);

      p->source->samples[i].length = file_get_dword_littleendian(fModule);
      p->source->samples[i].loop_start = file_get_dword_littleendian(fModule);
      p->source->samples[i].loop_length = file_get_dword_littleendian(fModule) - p->source->samples[i].loop_start;
      if (p->source->samples[i].loop_length < 2)
        p->source->samples[i].loop_length = 2;

      bufread(&p->source->samples[i].finetune, 1, 1, fModule);
      p->source->samples[i].finetune = LO_NYBBLE(p->source->samples[i].finetune);

      bufread(&p->source->samples[i].volume, 1, 1, fModule);

      bufread(&p->source->samples[i].attribute, 1, 1, fModule);

      total_sample_size += p->source->samples[i].length;
  }

  bufread(&p->source->head.order, 1, 128, fModule);

  tracks_offset = fModule->length - fModule->remain;
  sequences_offset = tracks_offset + 192 * track_count;
  comment_offset = sequences_offset + 64 * p->source->head.pattern_count;

  for (i = 0; i < p->source->head.pattern_count; i++)
  {
    note = p->source->patterns[i] = (modnote_t *)malloc(sizeof (modnote_t) * p->source->head.row_count * p->source->head.channel_count);
    if (!note)
    {
      for (j = 0; j < i; j++)
      {
        if (p->source->patterns[j])
        {
          free(p->source->patterns[j]);
          p->source->patterns[j] = NULL;
        }
      }
      return 0;
    }
    for (j = 0; j < p->source->head.channel_count; j++)
    {
      int track_number;
      bufseek(fModule, sequences_offset + 64 * i + 2 * j, SEEK_SET);
      track_number = file_get_word_littleendian(fModule);
      if (track_number--)
      {
        bufseek(fModule, tracks_offset + 192 * track_number, SEEK_SET);
        for (k = 0; k < p->source->head.row_count; k++)
        {
          unsigned char buf[3];
          bufread(buf, 1, 3, fModule);
          if (buf[0] || buf[1] || buf[2])
          {
            note[k * p->source->head.channel_count + j].period = (buf[0] / 4) ? extendedRawPeriodTable[buf[0] / 4] : 0;
            note[k * p->source->head.channel_count + j].sample = ((buf[0] << 4) + (buf[1] >> 4)) & 0x3f;
            note[k * p->source->head.channel_count + j].command = buf[1] & 0xf;
            note[k * p->source->head.channel_count + j].param = buf[2];
            if (note[k * p->source->head.channel_count + j].command == 0xf && note[k * p->source->head.channel_count + j].param == 0x00)
              note[k * p->source->head.channel_count + j].command = 0;
          }
        }
      }
    }
  }

  p->source->sample_data = (signed char *)malloc(total_sample_size);
  if (!p->source->sample_data)
  {
    for (i = 0; i < 128; i++)
    {
      if (p->source->patterns[i] != NULL)
      {
        free(p->source->patterns[i]);
        p->source->patterns[i] = NULL;
      }
    }

    return 0;
  }

  bufseek(fModule, comment_offset + comment_length, SEEK_SET);

  for (i = 0; i < sample_count; i++)
  {
    p->source->samples[i].offset = sample_offset;
    bufread(&p->source->sample_data[sample_offset], 1, p->source->samples[i].length, fModule);
    if (!(p->source->samples[i].attribute & 1))
      for (j = sample_offset; j < sample_offset + p->source->samples[i].length; j++)
        p->source->sample_data[j] ^= 0x80;
    sample_offset += p->source->samples[i].length;
  }

  p->source->original_sample_data = (signed char *)malloc(total_sample_size);
  if (p->source->original_sample_data == NULL)
  {
    free(p->source->sample_data);
    p->source->sample_data = NULL;

    for (i = 0; i < 128; i++)
    {
      if (p->source->patterns[i] != NULL)
      {
        free(p->source->patterns[i]);
        p->source->patterns[i] = NULL;
      }
    }

    return 0;
  }

  memcpy(p->source->original_sample_data, p->source->sample_data, total_sample_size);
  p->source->total_sample_size = total_sample_size;

  p->use_led_filter = 0;
  p->moduleLoaded = 1;

  return 1;

}

static void check_mod_tag(MODULE_HEADER *h, const char *buf)
{
  if (!strncmp(buf, "M.K.", 4))
  {
    h->format = FORMAT_MK; // ProTracker v1.x
    h->channel_count = 4;
    return;
  }
  else if (!strncmp(buf, "M!K!", 4))
  {
    h->format = FORMAT_MK2; // ProTracker v2.x (if >64 patterns)
    h->channel_count = 4;
    return;
  }
  else if (!strncmp(buf, "FLT4", 4))
  {
    h->format = FORMAT_FLT4; // StarTrekker (4 channel MODs only)
    h->channel_count = 4;
    return;
  }
  else if (!strncmp(buf, "FLT8", 4))
  {
    h->format = FORMAT_FLT8;
    h->channel_count = 8;
    return;
  }
  else if (!strncmp(buf + 1, "CHN", 3) && buf[0] >= '1' && buf[0] <= '9')
  {
    h->format = FORMAT_NCHN; // FastTracker II (1-9 channel MODs)
    h->channel_count = buf[0] - '0';
    return;
  }
  else if (!strncmp(buf + 2, "CH", 2) && buf[0] >= '1' && buf[0] <= '3' && buf[1] >= '0' && buf[1] <= '9')
  {
    h->format = FORMAT_NNCH; // FastTracker II (10-32 channel MODs);
    h->channel_count = (buf[0] - '0') * 10 + (buf[1] - '0');
    if (h->channel_count > 32)
    {
      h->format = FORMAT_UNKNOWN;
      h->channel_count = 4;
    }
    return;
  }
  else if (!strncmp(buf, "16CN", 4))
  {
    h->format = FORMAT_16CN;
    h->channel_count = 16;
    return;
  }
  else if (!strncmp(buf, "32CN", 4))
  {
    h->format = FORMAT_32CN;
    h->channel_count = 32;
    return;
  }
  else if (!strncmp(buf, "N.T.", 4))
  {
    h->format = FORMAT_MK; // NoiseTracker 1.0, same as ProTracker v1.x (?)
    h->channel_count = 4;
    return;
  }

  h->format = FORMAT_UNKNOWN; // May be The Ultimate SoundTracker, 15 samples
  h->channel_count = 4;
}

int playptmod_LoadMem(void *_p, const unsigned char *buf, unsigned int bufLength)
{
  player *p = (player *)_p;
  BUF *fModule = bufopen(buf, bufLength);
  modnote_t *note = NULL;

  char MK[5];

  int i, j, k;
  unsigned int total_sample_size = 0, sample_offset = 0, might_be_an_STK_tune = 0;

  p->source = (MODULE *)calloc(1, sizeof (MODULE));

  bufread(MK, 1, 3, fModule);
  if (!strncmp(MK, "MTM", 3))
  {
    i = playptmod_LoadMTM(p, fModule);
    bufclose(fModule);
    return i;
  }

  bufseek(fModule, 1080, SEEK_SET);
  bufread(MK, 1, 4, fModule);

  check_mod_tag(&p->source->head, MK);
  if (p->source->head.format == FORMAT_UNKNOWN)
    might_be_an_STK_tune = 1;

  bufseek(fModule, 20, SEEK_SET);

  for (i = 0; i < MOD_SAMPLES; i++)
  {
    if (might_be_an_STK_tune && (i > 14))
    {
      p->source->samples[i].loop_length = 2;
    }
    else
    {
      bufseek(fModule, 22, SEEK_CUR);

      p->source->samples[i].length = file_get_word_bigendian(fModule) << 1;

      bufread(&p->source->samples[i].finetune, 1, 1, fModule);
      p->source->samples[i].finetune = LO_NYBBLE(p->source->samples[i].finetune);

      bufread(&p->source->samples[i].volume, 1, 1, fModule);
      if (p->source->samples[i].volume > 64)
        p->source->samples[i].volume = 64;

      p->source->samples[i].loop_start = file_get_word_bigendian(fModule) << 1;
      p->source->samples[i].loop_length = file_get_word_bigendian(fModule) << 1;

      if (p->source->samples[i].loop_length < 2)
             p->source->samples[i].loop_length = 2;

      total_sample_size += p->source->samples[i].length;

      p->source->samples[i].attribute = 0;
    }
  }

  bufread(&p->source->head.order_count, 1, 1, fModule);
  if ((p->source->head.order_count == 0) || (p->source->head.order_count > 128))
  {
    bufclose(fModule);
    return 0;
  }

  bufread(&p->source->head.restart_pos, 1, 1, fModule);
  if (might_be_an_STK_tune && (p->source->head.restart_pos != 120))
  {
    bufclose(fModule);
    return 0;
  }

  if (p->source->head.restart_pos >= p->source->head.order_count)
    p->source->head.restart_pos = 0;

  if (might_be_an_STK_tune)
    p->source->head.format = FORMAT_STK;

  p->source->head.pattern_count = ((bufLength - total_sample_size) -
    (((p->source->head.format == FORMAT_STK) ? 600 : 1084) - 1)) / (256 * p->source->head.channel_count);

  for (i = 0; i < 128; i++)
  {
    bufread(&p->source->head.order[i], 1, 1, fModule);

    if (p->source->head.order[i] >= p->source->head.pattern_count)
      p->source->head.order[i] = 0;
  }

  if (p->source->head.format != FORMAT_STK)
    bufseek(fModule, 4, SEEK_CUR);

  for (i = 0; i < p->source->head.pattern_count; i++)
  {
    note = p->source->patterns[i] = (modnote_t *)malloc(sizeof (modnote_t) * MOD_ROWS * p->source->head.channel_count);
    if (p->source->patterns[i] == NULL)
    {
      bufclose(fModule);
      for (j = 0; j < i; j++)
      {
        if (p->source->patterns[j] != NULL)
        {
          free(p->source->patterns[j]);
          p->source->patterns[j] = NULL;
        }
      }
      return 0;
    }

    if (p->source->head.format == FORMAT_FLT8)
    {
      for (j = 0; j < MOD_ROWS; j++)
      {
        for (k = 0; k < 8; k++)
        {
          unsigned char bytes[4];

          if (k == 0 && j > 0) bufseek(bytes, -1024, SEEK_CUR);
          else if (k == 4) bufseek(bytes, 1024 - 4 * 4, SEEK_CUR);

          bufread(bytes, 1, 4, fModule);

          note->period = (LO_NYBBLE(bytes[0]) << 8) | bytes[1];
          note->sample = (bytes[0] & 0xF0) | HI_NYBBLE(bytes[2]);
          note->command = LO_NYBBLE(bytes[2]);
          note->param = bytes[3];

          if ((note->command == 0x0F) && (note->param == 0x00))
          {
            note->command = 0;
            note->param = 0;
          }

          note++;
        }
      }
    }
    else
    {
      for (j = 0; j < MOD_ROWS; j++)
      {
        for (k = 0; k < p->source->head.channel_count; k++)
        {
          unsigned char bytes[4];
          bufread(bytes, 1, 4, fModule);

          note->period = (LO_NYBBLE(bytes[0]) << 8) | bytes[1];
          note->sample = (bytes[0] & 0xF0) | HI_NYBBLE(bytes[2]);
          note->command = LO_NYBBLE(bytes[2]);
          note->param = bytes[3];

          if ((note->command == 0x0F) && (note->param == 0x00))
          {
            note->command = 0;
            note->param = 0;
          }

          if (p->source->head.format == FORMAT_NCHN || p->source->head.format == FORMAT_NNCH)
          {
            if (note->command == 0x08)
            {
              note->command = 0;
              note->param = 0;
            }
            else if ((note->command == 0x0E) && (HI_NYBBLE(note->param) == 0x08))
            {
              note->command = 0;
              note->param = 0;
            }
          }

          note++;
        }
      }
    }
  }

  p->source->sample_data = (signed char *)malloc(total_sample_size);
  if (p->source->sample_data == NULL)
  {
    for (i = 0; i < 128; i++)
    {
      if (p->source->patterns[i] != NULL)
      {
        free(p->source->patterns[i]);
        p->source->patterns[i] = NULL;
      }
    }

    bufclose(fModule);

    return 0;
  }

  for (i = 0; i < (p->source->head.format == FORMAT_STK ? 15 : 31); i++)
  {
    p->source->samples[i].offset = sample_offset;
    bufread(&p->source->sample_data[sample_offset], 1, p->source->samples[i].length, fModule);
    sample_offset += p->source->samples[i].length;
  }

  p->source->original_sample_data = (signed char *)malloc(total_sample_size);
  if (p->source->original_sample_data == NULL)
  {
    free(p->source->sample_data);
    p->source->sample_data = NULL;

    for (i = 0; i < 128; i++)
    {
      if (p->source->patterns[i] != NULL)
      {
        free(p->source->patterns[i]);
        p->source->patterns[i] = NULL;
      }
    }

    bufclose(fModule);

    return 0;
  }

  memcpy(p->source->original_sample_data, p->source->sample_data, total_sample_size);
  p->source->total_sample_size = total_sample_size;

  bufclose(fModule);

  p->source->head.row_count = MOD_ROWS;
  memset(p->source->head.vol, 64, PAULA_CHANNELS);
  for (i = 0; i < PAULA_CHANNELS; i++) p->source->head.pan[i] = ((i + 1) & 2) ? 192 : 64;

  p->use_led_filter = 0;
  p->moduleLoaded = 1;

  return 1;
}

int playptmod_Load(void *_p, const char *filename)
{
  player *p = (player *)_p;
  if (!p->moduleLoaded)
  {
    int ret;
    unsigned int fileSize;
    unsigned char *buffer = NULL;
    FILE *fileModule = NULL;

    fileModule = fopen(filename, "rb");
    if (fileModule == NULL)
      return 0;

    fseek(fileModule, 0, SEEK_END);
    fileSize = ftell(fileModule);
    fseek(fileModule, 0, SEEK_SET);

    buffer = (unsigned char *)malloc(fileSize);
    fread(buffer, 1, fileSize, fileModule);
    fclose(fileModule);

    ret = playptmod_LoadMem(_p, buffer, fileSize);

    free(buffer);

    return ret;
  }

  return 0;
}

static void effect_arpeggio(player *p, mod_channel *ch);
static void effect_portamento_up(player *p, mod_channel *ch);
static void effect_portamento_down(player *p, mod_channel *ch);
static void effect_glissando(player *p, mod_channel *ch);
static void effect_vibrato(player *p, mod_channel *ch);
static void effect_glissando_vslide(player *p, mod_channel *ch);
static void effect_vibrato_vslide(player *p, mod_channel *ch);
static void effect_tremolo(player *p, mod_channel *ch);
static void effect_pan(player *p, mod_channel *ch);
static void effect_sample_offset(player *p, mod_channel *ch);
static void effect_volume_slide(player *p, mod_channel *ch);
static void effect_position_jump(player *p, mod_channel *ch);
static void effect_set_volume(player *p, mod_channel *ch);
static void effect_pattern_break(player *p, mod_channel *ch);
static void effect_extended(player *p, mod_channel *ch);
static void effect_tempo(player *p, mod_channel *ch);
static void effecte_setfilter(player *p, mod_channel *ch);
static void effecte_fineportaup(player *p, mod_channel *ch);
static void effecte_fineportadown(player *p, mod_channel *ch);
static void effecte_glissandoctrl(player *p, mod_channel *ch);
static void effecte_vibratoctrl(player *p, mod_channel *ch);
static void effecte_setfinetune(player *p, mod_channel *ch);
static void effecte_patternloop(player *p, mod_channel *ch);
static void effecte_tremoloctrl(player *p, mod_channel *ch);
static void effecte_karplus_strong(player *p, mod_channel *ch);
static void effecte_retrignote(player *p, mod_channel *ch);
static void effecte_finevolup(player *p, mod_channel *ch);
static void effecte_finevoldown(player *p, mod_channel *ch);
static void effecte_notecut(player *p, mod_channel *ch);
static void effecte_notedelay(player *p, mod_channel *ch);
static void effecte_patterndelay(player *p, mod_channel *ch);
static void effecte_invertloop(player *p, mod_channel *ch);

typedef void (*effect_routine)(player *, mod_channel *);

static effect_routine effect_routines[] =
{
  effect_arpeggio,
  effect_portamento_up,
  effect_portamento_down,
  effect_glissando,
  effect_vibrato,
  effect_glissando_vslide,
  effect_vibrato_vslide,
  effect_tremolo,
  effect_pan,
  effect_sample_offset,
  effect_volume_slide,
  effect_position_jump,
  effect_set_volume,
  effect_pattern_break,
  effect_extended,
  effect_tempo
};

static effect_routine effecte_routines[] =
{
  effecte_setfilter,
  effecte_fineportaup,
  effecte_fineportadown,
  effecte_glissandoctrl,
  effecte_vibratoctrl,
  effecte_setfinetune,
  effecte_patternloop,
  effecte_tremoloctrl,
  effecte_karplus_strong,
  effecte_retrignote,
  effecte_finevolup,
  effecte_finevoldown,
  effecte_notecut,
  effecte_notedelay,
  effecte_patterndelay,
  effecte_invertloop
};

static void UpdateInvertLoop(player *p, mod_channel *ch)
{
  if (ch->invloop_speed)
  {
    ch->invloop_delay += pt_tab_invloop[ch->invloop_speed];
    if (ch->invloop_delay >= 128)
    {
      ch->invloop_delay = 0;

      if (ch->sample)
      {
        MODULE_SAMPLE *s = &p->source->samples[ch->sample - 1];

        if (s->loop_length >= 4)
        {
          ch->invloop_offset++;
          if (ch->invloop_offset >= (unsigned)(s->loop_start + s->loop_length))
            ch->invloop_offset = s->loop_start;

          p->source->sample_data[s->offset + ch->invloop_offset] ^= 0xFF;
        }
      }
    }
  }
}

static void effecte_setfilter(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
    p->use_led_filter = !(ch->param & 1);
}

static void effecte_fineportaup(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    if (p->aperiod)
    {
      ch->period -= LO_NYBBLE(ch->param);

      if (ch->period < p->minPeriod)
        ch->period = p->minPeriod;

      p->aperiod = ch->period;
    }
  }
}

static void effecte_fineportadown(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    if (p->aperiod)
    {
      ch->period += LO_NYBBLE(ch->param);

      if (ch->period > p->maxPeriod)
        ch->period = p->maxPeriod;

      p->aperiod = ch->period;
    }
  }
}

static void effecte_glissandoctrl(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
    ch->glissandoctrl = LO_NYBBLE(ch->param);
}

static void effecte_vibratoctrl(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
    ch->vibratoctrl = LO_NYBBLE(ch->param);
}

static void effecte_setfinetune(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
    ch->finetune = LO_NYBBLE(ch->param);
}

static void effecte_patternloop(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    unsigned char param = LO_NYBBLE(ch->param);
    if (!param)
    {
      ch->pattern_loop_row = p->mod_row;

      return;
    }

    if (ch->pattern_loop_times == 0)
    {
      ch->pattern_loop_times = param;
    }
    else
    {
      ch->pattern_loop_times--;
      if (ch->pattern_loop_times == 0)
        return;
    }

    p->PBreakPosition = ch->pattern_loop_row;
    p->PBreakFlag = 1;
  }
}

static void effecte_tremoloctrl(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
    ch->tremoloctrl = LO_NYBBLE(ch->param);
}

static void effecte_karplus_strong(player *p, mod_channel *ch)
{
  if (ch->sample)
  {
    MODULE_SAMPLE *s = &p->source->samples[ch->sample - 1];

    signed char *sample_loop_data = p->source->sample_data + (s->offset + s->loop_start);

    unsigned int loop_length = s->loop_length - 2;
    unsigned int loop_length_counter = loop_length;

    while (loop_length_counter--)
    {
      *sample_loop_data = (*sample_loop_data + *(sample_loop_data + 1)) >> 1;
      sample_loop_data++;
    }

    *sample_loop_data = (*sample_loop_data + *(sample_loop_data - loop_length)) >> 1;
  }
}

static void effecte_retrignote(player *p, mod_channel *ch)
{
  unsigned char retrig_on_tick = LO_NYBBLE(ch->param);

  if (retrig_on_tick)
  {
    if (!(p->mod_tick % retrig_on_tick))
      p->aflags |= AFLAG_START;
  }
}

static void effecte_finevolup(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    ch->volume += LO_NYBBLE(ch->param);

    if (ch->volume > 64)
      ch->volume = 64;

    p->avolume = ch->volume;
  }
}

static void effecte_finevoldown(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    ch->volume -= LO_NYBBLE(ch->param);

    if (ch->volume < 0)
      ch->volume = 0;

    p->avolume = ch->volume;
  }
}

static void effecte_notecut(player *p, mod_channel *ch)
{
  if (p->mod_tick == LO_NYBBLE(ch->param))
    ch->volume = p->avolume = 0;
}

static void effecte_notedelay(player *p, mod_channel *ch)
{
  unsigned char delay_tick = LO_NYBBLE(ch->param);

  if (!p->mod_tick)
    ch->tmp_aflags = p->aflags;

  if (p->mod_tick < delay_tick)
    p->aflags = AFLAG_DELAY;
  else if (p->mod_tick == delay_tick)
    p->aflags = ch->tmp_aflags;
}

static void effecte_patterndelay(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    if (!p->PattDelayTime2)
      p->PattDelayTime = LO_NYBBLE(ch->param) + 1;
  }
}

static void effecte_invertloop(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    ch->invloop_speed = LO_NYBBLE(ch->param);
    UpdateInvertLoop(p, ch);
  }
}

static void do_glissando(player *p, mod_channel *ch)
{
  if (p->aperiod)
  {
    if (ch->period < ch->tperiod)
    {
      ch->period += ch->glissandospeed;

      if (ch->period > ch->tperiod)
      ch->period = ch->tperiod;
    }
    else
    {
      ch->period -= ch->glissandospeed;

      if (ch->period < ch->tperiod)
      ch->period = ch->tperiod;
    }

    if (ch->glissandoctrl)
    {
      int i;
      short *tablePointer;
      if (p->minPeriod == PT_MIN_PERIOD)
      {
        tablePointer = (short *)&rawPeriodTable[ch->finetune * 37];
        for (i = 0; i < 36; i++)
        {
          if (tablePointer[i] <= ch->period)
          {
            p->aperiod = tablePointer[i];
            return;
          }
        }
      }
      else
      {
        tablePointer = (short *)&extendedRawPeriodTable[ch->finetune * 85];
        for (i = 0; i < 84; i++)
        {
          if (tablePointer[i] <= ch->period)
          {
            p->aperiod = tablePointer[i];
            return;
          }
        }
      }
    }
    else
    {
      p->aperiod = ch->period;
    }
  }
}

static void do_vibrato(player *p, mod_channel *ch)
{
  if (p->aperiod)
  {
    int vib_data, vib_pos = (ch->vibratopos >> 2) & 0x1F;

    switch (ch->vibratoctrl & 3)
    {
      case 0: vib_data = p->pt_tab_vibsine[vib_pos]; break;

      case 1:
      vib_data = (ch->vibratopos < 128) ? (vib_pos << 3) : (255 - (vib_pos << 3));
      break;

      default: vib_data = 255; break;
    }

    vib_data = (vib_data * ch->vibratodepth) >> 7;
    if (ch->vibratopos < 128)
      p->aperiod += (short)vib_data;
    else
      p->aperiod -= (short)vib_data;

    ch->vibratopos = (ch->vibratopos + ch->vibratospeed) & 0xFF;
  }
}

static void do_tremolo(player *p, mod_channel *ch)
{
  if (p->avolume)
  {
    int trem_data, trem_pos = (ch->tremolopos >> 2) & 0x1F;

    switch (ch->tremoloctrl & 3)
    {
      case 0: trem_data = p->pt_tab_vibsine[trem_pos]; break;

      case 1: // PT src typo (vibratopos)
      trem_data = (ch->vibratopos < 128) ? (trem_pos << 3) : (255 - (trem_pos << 3));
      break;

      default: trem_data = 255; break;
    }

    trem_data = (trem_data * ch->tremolodepth) >> 6;
    if (ch->tremolopos < 128)
    {
      p->avolume += (char)trem_data;
      if (p->avolume > 64)
        p->avolume = 64;
    }
    else
    {
      p->avolume -= (char)trem_data;
      if (p->avolume < 0)
        p->avolume = 0;
    }

    ch->tremolopos = (ch->tremolopos + ch->tremolospeed) & 0xFF;
  }
}

static void effect_arpeggio(player *p, mod_channel *ch)
{
  int i, noteToAdd, arpeggioTick = p->mod_tick % 3;
  short *tablePointer;

  if (arpeggioTick == 0)
  {
    p->aperiod = ch->period;
    return;
  }
  else if (arpeggioTick == 1)
  {
    noteToAdd = HI_NYBBLE(ch->param);
  }
  else if (arpeggioTick == 2)
  {
    noteToAdd = LO_NYBBLE(ch->param);
  }

  if (p->minPeriod == PT_MIN_PERIOD)
  {
    tablePointer = (short *)&rawPeriodTable[ch->finetune * 37];
    for (i = 0; i < 36; i++)
    {
      if (tablePointer[i] <= ch->period)
      {
        p->aperiod = tablePointer[i + noteToAdd];
        return;
      }
    }
  }
  else
  {
    tablePointer = (short *)&extendedRawPeriodTable[ch->finetune * 85];
    for (i =  0; i < 84; i++)
    {
      if (tablePointer[i] <= ch->period)
      {
        p->aperiod = tablePointer[i + noteToAdd];
        return;
      }
    }
  }
}

static void effect_portamento_up(player *p, mod_channel *ch)
{
  if (p->mod_tick && p->aperiod)
  {
    ch->period -= ch->param;

    if (ch->period < p->minPeriod)
      ch->period = p->minPeriod;

    p->aperiod = ch->period;
  }
}

static void effect_portamento_down(player *p, mod_channel *ch)
{
  if (p->mod_tick && p->aperiod)
  {
    ch->period += ch->param;

    if (ch->period > p->maxPeriod)
      ch->period = p->maxPeriod;

    p->aperiod = ch->period;
  }
}

static void effect_glissando(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    if (ch->param)
      ch->glissandospeed = ch->param;
  }
  else
  {
    do_glissando(p, ch);
  }
}

static void effect_vibrato(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    unsigned char hi_nybble = HI_NYBBLE(ch->param);
    unsigned char lo_nybble = LO_NYBBLE(ch->param);

    if (hi_nybble)
      ch->vibratospeed = hi_nybble << 2;

    if (lo_nybble)
      ch->vibratodepth = lo_nybble;
  }
  else
  {
    do_vibrato(p, ch);
  }
}

static void effect_glissando_vslide(player *p, mod_channel *ch)
{
  if (p->mod_tick)
  {
    do_glissando(p, ch);
    effect_volume_slide(p, ch);
  }
}

static void effect_vibrato_vslide(player *p, mod_channel *ch)
{
  if (p->mod_tick)
  {
    do_vibrato(p, ch);
    effect_volume_slide(p, ch);
  }
}

static void effect_tremolo(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    unsigned char hi_nybble = HI_NYBBLE(ch->param);
    unsigned char lo_nybble = LO_NYBBLE(ch->param);

    if (hi_nybble)
      ch->tremolospeed = hi_nybble << 2;

    if (lo_nybble)
      ch->tremolodepth = lo_nybble;
  }
  else
  {
    do_tremolo(p, ch);
  }
}

static void effect_pan(player *p, mod_channel *ch)
{
  if (p->source->head.format == FORMAT_NCHN || p->source->head.format == FORMAT_NNCH || p->source->head.format == FORMAT_MTM)
    mixer_set_ch_pan(p, ch->seqchannel, ch->param);
}

static void effect_sample_offset(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    if (ch->param)
      ch->sample_offset_temp = ch->param << 8;

    ch->offset += ch->sample_offset_temp;
  }
}

static void effect_volume_slide(player *p, mod_channel *ch)
{
  if (p->mod_tick)
  {
    unsigned char hi_nybble = HI_NYBBLE(ch->param);
    unsigned char lo_nybble = LO_NYBBLE(ch->param);

    if (!hi_nybble)
    {
      ch->volume -= lo_nybble;
      if (ch->volume < 0)
        ch->volume = 0;

      p->avolume = ch->volume;
    }
    else
    {
      ch->volume += hi_nybble;
      if (ch->volume > 64)
        ch->volume = 64;

      p->avolume = ch->volume;
    }
  }
}

static void effect_position_jump(player *p, mod_channel *ch)
{
  p->mod_order = ch->param - 1;
  p->PBreakPosition = 0;
  p->PosJumpAssert = 1;
}

static void effect_set_volume(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    if (ch->param > 64)
      ch->param = 64;

    p->avolume = ch->volume = ch->param;
  }
}

static void effect_pattern_break(player *p, mod_channel *ch)
{
  unsigned char pos = ((HI_NYBBLE(ch->param) * 10) + LO_NYBBLE(ch->param));

  if (pos > 63)
    pos = 0;

  p->PBreakPosition = pos;
  p->PosJumpAssert = 1;
}

static void effect_extended(player *p, mod_channel *ch)
{
  effecte_routines[HI_NYBBLE(ch->param)](p, ch);
}

static void pt_mod_speed(player *p, int speed)
{
  p->mod_speed = speed;
  p->mod_tick = 0;
}

static void pt_mod_tempo(player *p, int bpm)
{
  p->mod_bpm = bpm;
  p->mod_samplespertick = p->tempoTimerVal / bpm;
}

void playptmod_Stop(void *_p)
{
  player *p = (player *)_p;

  int i;

  mixer_cut_channels(p);

  p->modulePlaying = 0;

  for (i = 0; i < p->source->head.channel_count; i++)
  {
    p->source->channels[i].pattern_loop_times = 0;
    p->source->channels[i].glissandoctrl = 0;
    p->source->channels[i].vibratoctrl = 0;
    p->source->channels[i].tremoloctrl = 0;
    p->source->channels[i].finetune = 0;
    p->source->channels[i].invloop_speed = 0;
  }

  p->aflags = 0;

  p->PattDelayTime = 0;
  p->PattDelayTime2 = 0;

  p->PBreakPosition = 0;
  p->PosJumpAssert = 0;
}

static void effect_tempo(player *p, mod_channel *ch)
{
  if (!p->mod_tick)
  {
    if ((ch->param > 0) && (p->vsync_timing || (ch->param < 32)))
      pt_mod_speed(p, ch->param);
    else if (ch->param)
      pt_mod_tempo(p, ch->param);
  }
}

static void update_effect(player *p, mod_channel *ch)
{
  if (p->mod_tick)
    UpdateInvertLoop(p, ch);

  if (!(!ch->command && !ch->param))
    effect_routines[ch->command](p, ch);
}

static void read_note(player *p, mod_channel *ch)
{
  modnote_t *note = &p->source->patterns[p->mod_pattern][(p->mod_row * p->source->head.channel_count) + ch->seqchannel];

  if (note->sample)
  {
    if (ch->sample != note->sample)
      ch->flags |= FLAG_NEWSAMPLE;

    ch->sample = note->sample;
    ch->flags |= FLAG_SAMPLE;
    ch->finetune = p->source->samples[ch->sample - 1].finetune;
  }

  ch->command = note->command;
  ch->param = note->param;

  if (note->period)
  {
    int tempNote;

    if (ch->command == 0xE)
    {
      if (HI_NYBBLE(ch->param) == 0x5)
        ch->finetune = LO_NYBBLE(ch->param);
    }

    tempNote = period2note(p, 0, CLAMP(note->period, p->minPeriod, p->maxPeriod));

    ch->no_note = 0;

    ch->tperiod = p->minPeriod == PT_MIN_PERIOD ? rawPeriodTable[(37 * ch->finetune) + tempNote] : extendedRawPeriodTable[(85 * ch->finetune) + tempNote];
    ch->flags |= FLAG_NOTE;
  }
  else
  {
    ch->no_note = 1;
  }
}

static void update_channel(player *p, mod_channel *ch)
{
  p->aflags = 0;

  if (!p->mod_tick)
  {
    if (!p->PattDelayTime2)
      read_note(p, ch);

    if (ch->flags & FLAG_NOTE)
    {
      ch->flags &= ~FLAG_NOTE;

      if ((ch->command != 0x03) && (ch->command != 0x05))
      {
        ch->period = ch->tperiod;

        if (ch->sample)
          p->aflags |= AFLAG_START;
      }

      ch->tmp_aflags = 0;

      if (!(ch->vibratoctrl & 4))
        ch->vibratopos = 0;
      if (!(ch->tremoloctrl & 4))
        ch->tremolopos = 0;
    }

    if (ch->flags & FLAG_SAMPLE)
    {
      ch->flags &= ~FLAG_SAMPLE;

      if (ch->sample)
      {
        MODULE_SAMPLE *s = &p->source->samples[ch->sample - 1];

        ch->volume = s->volume;
        ch->invloop_offset = s->loop_start;

        if ((ch->command != 0x03) && (ch->command != 0x05))
        {
          ch->offset = 0;
          ch->bug_offset_not_added = 0;
        }

        if (ch->flags & FLAG_NEWSAMPLE)
        {
          ch->flags &= ~FLAG_NEWSAMPLE;

          if (ch->period && (ch->no_note || ch->command == 0x03 || ch->command == 0x05))
            p->aflags |= AFLAG_NEW_SAMPLE;
        }
      }
    }
  }

  p->aperiod = ch->period;
  p->avolume = ch->volume;

  update_effect(p, ch);

  if (!(p->aflags & AFLAG_DELAY))
  {
    if (p->aflags & AFLAG_NEW_SAMPLE)
    {
      if (ch->sample)
      {
        MODULE_SAMPLE *s = &p->source->samples[ch->sample - 1];
        if (s->length > 2)
          mixer_change_ch_src(p, ch->seqchannel, &p->source->sample_data[s->offset], s->length, s->loop_start, s->loop_length, s->attribute & 1 ? 2 : 1);
        else
          mixer_set_ch_src(p, ch->seqchannel, NULL, 0, 0, 0, 0, 0);
      }
    }
    else if (p->aflags & AFLAG_START)
    {
      if (ch->sample)
      {
        MODULE_SAMPLE *s = &p->source->samples[ch->sample - 1];

        if (s->length > 2)
        {
          if (ch->offset)
          {
            mixer_set_ch_src(p, ch->seqchannel, p->source->sample_data + s->offset, s->length, s->loop_start, s->loop_length, ch->offset, s->attribute & 1 ? 2 : 1);

            if (!ch->bug_offset_not_added)
            {
              ch->offset += ch->sample_offset_temp;
              ch->bug_offset_not_added = 1;
            }
          }
          else
          {
            mixer_set_ch_src(p, ch->seqchannel, p->source->sample_data + s->offset, s->length, s->loop_start, s->loop_length, 0, s->attribute & 1 ? 2 : 1);
          }
        }
        else
        {
          mixer_set_ch_src(p, ch->seqchannel, NULL, 0, 0, 0, 0, 0);
        }
      }
    }

    mixer_set_ch_vol(p, ch->seqchannel, p->avolume);

    if (p->aperiod > 0)
      mixer_set_ch_freq(p, ch->seqchannel, p->minPeriod == PT_MIN_PERIOD ? p->pt_period_freq_tab[(int)p->aperiod] : p->pt_extended_period_freq_tab[(int)p->aperiod]);
    else
      mixer_set_ch_vol(p, ch->seqchannel, 0);
  }
}

static void next_position(player *p)
{
  int pos = p->mod_order + 1;
  if (pos >= p->source->head.order_count)
    pos = 0;

  p->mod_row = p->PBreakPosition;
  p->mod_order = pos;
  p->mod_pattern = CLAMP(p->source->head.order[p->mod_order], 0, 127);

  p->PBreakPosition = 0;
  p->PosJumpAssert = 0;

  if (p->mod_order == p->mod_start_order && p->mod_row == 0)
      p->loop_counter++;
}

static void process_tick(player *p)
{
  int i;
  for (i = 0; i < p->source->head.channel_count; i++)
    update_channel(p, p->source->channels + i);

  if (p->modulePlaying)
  {
    if (++p->mod_tick >= p->mod_speed)
    {
      p->mod_tick = 0;
      p->mod_row++;

      if (p->PattDelayTime)
      {
        p->PattDelayTime2 = p->PattDelayTime;
        p->PattDelayTime = 0;
      }

      if (p->PattDelayTime2)
      {
        p->PattDelayTime2--;
        if (p->PattDelayTime2 != 0)
          p->mod_row--;
      }

      if (p->PBreakFlag)
      {
        p->PBreakFlag = 0;
        p->mod_row = p->PBreakPosition;
        p->PBreakPosition = 0;
      }

      if ((p->mod_row >= p->source->head.row_count) || p->PosJumpAssert)
        next_position(p);
    }
  }
}

static int pulsate_samples(player *p, int samples)
{
  if (p->mod_samplecounter == 0)
  {
    process_tick(p);
    p->mod_samplecounter += p->mod_samplespertick;
  }

  p->mod_samplecounter -= samples;
  if (p->mod_samplecounter < 0)
  {
    int ret_samples = samples + p->mod_samplecounter;
    p->mod_samplecounter = 0;

    return ret_samples;
  }

  return samples;
}

void playptmod_Render(void *_p, signed short *target, int length)
{
  player *p = (player *)_p;

  if (p->modulePlaying)
  {
    static const int soundBufferSamples = soundBufferSize / 4;

    while (length)
    {
      int igen, gen = pulsate_samples(p, length);
      length -= gen;

      while (gen)
      {
        if (gen >= p->vsync_samples_left)
        {
          igen = (int)floorf(p->vsync_samples_left);
          gen -= igen;
          p->vsync_samples_left -= igen;
        }
        else
        {
          igen = gen;
          p->vsync_samples_left -= gen;
          gen = 0;
        }

        p->vsync_samples_left = 0.0f;

        while (igen)
        {
          int pgen = CLAMP(igen, 0, soundBufferSamples);
          mixer_output_audio(p, target, pgen);
          target += (pgen * 2);
          igen -= pgen;
        }

        if (p->vsync_samples_left <= 1.0f)
          p->vsync_samples_left += p->vsync_block_length;
      }
    }
  }
}

void * playptmod_Create(int samplingFrequency)
{
  int i;

  player *p = calloc(1, sizeof(player));

  maketables(p, samplingFrequency);
  p->vsync_block_length = (float)samplingFrequency / 50.0f;

  p->soundFrequency = samplingFrequency;

  p->mixer_buffer_l = (float *)malloc(soundBufferSize * sizeof (float));
  p->mixer_buffer_r = (float *)malloc(soundBufferSize * sizeof (float));

  p->filter_c.led = calculate_rc_coefficient((float)samplingFrequency, 3000.0f); 
  p->filter_c.led_fb = 0.125f + 0.125f / (1.0f - p->filter_c.led);
  p->filter_c.high = calculate_rc_coefficient((float)samplingFrequency, 30.0f);

  p->use_led_filter = 0;

  p->minPeriod = PT_MIN_PERIOD;
  p->maxPeriod = PT_MAX_PERIOD;

  p->vsync_timing = 0;

  p->mixerBuffer = (signed char *)calloc(soundBufferSize, 1);

  mixer_cut_channels(p);

  return (void *) p;
}

void playptmod_Config(void *_p, int option, int value)
{
    player *p = (player *)_p;
    switch (option)
    {
    case PTMOD_OPTION_CLAMP_PERIODS:
        if (value)
        {
            p->minPeriod = PT_MIN_PERIOD;
            p->maxPeriod = PT_MAX_PERIOD;
        }
        else
        {
            p->minPeriod = p->calculatedMinPeriod;
            p->maxPeriod = p->calculatedMaxPeriod;
        }
        break;

	case PTMOD_OPTION_VSYNC_TIMING:
		p->vsync_timing = value;
		break;
    }
}

void playptmod_Play(void *_p, unsigned int start_order)
{
  player *p = (player *)_p;
  if (!p->modulePlaying && p->moduleLoaded)
  {
    int i;

    mixer_cut_channels(p);

    for (i = 0; i < p->source->head.channel_count; i++)
    {
      p->source->channels[i].volume = p->source->head.vol[i];
      p->source->channels[i].seqchannel = (char)i;
      p->source->channels[i].pattern_loop_row = 0;
      p->source->channels[i].pattern_loop_times = 0;
      p->source->channels[i].glissandoctrl = 0;
      p->source->channels[i].vibratoctrl = 0;
      p->source->channels[i].vibratopos = 0;
      p->source->channels[i].tremoloctrl = 0;
      p->source->channels[i].tremolopos = 0;
      p->source->channels[i].finetune = 0;
    }

    pt_mod_tempo(p, 125);
    pt_mod_speed(p, 6);

    p->mod_order = p->mod_start_order = start_order;
    p->loop_counter = 0;
    p->mod_pattern = p->source->head.order[0];
    p->mod_row = 0;
    p->mod_tick = 0;

    p->aflags = 0;
    p->PattDelayTime = 0;
    p->PattDelayTime2 = 0;
    p->PBreakPosition = 0;
    p->PosJumpAssert = 0;
    p->modulePlaying = 1;

    memcpy(p->source->sample_data, p->source->original_sample_data, p->source->total_sample_size);
  }
}

void playptmod_Free(void *_p)
{
  player *p = (player *)_p;
  if (p->moduleLoaded)
  {
    int i;

    p->modulePlaying = 0;

    for (i = 0; i < 128; i++)
    {
      if (p->source->patterns[i] != NULL)
        free(p->source->patterns[i]);
    }

    if (p->source->sample_data != NULL)
      free(p->source->sample_data);

    if (p->source->original_sample_data != NULL)
      free(p->source->original_sample_data);

    if (p->source != NULL)
      free(p->source);

    p->moduleLoaded = 0;
  }

  free(p->mixerBuffer);
  free(p->mixer_buffer_l);
  free(p->mixer_buffer_r);

  freetables(p);

  free(p);
}

unsigned int playptmod_LoopCounter(void *_p)
{
  player *p = (player *)_p;
  return p->loop_counter;
}

void playptmod_GetInfo(void *_p, playptmod_info *i)
{
  int n, c;
  player *p = (player *)_p;
  i->order = p->mod_order <= 128 ? p->mod_order : 0;
  i->pattern = p->mod_pattern;
  i->row = p->mod_row;
  i->speed = p->mod_speed;
  i->tempo = p->mod_bpm;
  for (c = 0, n = 0; n < p->source->head.channel_count; n++)
  {
    if (p->v[n].data) c++;
  }
  i->channels_playing = c;
}

/* END OF FILE */
