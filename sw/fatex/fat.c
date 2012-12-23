#define _LARGEFILE64_SOURCE

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include "dumpio.h"
#include "fat.h"

static struct dumpio *_io;

enum {
	FL_FILE = 1,
	FL_LIST = 2,
	FL_EXTRACT = 4,
	FL_VERBOSE = 8,
};

#define SHIFT do { argv[1] = argv[0]; argc--; argv++; } while(0)
#define XFAIL(p) do { if (p) { fprintf(stderr, "%s (%s:%d): operation failed: %s\n", __FUNCTION__, __FILE__, __LINE__, #p); abort(); } } while(0)

int main(int argc, char *argv[])
{
	int flags = 0;
	
	if (argc < 2) {
		fprintf(stderr, "usage: %s mode ...\n", argv[0]);
		return 1;
	}
	
	while (*argv[1]) {
		switch(*argv[1]) {
		case 't':
			flags |= FL_LIST;
			break;
		case 'x':
			flags |= FL_EXTRACT;
			break;
		case '-':
			break;
		case 'f':
			flags |= FL_FILE;
			break;
		case 'v':
			flags |= FL_VERBOSE;
			break;
		default:
			fprintf(stderr, "%s: unknown flag '%c'\n", argv[0], *argv[1]);
			break;
		}
		argv[1]++;
	}
	SHIFT;
	
	if (!FL_FILE) {
		fprintf(stderr, "%s: 'f' required flag\n", argv[0]);
		return 1;
	}
	
	if (argc < 2) {
		fprintf(stderr, "%s: no file arg?\n", argv[0]);
		return 1;
	}
	
	XFAIL((_io = dumpio_init(argv[1])) == NULL);
	SHIFT;
	
	/* off_t dumpio_pread(io, buf, sz, off) */
	/* off_t dumpio_size */
	
	struct fat32_handle h;
	int p = fat32_find_partition(_io);
	printf("partition at %d\n", p);
	XFAIL(p == -1);
	
	XFAIL(fat32_open(&h, _io, p));
	
	struct fat32_dirent de;
	struct fat32_file fd;
	
	fat32_open_root(&h, &fd);
	while (fat32_readdir(&fd, &de) == 0)
		printf("%s\n", de.name);
	
	return 0;
}
