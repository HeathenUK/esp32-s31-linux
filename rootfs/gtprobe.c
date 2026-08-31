/* GT1151/GT1158 probe: mirror the vendor component's exact I2C behaviour.
 * Poll 0x814E at 50 Hz, ack with 0 every cycle (the vendor does, even on
 * count==0), parse and print any points. 16-bit big-endian register address.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <time.h>

static int xfer(int fd, struct i2c_msg *m, int n)
{
	struct i2c_rdwr_ioctl_data d = { m, n };
	return ioctl(fd, I2C_RDWR, &d);
}
static int rd(int fd, uint16_t reg, uint8_t *buf, uint16_t len)
{
	uint8_t a[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg m[2] = {
		{ 0x14, 0, 2, a },
		{ 0x14, I2C_M_RD, len, buf },
	};
	return xfer(fd, m, 2) < 0 ? -1 : 0;
}
static int wr(int fd, uint16_t reg, uint8_t v)
{
	uint8_t a[3] = { reg >> 8, reg & 0xff, v };
	struct i2c_msg m = { 0x14, 0, 3, a };
	return xfer(fd, &m, 1) < 0 ? -1 : 0;
}
int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20;
	int fd = open("/dev/i2c-0", O_RDWR);
	uint8_t id[11] = {0}, st, buf[1 + 8*10 + 2];
	int i, n, frames = 0, polls = 0;

	if (fd < 0) { perror("open"); return 1; }
	if (rd(fd, 0x8140, id, 11) < 0) { perror("id read"); return 1; }
	printf("product id: %c%c%c%c sensor %02x\n", id[0], id[1], id[2], id[3], id[10]);
	struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (t.tv_sec - t0.tv_sec >= secs) break;
		polls++;
		if (rd(fd, 0x814E, &st, 1) < 0) { printf("read err\n"); break; }
		n = st & 0x0f;
		if (st != 0)
			printf("status 0x%02x cnt %d\n", st, n);
		if (n > 0 && n <= 10) {
			int len = 1 + 8*n + 2;
			if (rd(fd, 0x814E, buf, len) == 0) {
				uint8_t ck = 0;
				for (i = 0; i < len; i++) ck += buf[i];
				for (i = 0; i < n; i++) {
					uint8_t *p = buf + 1 + 8*i;
					printf("  pt%d id=%d x=%d y=%d s=%d%s\n", i,
					       p[0] & 0xf,
					       p[1] | (p[2] << 8),
					       p[3] | (p[4] << 8),
					       p[5] | (p[6] << 8),
					       ck ? " CKSUM-BAD" : "");
				}
				frames++;
			}
		}
		wr(fd, 0x814E, 0);	/* vendor acks every cycle */
		usleep(20000);
	}
	printf("done: %d polls, %d touch frames\n", polls, frames);
	close(fd);
	return 0;
}
