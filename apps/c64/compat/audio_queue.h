#pragma once
#include "runtime.h"

/* Single producer/single consumer. The producer owns a slot until publishing
 * ready; the consumer releases it only after the synchronous I2S call ends.
 * No allocation, libc FILE access, or core state is shared with this task. */
#ifndef EMU_AUDIO_SLOTS
#define EMU_AUDIO_SLOTS 3
#endif
#ifndef EMU_AUDIO_PREFILL
#define EMU_AUDIO_PREFILL 1
#endif
#define EMU_AUDIO_FRAMES 1152
static struct {
    int16_t *data;
    unsigned count[EMU_AUDIO_SLOTS], ready[EMU_AUDIO_SLOTS];
    unsigned head, stop, done;
    void *task;
    int sample_rate;
#ifdef EMU_AUDIO_DIAGNOSTICS
    unsigned underrun_gaps, producer_waits, submitted_frames, queued_frames;
    unsigned max_producer_wait_us;
#endif
} emu_audio;

static void emu_audio_task(void *unused)
{
    (void)unused;
    unsigned tail=0;
#ifdef EMU_AUDIO_DIAGNOSTICS
    bool starved = false;
    int64_t last_report_us = _pax_svc->get_time_us ? _pax_svc->get_time_us() : 0;
#endif
    /* Let the producer establish a small reserve before starting playback. */
    while (!__atomic_load_n(&emu_audio.stop,__ATOMIC_ACQUIRE)) {
        unsigned ready=0;
        for (unsigned i=0; i<EMU_AUDIO_SLOTS; ++i)
            ready += !!__atomic_load_n(&emu_audio.ready[i],__ATOMIC_ACQUIRE);
        if (ready >= EMU_AUDIO_PREFILL) break;
        _pax_svc->delay_ms(1);
    }
    /* audio_init starts the I2S speaker task. Starting it before the first
     * queued blocks are ready makes DMA emit silence and record underflows
     * during every app launch. Start only after the prefill is available. */
    if (!__atomic_load_n(&emu_audio.stop,__ATOMIC_ACQUIRE) && _pax_svc->audio_init)
        _pax_svc->audio_init(emu_audio.sample_rate);
    for (;;) {
        if (__atomic_load_n(&emu_audio.ready[tail],__ATOMIC_ACQUIRE)) {
#ifdef EMU_AUDIO_DIAGNOSTICS
            __atomic_fetch_add(&emu_audio.submitted_frames, emu_audio.count[tail], __ATOMIC_RELAXED);
#endif
            _pax_svc->audio_submit(emu_audio.data+tail*EMU_AUDIO_FRAMES*2,emu_audio.count[tail]);
            __atomic_store_n(&emu_audio.ready[tail],0,__ATOMIC_RELEASE);
            tail=(tail+1)%EMU_AUDIO_SLOTS;
#ifdef EMU_AUDIO_DIAGNOSTICS
            starved = false;
#endif
        } else if (__atomic_load_n(&emu_audio.stop,__ATOMIC_ACQUIRE)) {
            /* Publication may have raced the first ready load. The stop
             * acquire makes the producer's final publication visible. */
            if (!__atomic_load_n(&emu_audio.ready[tail],__ATOMIC_ACQUIRE)) break;
        }
        else {
#ifdef EMU_AUDIO_DIAGNOSTICS
            if (!starved) {
                __atomic_fetch_add(&emu_audio.underrun_gaps, 1, __ATOMIC_RELAXED);
                starved = true;
            }
#endif
            _pax_svc->delay_ms(1);
        }
#ifdef EMU_AUDIO_DIAGNOSTICS
        const int64_t now = _pax_svc->get_time_us ? _pax_svc->get_time_us() : 0;
        if (now - last_report_us >= 5000000) {
            if (_pax_svc->log_printf) {
                _pax_svc->log_printf("PAX audio queue: submitted=%u queued=%u underrun_gaps=%u producer_waits=%u max_wait_us=%u\n",
                    __atomic_load_n(&emu_audio.submitted_frames, __ATOMIC_RELAXED),
                    __atomic_load_n(&emu_audio.queued_frames, __ATOMIC_RELAXED),
                    __atomic_load_n(&emu_audio.underrun_gaps, __ATOMIC_RELAXED),
                    __atomic_load_n(&emu_audio.producer_waits, __ATOMIC_RELAXED),
                    __atomic_load_n(&emu_audio.max_producer_wait_us, __ATOMIC_RELAXED));
            }
            last_report_us = now;
        }
#endif
    }
    __atomic_store_n(&emu_audio.done,1,__ATOMIC_RELEASE);
    /* ABI-1 FreeRTOS task entry points must not return. PAPP2 worker threads
     * are joinable, so they return normally and can be reclaimed by the host. */
#ifndef PAPP_NATIVE_PAPP2
    for (;;) _pax_svc->delay_ms(100);
#endif
}

static void emu_audio_start(int rate)
{
    emu_audio.sample_rate = rate;
    emu_audio.head = 0;
    emu_audio.stop = 0;
    emu_audio.done = 0;
    for (unsigned i=0; i<EMU_AUDIO_SLOTS; ++i) {
        emu_audio.count[i] = 0;
        emu_audio.ready[i] = 0;
    }
#ifdef EMU_AUDIO_DIAGNOSTICS
    emu_audio.underrun_gaps = emu_audio.producer_waits = 0;
    emu_audio.submitted_frames = emu_audio.queued_frames = 0;
    emu_audio.max_producer_wait_us = 0;
#endif
    emu_audio.data=(int16_t *)calloc(EMU_AUDIO_SLOTS*EMU_AUDIO_FRAMES*2,sizeof(int16_t));
    if (!emu_audio.data) {
        if (_pax_svc->audio_init) _pax_svc->audio_init(rate);
        return; /* synchronous fallback */
    }
    if (_pax_svc->task_create(emu_audio_task,"pax_audio",4096,NULL,6,&emu_audio.task,1)) {
        free(emu_audio.data); emu_audio.data=NULL; emu_audio.task=NULL;
        if (_pax_svc->audio_init) _pax_svc->audio_init(rate);
    }
}

static void emu_audio_submit(int16_t *samples, unsigned count)
{
    if (!emu_audio.task) {
        _pax_svc->audio_submit(samples,count);
        return;
    }
    while (count) {
        unsigned chunk=count>EMU_AUDIO_FRAMES ? EMU_AUDIO_FRAMES:count;
        unsigned slot=emu_audio.head;
#ifdef EMU_AUDIO_DIAGNOSTICS
        const int64_t wait_start = _pax_svc->get_time_us ? _pax_svc->get_time_us() : 0;
        bool waited = false;
#endif
        while (__atomic_load_n(&emu_audio.ready[slot],__ATOMIC_ACQUIRE)) {
#ifdef EMU_AUDIO_DIAGNOSTICS
            if (!waited) {
                __atomic_fetch_add(&emu_audio.producer_waits, 1, __ATOMIC_RELAXED);
                waited = true;
            }
#endif
            _pax_svc->delay_ms(1);
        }
#ifdef EMU_AUDIO_DIAGNOSTICS
        if (waited && _pax_svc->get_time_us) {
            unsigned elapsed = (unsigned)(_pax_svc->get_time_us() - wait_start);
            unsigned old = __atomic_load_n(&emu_audio.max_producer_wait_us, __ATOMIC_RELAXED);
            while (elapsed > old && !__atomic_compare_exchange_n(&emu_audio.max_producer_wait_us,
                    &old, elapsed, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        }
#endif
        memcpy(emu_audio.data+slot*EMU_AUDIO_FRAMES*2,samples,chunk*2*sizeof(int16_t));
        emu_audio.count[slot]=chunk;
        __atomic_store_n(&emu_audio.ready[slot],1,__ATOMIC_RELEASE);
#ifdef EMU_AUDIO_DIAGNOSTICS
        __atomic_fetch_add(&emu_audio.queued_frames, chunk, __ATOMIC_RELAXED);
#endif
        emu_audio.head=(slot+1)%EMU_AUDIO_SLOTS;
        samples+=chunk*2; count-=chunk;
    }
}

static void emu_audio_finish(void)
{
    if (!emu_audio.task) return;
    __atomic_store_n(&emu_audio.stop,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&emu_audio.done,__ATOMIC_ACQUIRE)) _pax_svc->delay_ms(1);
    _pax_svc->task_delete(emu_audio.task);
    emu_audio.task=NULL;
    free(emu_audio.data); emu_audio.data=NULL;
}
