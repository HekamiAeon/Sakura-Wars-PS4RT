/* Pause from Crowbar's Guide menu (crowbar-sdk 1.2, SPEC section 4).
 *
 * When Crowbar starts the game it passes PS4RT_CONTROL_DIR. The runtime writes "0 ready" in
 * <dir>/state, then a watcher thread reads <dir>/request ten times a second:
 *   "<n> pause"  -> the game stops (answer "<n> paused"), or refuses while a movie plays
 *                   ("<n> refused": Crowbar then shows "Guide unavailable" instead of its menu);
 *   "<n> resume" -> the game starts again (answer "<n> running").
 * Stopping the game: the thread that presents frames waits in runtime_pause_gate() (called from
 * sceVideoOutSubmitFlip and sceGnmSubmitAndFlipCommandBuffers), audio output waits there too
 * (the device plays silence), and the guest's monotonic clocks stand still for the time paused,
 * so the game does not see the pause as a long frame. Without PS4RT_CONTROL_DIR nothing runs. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static int paused; /* written under pause_lock, read lock-free by runtime_pause_gate */
static uint64_t paused_since_ns, paused_total_ns;
static uint64_t last_movie_frame_ns; /* read and written with __atomic builtins */
static char control_dir[1024];

static uint64_t host_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Time the guest spent paused, to take off its monotonic clocks (runtime_kernel.c). */
uint64_t runtime_pause_offset_ns(void) {
    pthread_mutex_lock(&pause_lock);
    uint64_t total = paused_total_ns + (paused ? host_now_ns() - paused_since_ns : 0);
    pthread_mutex_unlock(&pause_lock);
    return total;
}

/* Called by the threads that present frames and play sound: waits while the game is paused.
 * Returns 1 when it waited (audio then restarts its cadence), 0 at once otherwise. */
int runtime_pause_gate(void) {
    if (!__atomic_load_n(&paused, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&pause_lock);
    int waited = 0;
    while (paused) { pthread_cond_wait(&pause_cond, &pause_lock); waited = 1; }
    pthread_mutex_unlock(&pause_lock);
    return waited;
}

/* The game is paused from Crowbar's Guide (read lock-free by the pad). */
int runtime_pause_active(void) { return __atomic_load_n(&paused, __ATOMIC_ACQUIRE); }

/* A movie frame was just queued (runtime_videodec.c): a pause now would cut the movie. */
void runtime_pause_movie_frame(void) { __atomic_store_n(&last_movie_frame_ns, host_now_ns(), __ATOMIC_RELEASE); }

static void write_state(long seq, const char *word) {
    char path[1100], tmp[1110];
    snprintf(path, sizeof(path), "%s/state", control_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) return;
    fprintf(out, "%ld %s\n", seq, word);
    if (fclose(out) != 0) { remove(tmp); return; }
#ifdef _WIN32
    remove(path); /* rename does not replace an existing file on Windows */
#endif
    if (rename(tmp, path) != 0) remove(tmp);
}

static void set_paused(int on) {
    pthread_mutex_lock(&pause_lock);
    if (on && !paused) { paused_since_ns = host_now_ns(); __atomic_store_n(&paused, 1, __ATOMIC_RELEASE); }
    else if (!on && paused) {
        paused_total_ns += host_now_ns() - paused_since_ns;
        __atomic_store_n(&paused, 0, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&pause_cond);
    }
    pthread_mutex_unlock(&pause_lock);
}

static void *watch(void *unused) {
    (void)unused;
    long last = 0;
    char path[1100];
    snprintf(path, sizeof(path), "%s/request", control_dir);
    for (;;) {
        struct timespec nap = {0, 100000000};
        nanosleep(&nap, NULL);
        FILE *in = fopen(path, "rb");
        if (!in) continue;
        long seq = 0;
        char word[16] = {0};
        int got = fscanf(in, "%ld %15s", &seq, word);
        fclose(in);
        if (got != 2 || seq <= last) continue;
        last = seq;
        if (!strcmp(word, "pause")) {
            uint64_t movie = __atomic_load_n(&last_movie_frame_ns, __ATOMIC_ACQUIRE);
            if (movie && host_now_ns() - movie < 300000000ull) { write_state(seq, "refused"); continue; }
            set_paused(1);
            write_state(seq, "paused");
        } else if (!strcmp(word, "resume")) {
            set_paused(0);
            write_state(seq, "running");
        }
    }
    return NULL;
}

__attribute__((constructor)) static void start_pause_watch(void) {
    const char *dir = getenv("PS4RT_CONTROL_DIR");
    if (!dir || !*dir || strlen(dir) >= sizeof(control_dir)) return;
    memcpy(control_dir, dir, strlen(dir) + 1);
    write_state(0, "ready");
    pthread_t t;
    if (pthread_create(&t, NULL, watch, NULL) == 0) pthread_detach(t);
}
