#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
struct r { uint32_t a,cpu; uint64_t cs; uint32_t f,n,m,p; } __attribute__((aligned(32)));
static struct r rs;
int main(void){
  rs.cpu=(uint32_t)-1;
  long rc = syscall(293, &rs, sizeof rs, 0, 0x53053053);
  printf("rseq rc=%ld errno=%d cpu_id=%d cpu_id_start=%u\n", rc, rc?errno:0, (int)rs.cpu, rs.a);
  rc = syscall(293, &rs, 20, 0, 0x53053053);
  printf("rseq(len=20) rc=%ld errno=%d\n", rc, rc?errno:0);
  return 0;
}
