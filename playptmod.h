#ifndef _PLAYPTMOD_H_
#define _PLAYPTMOD_H_

#ifdef __cplusplus
extern "C" {
#endif

void * playptmod_Create(int samplingFrequency);

#define PTMOD_OPTION_CLAMP_PERIODS 0
/* 1 (default) = Amiga / Protracker range
 * 0           = MSDOS / MTM / extended range */

#define PTMOD_OPTION_VSYNC_TIMING  1
/* 0 (default) = Speed command 20 or higher sets tempo
 * 1           = Speed command always sets speed */

#define PTMOD_OPTION_PATTERN_COUNT 2
/* Set before calling Load/LoadMem
 * 0 (default) = Calculate pattern count from file size remaining after
 *               subtracting the sample data and the header
 * 1           = Calculate the pattern count from the highest pattern number
 *               in the order list */

void playptmod_Config(void *p, int option, int value);

int playptmod_LoadMem(void *p, const unsigned char *buf, unsigned int bufLength);
int playptmod_Load(void *p, const char *filename);

void playptmod_Play(void *p, unsigned int start_order);
void playptmod_Stop(void *p);
void playptmod_Render(void *p, signed short *target, int length);

void playptmod_Mute(void *p, int channel, int mute);

unsigned int playptmod_LoopCounter(void *p);

typedef struct _ptmi
{
  unsigned char order;
  unsigned char pattern;
  unsigned char row;
  unsigned char speed;
  unsigned char tempo;
  unsigned char channels_playing;
} playptmod_info;

void playptmod_GetInfo(void *p, playptmod_info *i);

void playptmod_Free(void *p);

#ifdef __cplusplus
}
#endif

#endif
