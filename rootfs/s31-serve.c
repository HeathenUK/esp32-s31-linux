// SPDX-License-Identifier: GPL-2.0-only
/*
 * Serve one file over HTTP, once, then exit.
 *
 *     s31-serve <file> [port]
 *
 * The board has wget but no httpd and no nc, so files could only ever be
 * PULLED onto it - deploy.py works by serving from the host and having the
 * board fetch. Going the other way meant base64 over the serial console, which
 * is ~65 KB/s measured: fine for a screenshot, 13 minutes for a 52 MB video.
 *
 * Deliberately one-shot and trivial: it exists to hand a recording to the host
 * and then get out of the way. No directory listing, no parsing of the request
 * beyond draining it, no daemon left running to be forgotten about - a
 * background server that outlives its purpose is how this project has lost
 * whole afternoons before.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : NULL;
	int port = argc > 2 ? atoi(argv[2]) : 8080;
	struct sockaddr_in a = { 0 };
	char hdr[256], junk[2048];
	struct stat st;
	int ls, cs, fd, one = 1;
	off_t off = 0, left;

	if (!path) { fprintf(stderr, "usage: s31-serve <file> [port]\n"); return 2; }
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st)) { perror(path); return 1; }

	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 1)) {
		perror("bind/listen");
		return 1;
	}
	printf("serving %s (%lld bytes) on port %d\n", path,
	       (long long)st.st_size, port);
	fflush(stdout);

	cs = accept(ls, NULL, NULL);
	if (cs < 0) { perror("accept"); return 1; }
	/* Drain the request line and headers; we serve the same file regardless. */
	read(cs, junk, sizeof(junk));

	snprintf(hdr, sizeof(hdr),
		 "HTTP/1.0 200 OK\r\n"
		 "Content-Type: application/octet-stream\r\n"
		 "Content-Length: %lld\r\n\r\n", (long long)st.st_size);
	write(cs, hdr, strlen(hdr));

	/*
	 * sendfile, not a read/write loop: the payload never enters this
	 * process's address space, which matters on a board where free memory
	 * is the binding constraint and the file can be tens of megabytes.
	 */
	left = st.st_size;
	while (left > 0) {
		ssize_t n = sendfile(cs, fd, &off, left > 65536 ? 65536 : left);

		if (n <= 0) { perror("sendfile"); break; }
		left -= n;
	}
	close(cs);
	close(ls);
	close(fd);
	printf("sent %lld bytes\n", (long long)(st.st_size - left));
	return left ? 1 : 0;
}
