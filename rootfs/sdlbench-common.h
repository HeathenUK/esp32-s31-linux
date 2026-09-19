/* Shared measurement machinery for independent, stock-library SDL clients.
 * JSONL output is outside timed loops. No client/library/platform patches.
 */
#ifndef SDLBENCH_COMMON_H
#define SDLBENCH_COMMON_H
#ifndef SDLBENCH_SOURCE_ID
#define SDLBENCH_SOURCE_ID "unversioned"
#endif
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <linux/futex.h>

static int frames=60, warmup=5, width=320, height=200, audio_enabled, fullscreen, timers_enabled;
static const char *only;
static int failures;
static char output_buffer[32768];
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(2); }
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static uint64_t tv_us(struct timeval t) { return (uint64_t)t.tv_sec*1000000+t.tv_usec; }
static void json_string(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c=*s;
        if (c=='"' || c=='\\') printf("\\%c",c);
        else if (c<32) printf("\\u%04x",c);
        else putchar(c);
    }
    putchar('"');
}
static void error_record(const char *op, const char *why) {
    printf("{\"type\":\"error\",\"op\":"); json_string(op);
    printf(",\"reason\":"); json_string(why); puts("}"); failures++;
}
static void check_record(const char *name, int ok) {
    printf("{\"type\":\"check\",\"name\":\"%s\",\"pass\":%s}\n",name,ok?"true":"false");
    if (!ok) failures++;
}
static void snapshot_file(const char *label, const char *path) {
    char buf[2048]; FILE *f=fopen(path,"r");
    if (!f) return;
    while (fgets(buf,sizeof buf,f)) {
        buf[strcspn(buf,"\r\n")]=0;
        printf("{\"type\":\"snapshot\",\"label\":"); json_string(label);
        printf(",\"path\":"); json_string(path);
        printf(",\"value\":"); json_string(buf); puts("}");
    }
    fclose(f);
}
static void metadata(int version) {
    struct utsname u; uname(&u);
    printf("{\"type\":\"source\",\"sha256\":\"%s\"}\n",SDLBENCH_SOURCE_ID);
    printf("{\"type\":\"run\",\"sdl\":%d,\"pid\":%d,\"width\":%d,\"height\":%d,\"frames\":%d,\"warmup\":%d,\"audio_requested\":%d,\"fullscreen_requested\":%d,\"kernel\":",version,getpid(),width,height,frames,warmup,audio_enabled,fullscreen);
    json_string(u.release); printf(",\"build\":"); json_string(u.version); puts("}");
    snapshot_file("start","/proc/meminfo");
    snapshot_file("start","/proc/self/maps");
}
static void futex_probe(void) {
    int word=1; long r; int e;
    errno=0; r=syscall(SYS_futex,&word,FUTEX_WAIT_PRIVATE,0,NULL,NULL,0); e=errno;
    printf("{\"type\":\"futex\",\"op\":\"wait_private_mismatch\",\"rc\":%ld,\"errno\":%d,\"supported\":%s}\n",r,e,r==-1&&e==EAGAIN?"true":"false");
    errno=0; r=syscall(SYS_futex,&word,FUTEX_WAIT,0,NULL,NULL,0); e=errno;
    printf("{\"type\":\"futex\",\"op\":\"wait_shared_mismatch\",\"rc\":%ld,\"errno\":%d,\"supported\":%s}\n",r,e,r==-1&&e==EAGAIN?"true":"false");
}
static int wanted(const char *s) { return !only || !strcmp(only,s); }
static void parse_args(int argc, char **argv) {
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--timers")) timers_enabled=1;
        else if (!strcmp(argv[i],"--audio")) audio_enabled=1;
        else if (!strcmp(argv[i],"--fullscreen")) fullscreen=1;
        else if (!strcmp(argv[i],"--frames") && i+1<argc) frames=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--warmup") && i+1<argc) warmup=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--width") && i+1<argc) width=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--height") && i+1<argc) height=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--case") && i+1<argc) only=argv[++i];
        else { fprintf(stderr,"usage: %s [--frames 60] [--warmup 5] [--width 320] [--height 200] [--audio] [--timers] [--fullscreen] [--case NAME]\n",argv[0]); exit(2); }
    }
    if(frames<1||frames>4096||warmup<0||warmup>100||width<64||width>800||height<64||height>480) exit(2);
    setvbuf(stdout,output_buffer,_IOFBF,sizeof output_buffer);
    /* Bounds a dead event pump or SDL join even without working futexes. */
    alarm(180);
}
static unsigned long long compositor_ticks(void) {
    const char *p=getenv("SDLBENCH_LVDESK_PID");
    if (!p) return 0;
    char path[80],buf[2048]; snprintf(path,sizeof path,"/proc/%d/stat",atoi(p));
    FILE *f=fopen(path,"r"); if(!f) return 0;
    if(!fgets(buf,sizeof buf,f)) { fclose(f); return 0; } fclose(f);
    char *s=strrchr(buf,')'); if(!s) return 0; s+=2;
    unsigned long long result=0;
    for(int field=3;field<=15;field++) {
        char *end=strchr(s,' ');
        if(field==14||field==15) result+=strtoull(s,NULL,10);
        if(!end) break;
        s=end+1;
    }
    return result;
}
static uint64_t samples[4096];
static int compare_u64(const void *a,const void *b) {
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b; return (x>y)-(x<y);
}
typedef int (*bench_fn)(int);
static int measured_cases, timing_active;
static uint64_t stage_ns[4];
static const char *stage_names[4];
static void stage_add(int slot, const char *name, uint64_t elapsed) {
    if(timing_active) { stage_names[slot]=name; stage_ns[slot]+=elapsed; }
}
static void bench(const char *name, const char *boundary, bench_fn fn) {
    if(!wanted(name)) return;
    measured_cases++;
    uint64_t prime_start=now_ns();
    for(int i=0;i<warmup || (warmup && now_ns()-prime_start<250000000ULL);i++)
        if(fn(i)) { error_record(name,"warmup failed"); return; }
    struct rusage a,b; unsigned long long c0=compositor_ticks();
    getrusage(RUSAGE_SELF,&a);
    memset(stage_ns,0,sizeof stage_ns); memset(stage_names,0,sizeof stage_names);
    timing_active=1;
    uint64_t t0=now_ns(); int n;
    for(n=0;n<frames;n++) {
        uint64_t t=now_ns();
        if(fn(n+warmup)) break;
        samples[n]=now_ns()-t;
    }
    uint64_t elapsed=now_ns()-t0; timing_active=0; getrusage(RUSAGE_SELF,&b);
    unsigned long long c1=compositor_ticks();
    if(n!=frames) error_record(name,"operation failed");
    if(!n) return;
    qsort(samples,n,sizeof samples[0],compare_u64);
    printf("{\"type\":\"measure\",\"case\":\"%s\",\"boundary\":\"%s\",\"n\":%d,\"wall_ns\":%"PRIu64",\"mean_ns\":%"PRIu64",\"p50_ns\":%"PRIu64",\"p95_ns\":%"PRIu64",\"max_ns\":%"PRIu64",\"user_us\":%"PRIu64",\"sys_us\":%"PRIu64",\"minor_faults\":%ld,\"major_faults\":%ld,\"voluntary_cs\":%ld,\"involuntary_cs\":%ld,\"lvdesk_ticks\":%llu,\"clk_tck\":%ld}\n",name,boundary,n,elapsed,elapsed/n,samples[(n-1)/2],samples[(95*n+99)/100-1],samples[n-1],tv_us(b.ru_utime)-tv_us(a.ru_utime),tv_us(b.ru_stime)-tv_us(a.ru_stime),b.ru_minflt-a.ru_minflt,b.ru_majflt-a.ru_majflt,b.ru_nvcsw-a.ru_nvcsw,b.ru_nivcsw-a.ru_nivcsw,c1-c0,sysconf(_SC_CLK_TCK));
    for(int j=0;j<4;j++) if(stage_names[j])
        printf("{\"type\":\"stage\",\"case\":\"%s\",\"stage\":\"%s\",\"n\":%d,\"total_ns\":%"PRIu64",\"mean_ns\":%"PRIu64"}\n",name,stage_names[j],n,stage_ns[j],stage_ns[j]/n);
}
static int timer_only(int i) { (void)i; return 0; }
#endif
