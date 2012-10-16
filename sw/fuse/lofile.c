/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall `pkg-config fuse --cflags --libs` lofile.c -o lofile
*/

#define FUSE_USE_VERSION 26

#define _LARGEFILE64_SOURCE

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

struct fwdtab {
	unsigned short phys;
	short confidence;
} __attribute__((packed));

#define NPATCHES 256
struct patch {
	int sector;
	int pg;
	int confidence;
	int fd;
};

static struct fwdtab *_tab;
static int _tabsz;
static int _fd0, _fd1;
static struct patch patches[NPATCHES * 2];

#define PAGESZ 8832L
#define SECBLOCK 1024L
#define ECCSZ 70L
#define SECCNT 8
#define BLOCKSZ 0x200
#define PATCHSECSZ 512L

#define XFAIL(p) do { if (p) { fprintf(stderr, "%s (%s:%d): operation failed: %s\n", __FUNCTION__, __FILE__, __LINE__, #p); abort(); } } while(0)

static const char *lofile_path = "/lofile";

static int lofile_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0555;
		stbuf->st_nlink = 2;
	} else if (strcmp(path, lofile_path) == 0) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = _tabsz * SECBLOCK * SECCNT * BLOCKSZ;
	} else
		res = -ENOENT;

	return res;
}

static int lofile_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, lofile_path + 1, NULL, 0);

	return 0;
}

static int lofile_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, lofile_path) != 0)
		return -ENOENT;

	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int lofile_read_a_little(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	(void) fi;
	int block;
	int phys, cs, pg, sec, secofs;
	int i;

	if(strcmp(path, lofile_path) != 0)
		return -ENOENT;
	
	if (offset >= _tabsz * SECBLOCK * SECCNT * BLOCKSZ)
		return 0;
	
	if ((size + offset % SECBLOCK) > SECBLOCK)
		size = SECBLOCK - (offset % SECBLOCK);
	
	block = offset / (off_t)(SECBLOCK * SECCNT * BLOCKSZ);
	if (_tab[block].confidence < 0) {
		fprintf(stderr, "*** no mapping for block %04x\n", block);
		memset(buf, 0, size);
		return size;
	}
	fprintf(stderr, "  rq for block %04x, phys %04x, confidence %d\n", block, _tab[block].phys, _tab[block].confidence);
	
	phys = _tab[block].phys;
	sec = offset % (off_t)(SECBLOCK * SECCNT) / (off_t)SECBLOCK;
	secofs = offset % (off_t)SECBLOCK;
	
	for (i = 0; i < NPATCHES; i++)
		if (patches[i].sector && ((patches[i].sector / 0x10) == (offset / (512 * 0x10)))) {
			/* we have a patch... */
			fprintf(stderr, "  applying patch from fd %d, pg %x for sector %x\n", patches[i].fd, patches[i].pg, patches[i].sector);
			return pread64(patches[i].fd, buf, size,
			               patches[i].pg * PAGESZ +
			               sec * (SECBLOCK + ECCSZ) +
			               secofs);
		}

	pg = offset % (off_t)(SECBLOCK * SECCNT * BLOCKSZ) / (off_t)(SECBLOCK * SECCNT);
	
	pg = (pg >> 1) + ((pg & 1) << 8);

	cs = pg & 1;
	pg = pg >> 1;
	
	fprintf(stderr, "    offset %08x -> virtblock %04x, cs %d, pg %02x, sec %d, secofs %02x\n", offset, block, cs, pg, sec, secofs);

	return pread64(cs ? _fd1 : _fd0,
	               buf, size,
    	               (phys * (BLOCKSZ / 2) + pg) * PAGESZ +
	               sec * (SECBLOCK + ECCSZ) +
	               secofs);
}

static int lofile_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi) {
	size_t retsz = 0;
	
	printf("read request: offset %08x, size %08x\n", offset, size);
	while (size) {
		int rv;
		
		rv = lofile_read_a_little(path, buf, size, offset, fi);
		if (rv == 0)
			break;
		size -= rv;
		buf += rv;
		retsz += rv;
		offset += rv;
	}
	
	return retsz;
}

static struct fuse_operations lofile_oper = {
	.getattr	= lofile_getattr,
	.readdir	= lofile_readdir,
	.open		= lofile_open,
	.read		= lofile_read,
};

#define SHIFT do { argc--; argv[1] = argv[0]; argv++; } while(0)

int main(int argc, char *argv[])
{
	int fd;
	off_t ofs;
	
	if (argc < 4) {
		fprintf(stderr, "usage: %s blocktable cs0 cs1 [+ patchfile patchlist]* fuseoptions...\n", argv[0]);
		return 1;
	}
	
	XFAIL((fd = open(argv[1], O_RDONLY)) < 0);
	ofs = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	XFAIL((_tab = malloc(ofs)) == NULL);
	read(fd, _tab, ofs);
	close(fd);
	_tabsz = ofs / sizeof(struct fwdtab);
	
	XFAIL((_fd0 = open(argv[2], O_RDONLY)) < 0);
	XFAIL((_fd1 = open(argv[3], O_RDONLY)) < 0);
	
	SHIFT; SHIFT; SHIFT;
	while (argc >= 4 && !strcmp(argv[1], "+")) {
		/* load patch files */
		int pfilfd, plisfd;
		int i;
		struct patch thispatches[NPATCHES] = {{0}};
		
		XFAIL((pfilfd = open(argv[2], O_RDONLY)) < 0);
		XFAIL((plisfd = open(argv[3], O_RDONLY)) < 0);
		
		XFAIL(read(plisfd, thispatches, sizeof(thispatches)) != sizeof(thispatches));
		for (i = 0; i < NPATCHES; i++) {
			int j;
			
			if (thispatches[i].sector == 0)
				break;
			
			for (j = 0; j < NPATCHES * 2; j++)
				if (patches[j].sector == 0 || patches[j].sector == thispatches[i].sector)
					break;
			if (j == NPATCHES * 2) {
				printf("*** patch table full!\n");
				break;
			}
			if (thispatches[i].confidence > patches[j].confidence) {
				printf("installing patch: sector %x -> fd %d, pg %x\n", thispatches[i].sector, pfilfd, thispatches[i].pg);
				patches[j].sector = thispatches[i].sector;
				patches[j].pg = thispatches[i].pg;
				patches[j].confidence = thispatches[i].confidence;
				patches[j].fd = pfilfd;
			}
		}
		
		close(plisfd);
		
		SHIFT; SHIFT; SHIFT;
	}
	
	return fuse_main(argc, argv, &lofile_oper, NULL);
}
