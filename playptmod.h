#ifndef _PLAYPTMOD_H_
#define _PLAYPTMOD_H_

#ifdef __cplusplus
extern "C" {
#endif

void * playptmod_Create(int samplingFrequency);

#define PTMOD_OPTION_CLAMP_PERIODS 0

void playptmod_Config(void *p, int option, int value);

int playptmod_LoadMem(void *p, const unsigned char *buf, unsigned int bufLength);
int playptmod_Load(void *p, const char *filename);

void playptmod_Play(void *p, unsigned int start_order);
void playptmod_Stop(void *p);
void playptmod_Render(void *p, signed short *target, int length);

unsigned int playptmod_LoopCounter(void *p);

typedef struct _ptmi
{
  unsigned char order;
  unsigned char pattern;
  unsigned char row;
  unsigned char channels_playing;
} playptmod_info;

void playptmod_GetInfo(void *p, playptmod_info *i);

void playptmod_Free(void *p);

#ifdef __cplusplus
}
#endif

#endif
