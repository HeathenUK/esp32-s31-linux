/* Include after SDL.h and sdlbench-common.h. */
static SDL_mutex *test_mutex;
static SDL_sem *test_sem;
static int waiter(void *unused) {
    (void)unused;
    if(SDL_SemWait(test_sem)) return 1;
    if(SDL_LockMutex(test_mutex)) return 2;
    return SDL_UnlockMutex(test_mutex);
}
static void thread_probe(void) {
    if(!wanted("threads")) return;
    test_mutex=SDL_CreateMutex(); test_sem=SDL_CreateSemaphore(0);
    if(!test_mutex||!test_sem) { error_record("threads",SDL_GetError()); return; }
    SDL_LockMutex(test_mutex);
#if SDL_MAJOR_VERSION == 2
    SDL_Thread *t=SDL_CreateThread(waiter,"sdlbench-wait",NULL);
#else
    SDL_Thread *t=SDL_CreateThread(waiter,NULL);
#endif
    if(!t) { error_record("create_thread",SDL_GetError()); SDL_UnlockMutex(test_mutex); return; }
    struct rusage a,b; getrusage(RUSAGE_SELF,&a); uint64_t start=now_ns();
    SDL_Delay(150); /* semaphore contention */
    SDL_SemPost(test_sem);
    SDL_Delay(150); /* mutex contention */
    SDL_UnlockMutex(test_mutex);
    int status=-1; SDL_WaitThread(t,&status);
    uint64_t elapsed=now_ns()-start; getrusage(RUSAGE_SELF,&b);
    printf("{\"type\":\"threads\",\"wall_ns\":%"PRIu64",\"cpu_us\":%"PRIu64",\"worker_status\":%d}\n",elapsed,tv_us(b.ru_utime)+tv_us(b.ru_stime)-tv_us(a.ru_utime)-tv_us(a.ru_stime),status);
    check_record("thread_completion",status==0);
    SDL_DestroySemaphore(test_sem); SDL_DestroyMutex(test_mutex);
}
static uint64_t audio_last,audio_max_gap,audio_sum_gap,audio_intervals;
static unsigned long audio_calls,audio_bytes,audio_late;
static uint64_t audio_period;
static int audio_open;
static void audio_callback(void *unused, Uint8 *stream, int len) {
    (void)unused;
    uint64_t t=now_ns();
    if(audio_last) {
        uint64_t gap=t-audio_last;
        if(gap>audio_max_gap) audio_max_gap=gap;
        audio_sum_gap+=gap; audio_intervals++;
        if(gap>audio_period*2) audio_late++;
    }
    audio_last=t; audio_calls++; audio_bytes+=len;
    memset(stream,0,len); /* S16 silence: exercises real device without sound */
}
static void start_audio(void) {
    if(!audio_enabled) return;
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)) { error_record("audio_init",SDL_GetError()); return; }
    SDL_AudioSpec want,got; memset(&want,0,sizeof want);
    want.freq=22050; want.format=AUDIO_S16SYS; want.channels=1; want.samples=512;
    want.callback=audio_callback;
    if(SDL_OpenAudio(&want,&got)) { error_record("audio_open",SDL_GetError()); return; }
    audio_open=1; audio_period=(uint64_t)got.samples*1000000000ULL/got.freq;
    printf("{\"type\":\"audio_open\",\"frequency\":%d,\"samples\":%u,\"channels\":%u,\"format\":%u,\"period_ns\":%"PRIu64"}\n",got.freq,got.samples,got.channels,got.format,audio_period);
    SDL_PauseAudio(0);
}
static void stop_audio(void) {
    if(!audio_open) return;
    SDL_PauseAudio(1);
    printf("{\"type\":\"audio\",\"callbacks\":%lu,\"bytes\":%lu,\"mean_gap_ns\":%"PRIu64",\"max_gap_ns\":%"PRIu64",\"gaps_over_2_periods\":%lu}\n",audio_calls,audio_bytes,audio_intervals?audio_sum_gap/audio_intervals:0,audio_max_gap,audio_late);
    check_record("audio_callbacks",audio_calls>0);
    SDL_CloseAudio();
}
static int pump(int i) {
    (void)i; SDL_Event event; int n=0;
    while(SDL_PollEvent(&event)) if(++n>4096) return -1;
    return 0;
}
static int idle_wait(int i) { (void)i; SDL_Delay(20); return 0; }
static void ready(void) {
    printf("{\"type\":\"subsystems\",\"sdl_flags\":%u,\"timer_requested\":%d}\n",(unsigned)SDL_WasInit(0),timers_enabled);
    snapshot_file("loaded","/proc/self/maps");
    const char *delay=getenv("SDLBENCH_START_DELAY_MS");
    fflush(stdout);
    if(delay) SDL_Delay((Uint32)atoi(delay));
}
static void hold_picture(void) {
    const char *ms=getenv("SDLBENCH_HOLD_MS");
    if(ms) { int n=atoi(ms); if(n>0 && n<=120000) { fflush(stdout); SDL_Delay(n); } }
}
static void finish(void) {
    stop_audio();
    snapshot_file("end","/proc/meminfo");
    SDL_Quit();
    if(only && !measured_cases && strcmp(only,"threads")) error_record("case","unknown case");
    printf("{\"type\":\"done\",\"failures\":%d}\n",failures);
}
