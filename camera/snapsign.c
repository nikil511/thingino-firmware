/*
 * snapsign - sign a JPEG the way raptor signs /snap.jpg
 *
 * Reads a JPEG on stdin, inserts raptor's capture-time EXIF block and
 * appends raptor's APP15 Ed25519 signature segment, and writes the
 * result to stdout. Uses raptor-common's rss_jpeg.c unchanged, and the
 * same key file as raptor (32-byte seed, hex public key in <key>.pub),
 * so a camera that later switches to raptor keeps the same identity.
 *
 *   prudyntctl snapshot -c 0 | snapsign > signed.jpg
 *   snapsign -p          print the public key (creating the key if needed)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include "rss_jpeg.h"

#define DEFAULT_KEY "/etc/raptor/sign_ed25519.key"
#define MAX_JPEG (8 * 1024 * 1024)
#define HEADROOM 512

static int read_all(int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		ssize_t n = read(fd, buf + got, len - got);
		if (n <= 0)
			return -1;
		got += (size_t)n;
	}
	return 0;
}

static int load_key(rss_sign_key_t *key, const char *path)
{
	uint8_t seed[32];
	int fd = open(path, O_RDONLY);

	if (fd >= 0) {
		int rc = read_all(fd, seed, sizeof(seed));
		close(fd);
		if (rc < 0) {
			fprintf(stderr, "snapsign: %s: short key file\n", path);
			return -1;
		}
	} else {
		int ur = open("/dev/urandom", O_RDONLY);
		if (ur < 0 || read_all(ur, seed, sizeof(seed)) < 0) {
			fprintf(stderr, "snapsign: /dev/urandom unavailable\n");
			return -1;
		}
		close(ur);
		char dir[256];
		snprintf(dir, sizeof(dir), "%s", path);
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			mkdir(dir, 0755);
		}
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0 || write(fd, seed, sizeof(seed)) != (ssize_t)sizeof(seed) || fsync(fd) < 0) {
			fprintf(stderr, "snapsign: %s: %s\n", path, strerror(errno));
			return -1;
		}
		close(fd);
	}

	crypto_ed25519_key_pair(key->secret, key->public, seed);
	crypto_wipe(seed, sizeof(seed));

	uint8_t digest[64];
	crypto_sha512(digest, key->public, sizeof(key->public));
	memcpy(key->fingerprint, digest, sizeof(key->fingerprint));

	char pub_path[272], hex[65];
	snprintf(pub_path, sizeof(pub_path), "%s.pub", path);
	for (int i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", key->public[i]);
	fd = open(pub_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, hex, 64) != 64)
			fprintf(stderr, "snapsign: %s: write failed\n", pub_path);
		close(fd);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *key_path = DEFAULT_KEY;
	int print_pub = 0;
	int opt;

	while ((opt = getopt(argc, argv, "k:p")) != -1) {
		if (opt == 'k')
			key_path = optarg;
		else if (opt == 'p')
			print_pub = 1;
		else {
			fprintf(stderr, "usage: snapsign [-k key] [-p] < in.jpg > out.jpg\n");
			return 2;
		}
	}

	rss_sign_key_t key;
	if (load_key(&key, key_path) < 0)
		return 1;

	if (print_pub) {
		for (int i = 0; i < 32; i++)
			printf("%02x", key.public[i]);
		printf("\n");
		return 0;
	}

	size_t cap = 256 * 1024, len = 0;
	uint8_t *buf = malloc(cap);
	for (;;) {
		if (!buf) {
			fprintf(stderr, "snapsign: out of memory\n");
			return 1;
		}
		if (cap - len < HEADROOM) {
			if (cap >= MAX_JPEG) {
				fprintf(stderr, "snapsign: input too large\n");
				return 1;
			}
			cap *= 2;
			buf = realloc(buf, cap);
			continue;
		}
		ssize_t n = read(0, buf + len, cap - len - HEADROOM);
		if (n < 0) {
			fprintf(stderr, "snapsign: read: %s\n", strerror(errno));
			return 1;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t utc_us = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;

	int n = rss_jpeg_insert_exif(buf, cap, len, utc_us);
	if (n < 0) {
		fprintf(stderr, "snapsign: input is not a JPEG\n");
		return 1;
	}
	n = rss_jpeg_sign(buf, cap, (size_t)n, &key);
	if (n < 0) {
		fprintf(stderr, "snapsign: input is not a complete JPEG\n");
		return 1;
	}
	crypto_wipe(&key, sizeof(key));

	return fwrite(buf, 1, (size_t)n, stdout) == (size_t)n ? 0 : 1;
}
