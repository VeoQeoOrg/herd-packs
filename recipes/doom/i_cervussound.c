#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include <sys/cervus.h>

#define CERVUS_RATE      22050
#define CERVUS_CHANNELS  8
#define CERVUS_CHUNK     630

typedef struct {
    int16_t *pcm;
    int      frames;
} cached_sfx_t;

typedef struct {
    const int16_t *pcm;
    int            frames;
    int            pos;
    int            left;
    int            right;
    sfxinfo_t     *sfx;
} mix_channel_t;

int   use_libsamplerate   = 0;
float libsamplerate_scale = 0.65f;

static boolean       g_open;
static mix_channel_t g_chan[CERVUS_CHANNELS];
static int16_t       g_out[CERVUS_CHUNK * 2];
static int32_t       g_acc[CERVUS_CHUNK * 2];

static cached_sfx_t *sfx_cache(sfxinfo_t *sfx)
{
    if (sfx->driver_data) return (cached_sfx_t *)sfx->driver_data;

    int lump = sfx->lumpnum;
    if (lump < 0) return NULL;

    const uint8_t *data = W_CacheLumpNum(lump, PU_STATIC);
    int size = W_LumpLength(lump);
    if (!data || size < 8 + 32) return NULL;

    unsigned format = data[0] | (data[1] << 8);
    unsigned rate   = data[2] | (data[3] << 8);
    unsigned count  = data[4] | (data[5] << 8) | (data[6] << 16) | ((unsigned)data[7] << 24);
    if (format != 3 || rate == 0) return NULL;
    if (count > (unsigned)(size - 8)) count = (unsigned)(size - 8);
    if (count < 32) return NULL;

    const uint8_t *src = data + 8 + 16;
    unsigned src_frames = count - 32;

    unsigned long long out_frames =
        ((unsigned long long)src_frames * CERVUS_RATE + rate - 1) / rate;
    if (out_frames == 0) return NULL;

    cached_sfx_t *c = Z_Malloc(sizeof *c, PU_STATIC, NULL);
    c->frames = (int)out_frames;
    c->pcm = Z_Malloc((size_t)c->frames * sizeof(int16_t), PU_STATIC, NULL);

    for (int i = 0; i < c->frames; i++) {
        unsigned long long si = (unsigned long long)i * rate / CERVUS_RATE;
        if (si >= src_frames) si = src_frames - 1;
        c->pcm[i] = (int16_t)(((int)src[si] - 128) << 7);
    }

    sfx->driver_data = c;
    return c;
}

static void set_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= CERVUS_CHANNELS) return;
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;

    g_chan[channel].left  = vol * (254 - sep) / 254;
    g_chan[channel].right = vol * sep / 254;
}

static boolean I_Cervus_InitSound(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;
    if (cervus_audio_open(CERVUS_RATE) != 0) {
        fputs("doom: no audio device, playing silently\n", stderr);
        return false;
    }
    memset(g_chan, 0, sizeof g_chan);
    g_open = true;
    return true;
}

static void I_Cervus_ShutdownSound(void)
{
    if (!g_open) return;
    cervus_audio_close();
    g_open = false;
}

static int I_Cervus_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9];
    M_snprintf(name, sizeof name, "ds%s", DEH_String(sfx->name));
    return W_GetNumForName(name);
}

static void I_Cervus_Update(void)
{
    if (!g_open) return;

    memset(g_acc, 0, sizeof g_acc);
    int active = 0;

    for (int c = 0; c < CERVUS_CHANNELS; c++) {
        mix_channel_t *ch = &g_chan[c];
        if (!ch->pcm) continue;
        active = 1;

        int n = ch->frames - ch->pos;
        if (n > CERVUS_CHUNK) n = CERVUS_CHUNK;

        for (int i = 0; i < n; i++) {
            int s = ch->pcm[ch->pos + i];
            g_acc[i * 2]     += s * ch->left  / 127;
            g_acc[i * 2 + 1] += s * ch->right / 127;
        }

        ch->pos += n;
        if (ch->pos >= ch->frames) { ch->pcm = NULL; ch->sfx = NULL; }
    }

    if (!active) return;

    for (int i = 0; i < CERVUS_CHUNK * 2; i++) {
        int32_t v = g_acc[i];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        g_out[i] = (int16_t)v;
    }

    cervus_audio_write(g_out, sizeof g_out);
}

static void I_Cervus_UpdateSoundParams(int channel, int vol, int sep)
{
    set_params(channel, vol, sep);
}

static int I_Cervus_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (!g_open || channel < 0 || channel >= CERVUS_CHANNELS) return -1;

    cached_sfx_t *c = sfx_cache(sfx);
    if (!c) return -1;

    g_chan[channel].pcm    = c->pcm;
    g_chan[channel].frames = c->frames;
    g_chan[channel].pos    = 0;
    g_chan[channel].sfx    = sfx;
    set_params(channel, vol, sep);
    return channel;
}

static void I_Cervus_StopSound(int channel)
{
    if (channel < 0 || channel >= CERVUS_CHANNELS) return;
    g_chan[channel].pcm = NULL;
    g_chan[channel].sfx = NULL;
}

static boolean I_Cervus_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= CERVUS_CHANNELS) return false;
    return g_chan[channel].pcm != NULL;
}

static void I_Cervus_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    for (int i = 0; i < num_sounds; i++) sfx_cache(&sounds[i]);
}

static snddevice_t sound_cervus_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    sound_cervus_devices,
    arrlen(sound_cervus_devices),
    I_Cervus_InitSound,
    I_Cervus_ShutdownSound,
    I_Cervus_GetSfxLumpNum,
    I_Cervus_Update,
    I_Cervus_UpdateSoundParams,
    I_Cervus_StartSound,
    I_Cervus_StopSound,
    I_Cervus_SoundIsPlaying,
    I_Cervus_PrecacheSounds,
};

static boolean I_Cervus_InitMusic(void)    { return false; }
static void    I_Cervus_ShutdownMusic(void) { }
static void    I_Cervus_SetMusicVolume(int v) { (void)v; }
static void    I_Cervus_PauseSong(void)     { }
static void    I_Cervus_ResumeSong(void)    { }
static void   *I_Cervus_RegisterSong(void *d, int l) { (void)d; (void)l; return NULL; }
static void    I_Cervus_UnRegisterSong(void *h) { (void)h; }
static void    I_Cervus_PlaySong(void *h, boolean loop) { (void)h; (void)loop; }
static void    I_Cervus_StopSong(void)      { }
static boolean I_Cervus_MusicIsPlaying(void) { return false; }
static void    I_Cervus_PollMusic(void)     { }

static snddevice_t music_cervus_devices[] = {
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module = {
    music_cervus_devices,
    arrlen(music_cervus_devices),
    I_Cervus_InitMusic,
    I_Cervus_ShutdownMusic,
    I_Cervus_SetMusicVolume,
    I_Cervus_PauseSong,
    I_Cervus_ResumeSong,
    I_Cervus_RegisterSong,
    I_Cervus_UnRegisterSong,
    I_Cervus_PlaySong,
    I_Cervus_StopSong,
    I_Cervus_MusicIsPlaying,
    I_Cervus_PollMusic,
};
