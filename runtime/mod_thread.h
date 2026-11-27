/* Threads, a lock and a wake-up for the room's mods (mod_sync.c, mod_fetch.c), without the runtime's
 * headers: Win32 on Windows, pthreads elsewhere - so that tests/mod_sync_test.c builds them alone. */
#pragma once
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK ModLock;
typedef CONDITION_VARIABLE ModWake;
#define MOD_LOCK_INIT SRWLOCK_INIT
#define MOD_WAKE_INIT CONDITION_VARIABLE_INIT
static inline void mod_lock(ModLock *l) { AcquireSRWLockExclusive(l); }
static inline void mod_unlock(ModLock *l) { ReleaseSRWLockExclusive(l); }
static inline void mod_wake(ModWake *w) { WakeAllConditionVariable(w); }
static inline void mod_wait(ModWake *w, ModLock *l, unsigned ms) { SleepConditionVariableSRW(w, l, ms, 0); }
static inline uint64_t mod_now_ms(void) { return GetTickCount64(); }
static inline void mod_sleep(unsigned ms) { Sleep(ms); }
typedef struct { void (*fn)(void *); void *arg; } ModThreadStart;
static DWORD WINAPI mod_thread_main(LPVOID p) {
    ModThreadStart s = *(ModThreadStart *)p;
    HeapFree(GetProcessHeap(), 0, p);
    s.fn(s.arg);
    return 0;
}
static inline int mod_thread(void (*fn)(void *), void *arg) {
    ModThreadStart *s = HeapAlloc(GetProcessHeap(), 0, sizeof *s);
    if (!s) return 0;
    s->fn = fn, s->arg = arg;
    HANDLE h = CreateThread(NULL, 0, mod_thread_main, s, 0, NULL);
    if (!h) { HeapFree(GetProcessHeap(), 0, s); return 0; }
    CloseHandle(h);
    return 1;
}
#else
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
typedef pthread_mutex_t ModLock;
typedef pthread_cond_t ModWake;
#define MOD_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define MOD_WAKE_INIT PTHREAD_COND_INITIALIZER
static inline void mod_lock(ModLock *l) { pthread_mutex_lock(l); }
static inline void mod_unlock(ModLock *l) { pthread_mutex_unlock(l); }
static inline void mod_wake(ModWake *w) { pthread_cond_broadcast(w); }
static inline void mod_wait(ModWake *w, ModLock *l, unsigned ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) t.tv_sec++, t.tv_nsec -= 1000000000L;
    pthread_cond_timedwait(w, l, &t);
}
static inline uint64_t mod_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static inline void mod_sleep(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
typedef struct { void (*fn)(void *); void *arg; } ModThreadStart;
static void *mod_thread_main(void *p) {
    ModThreadStart s = *(ModThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
static inline int mod_thread(void (*fn)(void *), void *arg) {
    ModThreadStart *s = malloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn, s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, mod_thread_main, s)) { free(s); return 0; }
    pthread_detach(t);
    return 1;
}
#endif
