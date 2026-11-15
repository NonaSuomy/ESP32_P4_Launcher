/*
 * Quake Sound shim for PSRAM App — replaces snd_esp32.c.
 *
 * Keeps the same mixer architecture but routes audio output through
 * app_services_t::audio_submit() instead of rg_audio_submit().
 * The audio_task runs on a separate core via service table task_create.
 */
#include "psram_app.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "quakedef.h"

extern const app_services_t *_papp_svc;
extern volatile int papp_exit_requested;

/* C ABI supplied by pax_mp3.cpp. */
extern void *papp_mp3_create(void);
extern void papp_mp3_reset(void *decoder);
extern void papp_mp3_destroy(void *decoder);
extern int papp_mp3_decode(void *decoder, const uint8_t *input, int input_length,
                           int16_t *output, int output_capacity_samples,
                           int *consumed, int *samples, int *sample_rate,
                           int *channels);

/* Forward declarations from common.c */
byte *COM_LoadFile(char *path, int usehunk);

#define MAX_SFX 256

/* Generation counter to invalidate channels across level changes */
static int sound_generation = 0;

static sfx_t known_sfx[MAX_SFX];
channel_t channels[MAX_CHANNELS];

static int num_sfx = 0;
static sfx_t *ambient_sfx[NUM_AMBIENTS];
static bool snd_ambient = 1;

/* The game task updates channels while the audio task consumes them. */
static uint32_t sound_lock;

static void SoundLock(void)
{
    while (__atomic_exchange_n(&sound_lock, 1, __ATOMIC_ACQUIRE)) {
        /* The critical sections are short; don't yield and risk missing audio. */
    }
}

static void SoundUnlock(void)
{
    __atomic_store_n(&sound_lock, 0, __ATOMIC_RELEASE);
}

/* sound.h visible stuff */
cvar_t bgmvolume = {"bgmvolume", "1", true};
cvar_t volume = {"volume", "1.0", true};
cvar_t nosound = {"nosound", "0"};
cvar_t ambient_level = {"ambient_level", "0.3"};
cvar_t ambient_fade = {"ambient_fade", "100"};

int total_channels;

qboolean snd_initialized = false;

vec3_t listener_origin;
vec3_t listener_forward;
vec3_t listener_right;
vec3_t listener_up;
vec_t sound_nominal_clip_dist = 1000.0;

int paintedtime;
static int64_t hardware_sample_offset = 0;
static int64_t total_samples_submitted = 0;

#define AUDIO_BUFFER_SAMPLES 256

#define MUSIC_NO_REQUEST (-2)
#define MUSIC_STOP_REQUEST (-1)
#define MUSIC_INPUT_BYTES 4096
#define MUSIC_PCM_SAMPLES 2304 /* 1152 frames x 2 interleaved channels */
#define MUSIC_OUTPUT_RATE 22050

static int64_t mix_buffer[AUDIO_BUFFER_SAMPLES * 2];
static int16_t output_buffer[AUDIO_BUFFER_SAMPLES * 2]; /* stereo interleaved L,R */

/*
 * Quake's original CD music API is driven by the game task, while decoding
 * and mixing run in the audio task.  The game task only posts a small request;
 * all file and decoder operations happen in the audio task.
 */
static volatile int music_request_track = MUSIC_NO_REQUEST;
static volatile int music_request_loop = 1;
static volatile int music_request_pause = MUSIC_NO_REQUEST;

static void *music_file = NULL;
static void *music_decoder = NULL;
static uint8_t music_input[MUSIC_INPUT_BYTES];
static size_t music_input_pos = 0;
static size_t music_input_len = 0;
static int16_t music_pcm[MUSIC_PCM_SAMPLES];
static int music_pcm_frames = 0;
static int music_pcm_channels = 0;
static int music_sample_rate = MUSIC_OUTPUT_RATE;
static uint32_t music_step_fixed = 1u << 16;
static uint64_t music_pos_fixed = 0;
static int music_track = 0;
static int music_looping = 1;
static int music_paused = 0;
static int music_active = 0;
static int music_logged_format = 0;

/* Sound task handle — accessible for cleanup */
void *quake_sound_task_handle = NULL;
static volatile int quake_sound_task_done = 0;

/* Forward declarations */
static sfx_t *FindSfxName(char *name);

/* LoadSound: loads a WAV file into Hunk memory */
static bool LoadSound(sfx_t *s)
{
    char namebuffer[256];
    sprintf(namebuffer, "sound/%s", s->name);

    byte *data = COM_LoadFile(namebuffer, 1);

    if (!data)
        return false;

    wavinfo_t info = GetWavinfo(s->name, (byte *)data, com_filesize);

    if (info.channels != 1) {
        return false;
    }

    s->cache.data = data + info.dataofs;
    s->cache.sampleRate = info.rate;
    s->cache.sampleWidth = info.width;
    s->cache.loopStart = info.loopstart;
    s->cache.sampleCount = info.samples;

    int outputRate = 22050;
    s->cache.stepFixedPoint = (uint32_t)((float)info.rate * ESP32_SOUND_STEP / outputRate);
    s->cache.effectiveLength = (s->cache.sampleCount * (int64_t)ESP32_SOUND_STEP) / s->cache.stepFixedPoint;

    return true;
}

static sfx_t *FindSfxName(char *name)
{
    if (name == NULL) return NULL;
    if (strlen(name) >= MAX_QPATH) return NULL;

    for (int i = 0; i < num_sfx; ++i)
        if (!strcmp(known_sfx[i].name, name))
            return &known_sfx[i];

    if (num_sfx == MAX_SFX) return NULL;

    sfx_t *sfx = &known_sfx[num_sfx];
    strcpy(sfx->name, name);

    if (!LoadSound(sfx)) return NULL;

    num_sfx++;
    return sfx;
}

static void UpdateAmbientSounds(void)
{
    mleaf_t *l;
    float vol;

    if (!snd_initialized || !snd_ambient || !cl.worldmodel) return;

    l = Mod_PointInLeaf(listener_origin, cl.worldmodel);
    if (!l || !ambient_level.value) {
        for (int i = 0; i < NUM_AMBIENTS; i++)
            channels[i].sfx = NULL;
        return;
    }

    for (int i = 0; i < NUM_AMBIENTS; i++) {
        channels[i].sfx = ambient_sfx[i];
        vol = ambient_level.value * l->ambient_sound_level[i];
        if (vol < 8) vol = 0;

        if (channels[i].master_vol < vol) {
            channels[i].master_vol += host_frametime * ambient_fade.value;
            if (channels[i].master_vol > vol) channels[i].master_vol = vol;
        } else if (channels[i].master_vol > vol) {
            channels[i].master_vol -= host_frametime * ambient_fade.value;
            if (channels[i].master_vol < vol) channels[i].master_vol = vol;
        }

        channels[i].leftvol = channels[i].rightvol = (int)channels[i].master_vol;
    }
}

channel_t *SND_PickChannel(int entnum, int entchannel)
{
    int first_to_die = -1;
    int life_left = 0x7fffffff;

    for (int i = NUM_AMBIENTS; i < NUM_AMBIENTS + MAX_DYNAMIC_CHANNELS; ++i) {
        if (entchannel != 0 && channels[i].entnum == entnum &&
            (channels[i].entchannel == entchannel || entchannel == -1)) {
            first_to_die = i;
            break;
        }
        if (channels[i].entnum == cl.viewentity && entnum != cl.viewentity && channels[i].sfx)
            continue;
        if (channels[i].end - paintedtime < life_left) {
            life_left = channels[i].end - paintedtime;
            first_to_die = i;
        }
    }

    if (first_to_die == -1) return NULL;
    channels[first_to_die].sfx = NULL;
    channels[first_to_die].generation = sound_generation;
    return &channels[first_to_die];
}

void SND_Spatialize(channel_t *ch)
{
    vec_t dot, dist, lscale, rscale, scale;
    vec3_t source_vec;

    if (ch->entnum == cl.viewentity) {
        ch->leftvol = ch->master_vol;
        ch->rightvol = ch->master_vol;
        return;
    }

    VectorSubtract(ch->origin, listener_origin, source_vec);
    dist = VectorNormalize(source_vec) * ch->dist_mult;
    dot = DotProduct(listener_right, source_vec);

    rscale = 1.0 + dot;
    lscale = 1.0 - dot;

    scale = (1.0 - dist) * rscale;
    ch->rightvol = (int)(ch->master_vol * scale);
    if (ch->rightvol < 0) ch->rightvol = 0;

    scale = (1.0 - dist) * lscale;
    ch->leftvol = (int)(ch->master_vol * scale);
    if (ch->leftvol < 0) ch->leftvol = 0;
}

static void MusicCloseNow(void)
{
    if (music_file) {
        _papp_svc->file_close(music_file);
        music_file = NULL;
    }
    if (music_decoder) {
        papp_mp3_destroy(music_decoder);
        music_decoder = NULL;
    }
    music_input_pos = 0;
    music_input_len = 0;
    music_pcm_frames = 0;
    music_pcm_channels = 0;
    music_track = 0;
    music_active = 0;
    music_logged_format = 0;
    music_pos_fixed = 0;
}

static int MusicSkipId3(void)
{
    uint8_t header[10];
    size_t got;

    if (!music_file)
        return 0;

    got = _papp_svc->file_read(header, 1, sizeof(header), music_file);
    if (got == sizeof(header) && header[0] == 'I' && header[1] == 'D' &&
        header[2] == '3' && !(header[6] & 0x80) && !(header[7] & 0x80) &&
        !(header[8] & 0x80) && !(header[9] & 0x80)) {
        long tag_size = 10L + ((long)header[6] << 21) +
                        ((long)header[7] << 14) + ((long)header[8] << 7) +
                        (long)header[9];
        if (header[5] & 0x10)
            tag_size += 10;
        return _papp_svc->file_seek(music_file, tag_size, 0) == 0;
    }

    _papp_svc->file_seek(music_file, 0, 0);
    return 1;
}

static void MusicResetInput(void)
{
    music_input_pos = 0;
    music_input_len = 0;
    music_pcm_frames = 0;
    music_pos_fixed = 0;
    if (music_decoder)
        papp_mp3_reset(music_decoder);
}

static int MusicRewind(void)
{
    if (!music_file || !music_decoder)
        return 0;
    if (_papp_svc->file_seek(music_file, 0, 0) != 0)
        return 0;
    MusicResetInput();
    return MusicSkipId3();
}

static int MusicReadMore(void)
{
    size_t remaining;
    size_t room;
    size_t got;

    if (!music_file)
        return 0;

    remaining = music_input_len - music_input_pos;
    if (music_input_pos > 0 && remaining > 0)
        memmove(music_input, music_input + music_input_pos, remaining);
    music_input_pos = 0;
    music_input_len = remaining;

    room = sizeof(music_input) - music_input_len;
    if (!room)
        return 0;
    got = _papp_svc->file_read(music_input + music_input_len, 1, room,
                               music_file);
    music_input_len += got;
    return got > 0;
}

static int MusicDecodeNext(void)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        int consumed = 0;
        int samples = 0;
        int sample_rate = 0;
        int channels = 0;
        int result;
        int available;

        if (!music_file || !music_decoder)
            return 0;

        available = (int)(music_input_len - music_input_pos);
        if (available < 1536) {
            if (!MusicReadMore()) {
                if (music_looping && MusicRewind())
                    continue;
                MusicCloseNow();
                return 0;
            }
            available = (int)(music_input_len - music_input_pos);
        }

        result = papp_mp3_decode(music_decoder, music_input + music_input_pos,
                                 available, music_pcm, MUSIC_PCM_SAMPLES,
                                 &consumed, &samples, &sample_rate, &channels);
        if (consumed > 0) {
            size_t advance = (size_t)consumed;
            if (advance > music_input_len - music_input_pos)
                advance = music_input_len - music_input_pos;
            music_input_pos += advance;
        }

        if (result == 0 && samples > 0 && (channels == 1 || channels == 2)) {
            music_pcm_channels = channels;
            music_pcm_frames = samples / channels;
            music_sample_rate = sample_rate > 0 ? sample_rate : MUSIC_OUTPUT_RATE;
            music_step_fixed = (uint32_t)(((uint64_t)music_sample_rate << 16) /
                                          MUSIC_OUTPUT_RATE);
            if (!music_step_fixed)
                music_step_fixed = 1;
            if (!music_logged_format) {
                _papp_svc->log_printf(
                    "QUAKE MUSIC: decoded track %02d: %d Hz, %d channel(s), %d frames\n",
                    music_track, music_sample_rate, music_pcm_channels,
                    music_pcm_frames);
                music_logged_format = 1;
            }
            return 1;
        }

        /* A malformed frame or a non-MP3 prefix: resynchronize. */
        if (consumed == 0 && music_input_pos < music_input_len)
            music_input_pos++;

        if (result != 0 && music_input_pos >= music_input_len)
            MusicReadMore();
    }

    _papp_svc->log_printf("QUAKE MUSIC: MP3 decode/resync failed on track %02d\n",
                          music_track);
    return 0;
}

static void MusicOpenTrack(int track, int looping)
{
    char path[96];

    MusicCloseNow();
    if (track <= 0)
        return;

    /* The launcher VFS only translates /sd/... paths. Quake's normal file
       loader expands relative names through com_gamedir, but this CD-track
       shim is outside that path, so use the full id1 path explicitly. */
    snprintf(path, sizeof(path), "/sd/roms/quake/id1/music/track%02d.mp3", track);
    music_file = _papp_svc->file_open(path, "rb");
    if (!music_file) {
        /* Accept the unpadded spelling too; standard Quake distributions use
           track02, but a number of soundtrack packs use track2. */
        snprintf(path, sizeof(path), "/sd/roms/quake/id1/music/track%d.mp3", track);
        music_file = _papp_svc->file_open(path, "rb");
    }
    if (!music_file) {
        _papp_svc->log_printf(
            "QUAKE MUSIC: missing %s (MP3 is supported; OGG is not decoded yet)\n",
            path);
        return;
    }

    music_decoder = papp_mp3_create();
    if (!music_decoder || !MusicSkipId3()) {
        _papp_svc->log_printf("QUAKE MUSIC: cannot initialize %s\n", path);
        MusicCloseNow();
        return;
    }

    music_track = track;
    music_looping = looping ? 1 : 0;
    music_paused = 0;
    music_active = 1;
    _papp_svc->log_printf("QUAKE MUSIC: playing track %02d (%s)\n", track, path);
}

static void MusicApplyRequests(void)
{
    int requested_track = music_request_track;
    int requested_pause = music_request_pause;

    if (requested_track != MUSIC_NO_REQUEST) {
        music_request_track = MUSIC_NO_REQUEST;
        if (requested_track == MUSIC_STOP_REQUEST)
            MusicCloseNow();
        else
            MusicOpenTrack(requested_track, music_request_loop);
    }

    if (requested_pause != MUSIC_NO_REQUEST) {
        music_request_pause = MUSIC_NO_REQUEST;
        music_paused = requested_pause ? 1 : 0;
    }
}

static void MusicMix(int frame, int bgm_volume)
{
    int attempts = 0;

    if (!music_active || music_paused || !bgm_volume)
        return;

    while (attempts++ < 3) {
        int source_frame;
        int16_t left;
        int16_t right;

        if (music_pcm_frames <= 0 && !MusicDecodeNext())
            return;
        if (music_pcm_frames <= 0)
            return;

        if (music_pos_fixed >= ((uint64_t)music_pcm_frames << 16)) {
            music_pos_fixed -= (uint64_t)music_pcm_frames << 16;
            music_pcm_frames = 0;
            continue;
        }

        source_frame = (int)(music_pos_fixed >> 16);
        if (source_frame >= music_pcm_frames)
            continue;

        if (music_pcm_channels == 1) {
            left = music_pcm[source_frame];
            right = left;
        } else {
            left = music_pcm[source_frame * 2];
            right = music_pcm[source_frame * 2 + 1];
        }
        mix_buffer[frame * 2] += (int64_t)left * bgm_volume;
        mix_buffer[frame * 2 + 1] += (int64_t)right * bgm_volume;
        music_pos_fixed += music_step_fixed;
        return;
    }
}

static void audio_task(void *arg)
{
    int64_t deadline = _papp_svc->get_time_us();
    int remainder = 0;
    unsigned blocks = 0;
    int64_t window_start = deadline;
    while (!papp_exit_requested && snd_initialized) {
        memset(mix_buffer, 0, sizeof(mix_buffer));
        int volumeInt = (int)(volume.value * 256);
        int bgmVolumeInt = (int)(bgmvolume.value * 256);
        MusicApplyRequests();

        for (int i = 0; i < AUDIO_BUFFER_SAMPLES; ++i)
            MusicMix(i, bgmVolumeInt);

        SoundLock();
        int current_gen = sound_generation;
        int channel_count = total_channels;
        for (int i = 0; i < channel_count; ++i) {
            channel_t *chan = &channels[i];

            if (chan->generation != current_gen) {
                chan->sfx = NULL;
                continue;
            }

            if (!chan->sfx || (!chan->leftvol && !chan->rightvol)) continue;

            sfxcache_t *cache = &chan->sfx->cache;
            if (!cache->data) continue;

            int pos = chan->pos;
            int length = cache->sampleCount * ESP32_SOUND_STEP;
            uint32_t step = cache->stepFixedPoint;
            int width = cache->sampleWidth;
            uint8_t *data = (uint8_t *)cache->data;
            int lvol = chan->leftvol;
            int rvol = chan->rightvol;

            for (int j = 0; j < AUDIO_BUFFER_SAMPLES; ++j) {
                if (pos >= length) {
                    if (cache->loopStart < 0) {
                        chan->sfx = NULL;
                        break;
                    }
                    pos = (cache->loopStart * ESP32_SOUND_STEP) + (pos % length);
                }

                int32_t sample;
                uint8_t *p = data + (pos >> 12) * width;
                if (width == 1)
                    sample = ((int32_t)*p - 128) << 8;
                else
                    sample = (int16_t)(p[0] | (p[1] << 8));

                mix_buffer[2 * j] += (int64_t)sample * lvol;
                mix_buffer[2 * j + 1] += (int64_t)sample * rvol;
                pos += step;
            }
            chan->pos = pos;
        }
        paintedtime += AUDIO_BUFFER_SAMPLES;
        SoundUnlock();

        for (int i = 0; i < AUDIO_BUFFER_SAMPLES; ++i) {
            int32_t l = (int32_t)((mix_buffer[2 * i] * volumeInt) >> 16);
            int32_t r = (int32_t)((mix_buffer[2 * i + 1] * volumeInt) >> 16);

            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            output_buffer[i * 2] = (int16_t)l;
            output_buffer[i * 2 + 1] = (int16_t)r;
        }

        _papp_svc->audio_submit((short *)output_buffer, AUDIO_BUFFER_SAMPLES);
        SoundLock();
        total_samples_submitted += AUDIO_BUFFER_SAMPLES;
        SoundUnlock();

        /* Keep output at the device's sample rate even when submit() returns
         * before the speaker queue fills. Absolute deadlines avoid cumulative
         * rounding drift and match the independently paced Quake II mixer. */
        remainder += (AUDIO_BUFFER_SAMPLES * 1000000) % MUSIC_OUTPUT_RATE;
        deadline += (AUDIO_BUFFER_SAMPLES * 1000000) / MUSIC_OUTPUT_RATE;
        if (remainder >= MUSIC_OUTPUT_RATE) {
            ++deadline;
            remainder -= MUSIC_OUTPUT_RATE;
        }
        int64_t now = _papp_svc->get_time_us();
        if (deadline < now - 25000) deadline = now;
        while (!papp_exit_requested && snd_initialized && now < deadline) {
            int ms = (int)((deadline - now + 999) / 1000);
            _papp_svc->delay_ms(ms > 10 ? 10 : ms);
            now = _papp_svc->get_time_us();
        }
        if (++blocks % 440 == 0 && _papp_svc->log_printf) {
            int64_t elapsed = now - window_start;
            unsigned rate = elapsed > 0
                ? (unsigned)(440LL * AUDIO_BUFFER_SAMPLES * 1000000 / elapsed) : 0;
            _papp_svc->log_printf("QUAKE AUDIO STREAM blocks=%u rate=%u Hz\n",
                                 blocks, rate);
            window_start = now;
        }
    }
    quake_sound_task_done = 1;
    while (1) _papp_svc->delay_ms(100);
}

void S_Init(void)
{
    Cvar_RegisterVariable(&bgmvolume);
    Cvar_RegisterVariable(&volume);
    Cvar_RegisterVariable(&nosound);
    Cvar_RegisterVariable(&ambient_level);
    Cvar_RegisterVariable(&ambient_fade);

    total_channels = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;
    memset(channels, 0, sizeof(channels));
    sound_generation = 1;

    snd_initialized = true;
    paintedtime = 0;
    total_samples_submitted = 0;
    music_request_track = MUSIC_NO_REQUEST;
    music_request_loop = 1;
    music_request_pause = MUSIC_NO_REQUEST;
    music_track = 0;
    music_active = 0;

    _papp_svc->audio_init(22050);
    _papp_svc->task_create(audio_task, "quake_snd", 8192, NULL, 8,
                           &quake_sound_task_handle, 1);
}

void S_AmbientOff(void) { snd_ambient = false; }
void S_AmbientOn(void) { snd_ambient = true; }

void S_Shutdown(void)
{
    if (!snd_initialized) return;
    snd_initialized = false;
    _papp_svc->delay_ms(100);
}

void papp_sound_shutdown(void)
{
    if (quake_sound_task_handle) {
        for (int i = 0; i < 100 && !quake_sound_task_done; i++)
            _papp_svc->delay_ms(10);
        _papp_svc->task_delete(quake_sound_task_handle);
        quake_sound_task_handle = NULL;
    }
    MusicCloseNow();
}

/* Called by cd_null.c, which provides Quake's platform-independent CD API. */
void papp_music_play(int track, int looping)
{
    music_request_loop = looping ? 1 : 0;
    music_request_track = track > 0 ? track : MUSIC_STOP_REQUEST;
    _papp_svc->log_printf("QUAKE MUSIC: requested track %02d, loop=%d\n",
                          track, looping ? 1 : 0);
}

void papp_music_stop(void)
{
    music_request_track = MUSIC_STOP_REQUEST;
    _papp_svc->log_printf("QUAKE MUSIC: stop requested\n");
}

void papp_music_pause(void)
{
    music_request_pause = 1;
}

void papp_music_resume(void)
{
    music_request_pause = 0;
}

void S_StartSound(int entnum, int entchannel, sfx_t *sfx, vec3_t origin,
                  float fvol, float attenuation)
{
    if (!snd_initialized || nosound.value || !sfx) return;

    SoundLock();
    channel_t *target_chan = SND_PickChannel(entnum, entchannel);
    if (!target_chan) { SoundUnlock(); return; }

    memset(target_chan, 0, sizeof(*target_chan));
    VectorCopy(origin, target_chan->origin);
    target_chan->dist_mult = attenuation / sound_nominal_clip_dist;
    target_chan->master_vol = (int)(fvol * 255);
    target_chan->entnum = entnum;
    target_chan->entchannel = entchannel;
    target_chan->generation = sound_generation;
    SND_Spatialize(target_chan);

    if (target_chan->leftvol || target_chan->rightvol) {
        target_chan->sfx = sfx;
        target_chan->pos = 0;
        target_chan->end = paintedtime + sfx->cache.effectiveLength;
    }
    SoundUnlock();
}

void S_StaticSound(sfx_t *sfx, vec3_t origin, float vol, float attenuation)
{
    if (!snd_initialized || !sfx || sfx->cache.loopStart == -1) return;

    SoundLock();
    if (total_channels >= MAX_CHANNELS) { SoundUnlock(); return; }
    channel_t *ss = &channels[total_channels++];
    memset(ss, 0, sizeof(*ss));
    ss->sfx = sfx;
    VectorCopy(origin, ss->origin);
    ss->master_vol = (int)vol;
    ss->dist_mult = (attenuation / 64) / sound_nominal_clip_dist;
    ss->end = paintedtime + sfx->cache.effectiveLength;
    ss->generation = sound_generation;
    SND_Spatialize(ss);
    SoundUnlock();
}

void S_LocalSound(char *name)
{
    sfx_t *sfx = FindSfxName(name);
    if (sfx) S_StartSound(cl.viewentity, -1, sfx, vec3_origin, 1, 1);
}

void S_StopSound(int entnum, int entchannel)
{
    if (!snd_initialized) return;
    SoundLock();
    for (int i = 0; i < MAX_CHANNELS; ++i)
        if (channels[i].entnum == entnum && channels[i].entchannel == entchannel)
            channels[i].sfx = NULL;
    SoundUnlock();
}

sfx_t *S_PrecacheSound(char *name) { return FindSfxName(name); }
void S_TouchSound(char *name) { S_PrecacheSound(name); }

void S_Update(vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
    if (!snd_initialized) return;

    SoundLock();
    VectorCopy(origin, listener_origin);
    VectorCopy(forward, listener_forward);
    VectorCopy(right, listener_right);
    VectorCopy(up, listener_up);

    int soundtime_now = (int)total_samples_submitted;

    UpdateAmbientSounds();
    for (int i = NUM_AMBIENTS; i < total_channels; ++i) {
        if (!channels[i].sfx) continue;
        if (channels[i].end <= soundtime_now) {
            channels[i].sfx = NULL;
            continue;
        }
        SND_Spatialize(&channels[i]);
    }
    SoundUnlock();
}

void S_StopAllSounds(qboolean clear)
{
    if (!snd_initialized) return;
    SoundLock();
    total_channels = MAX_DYNAMIC_CHANNELS + NUM_AMBIENTS;
    memset(channels, 0, MAX_CHANNELS * sizeof(channel_t));
    sound_generation++;
    paintedtime = 0;
    total_samples_submitted = 0;
    SoundUnlock();
}

void S_ClearBuffer(void) {}
void S_BeginPrecaching(void)
{
    SoundLock();
    num_sfx = 0;
    memset(known_sfx, 0, sizeof(known_sfx));
    sound_generation++;
    SoundUnlock();
}
void S_EndPrecaching(void) {}
void S_ClearPrecache(void) {}
void S_ExtraUpdate(void) {}

void S_PrecacheAmbients(void)
{
    ambient_sfx[AMBIENT_WATER] = S_PrecacheSound("ambience/water1.wav");
    ambient_sfx[AMBIENT_SKY] = S_PrecacheSound("ambience/wind2.wav");
}
