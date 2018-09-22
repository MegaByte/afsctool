// kate: auto-insert-doxygen true; backspace-indents true; indent-width 4; keep-extra-spaces true; replace-tabs false; tab-indents true; tab-width 4;
/*
 * @file afsctool.c
 * Copyright "brkirch" (https://brkirch.wordpress.com/afsctool/) 
 * Parallel processing modifications and other tweaks (C) 2015 René J.V. Bertin
 * This code is made available under the GPL3 License
 * (See License.txt)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>
#include <signal.h>

#include <zlib.h>
#ifdef HAS_LZVN
#	include <FastCompression.h>
#endif

#include <sys/attr.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

#include "afsctool.h"
#ifdef SUPPORT_PARALLEL
#	include "ParallelProcess.h"
	static ParallelFileProcessor *PP = NULL;
	static bool exclusive_io = true;
#endif
#include "afsctool_fullversion.h"

#define xfree(x)	if((x)){free((x)); (x)=NULL;}

#if MAC_OS_X_VERSION_MIN_REQUIRED <= MAC_OS_X_VERSION_10_5
	static bool legacy_output = true;
#else
	static bool legacy_output = false;
#endif

// some constants borrowed from libarchive
#define MAX_DECMPFS_XATTR_SIZE	3802	/* max size that can be stored in the xattr. */
#ifndef DECMPFS_MAGIC
#	define DECMPFS_MAGIC		'cmpf'
#endif
#ifndef DECMPFS_XATTR_NAME
#	define DECMPFS_XATTR_NAME	"com.apple.decmpfs"
#endif

// use a hard-coded count so all arrays are always sized equally (and the compiler can warn better)
const int sizeunits = 6;
const char *sizeunit10_short[sizeunits] = {"KB", "MB", "GB", "TB", "PB", "EB"};
const char *sizeunit10_long[sizeunits] = {"kilobytes", "megabytes", "gigabytes", "terabytes", "petabytes", "exabytes"};
const long long int sizeunit10[sizeunits] = {1000, 1000 * 1000, 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000,
	(long long int) 1000 * 1000 * 1000 * 1000 * 1000, (long long int) 1000 * 1000 * 1000 * 1000 * 1000 * 1000};
const char *sizeunit2_short[sizeunits] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
const char *sizeunit2_long[sizeunits] = {"kibibytes", "mebibytes", "gibibytes", "tebibytes", "pebibytes", "exbibytes"};
const long long int sizeunit2[sizeunits] = {1024, 1024 * 1024, 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024,
	(long long int) 1024 * 1024 * 1024 * 1024 * 1024, (long long int) 1024 * 1024 * 1024 * 1024 * 1024 * 1024};

int printVerbose = 0;
void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp, bool onAPFS);

char* getSizeStr(long long int size, long long int size_rounded, int likeFinder)
{
	static char sizeStr[128];
	static int len = sizeof(sizeStr)/sizeof(char);
	int unit2, unit10;
	
	for (unit2 = 0; unit2 + 1 < sizeunits && (size_rounded / sizeunit2[unit2 + 1]) > 0; unit2++);
	for (unit10 = 0; unit10 + 1 < sizeunits && (size_rounded / sizeunit10[unit10 + 1]) > 0; unit10++);

	snprintf(sizeStr, len, "%lld bytes", size);

#ifdef PRINT_SI_SIZES
	int print_si_sizes = 1;
#else
	int print_si_sizes = likeFinder;
#endif
	if (print_si_sizes) {
		// the Finder will happily print "0 bytes on disk" so here we don't bother
		// determining if the human-readable value is > 0.
		switch (unit10)
		{
			case 0:
				snprintf(sizeStr, len, "%s / %0.0f %s (%s, base-10)", sizeStr,
						 (double) size_rounded / sizeunit10[unit10], sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
			case 1:
				snprintf(sizeStr, len, "%s / %.12g %s (%s, base-10)", sizeStr,
						 (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 100) + 5) / 10) / 10,
						 sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
			default:
				snprintf(sizeStr, len, "%s / %0.12g %s (%s, base-10)", sizeStr,
						 (double) (((long long int) ((double) size_rounded / sizeunit10[unit10] * 1000) + 5) / 10) / 100,
						 sizeunit10_short[unit10], sizeunit10_long[unit10]);
				break;
		}
	}
	if (!likeFinder) {
		double humanReadable;
		switch (unit2)
		{
			case 0:
				// this should actually be the only case were we'd need
				// to check if the human readable value is sensical...
				humanReadable = (double) size_rounded / sizeunit2[unit2];
				if( humanReadable >= 1 ) {
					snprintf(sizeStr, len, "%s / %0.0f %s", sizeStr,
						 humanReadable, sizeunit2_short[unit2]);
				}
				break;
			case 1:
				humanReadable = (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 100) + 5) / 10) / 10;
				if( humanReadable > 0 ) {
					snprintf(sizeStr, len, "%s / %.12g %s", sizeStr,
						 humanReadable, sizeunit2_short[unit2]);
				}
				break;
			default:
				humanReadable = (double) (((long long int) ((double) size_rounded / sizeunit2[unit2] * 1000) + 5) / 10) / 100;
				if( humanReadable > 0 ) {
					snprintf(sizeStr, len, "%s / %0.12g %s", sizeStr,
						 humanReadable, sizeunit2_short[unit2]);
				}
				break;
		}
	}
	
	return sizeStr;
}

long long int roundToBlkSize(long long int size, struct stat *fileinfo)
{
	if (size <= 0) {
		return size;
	} else if (size < fileinfo->st_blksize) {
// 		fprintf( stderr, "size=%lld -> blksize %d\n", size, fileinfo->st_blksize );
		return fileinfo->st_blksize;
	} else {
		// round up to the next multiple of st_blksize:
		long long int remainder = size % fileinfo->st_blksize;
// 		fprintf( stderr, "size=%lld -> multiple of blksize %d: %lld (%g ; %d)\n", size,
// 				 fileinfo->st_blksize, (remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size,
// 				 (double) ((remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size) / fileinfo->st_blocks,
// 				 fileinfo->st_blksize);
		return (remainder != 0) ? size + (fileinfo->st_blksize - remainder) : size;
	}
}

static bool quitRequested = FALSE;

static void signal_handler(int sig)
{
	fprintf( stderr, "Received signal %d: afsctool will quit\n", sig );
#ifdef SUPPORT_PARALLEL
	stopParallelProcessor(PP);
#endif
}

bool fileIsCompressable(const char *inFile, struct stat *inFileInfo, bool *isAPFS)
{
	struct statfs fsInfo;
	errno = 0;
	int ret = statfs(inFile, &fsInfo);
	// https://github.com/RJVB/afsctool/pull/1#issuecomment-352727426
	uint32_t MNTTYPE_ZFS_SUBTYPE = 'Z'<<24|'F'<<16|'S'<<8;
// // 	fprintf( stderr, "statfs=%d f_type=%u f_fssubtype=%u \"%s\" ISREG=%d UF_COMPRESSED=%d compressable=%d\n",
// // 			ret, fsInfo.f_type, fsInfo.f_fssubtype, fsInfo.f_fstypename,
// // 			S_ISREG(inFileInfo->st_mode), (inFileInfo->st_flags & UF_COMPRESSED),
// // 			ret >= 0 && fsInfo.f_type == 17
// // 				&& S_ISREG(inFileInfo->st_mode)
// // 				&& (inFileInfo->st_flags & UF_COMPRESSED) == 0 );
	bool _isAPFS = !strncasecmp(fsInfo.f_fstypename, "apfs", 4);
	bool _isZFS = (fsInfo.f_fssubtype == MNTTYPE_ZFS_SUBTYPE);
	if (isAPFS) {
		*isAPFS = ret >= 0 ? _isAPFS && !_isZFS : false;
	}
	if (ret < 0) {
		fprintf( stderr, "\"%s\": %s\n", inFile, strerror(errno) );
		return false;
	}
#ifndef HFSCOMPRESS_TO_ZFS
	if (_isZFS) {
		// ZFS doesn't do HFS/decmpfs compression. It may pretend to, but in
		// that case it will *de*compress the data before committing it. We
		// won't play that game, wasting cycles and rewriting data for nothing.
		return false;
	}
#endif
#ifdef VOL_CAP_FMT_DECMPFS_COMPRESSION
	// https://opensource.apple.com/source/copyfile/copyfile-146/copyfile.c.auto.html
	int rv;
	struct attrlist attrs;
	char volroot[MAXPATHLEN + 1];
	struct {
		uint32_t length;
		vol_capabilities_attr_t volAttrs;
	} volattrs;

	strlcpy(volroot, fsInfo.f_mntonname, sizeof(volroot));
	memset(&attrs, 0, sizeof(attrs));
	attrs.bitmapcount = ATTR_BIT_MAP_COUNT;
	attrs.volattr = ATTR_VOL_CAPABILITIES;

	errno = 0;
	rv = getattrlist(volroot, &attrs, &volattrs, sizeof(volattrs), 0);
// 	fprintf( stderr, "volattrs for \"%s\": rv=%d VOL_CAP_FMT_DECMPFS_COMPRESSION=%d:%d\n", volroot,
// 		rv,
// 		(bool)(volattrs.volAttrs.capabilities[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION),
// 		(bool)(volattrs.volAttrs.valid[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION)
// 	);
	if (errno) {
		fprintf( stderr, "Error getting volattrs for \"%s\": %s\n", volroot, strerror(errno) );
	}
	return (rv != -1 &&
		(volattrs.volAttrs.capabilities[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION) &&
		(volattrs.volAttrs.valid[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_DECMPFS_COMPRESSION)
		&& S_ISREG(inFileInfo->st_mode)
		&& (inFileInfo->st_flags & UF_COMPRESSED) == 0 );
#else
	return (ret >= 0
		&& (!strncasecmp(fsInfo.f_fstypename, "hfs", 3) || _isAPFS)
		&& S_ISREG(inFileInfo->st_mode)
		&& (inFileInfo->st_flags & UF_COMPRESSED) == 0);
#endif
}

/*! Mac OS X basename() can modify the input string when not in 'legacy' mode on 10.6
 * and indeed it does. So we use our own which doesn't, and also doesn't require internal
 * storage.
 */
static const char *lbasename(const char *url)
{ const char *c = NULL;
	if (url)
	{
		if ((c =  strrchr( url, '/' )))
		{
			c++;
		}
		else
		{
			c = url;
		}
	}
	return c;
}

const char *compressionTypeName(int type)
{
	char *name = "";
	switch (type) {
#define STR(v)		#v
#define STRVAL(v)	STR(v)
		case CMP_ZLIB_XATTR:
			name = "ZLIB in decmpfs xattr (" STRVAL(CMP_ZLIB_XATTR) ")";
			break;
		case CMP_ZLIB_RESOURCE_FORK:
			name = "ZLIB in resource fork (" STRVAL(CMP_ZLIB_RESOURCE_FORK) ")";
			break;
		case CMP_LZVN_XATTR:
			name = "LZVN in decmpfs xattr (" STRVAL(CMP_LZVN_XATTR) ")";
			break;
		case CMP_LZVN_RESOURCE_FORK:
			name = "LZVN in resource fork (" STRVAL(CMP_LZVN_RESOURCE_FORK) ")";
			break;
		case CMP_LZFSE_XATTR:
			name = "LZFSE in decmpfs xattr (" STRVAL(CMP_LZFSE_XATTR) ")";
			break;
		case CMP_LZFSE_RESOURCE_FORK:
			name = "LZFSE in resource fork (" STRVAL(CMP_LZFSE_RESOURCE_FORK) ")";
			break;
	}
	return name;
}

#ifdef SUPPORT_PARALLEL
void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo, void *worker )
#else
void compressFile(const char *inFile, struct stat *inFileInfo, struct folder_info *folderinfo)
#endif
{
	long long int maxSize = folderinfo->maxSize;
	int compressionlevel = folderinfo->compressionlevel;
	int comptype = folderinfo->compressiontype;
	bool allowLargeBlocks = folderinfo->allowLargeBlocks;
	double minSavings = folderinfo->minSavings;
	bool checkFiles = folderinfo->check_files;
	bool backupFile = folderinfo->backup_file;

	FILE *in;
	// 64Kb block size (HFS compression is "64K chunked")
	const int compblksize = 0x10000;
	unsigned int numBlocks, outdecmpfsSize = 0;
	void *inBuf = NULL, *outBuf = NULL, *outBufBlock = NULL, *outdecmpfsBuf = NULL, *currBlock = NULL, *blockStart = NULL;
	long long int inBufPos, filesize = inFileInfo->st_size;
	unsigned long int cmpedsize;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, outBufSize = 0;
	UInt32 cmpf = DECMPFS_MAGIC, orig_mode;
	struct timeval times[2];
	char *backupName = NULL;
#ifdef HAS_LZVN
	void *lzvn_WorkSpace = NULL;
#endif
	bool supportsLargeBlocks;

	if (quitRequested)
	{
		return;
	}

	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
	
	if (!fileIsCompressable(inFile, inFileInfo, &folderinfo->onAPFS)){
		return;
	}
	if (filesize > maxSize && maxSize != 0){
		if (folderinfo->print_info > 2)
		{
			fprintf( stderr, "Skipping file %s size %lld > max size %lld\n", inFile, filesize, maxSize );
		}
		return;
	}
	if (filesize == 0){
		if (folderinfo->print_info > 2)
		{
			fprintf( stderr, "Skipping empty file %s\n", inFile );
		}
		return;
	}
	orig_mode = inFileInfo->st_mode;
	if ((orig_mode & S_IWUSR) == 0) {
		chmod(inFile, orig_mode | S_IWUSR);
		lstat(inFile, inFileInfo);
	}
	if ((orig_mode & S_IRUSR) == 0) {
		chmod(inFile, orig_mode | S_IRUSR);
		lstat(inFile, inFileInfo);
	}

	if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0 || chflags(inFile, inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		return;
	}
	
	xattrnamesize = listxattr(inFile, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to get file information (%lu bytes; %s)\n",
					inFile, (unsigned long) xattrnamesize, strerror(errno));
			return;
		}
		if ((xattrnamesize = listxattr(inFile, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "%s: listxattr: %s\n", inFile, strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			if ((strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22) ||
				(strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17))
				return;
		}
		free(xattrnames);
	}

	numBlocks = (filesize + compblksize - 1) / compblksize;
	// TODO: make compression-type specific (as far as that's possible).
	if ((filesize + 0x13A + (numBlocks * 9)) > 2147483647) {
		fprintf( stderr, "Skipping file %s with unsupportable size %lld\n", inFile, filesize );
		return;
	}

#ifdef SUPPORT_PARALLEL
	bool locked = false;
	if( exclusive_io && worker ){
		// Lock the IO lock. We'll unlock it when we're done, but we don't bother
		// when we have to return before that as our caller (a worker thread)
		// will clean up for us.
		locked = lockParallelProcessorIO(worker);
	}
#endif
	in = fopen(inFile, "r+");
	if (in == NULL)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		return;
	}
	inBuf = malloc(filesize);
	if (inBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate input buffer of %lld bytes (%s)\n", inFile, filesize, strerror(errno));
		fclose(in);
		utimes(inFile, times);
		return;
	}
	if (fread(inBuf, filesize, 1, in) != 1)
	{
		fprintf(stderr, "%s: Error reading file (%s)\n", inFile, strerror(errno));
		fclose(in);
		utimes(inFile, times);
		free(inBuf);
		return;
	}
	fclose(in); in = NULL;
	if (backupFile)
	{ int fd, bkNameLen;
	  FILE *fp;
	  char *infile, *inname = NULL;
		if ((infile = strdup(inFile)))
		{ 
			inname = (char*) lbasename(infile);
			// avoid filename overflow; assume 32 fixed template char for mkstemps
			// just to be on the safe side (even in parallel mode).
			if (strlen(inname) > 1024 - 32)
			{
				// truncate
				inname[1024-32] = '\0';
			}
#ifdef SUPPORT_PARALLEL
			// add the processor ID for the unlikely case that 2 threads try to backup a file with the same name
			// at the same time, and mkstemps() somehow generates the same temp. name. I've seen it generate EEXIST
			// errors which suggest that might indeed happen.
			bkNameLen = asprintf(&backupName, "/tmp/afsctbk.%d.XXXXXX.%s", currentParallelProcessorID(worker), inname);
#else
			bkNameLen = asprintf(&backupName, "/tmp/afsctbk.XXXXXX.%s", inname);
#endif
		}
		if (!infile || bkNameLen < 0)
		{
			fprintf(stderr, "%s: malloc error, unable to generate temporary backup filename (%s)\n", inFile, strerror(errno));
			xfree(infile);
			goto bail;
		}
		if ((fd = mkstemps(backupName, strlen(inname)+1)) < 0 || !(fp = fdopen(fd, "w")))
		{
			fprintf(stderr, "%s: error creating temporary backup file %s (%s)\n", inFile, backupName, strerror(errno));
			xfree(infile);
			goto bail;
		}
		xfree(infile);
		if (fwrite(inBuf, filesize, 1, fp) != 1)
		{
			fprintf(stderr, "%s: Error writing to backup file %s (%lld bytes; %s)\n", inFile, backupName, filesize, strerror(errno));
			fclose(fp);
			goto bail;
		}
		fclose(fp);
		utimes(backupName, times);
		chmod(backupName, orig_mode);
	}
#ifdef SUPPORT_PARALLEL
	if( exclusive_io && worker ){
		locked = unLockParallelProcessorIO(worker);
	}
#endif

	outdecmpfsBuf = malloc(3802);
	if (outdecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer (3802 bytes; %s)\n", inFile, strerror(errno));
		utimes(inFile, times);
		goto bail;
	}

	struct compressionType {
		UInt32 xattr, resourceFork;
	} compressionType;
	uLong zlib_EstimatedCompressedChunkSize;
#ifdef HAS_LZVN
	size_t lzvn_EstimatedCompressedSize;

	lzvn_chunk_table *chunkTable = (lzvn_chunk_table*) outBuf;
	ssize_t chunkTableByteSize;
#endif

	switch (comptype) {
		case ZLIB: {
			cmpedsize = zlib_EstimatedCompressedChunkSize = compressBound(compblksize);
			struct compressionType t = {CMP_ZLIB_XATTR, CMP_ZLIB_RESOURCE_FORK};
			compressionType = t;
   #define ZLIB_SINGLESHOT_OUTBUF
#ifdef ZLIB_SINGLESHOT_OUTBUF
			outBufSize = filesize + 0x13A + (numBlocks * 9);
#else
			outBufSize = 0x104 + sizeof(UInt32) + numBlocks * 8;
#endif
#define SET_BLOCKSTART()	blockStart = outBuf + 0x104
			outBuf = malloc(outBufSize);
			break;
		}
#ifdef HAS_LZVN
		case LZVN: {
			cmpedsize = lzvn_EstimatedCompressedSize = MAX(lzvn_encode_work_size(), compblksize);
			lzvn_WorkSpace = malloc(cmpedsize);
			// for this compressor we will let the outBuf grow incrementally. Slower,
			// but use only as much memory as required.
			outBuf = calloc(numBlocks, sizeof(chunkTable));
			chunkTable = outBuf;
			if (!lzvn_WorkSpace || !chunkTable) {
				fprintf(stderr,
						"%s: malloc error, unable to allocate %lu bytes for lzvn workspace or %u element chunk table(%s)\n",
						inFile, cmpedsize, numBlocks, strerror(errno));
				utimes(inFile, times);
				goto bail;
			}
			struct compressionType t = {CMP_LZVN_XATTR, CMP_LZVN_RESOURCE_FORK};
			compressionType = t;
			outBufSize = chunkTableByteSize = numBlocks * sizeof(chunkTable);
			chunkTable[0] = chunkTableByteSize;
// 			fprintf(stderr, "%s: chunkTable[0:%u]=%zd\n", inFile, numBlocks, chunkTableByteSize);
			break;
		}
#endif
		default:
			fprintf(stderr, "%s: unsupported compression type %d bytes\n",
					inFile, comptype);
			utimes(inFile, times);
			goto bail;
			break;
	}

	if (outBuf == NULL && outBufSize != 0)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer of %lu bytes (%s)\n",
				inFile, outBufSize, strerror(errno));
		utimes(inFile, times);
		goto bail;
	}

	outBufBlock = malloc(cmpedsize);
	if (outBufBlock == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate compression buffer of %lu bytes (%s)\n",
				inFile, cmpedsize, strerror(errno));
		utimes(inFile, times);
		goto bail;
	}
	// The header of the compression resource fork (16 bytes):
	// compression magic number
	decmpfs_disk_header *decmpfsAttr = (decmpfs_disk_header*) outdecmpfsBuf;
	// *(UInt32 *) outdecmpfsBuf = EndianU32_NtoL(cmpf);
	decmpfsAttr->compression_magic = OSSwapHostToLittleInt32(cmpf);
	// the compression type: 4 == compressed data in the resource fork.
	// FWIW, libarchive has the following comment in archive_write_disk_posix.c :
	//* If the compressed size is smaller than MAX_DECMPFS_XATTR_SIZE [3802]
	//* and the block count in the file is only one, store compressed
	//* data to decmpfs xattr instead of the resource fork.
	// We do the same below.
	// *(UInt32 *) (outdecmpfsBuf + 4) = EndianU32_NtoL(compressionType.resourceFork);
	decmpfsAttr->compression_type = OSSwapHostToLittleInt32(compressionType.resourceFork);
	// the uncompressed filesize
	// *(UInt64 *) (outdecmpfsBuf + 8) = EndianU64_NtoL(filesize);
	decmpfsAttr->uncompressed_size = OSSwapHostToLittleInt64(filesize);
	// outdecmpfsSize = 0x10;
	outdecmpfsSize = sizeof(decmpfs_disk_header);

	unsigned long currBlockLen, currBlockOffset;
	switch (comptype) {
		case ZLIB:
			*(UInt32 *) outBuf = EndianU32_NtoB(0x100);
			*(UInt32 *) (outBuf + 12) = EndianU32_NtoB(0x32);
			memset(outBuf + 16, 0, 0xF0);
			SET_BLOCKSTART();
			// block table: numBlocks + offset,blocksize pairs for each of the blocks
			*(UInt32 *) blockStart = EndianU32_NtoL(numBlocks);
			// actual compressed data starts after the block table
			currBlock = blockStart + sizeof(UInt32) + (numBlocks * 8);
			currBlockLen = outBufSize - (currBlock - outBuf);
			supportsLargeBlocks = true;
			break;
#ifdef HAS_LZVN
		case LZVN:
			supportsLargeBlocks = false;
			break;
#endif
		default:
			// noop
			break;
	}
	int blockNr;

	// chunking loop that compresses the 64K chunks and handles the result(s).
	// Note that LZVN appears not to be chunked like that (maybe inside lzvn_encode()?)
	// TODO: refactor and merge with the switch() above if LZFSE compression doesn't need chunking either.
	for (inBufPos = 0, blockNr = 0, currBlockOffset = 0
		; inBufPos < filesize
		; inBufPos += compblksize, currBlock += cmpedsize, currBlockOffset += cmpedsize, ++blockNr)
	{
		void *cursor = inBuf + inBufPos;
		uLong bytesAfterCursor = ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos;
		switch (comptype) {
			case ZLIB:
				// reset cmpedsize; it may be changed by compress2().
				cmpedsize = zlib_EstimatedCompressedChunkSize;
				if (compress2(outBufBlock, &cmpedsize, cursor, bytesAfterCursor, compressionlevel) != Z_OK)
				{
					utimes(inFile, times);
					goto bail;
				}
#ifndef ZLIB_SINGLESHOT_OUTBUF
				if (currBlockOffset == 0) {
					currBlockOffset = outBufSize;
				} 
				outBufSize += cmpedsize;
// 				fprintf(stderr, "\tchunk #%d from %lu input bytes: increasing outBuf %p by %lu to %zd bytes\n"
// 						"\tblockStart=%p currBlock=%p outdecmpfsBuf=%p magic=%p\n",
// 						blockNr, bytesAfterCursor, outBuf, cmpedsize, outBufSize, blockStart, currBlock,
// 						outdecmpfsBuf, OSSwapLittleToHostInt32(decmpfsAttr->compression_magic));
				if (!(outBuf = realloc(outBuf, outBufSize + 1))) {
					fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
							inFile, outBufSize, strerror(errno));
					utimes(inFile, times);
					goto bail;
				}
				// update this one!
				SET_BLOCKSTART();
				currBlock = outBuf + currBlockOffset;
				currBlockLen = outBufSize;
// 				fprintf(stderr, "%s: chunk #%d: outBuf increased by %lu to %zd bytes; "
// 						"blockStart=%p, currBlock=&outBuf[%lu]=%p outdecmpfsBuf=%p magic=%p\n",
// 						inFile, blockNr, cmpedsize, outBufSize, blockStart, currBlockOffset, currBlock,
// 						outdecmpfsBuf, OSSwapLittleToHostInt32(decmpfsAttr->compression_magic));
#endif
				break;
#ifdef HAS_LZVN
			case LZVN:{
				UInt32 prevLast = ((UInt32*)outBufBlock)[cmpedsize/sizeof(UInt32)-1];
				cmpedsize =
					lzvn_encode(outBufBlock, lzvn_EstimatedCompressedSize, cursor, bytesAfterCursor, lzvn_WorkSpace);
				if (cmpedsize <= 0)
				{
					fprintf( stderr, "%s: lzvn compression failed on chunk #%d\n", inFile, blockNr);
					utimes(inFile, times);
					goto bail;
				}
				// next offset will start at this offset
				if (blockNr < numBlocks) {
					chunkTable[blockNr+1] = chunkTable[blockNr] + cmpedsize;
				}
				if (currBlockOffset == 0) {
					currBlockOffset = outBufSize;
				} 
				outBufSize += cmpedsize;
// 				fprintf(stderr, "\tchunk #%d from %lu input bytes: increasing outBuf %p by %lu to %zd bytes\n",
// 						blockNr, bytesAfterCursor, outBuf, cmpedsize, outBufSize);
				if (!(outBuf = realloc(outBuf, outBufSize))) {
					fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
							inFile, outBufSize, strerror(errno));
					utimes(inFile, times);
					goto bail;
				}
				// update this one!
				chunkTable = outBuf;
// 				fprintf(stderr, "%s: chunk #%d: outBuf increased by %lu to %zd bytes; currBlock=&outBuf[%lu]=%p; out[0]=0x%x\n",
// 						inFile, blockNr, cmpedsize, outBufSize, currBlockOffset, outBuf + currBlockOffset,
// 						((UInt32*)outBufBlock)[0]);
				currBlock = outBuf + currBlockOffset;
				currBlockLen = outBufSize;
				if (blockNr > 1 && ((UInt32*)currBlock)[-1] != prevLast) {
					fprintf(stderr, "%s: warning, possible chunking overlap: prevLast=%lu currBlock[-1]=%lu currBlock[0]=%lu\n",
							inFile, prevLast, ((UInt32*)currBlock)[-sizeof(UInt32)], ((UInt32*)currBlock)[0]);
				}
				break;
			}
#endif
			default:
				// noop
				break;
		}
		if (supportsLargeBlocks && cmpedsize > (((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos))
		{
			if (!allowLargeBlocks && (((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos) == compblksize)
			{
				if (printVerbose >= 2) {
					fprintf(stderr, "%s: allow large blocks to compress (-L)\n", inFile);
				}
				utimes(inFile, times);
				goto bail;
			}
			*(unsigned char *) outBufBlock = 0xFF;
			memcpy(outBufBlock + 1, inBuf + inBufPos, ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos);
			cmpedsize = ((filesize - inBufPos) > compblksize) ? compblksize : filesize - inBufPos;
			cmpedsize++;
		}
		if (((cmpedsize + outdecmpfsSize) <= 3802) && (numBlocks <= 1))
		{
			// store in directly into the attribute instead of using the resource fork.
			// *(UInt32 *) (outdecmpfsBuf + 4) = EndianU32_NtoL(compressionType.xattr);
			decmpfsAttr->compression_type = OSSwapHostToLittleInt32(compressionType.xattr);
			memcpy(outdecmpfsBuf + outdecmpfsSize, outBufBlock, cmpedsize);
			outdecmpfsSize += cmpedsize;
			break;
		}
		if (currBlockOffset + cmpedsize <= currBlockLen) {
			memcpy(currBlock, outBufBlock, cmpedsize);
		} else {
			fprintf( stderr, "%s: result buffer overrun at chunk #%d (%lu >= %lu)\n", inFile, blockNr,
				currBlockOffset + cmpedsize, currBlockLen);
			utimes(inFile, times);
			goto bail;
		}
		switch (comptype) {
			case ZLIB:
				*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x4) = EndianU32_NtoL(currBlock - blockStart);
				*(UInt32 *) (blockStart + ((inBufPos / compblksize) * 8) + 0x8) = EndianU32_NtoL(cmpedsize);
// 				fprintf(stderr, "blockOffset@%zd = %zd, blockLen@%zd = %zd\n",
// 						((inBufPos / compblksize) * 8) + 0x4, currBlock - blockStart,
// 						((inBufPos / compblksize) * 8) + 0x8, cmpedsize);
				break;
			default:
				// noop
				break;
		}
	}
	// deallocate memory that isn't needed anymore.
#ifdef HAS_LZVN
	xfree(lzvn_WorkSpace);
#endif
	free(outBufBlock); outBufBlock = NULL;

	//if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK)
	if (OSSwapLittleToHostInt32(decmpfsAttr->compression_type) == compressionType.resourceFork)
	{
		if ((((double) (currBlock - outBuf + outdecmpfsSize + 50) / filesize) >= (1.0 - minSavings / 100) && minSavings != 0.0) ||
			currBlock - outBuf + outdecmpfsSize + 50 >= filesize)
		{
			// reject the compression result: doesn't meet user criteria or the compressed
			// version is larger than the uncompressed file.
			// TODO: shouldn't this be checked for all HFS-compressed types,
			//       not just for the resource-fork based variant?
			utimes(inFile, times);
			goto bail;
		}
		// create and write the resource fork:
		switch (comptype) {
			case ZLIB:
#ifndef ZLIB_SINGLESHOT_OUTBUF
// 				fprintf(stderr, "\tfinal increase outBuf %p by %lu to %zd bytes\n"
// 						"\tblockStart=%p currBlock=%p\n",
// 						outBuf, cmpedsize, outBufSize, blockStart, currBlock);
				currBlockOffset = currBlock - outBuf;
				outBufSize += currBlockOffset + sizeof(decmpfs_resource_zlib_trailer);
				if (!(outBuf = realloc(outBuf, outBufSize + 1))) {
					fprintf(stderr, "%s: malloc error, unable to increase output buffer to %lu bytes (%s)\n",
							inFile, outBufSize, strerror(errno));
					utimes(inFile, times);
					goto bail;
				}
				// update this one!
				SET_BLOCKSTART();
				currBlock = outBuf + currBlockOffset;
// 				fprintf(stderr, "%s: chunk #%d: final outBuf increase by %lu to %zd bytes; "
// 						"blockStart=%p, currBlock=&outBuf[%lu]=%p\n",
// 						inFile, blockNr, cmpedsize, outBufSize, blockStart, currBlockOffset, currBlock);
#endif
				*(UInt32 *) (outBuf + 4) = EndianU32_NtoB(currBlock - outBuf);
				*(UInt32 *) (outBuf + 8) = EndianU32_NtoB(currBlock - outBuf - 0x100);
				*(UInt32 *) (blockStart - 4) = EndianU32_NtoB(currBlock - outBuf - 0x104);
				decmpfs_resource_zlib_trailer *resourceTrailer = (decmpfs_resource_zlib_trailer*) currBlock;
// 				memset(currBlock, 0, 24);
				memset(&resourceTrailer->empty[0], 0, 24);
// 				*(UInt16 *) (currBlock + 24) = EndianU16_NtoB(0x1C);
// 				*(UInt16 *) (currBlock + 26) = EndianU16_NtoB(0x32);
// 				*(UInt16 *) (currBlock + 28) = 0;
				resourceTrailer->magic1 = OSSwapHostToBigInt16(0x1C);
				resourceTrailer->magic2 = OSSwapHostToBigInt16(0x32);
				resourceTrailer->spacer1 = 0;
// 				*(UInt32 *) (currBlock + 30) = EndianU32_NtoB(cmpf);
				resourceTrailer->compression_magic = OSSwapHostToBigInt32(cmpf);
// 				*(UInt32 *) (currBlock + 34) = EndianU32_NtoB(0xA);
// 				*(UInt64 *) (currBlock + 38) = EndianU64_NtoL(0xFFFF0100);
// 				*(UInt32 *) (currBlock + 46) = 0;
				resourceTrailer->magic3 = OSSwapHostToBigInt32(0xA);
				resourceTrailer->magic4 = OSSwapHostToLittleInt64(0xFFFF0100);
				resourceTrailer->spacer2 = 0;
// 				fprintf(stderr, "setxattr(XATTR_RESOURCEFORK_NAME) outBuf=%p len=%lu\n", outBuf, currBlock - outBuf + 50);
				if (setxattr(inFile, XATTR_RESOURCEFORK_NAME, outBuf, currBlock - outBuf + 50, 0,
					XATTR_NOFOLLOW | XATTR_CREATE) < 0)
				{
					fprintf(stderr, "%s: setxattr: %s\n", inFile, strerror(errno));
					goto bail;
				}
				break;
#ifdef HAS_LZVN
			case LZVN: {
// 				fprintf(stderr, "lzvn chunk table: {%u", chunkTable[0]);
// 				for (int i = 1; i < numBlocks; ++i) {
// 					fprintf(stderr, ",%u", chunkTable[i]);
// 				}
// 				// chunkTable is just a window on outBuf so we should be able
// 				// to read element [numBlocks].
// 				fprintf(stderr, ",%u}\n", chunkTable[numBlocks]);
				if (setxattr(inFile, XATTR_RESOURCEFORK_NAME, outBuf, outBufSize, 0,
					XATTR_NOFOLLOW | XATTR_CREATE) < 0)
				{
					fprintf(stderr, "%s: setxattr: %s\n", inFile, strerror(errno));
					goto bail;
				}
				break;
			}
#endif
			default:
				// noop
				break;
		}
	}
	// set the decmpfs attribute, which may or may not contain compressed data.
	if (setxattr(inFile, DECMPFS_XATTR_NAME, outdecmpfsBuf, outdecmpfsSize, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
	{
		fprintf(stderr, "%s: setxattr: %s\n", inFile, strerror(errno));
		goto bail;
	}

#ifdef SUPPORT_PARALLEL
	// 20160928: the actual rewrite of the file is never done in parallel
	if( worker ){
		locked = lockParallelProcessorIO(worker);
	}
#else
	signal(SIGINT, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
#endif
	// we rewrite the data fork - it should contain 0 bytes if compression succeeded.
// 	in = fopen(inFile, "w");
	in = NULL;
	int fdIn = open(inFile, O_WRONLY|O_TRUNC|O_EXLOCK);
	if (fdIn == -1)
	{
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		goto bail;
	}
	if (fchflags(fdIn, UF_COMPRESSED | inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		if (fremovexattr(fdIn, DECMPFS_XATTR_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
// 		if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK &&
		if (OSSwapLittleToHostInt32(decmpfsAttr->compression_type) == compressionType.resourceFork &&
			fremovexattr(fdIn, XATTR_RESOURCEFORK_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
		{
			fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
		}
		close(fdIn);
		in = fopen(inFile, "w");
		if (in == NULL)
		{
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			if (backupName)
			{
				fprintf(stderr, "\ta backup is available as %s\n", backupName);
				xfree(backupName);
			}
			goto bail;
		}
		if (fwrite(inBuf, filesize, 1, in) != 1)
		{
			fprintf(stderr, "%s: Error writing to file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
			if (backupName)
			{
				fprintf(stderr, "\ta backup is available as %s\n", backupName);
				xfree(backupName);
			}
			goto bail;
		}
		fclose(in);
		utimes(inFile, times);
		goto bail;
	}
// 	fclose(in); in = NULL;
	fsync(fdIn);
	close(fdIn);
	if (checkFiles)
	{
		lstat(inFile, inFileInfo);
		fdIn = open(inFile, O_RDONLY|O_EXLOCK);
		if (fdIn == -1)
		{
			fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
			goto bail;
		}
		bool sizeMismatch = false, readFailure = false, contentMismatch = false;
		ssize_t checkRead= -2;
		if ((sizeMismatch = inFileInfo->st_size != filesize) || 
			(readFailure = (checkRead = read(fdIn, outBuf, filesize)) != filesize) ||
			(contentMismatch = memcmp(outBuf, inBuf, filesize) != 0))
		{
// 			fclose(in);
			close(fdIn);
			printf("%s: Compressed file check failed, reverting file changes\n", inFile);
			fprintf(stderr, "\tsize mismatch=%d read=%zd failure=%d content mismatch=%d\n",
				sizeMismatch, checkRead, readFailure, contentMismatch);
			if (backupName)
			{
				fprintf(stderr, "\tin case of further failures, a backup will be available as %s\n", backupName);
			}
			if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
			{
				fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
				xfree(backupName);
				goto bail;
			}
			if (removexattr(inFile, DECMPFS_XATTR_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
// 			if (EndianU32_LtoN(*(UInt32 *) (outdecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK && 
			if (OSSwapLittleToHostInt32(decmpfsAttr->compression_type) == compressionType.resourceFork &&
				removexattr(inFile, XATTR_RESOURCEFORK_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
			{
				fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
			}
			in = fopen(inFile, "w");
			if (in == NULL)
			{
				fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
				xfree(backupName);
				goto bail;
			}
			if (fwrite(inBuf, filesize, 1, in) != 1)
			{
				fprintf(stderr, "%s: Error writing to file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
				xfree(backupName);
				goto bail;
			}
		}
		fclose(in);
		in = NULL;
	}
bail:
	utimes(inFile, times);
	if (inFileInfo->st_mode != orig_mode) {
		chmod(inFile, orig_mode);
	}
#ifdef SUPPORT_PARALLEL
	if( worker ){
		locked = unLockParallelProcessorIO(worker);
	}
#endif
	if (in)
	{
		fclose(in);
	}
	if (backupName)
	{
		// a backupName is set and hasn't been unset because of a processing failure:
		// remove the file now.
		unlink(backupName);
		free(backupName);
		backupName = NULL;
	}
#ifndef SUPPORT_PARALLEL
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
#endif
	xfree(inBuf);
	xfree(outBuf);
	xfree(outdecmpfsBuf);
	xfree(outBufBlock);
#ifdef HAS_LZVN
	xfree(lzvn_WorkSpace);
#endif
}

void decompressFile(const char *inFile, struct stat *inFileInfo, bool backupFile)
{
	FILE *in;
	int uncmpret;
	unsigned int compblksize = 0x10000, numBlocks, currBlock;
	long long int filesize;
	unsigned long int uncmpedsize;
	void *inBuf = NULL, *outBuf = NULL, *indecmpfsBuf = NULL, *blockStart;
	char *xattrnames, *curr_attr;
	ssize_t xattrnamesize, indecmpfsLen = 0, inRFLen = 0, getxattrret, RFpos = 0;
	struct timeval times[2];
	UInt32 orig_mode;

	if (quitRequested)
	{
		return;
	}

	// not implemented
	backupFile = false;

	times[0].tv_sec = inFileInfo->st_atimespec.tv_sec;
	times[0].tv_usec = inFileInfo->st_atimespec.tv_nsec / 1000;
	times[1].tv_sec = inFileInfo->st_mtimespec.tv_sec;
	times[1].tv_usec = inFileInfo->st_mtimespec.tv_nsec / 1000;
	
	if (!S_ISREG(inFileInfo->st_mode))
		return;
	if ((inFileInfo->st_flags & UF_COMPRESSED) == 0)
		return;
	orig_mode = inFileInfo->st_mode;
	if ((orig_mode & S_IWUSR) == 0) {
		chmod(inFile, orig_mode | S_IWUSR);
		lstat(inFile, inFileInfo);
	}
	if ((orig_mode & S_IRUSR) == 0) {
		chmod(inFile, orig_mode | S_IRUSR);
		lstat(inFile, inFileInfo);
	}

	xattrnamesize = listxattr(inFile, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);

	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
			goto bail;
		}
		if ((xattrnamesize = listxattr(inFile, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "%s: listxattr: %s\n", inFile, strerror(errno));
			xfree(xattrnames);
			goto bail;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			if (strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22)
			{
				inRFLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (inRFLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					xfree(xattrnames);
					goto bail;
				}
				if (inRFLen != 0)
				{
					inBuf = malloc(inRFLen);
					if (inBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate input buffer\n", inFile);
						goto bail;
					}
					do
					{
						getxattrret = getxattr(inFile, curr_attr, inBuf + RFpos, inRFLen - RFpos, RFpos, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (getxattrret < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							xfree(xattrnames);
							goto bail;
						}
						RFpos += getxattrret;
					} while (RFpos < inRFLen && getxattrret > 0);
				}
			}
			if (strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)
			{
				indecmpfsLen = getxattr(inFile, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				if (indecmpfsLen < 0)
				{
					fprintf(stderr, "%s: getxattr: %s\n", inFile, strerror(errno));
					xfree(xattrnames);
					goto bail;
				}
				if (indecmpfsLen != 0)
				{
					indecmpfsBuf = malloc(indecmpfsLen);
					if (indecmpfsBuf == NULL)
					{
						fprintf(stderr, "%s: malloc error, unable to allocate xattr buffer\n", inFile);
						xfree(inBuf);
						goto bail;
					}
					if (indecmpfsLen != 0)
					{
						indecmpfsBuf = malloc(indecmpfsLen);
						if (indecmpfsBuf == NULL)
						{
							fprintf(stderr, "%s: malloc error, unable to get file information\n", inFile);
							goto bail;
						}
						indecmpfsLen = getxattr(inFile, curr_attr, indecmpfsBuf, indecmpfsLen, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (indecmpfsLen < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							xfree(xattrnames);
							goto bail;
						}
					}
				}
			}
		}
		xfree(xattrnames);
	}

	if (indecmpfsBuf == NULL)
	{
		fprintf(stderr, "%s: Decompression failed; file flags indicate file is compressed but it does not have a com.apple.decmpfs extended attribute\n", inFile);
		goto bail;
	}
	if (indecmpfsLen < sizeof(decmpfs_disk_header))
	{
		fprintf(stderr, "%s: Decompression failed; extended attribute com.apple.decmpfs is only %ld bytes (it is required to have a 16 byte header)\n", inFile, indecmpfsLen);
		goto bail;
	}

	decmpfs_disk_header *decmpfsAttr = (decmpfs_disk_header*) indecmpfsBuf;
	// filesize = EndianU64_LtoN(*(UInt64 *) (indecmpfsBuf + 8));
	filesize = OSSwapLittleToHostInt64(decmpfsAttr->uncompressed_size);
	if (filesize == 0)
	{
		fprintf(stderr, "%s: Decompression failed; file size given in header is 0\n", inFile);
		goto bail;
	}
	outBuf = malloc(filesize);
	if (outBuf == NULL)
	{
		fprintf(stderr, "%s: malloc error, unable to allocate output buffer (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
		goto bail;
	}

#if !__has_builtin(__builtin_available)
#	warning "Please use clang 5 or newer if you can"
	// determine the Darwin major version number
	static int darwinMajor = 0;
	if (darwinMajor == 0)
	{
		FILE *uname = popen("uname -r", "r");
		if (uname)
		{
			fscanf(uname, "%d", &darwinMajor);
			fprintf(stderr, "darwinMajor=%d\n", darwinMajor);
		}
	}
#endif

	bool removeResourceFork = false;
	bool doSimpleDecompression = false;
	int compressionType = OSSwapLittleToHostInt32(decmpfsAttr->compression_type);
	switch (compressionType) {
		// ZLIB decompression is handled in what can be seen as a reference implementation
		// that does the entire decompression explicitly in userland.
		// There is also a simplified approach which exploits the fact that the kernel
		// will decompress files transparently when reading their content. This approach
		// is used for LZFN and LZFSE decompression.
		// if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == CMP_ZLIB_RESOURCE_FORK)
		case CMP_ZLIB_RESOURCE_FORK: {
			if (inBuf == NULL)
			{
				fprintf(stderr,
					"%s: Decompression failed; resource fork required for compression type %d but none exists\n",
					inFile, CMP_ZLIB_RESOURCE_FORK);
				xfree(outBuf);
				goto bail;
			}
			if (inRFLen < 0x13A ||
				inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4)
			{
				fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
				if (inBuf != NULL)
					free(inBuf);
				if (indecmpfsBuf != NULL)
					free(indecmpfsBuf);
				xfree(outBuf);
				goto bail;
			}

			{ FILE *cfp = fopen("/tmp/afsctool-last-zlib-rfork.bin", "w");
				if (cfp) {
					fwrite(inBuf, inRFLen, 1, cfp);
					fclose(cfp);
				} else {
					fprintf(stderr, "%s: opening dump file failed (%s)\n", inFile, strerror(errno));
				}
			}

			blockStart = inBuf + EndianU32_BtoN(*(UInt32 *) inBuf) + 0x4;
			numBlocks = EndianU32_NtoL(*(UInt32 *) blockStart);
			
			if (inRFLen < EndianU32_BtoN(*(UInt32 *) inBuf) + 0x3A + (numBlocks * 8))
			{
				fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
				xfree(outBuf);
				goto bail;
			}
			if (compblksize * (numBlocks - 1) + (filesize % compblksize) > filesize)
			{
				fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
				xfree(outBuf);
				goto bail;
			}
			for (currBlock = 0; currBlock < numBlocks; currBlock++)
			{
				if (blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))) + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))) > inBuf + inRFLen)
				{
					fprintf(stderr, "%s: Decompression failed; resource fork data is incomplete\n", inFile);
					xfree(outBuf);
					goto bail;
				}
				if (currBlock + 1 != numBlocks)
					uncmpedsize = compblksize;
				else
					uncmpedsize = (filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize;
				if ((compblksize * currBlock) + uncmpedsize > filesize)
				{
					fprintf(stderr, "%s: Decompression failed; file size given in header is incorrect\n", inFile);
					xfree(outBuf);
					goto bail;
				}
				if ((*(unsigned char *) (blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))))) == 0xFF)
				{
					uncmpedsize = EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))) - 1;
					memcpy(outBuf + (compblksize * currBlock), blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))) + 1, uncmpedsize);
				}
				else
				{
					if ((uncmpret = uncompress(outBuf + (compblksize * currBlock), &uncmpedsize, blockStart + EndianU32_LtoN(*(UInt32 *) (blockStart + 0x4 + (currBlock * 8))), EndianU32_LtoN(*(UInt32 *) (blockStart + 0x8 + (currBlock * 8))))) != Z_OK)
					{
						if (uncmpret == Z_BUF_ERROR)
						{
							fprintf(stderr, "%s: Decompression failed; uncompressed data block too large\n", inFile);
							xfree(outBuf);
							goto bail;
						}
						else if (uncmpret == Z_DATA_ERROR)
						{
							fprintf(stderr, "%s: Decompression failed; compressed data block is corrupted\n", inFile);
							xfree(outBuf);
							goto bail;
						}
						else if (uncmpret == Z_MEM_ERROR)
						{
							fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
							xfree(outBuf);
							goto bail;
						}
						else
						{
							fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
							xfree(outBuf);
							goto bail;
						}
					}
				}
				if (uncmpedsize != ((filesize - (currBlock * compblksize) < compblksize) ? filesize - (currBlock * compblksize) : compblksize))
				{
					fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
					xfree(outBuf);
					goto bail;
				}
			}
			removeResourceFork = true;
			break;
		}
		// else if (EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4)) == CMP_ZLIB_XATTR)
		case CMP_ZLIB_XATTR: {
			const size_t dataStart = sizeof(decmpfs_disk_header) /* 0x10 */;
			if (indecmpfsLen == dataStart)
			{
				fprintf(stderr,
					"%s: Decompression failed; compression type %d expects compressed data in extended attribute com.apple.decmpfs but none exists\n",
					inFile, CMP_ZLIB_XATTR);
				xfree(outBuf);
				goto bail;
			}
			uncmpedsize = filesize;
			if ((*(unsigned char *) (indecmpfsBuf + dataStart)) == 0xFF)
			{
				const size_t offset = dataStart + 1;
				uncmpedsize = indecmpfsLen - offset;
				memcpy(outBuf, indecmpfsBuf + offset, uncmpedsize);
			}
			else
			{
				if ((uncmpret = uncompress(outBuf, &uncmpedsize, indecmpfsBuf + dataStart, indecmpfsLen - dataStart)) != Z_OK)
				{
					if (uncmpret == Z_BUF_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; uncompressed data too large\n", inFile);
						xfree(outBuf);
						goto bail;
					}
					else if (uncmpret == Z_DATA_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; compressed data is corrupted\n", inFile);
						xfree(outBuf);
						goto bail;
					}
					else if (uncmpret == Z_MEM_ERROR)
					{
						fprintf(stderr, "%s: Decompression failed; out of memory\n", inFile);
						xfree(outBuf);
						goto bail;
					}
					else
					{
						fprintf(stderr, "%s: Decompression failed; an error occurred during decompression\n", inFile);
						xfree(outBuf);
						goto bail;
					}
				}
			}
			if (uncmpedsize != filesize)
			{
				fprintf(stderr, "%s: Decompression failed; uncompressed data block too small\n", inFile);
				xfree(outBuf);
				goto bail;
			}
			break;
		}
		case CMP_LZVN_XATTR:
		case CMP_LZVN_RESOURCE_FORK: {
			{ FILE *cfp = fopen("/tmp/afsctool-last-lzvn-rfork.bin", "w");
				if (cfp) {
					fwrite(inBuf, inRFLen, 1, cfp);
					fclose(cfp);
				}
			}

	        if (
#if __has_builtin(__builtin_available)
				// we can do simplified runtime OS version detection: accept LZVN on 10.9 and up.
				__builtin_available(macOS 10.9, *)
#else
				darwinMajor >= 13
#endif
			) {
				doSimpleDecompression = true;
			}
			else
			{
				fprintf(stderr, "%s: Decompression failed; unsupported compression type %s\n",
						inFile, compressionTypeName(compressionType));
				xfree(outBuf);
				goto bail;
			}
			break;
		}
		case CMP_LZFSE_XATTR:
		case CMP_LZFSE_RESOURCE_FORK: {
	        if (
#if __has_builtin(__builtin_available)
				// we can do simplified runtime OS version detection: accept LZFSE on 10.11 and up.
				__builtin_available(macOS 10.11, *)
#else
				darwinMajor >= 15
#endif
			) {
				doSimpleDecompression = true;
			}
			else
			{
				fprintf(stderr, "%s: Decompression failed; unsupported compression type %s\n",
						inFile, compressionTypeName(compressionType));
				xfree(outBuf);
				goto bail;
			}
			break;
		}
		default: {
			fprintf(stderr, "%s: Decompression failed; unknown compression type %s\n",
					inFile, compressionTypeName(compressionType));
			xfree(outBuf);
			goto bail;
			break;
		}
	}

	if (doSimpleDecompression)
	{
		// the simple approach: let the kernel handle decompression
		// by reading the file content using fread(), into outBuf.
		// First, these can be freed already:
		xfree(inBuf);
		xfree(indecmpfsBuf);
		errno = 0;
		in = fopen(inFile, "r");
		if (in && fread(outBuf, filesize, 1, in) != 1) {
			fprintf(stderr, "%s: decompression failed: %s\n", inFile, strerror(errno));
			fclose(in);
			goto bail;
		}
		fclose(in);
		removeResourceFork = true;
	}

	if (chflags(inFile, (~UF_COMPRESSED) & inFileInfo->st_flags) < 0)
	{
		fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		xfree(outBuf);
		goto bail;
	}

	in = fopen(inFile, "r+");
	if (in == NULL)
	{
		if (chflags(inFile, UF_COMPRESSED & inFileInfo->st_flags) < 0)
			fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		fprintf(stderr, "%s: %s\n", inFile, strerror(errno));
		xfree(outBuf);
		goto bail;
	}

	if (fwrite(outBuf, filesize, 1, in) != 1)
	{
		fprintf(stderr, "%s: Error writing to file (%lld bytes; %s)\n", inFile, filesize, strerror(errno));
		fclose(in);
		if (chflags(inFile, UF_COMPRESSED | inFileInfo->st_flags) < 0)
		{
			fprintf(stderr, "%s: chflags: %s\n", inFile, strerror(errno));
		}
		xfree(outBuf);
		goto bail;
	}

	fclose(in);

	if (removexattr(inFile, DECMPFS_XATTR_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
	}
	if (removeResourceFork && 
		removexattr(inFile, XATTR_RESOURCEFORK_NAME, XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION) < 0)
	{
		fprintf(stderr, "%s: removexattr: %s\n", inFile, strerror(errno));
	}

bail:
	utimes(inFile, times);
	if (inFileInfo->st_mode != orig_mode) {
		chmod(inFile, orig_mode);
	}
	xfree(inBuf);
	xfree(indecmpfsBuf);
	xfree(outBuf);
}

bool checkForHardLink(const char *filepath, const struct stat *fileInfo, const struct folder_info *folderinfo)
{
	static ino_t *hardLinks = NULL;
	static char **paths = NULL, *list_item;
	static long int currSize = 0, numLinks = 0;
	long int right_pos, left_pos = 0, curr_pos = 1;
	
	if (fileInfo != NULL && fileInfo->st_nlink > 1)
	{
		if (hardLinks == NULL)
		{
			currSize = 1;
			hardLinks = (ino_t *) malloc(currSize * sizeof(ino_t));
			if (hardLinks == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
			paths = (char **) malloc(currSize * sizeof(char *));
			if (paths == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
		}
		
		if (numLinks > 0)
		{
			left_pos = 0;
			right_pos = numLinks + 1;
			
			while (hardLinks[curr_pos-1] != fileInfo->st_ino)
			{
				curr_pos = (right_pos - left_pos) / 2;
				if (curr_pos == 0) break;
				curr_pos += left_pos;
				if (hardLinks[curr_pos-1] > fileInfo->st_ino)
					right_pos = curr_pos;
				else if (hardLinks[curr_pos-1] < fileInfo->st_ino)
					left_pos = curr_pos;
			}
			if (curr_pos != 0 && hardLinks[curr_pos-1] == fileInfo->st_ino)
			{
				if (strcmp(filepath, paths[curr_pos-1]) != 0 || strlen(filepath) != strlen(paths[curr_pos-1]))
				{
					if (folderinfo->print_info > 1)
						printf("%s: skipping, hard link to this %s exists at %s\n", filepath, (fileInfo->st_mode & S_IFDIR) ? "folder" : "file", paths[curr_pos-1]);
					return TRUE;
				}
				else
					return FALSE;
			}
		}
		if (currSize < numLinks + 1)
		{
			currSize *= 2;
			hardLinks = (ino_t *) realloc(hardLinks, currSize * sizeof(ino_t));
			if (hardLinks == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
			paths = (char **) realloc(paths, currSize * sizeof(char *));
			if (paths == NULL)
			{
				fprintf(stderr, "Malloc error allocating memory for list of file hard links, exiting...\n");
				exit(ENOMEM);
			}
		}
		if ((numLinks != 0) && ((numLinks - 1) >= left_pos))
		{
			memmove(&hardLinks[left_pos+1], &hardLinks[left_pos], (numLinks - left_pos) * sizeof(ino_t));
			if (paths != NULL)
				memmove(&paths[left_pos+1], &paths[left_pos], (numLinks - left_pos) * sizeof(char *));
		}
		hardLinks[left_pos] = fileInfo->st_ino;
		list_item = (char *) malloc(strlen(filepath) + 1);
		strcpy(list_item, filepath);
		paths[left_pos] = list_item;
		numLinks++;
	}
	else if (fileInfo == NULL && hardLinks != NULL)
	{
		free(hardLinks);
		hardLinks = NULL;
		currSize = 0;
		numLinks = 0;
		if (paths != NULL)
		{
			for (curr_pos = 0; curr_pos < numLinks; curr_pos++)
				free(paths[curr_pos]);
			free(paths);
		}
	}
	return FALSE;
}

void add_extension_to_filetypeinfo(const char *filepath, struct filetype_info *filetypeinfo)
{
	long int right_pos, left_pos = 0, curr_pos = 1, i, fileextensionlen;
	const char *fileextension;
	
	for (i = strlen(filepath) - 1; i > 0; i--)
		if (filepath[i] == '.' || filepath[i] == '/')
			break;
	if (i != 0 && i != strlen(filepath) - 1 && filepath[i] != '/' && filepath[i-1] != '/')
		fileextension = &filepath[i+1];
	else
		return;
	
	if (filetypeinfo->extensions == NULL)
	{
		filetypeinfo->extensionssize = 1;
		filetypeinfo->extensions = (char **) malloc(filetypeinfo->extensionssize * sizeof(char *));
		if (filetypeinfo->extensions == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	
	if (filetypeinfo->numextensions > 0)
	{
		left_pos = 0;
		right_pos = filetypeinfo->numextensions + 1;
		
		while (strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) != 0)
		{
			curr_pos = (right_pos - left_pos) / 2;
			if (curr_pos == 0) break;
			curr_pos += left_pos;
			if (strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) > 0)
				right_pos = curr_pos;
			else if (strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) < 0)
				left_pos = curr_pos;
		}
		if (curr_pos != 0 && strcasecmp(filetypeinfo->extensions[curr_pos-1], fileextension) == 0)
			return;
	}
	if (filetypeinfo->extensionssize < filetypeinfo->numextensions + 1)
	{
		filetypeinfo->extensionssize *= 2;
		filetypeinfo->extensions = (char **) realloc(filetypeinfo->extensions, filetypeinfo->extensionssize * sizeof(char *));
		if (filetypeinfo->extensions == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	if ((filetypeinfo->numextensions != 0) && ((filetypeinfo->numextensions - 1) >= left_pos))
		memmove(&filetypeinfo->extensions[left_pos+1], &filetypeinfo->extensions[left_pos], (filetypeinfo->numextensions - left_pos) * sizeof(char *));
	filetypeinfo->extensions[left_pos] = (char *) malloc(strlen(fileextension) + 1);
	strcpy(filetypeinfo->extensions[left_pos], fileextension);
	for (fileextensionlen = strlen(fileextension), i = 0; i < fileextensionlen; i++)
		filetypeinfo->extensions[left_pos][i] = tolower(filetypeinfo->extensions[left_pos][i]);
	filetypeinfo->numextensions++;
}

char* getFileType(const char *filepath)
{
	CFStringRef filepathCF = CFStringCreateWithCString(kCFAllocatorDefault, filepath, kCFStringEncodingUTF8);
	CFStringRef filetype;
	MDItemRef fileMDItem;
	char *filetypeMaxLenStr, *filetypeStr;
	UInt32 filetypeMaxLen;
	
	fileMDItem = MDItemCreate(kCFAllocatorDefault, filepathCF);
	CFRelease(filepathCF);
	if (fileMDItem == NULL)
		return NULL;
	filetype = (CFStringRef) MDItemCopyAttribute(fileMDItem, kMDItemContentType);
	CFRelease(fileMDItem);
	if (filetype == NULL)
		return NULL;
	filetypeMaxLen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(filetype), kCFStringEncodingUTF8) + 1;
	filetypeMaxLenStr = (char *) malloc(filetypeMaxLen);
	if (!CFStringGetCString(filetype, filetypeMaxLenStr, filetypeMaxLen, kCFStringEncodingUTF8))
	{
		CFRelease(filetype);
		return NULL;
	}
	CFRelease(filetype);
	filetypeStr = (char *) malloc(strlen(filetypeMaxLenStr) + 1);
	if (filetypeStr == NULL) return NULL;
	strcpy(filetypeStr, filetypeMaxLenStr);
	free(filetypeMaxLenStr);
	
	return filetypeStr;
}

struct filetype_info* getFileTypeInfo(const char *filepath, const char *filetype, struct folder_info *folderinfo)
{
	long int right_pos, left_pos = 0, curr_pos = 1;
	
	if (filetype == NULL)
		return NULL;
	
	if (folderinfo->filetypes == NULL)
	{
		folderinfo->filetypessize = 1;
		folderinfo->filetypes = (struct filetype_info *) malloc(folderinfo->filetypessize * sizeof(struct filetype_info));
		if (folderinfo->filetypes == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	
	if (folderinfo->numfiletypes > 0)
	{
		left_pos = 0;
		right_pos = folderinfo->numfiletypes + 1;
		
		while (strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) != 0)
		{
			curr_pos = (right_pos - left_pos) / 2;
			if (curr_pos == 0) break;
			curr_pos += left_pos;
			if (strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) > 0)
				right_pos = curr_pos;
			else if (strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) < 0)
				left_pos = curr_pos;
		}
		if (curr_pos != 0 && strcmp(folderinfo->filetypes[curr_pos-1].filetype, filetype) == 0)
		{
			add_extension_to_filetypeinfo(filepath, &folderinfo->filetypes[curr_pos-1]);
			return &folderinfo->filetypes[curr_pos-1];
		}
	}
	if (folderinfo->filetypessize < folderinfo->numfiletypes + 1)
	{
		folderinfo->filetypessize *= 2;
		folderinfo->filetypes = (struct filetype_info *) realloc(folderinfo->filetypes, folderinfo->filetypessize * sizeof(struct filetype_info));
		if (folderinfo->filetypes == NULL)
		{
			fprintf(stderr, "Malloc error allocating memory for list of file types, exiting...\n");
			exit(ENOMEM);
		}
	}
	if ((folderinfo->numfiletypes != 0) && ((folderinfo->numfiletypes - 1) >= left_pos))
		memmove(&folderinfo->filetypes[left_pos+1], &folderinfo->filetypes[left_pos], (folderinfo->numfiletypes - left_pos) * sizeof(struct filetype_info));
	folderinfo->filetypes[left_pos].filetype = (char *) malloc(strlen(filetype) + 1);
	strcpy(folderinfo->filetypes[left_pos].filetype, filetype);
	folderinfo->filetypes[left_pos].extensions = NULL;
	folderinfo->filetypes[left_pos].extensionssize = 0;
	folderinfo->filetypes[left_pos].numextensions = 0;
	folderinfo->filetypes[left_pos].uncompressed_size = 0;
	folderinfo->filetypes[left_pos].uncompressed_size_rounded = 0;
	folderinfo->filetypes[left_pos].compressed_size = 0;
	folderinfo->filetypes[left_pos].compressed_size_rounded = 0;
	folderinfo->filetypes[left_pos].compattr_size = 0;
	folderinfo->filetypes[left_pos].total_size = 0;
	folderinfo->filetypes[left_pos].num_compressed = 0;
	folderinfo->filetypes[left_pos].num_files = 0;
	folderinfo->filetypes[left_pos].num_hard_link_files = 0;
	add_extension_to_filetypeinfo(filepath, &folderinfo->filetypes[left_pos]);
	folderinfo->numfiletypes++;
	return &folderinfo->filetypes[left_pos];
}

void printFileInfo(const char *filepath, struct stat *fileinfo, bool appliedcomp, bool onAPFS)
{
	char *xattrnames, *curr_attr, *filetype;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded, filesize_reported = 0;
	int numxattrs = 0, numhiddenattr = 0;
	UInt32 compressionType = 0;
	bool hasRF = FALSE;
	
	printf("%s:\n", filepath);
	
	xattrnamesize = listxattr(filepath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "malloc error, unable to get file information\n");
			return;
		}
		if ((xattrnamesize = listxattr(filepath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "listxattr: %s\n", strerror(errno));
			free(xattrnames);
			return;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			xattrsize = getxattr(filepath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
			if (xattrsize < 0)
			{
				fprintf(stderr, "getxattr: %s\n", strerror(errno));
				free(xattrnames);
				return;
			}
			numxattrs++;
			if (strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				hasRF = TRUE;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
				if (xattrsize > 0) {
					void *indecmpfsBuf = calloc(xattrsize, 1);
					if (indecmpfsBuf) {
						ssize_t indecmpfsLen = getxattr(filepath, curr_attr, indecmpfsBuf, xattrsize, 
														0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (indecmpfsLen >= 0) {
							if ((*(UInt32 *) indecmpfsBuf) != EndianU32_NtoL(DECMPFS_MAGIC)) {
								fprintf( stderr, "Warning: \"%s\" has unknown compression magic 0x%x\n",
										 filepath, *(UInt32 *) indecmpfsBuf );
							}
							compressionType = EndianU32_LtoN(*(UInt32 *) (indecmpfsBuf + 4));
							filesize_reported = EndianU64_LtoN(*(UInt64 *) (indecmpfsBuf + 8));
						}
						free(indecmpfsBuf);
					}
				}
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}

	if ((int)onAPFS == -1) {
		fileIsCompressable(filepath, fileinfo, &onAPFS);
	}

	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
	{
		if (appliedcomp)
			printf("Unable to compress file.\n");
		else
			printf("File is not HFS+/APFS compressed.\n");
		if ((filetype = getFileType(filepath)) != NULL)
		{
			printf("File content type: %s\n", filetype);
			free(filetype);
		}
		if (hasRF)
		{
			printf("File data fork size: %lld bytes\n", fileinfo->st_size);
			printf("File resource fork size: %ld bytes\n", RFsize);
            if (compattrsize && printVerbose > 2)
                printf("File DECMPFS attribute size: %ld bytes\n", compattrsize);
			filesize = fileinfo->st_size;
			filesize_rounded = roundToBlkSize(filesize, fileinfo);
			filesize += RFsize;
			filesize_rounded = roundToBlkSize(filesize_rounded + RFsize, fileinfo);
			printf("File size (data fork + resource fork; reported size by Mac OS X Finder): %s\n",
				   getSizeStr(filesize, filesize_rounded, 1));
		}
		else
		{
            if (compattrsize && printVerbose > 2)
                printf("File DECMPFS attribute size: %ld bytes\n", compattrsize);
			filesize = fileinfo->st_size;
			filesize_rounded = roundToBlkSize(filesize, fileinfo);
			printf("File data fork size (reported size by Mac OS X Finder): %s\n",
				   getSizeStr(filesize, filesize_rounded, 1));
		}
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		filesize = roundToBlkSize(fileinfo->st_size, fileinfo);
		filesize = roundToBlkSize(filesize + RFsize, fileinfo);
		filesize += compattrsize + xattrssize;
		if (!onAPFS) {
			printf("Approximate overhead of extended HFS+ attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
			filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		}
		printf("Approximate total file size (data fork + resource fork + EA + EA overhead + file overhead): %s\n", 
			   getSizeStr(filesize, filesize, 0));

	}
	else
	{
		if (!appliedcomp)
			printf("File is HFS+/APFS compressed.\n");
		switch (compressionType) {
			case CMP_ZLIB_XATTR:
			case CMP_ZLIB_RESOURCE_FORK:
				printf("Compression type: %s\n", compressionTypeName(compressionType));
				break;
			case CMP_LZVN_XATTR:
			case CMP_LZVN_RESOURCE_FORK:
				printf("Compression type: %s\n", compressionTypeName(compressionType));
				break;
			case CMP_LZFSE_XATTR:
			case CMP_LZFSE_RESOURCE_FORK:
				printf("Compression type: %s\n", compressionTypeName(compressionType));
				break;
			default:
				printf("Unknown compression type: %u\n", compressionType);
				break;
		}
		if ((filetype = getFileType(filepath)) != NULL)
		{
			printf("File content type: %s\n", filetype);
			free(filetype);
		}
		if (printVerbose > 2)
		{
			if (RFsize)
				printf("File resource fork size: %ld bytes\n", RFsize);
	        if (compattrsize)
	            printf("File DECMPFS attribute size: %ld bytes\n", compattrsize);
		}
		filesize = fileinfo->st_size;
		printf("File size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
			   getSizeStr(filesize, filesize, 1));
		if (legacy_output) {
			filesize = RFsize;
			filesize_rounded = roundToBlkSize(filesize, fileinfo);
			printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
				   getSizeStr(filesize, filesize_rounded, 1));
		}
		filesize = RFsize;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		filesize += compattrsize;
		filesize_rounded += compattrsize;
		printf("File size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
		printf("Compression savings: %0.1f%%\n", (1.0 - (((double) RFsize + compattrsize) / fileinfo->st_size)) * 100.0);
		printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
		printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
		filesize = roundToBlkSize(RFsize, fileinfo);
		filesize += compattrsize + xattrssize;
		if (!onAPFS) {
			printf("Approximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
			filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		}
		printf("Approximate total file size (compressed data fork + EA + EA overhead + file overhead): %s\n",
			   getSizeStr(filesize, filesize, 0));
		if (filesize_reported) {
			printf("Uncompressed file size reported in compressed header: %lld bytes\n", filesize_reported);
		}
	}
}

long long process_file(const char *filepath, const char *filetype, struct stat *fileinfo, struct folder_info *folderinfo)
{
	char *xattrnames, *curr_attr;
	const char *fileextension = NULL;
	ssize_t xattrnamesize, xattrssize = 0, xattrsize, RFsize = 0, compattrsize = 0;
	long long int filesize, filesize_rounded, ret;
	int numxattrs = 0, numhiddenattr = 0, i;
	struct filetype_info *filetypeinfo = NULL;
	bool filetype_found = FALSE;
	
	if (quitRequested)
	{
		return 0;
	}

	xattrnamesize = listxattr(filepath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
	
	if (xattrnamesize > 0)
	{
		xattrnames = (char *) malloc(xattrnamesize);
		if (xattrnames == NULL)
		{
			fprintf(stderr, "malloc error, unable to get file information\n");
			return 0;
		}
		if ((xattrnamesize = listxattr(filepath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
		{
			fprintf(stderr, "listxattr: %s\n", strerror(errno));
			free(xattrnames);
			return 0;
		}
		for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
		{
			xattrsize = getxattr(filepath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
			if (xattrsize < 0)
			{
				fprintf(stderr, "getxattr: %s\n", strerror(errno));
				free(xattrnames);
				return 0;
			}
			numxattrs++;
			if (strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22)
			{
				RFsize += xattrsize;
				numhiddenattr++;
			}
			else if (strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)
			{
				compattrsize += xattrsize;
				numhiddenattr++;
			}
			else
				xattrssize += xattrsize;
		}
		free(xattrnames);
	}
	
	folderinfo->num_files++;
	
	if (folderinfo->filetypeslist != NULL && filetype != NULL)
	{
		for (i = strlen(filepath) - 1; i > 0; i--)
			if (filepath[i] == '.' || filepath[i] == '/')
				break;
		if (i != 0 && i != strlen(filepath) - 1 && filepath[i] != '/' && filepath[i-1] != '/')
			fileextension = &filepath[i+1];
		for (i = 0; i < folderinfo->filetypeslistlen; i++)
			if (strcmp(folderinfo->filetypeslist[i], filetype) == 0 ||
				strcmp("ALL", folderinfo->filetypeslist[i]) == 0 ||
				(fileextension != NULL && strcasecmp(fileextension, folderinfo->filetypeslist[i]) == 0))
				filetype_found = TRUE;
	}
	if (folderinfo->filetypeslist != NULL && filetype_found)
		filetypeinfo = getFileTypeInfo(filepath, filetype, folderinfo);
	if (filetype_found && filetypeinfo != NULL) filetypeinfo->num_files++;
	
	if ((fileinfo->st_flags & UF_COMPRESSED) == 0)
	{
		ret = filesize_rounded = filesize = fileinfo->st_size;
		filesize_rounded = roundToBlkSize(filesize_rounded, fileinfo);
		filesize += RFsize;
		filesize_rounded = roundToBlkSize(filesize_rounded + RFsize, fileinfo);
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->uncompressed_size += filesize;
			filetypeinfo->uncompressed_size_rounded += filesize_rounded;
			filetypeinfo->compressed_size += filesize;
			filetypeinfo->compressed_size_rounded += filesize_rounded;
		}
		filesize = roundToBlkSize(fileinfo->st_size, fileinfo);
		filesize = roundToBlkSize(filesize + RFsize, fileinfo);
		filesize += compattrsize + xattrssize;
		if (!folderinfo->onAPFS) {
			filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		}
		folderinfo->total_size += filesize;
		if (filetypeinfo != NULL && filetype_found)
			filetypeinfo->total_size += filesize;
	}
	else
	{
		if (folderinfo->print_files)
		{
			if (folderinfo->print_info > 1)
			{
				printf("%s:\n", filepath);
				if (filetype != NULL)
					printf("File content type: %s\n", filetype);
				filesize = fileinfo->st_size;
				printf("File size (uncompressed data fork; reported size by Mac OS 10.6+ Finder): %s\n",
					   getSizeStr(filesize, filesize, 1));
				if (legacy_output) {
					filesize = RFsize;
					filesize_rounded = roundToBlkSize(filesize, fileinfo);
					printf("File size (compressed data fork - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
						   getSizeStr(filesize, filesize_rounded, 1));
				}
				filesize = RFsize;
				filesize_rounded = roundToBlkSize(filesize, fileinfo);
				filesize += compattrsize;
				filesize_rounded += compattrsize;
				printf("File size (compressed data fork): %s\n", getSizeStr(filesize, filesize_rounded, 0));
				printf("Compression savings: %0.1f%%\n", (1.0 - (((double) RFsize + compattrsize) / fileinfo->st_size)) * 100.0);
				printf("Number of extended attributes: %d\n", numxattrs - numhiddenattr);
				printf("Total size of extended attribute data: %ld bytes\n", xattrssize);
				filesize = roundToBlkSize(RFsize, fileinfo);
				filesize += compattrsize + xattrssize;
				if (!folderinfo->onAPFS) {
					printf("Approximate overhead of extended attributes: %ld bytes\n", ((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey));
					filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
				}
				printf("Approximate total file size (compressed data fork + EA + EA overhead + file overhead): %s\n",
					   getSizeStr(filesize, filesize, 0));
			}
			else if (!folderinfo->compress_files)
			{
				printf("%s\n", filepath);
			}
		}

		filesize = fileinfo->st_size;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		folderinfo->uncompressed_size += filesize;
		folderinfo->uncompressed_size_rounded += filesize_rounded;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->uncompressed_size += filesize;
			filetypeinfo->uncompressed_size_rounded += filesize_rounded;
		}
		ret = filesize = RFsize;
		filesize_rounded = roundToBlkSize(filesize, fileinfo);
		folderinfo->compressed_size += filesize;
		folderinfo->compressed_size_rounded += filesize_rounded;
		folderinfo->compattr_size += compattrsize;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->compressed_size += filesize;
			filetypeinfo->compressed_size_rounded += filesize_rounded;
			filetypeinfo->compattr_size += compattrsize;
		}
		filesize = roundToBlkSize(RFsize, fileinfo);
		filesize += compattrsize + xattrssize;
		if (!folderinfo->onAPFS) {
			filesize += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFile);
		}
		folderinfo->total_size += filesize;
		folderinfo->num_compressed++;
		if (filetypeinfo != NULL && filetype_found)
		{
			filetypeinfo->total_size += filesize;
			filetypeinfo->num_compressed++;
		}
	}
	return ret;
}

void printFolderInfo(struct folder_info *folderinfo, bool hardLinkCheck)
{
	long long foldersize, foldersize_rounded;

	printf("Total number of files: %lld\n", folderinfo->num_files);
	if (hardLinkCheck)
		printf("Total number of file hard links: %lld\n", folderinfo->num_hard_link_files);
	printf("Total number of folders: %lld\n", folderinfo->num_folders);
	if (hardLinkCheck)
		printf("Total number of folder hard links: %lld\n", folderinfo->num_hard_link_folders);
	printf("Total number of items (number of files + number of folders): %lld\n", folderinfo->num_files + folderinfo->num_folders);
	foldersize = folderinfo->uncompressed_size;
	foldersize_rounded = folderinfo->uncompressed_size_rounded;
	if ((folderinfo->num_hard_link_files == 0 && folderinfo->num_hard_link_folders == 0) || !hardLinkCheck)
		printf("Folder size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 1));
	else
		printf("Folder size (uncompressed): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 0));
	foldersize = folderinfo->compressed_size;
	foldersize_rounded = folderinfo->compressed_size_rounded;
	if (legacy_output
			&& ((folderinfo->num_hard_link_files == 0 && folderinfo->num_hard_link_folders == 0) || !hardLinkCheck)) {
		printf("Folder size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
			   getSizeStr(foldersize, foldersize_rounded, 1));
	} else {
		printf("Folder size (compressed - decmpfs xattr): %s\n", getSizeStr(foldersize, foldersize_rounded, 0));
	}
	foldersize = folderinfo->compressed_size + folderinfo->compattr_size;
	foldersize_rounded = folderinfo->compressed_size_rounded + folderinfo->compattr_size;
	printf("Folder size (compressed): %s\n", getSizeStr(foldersize, foldersize_rounded, 0));
	printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (folderinfo->compressed_size + folderinfo->compattr_size) / folderinfo->uncompressed_size)) * 100.0);
	foldersize = folderinfo->total_size;
	printf("Approximate total folder size (files + file overhead + folder overhead): %s\n",
		   getSizeStr(foldersize, foldersize, 0));
}

void process_folder(FTS *currfolder, struct folder_info *folderinfo)
{
	FTSENT *currfile;
	char *xattrnames, *curr_attr, *filetype = NULL, *fileextension;
	ssize_t xattrnamesize, xattrssize, xattrsize;
	int numxattrs, i;
	bool volume_search, filetype_found;
	struct filetype_info *filetypeinfo = NULL;
	
	currfile = fts_read(currfolder);
	if (currfile == NULL)
	{
		fts_close(currfolder);
		return;
	}
	volume_search = (strncasecmp("/Volumes/", currfile->fts_path, 9) == 0 && strlen(currfile->fts_path) >= 8);
	
	do
	{
		if (!quitRequested
			&& (volume_search || strncasecmp("/Volumes/", currfile->fts_path, 9) != 0 || strlen(currfile->fts_path) < 9)
			&& (strncasecmp("/dev/", currfile->fts_path, 5) != 0 || strlen(currfile->fts_path) < 5))
		{
			if (S_ISDIR(currfile->fts_statp->st_mode) && currfile->fts_ino != 2)
			{
				if (currfile->fts_info & FTS_D)
				{
					if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo))
					{
						numxattrs = 0;
						xattrssize = 0;
						
						xattrnamesize = listxattr(currfile->fts_path, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						
						if (xattrnamesize > 0)
						{
							xattrnames = (char *) malloc(xattrnamesize);
							if (xattrnames == NULL)
							{
								fprintf(stderr, "malloc error, unable to get folder information\n");
								continue;
							}
							if ((xattrnamesize = listxattr(currfile->fts_path, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
							{
								fprintf(stderr, "listxattr: %s\n", strerror(errno));
								free(xattrnames);
								continue;
							}
							for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
							{
								xattrsize = getxattr(currfile->fts_path, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
								if (xattrsize < 0)
								{
									fprintf(stderr, "getxattr: %s\n", strerror(errno));
									free(xattrnames);
									continue;
								}
								numxattrs++;
								xattrssize += xattrsize;
							}
							free(xattrnames);
						}
						folderinfo->num_folders++;
						folderinfo->total_size += xattrssize;
						if (!folderinfo->onAPFS) {
							folderinfo->total_size += (((ssize_t) numxattrs) * sizeof(HFSPlusAttrKey)) + sizeof(HFSPlusCatalogFolder);
						}
					}
					else
					{
						folderinfo->num_hard_link_folders++;
						fts_set(currfolder, currfile, FTS_SKIP);
						
						folderinfo->num_folders++;
						if (!folderinfo->onAPFS) {
							folderinfo->total_size += sizeof(HFSPlusCatalogFolder);
						}
					}
				}
			}
			else if (S_ISREG(currfile->fts_statp->st_mode) || S_ISLNK(currfile->fts_statp->st_mode))
			{
				filetype_found = FALSE;
				if (folderinfo->filetypeslist != NULL)
				{
					filetype = getFileType(currfile->fts_path);
					if (filetype == NULL)
					{
						filetype = (char *) malloc(10);
						strcpy(filetype, "UNDEFINED");
					}
				}
				if (filetype != NULL)
				{
					fileextension = NULL;
					for (i = strlen(currfile->fts_path) - 1; i > 0; i--)
						if (currfile->fts_path[i] == '.')
							break;
					if (i != 0 && i != strlen(currfile->fts_path) - 1 && currfile->fts_path[i] != '/' && currfile->fts_path[i-1] != '/')
						fileextension = &currfile->fts_path[i+1];
					for (i = 0; i < folderinfo->filetypeslistlen; i++)
						if (strcmp(folderinfo->filetypeslist[i], filetype) == 0 ||
							strcmp("ALL", folderinfo->filetypeslist[i]) == 0 ||
							(fileextension != NULL && strcasecmp(fileextension, folderinfo->filetypeslist[i]) == 0))
							filetype_found = TRUE;
					if (folderinfo->invert_filetypelist)
					{
						if (filetype_found)
							filetype_found = FALSE;
						else
							filetype_found = TRUE;
					}
				}
				
				if (!folderinfo->check_hard_links || !checkForHardLink(currfile->fts_path, currfile->fts_statp, folderinfo))
				{
					if (folderinfo->compress_files && S_ISREG(currfile->fts_statp->st_mode))
					{
						if (folderinfo->filetypeslist == NULL || filetype_found)
						{
#ifdef SUPPORT_PARALLEL
							if (PP)
							{
								if (fileIsCompressable(currfile->fts_path, currfile->fts_statp, &folderinfo->onAPFS))
									addFileToParallelProcessor( PP, currfile->fts_path, currfile->fts_statp, folderinfo, false );
								else
									process_file(currfile->fts_path, NULL, currfile->fts_statp, getParallelProcessorJobInfo(PP));
							}
							else
#endif
							{
								compressFile(currfile->fts_path, currfile->fts_statp, folderinfo, NULL);
							}
						}
						lstat(currfile->fts_path, currfile->fts_statp);
						if (((currfile->fts_statp->st_flags & UF_COMPRESSED) == 0) && folderinfo->print_files)
						{
							if (folderinfo->print_info > 0)
								printf("Unable to compress: ");
							printf("%s\n", currfile->fts_path);
						}
					}
					process_file(currfile->fts_path, filetype, currfile->fts_statp, folderinfo);
				}
				else
				{
					folderinfo->num_hard_link_files++;
					
					folderinfo->num_files++;
					if (!folderinfo->onAPFS) {
						folderinfo->total_size += sizeof(HFSPlusCatalogFile);
					}
					if (filetype_found && (filetypeinfo = getFileTypeInfo(currfile->fts_path, filetype, folderinfo)) != NULL)
					{
						filetypeinfo->num_hard_link_files++;
						
						filetypeinfo->num_files++;
						if (!folderinfo->onAPFS) {
							filetypeinfo->total_size += sizeof(HFSPlusCatalogFile);
						}
					}
				}
				if (filetype != NULL) free(filetype);
			}
		}
		else
			fts_set(currfolder, currfile, FTS_SKIP);
	} while (!quitRequested && (currfile = fts_read(currfolder)) != NULL);
	checkForHardLink(NULL, NULL, NULL);
	fts_close(currfolder);
}

void printUsage()
{
	printf("afsctool %s\n"
		   "Report if file is HFS+/APFS compressed:                   afsctool [-v] file[s]\n"
		   "Report if folder contains HFS+/APFS compressed files:     afsctool [-fvv[v]i] [-t <ContentType/Extension>] folder[s]\n"
		   "List HFS+/APFS compressed files in folder:                afsctool -l[fvv[v]] folder\n"
		   "Decompress HFS+/APFS compressed file or folder:           afsctool -d[i] [-t <ContentType>] file[s]/folder[s]\n"
		   "\tNB: this removes the entire resource fork from the specified files!\n"
		   "Create archive file with compressed data in data fork:    afsctool -a[d] src dst [... srcN dstN]\n"
		   "Extract HFS+/APFS compression archive to file:            afsctool -x[d] src dst [... srcN dstN]\n"
#ifdef SUPPORT_PARALLEL
		   "Apply HFS+/APFS compression to file or folder:            afsctool -c[nlfvv[v]ib] [-jN|-JN] [-S [-RM] ] [-<level>] [-m <size>] [-s <percentage>] [-t <ContentType>] [-T compressor] file[s]/folder[s]\n\n"
#else
		   "Apply HFS+/APFS compression to file or folder:            afsctool -c[nlfvv[v]ib] [-<level>] [-m <size>] [-s <percentage>] [-t <ContentType>] [-T compressor] file[s]/folder[s]\n\n"
#endif
		   "Options:\n"
		   "-v Increase verbosity level\n"
		   "-f Detect hard links\n"
		   "-l List files that are HFS+/APFS compressed (or if the -c option is given, files which fail to compress)\n"
		   "-L Allow large compressed blocks (not recommended; always true for LZVN compression)\n"
		   "-n Do not verify files after compression (not recommended)\n"
		   "-m <size> Largest file size to compress, in bytes\n"
		   "-s <percentage> For compression to be applied, compression savings must be at least this percentage\n"
		   "-t <ContentType/Extension> Return statistics for files of given content type and when compressing,\n"
		   "                           if this option is given then only files of content type(s) or extension(s) specified with this option will be compressed\n"
		   "-i Compress or show statistics for files that don't have content type(s) or extension(s) given by -t <ContentType/Extension> instead of those that do\n"
		   "-b make a backup of files before compressing them\n"
#ifdef SUPPORT_PARALLEL
		   "-jN compress (only compressable) files using <N> threads (compression is concurrent, disk IO is exclusive)\n"
		   "-JN read, compress and write files (only compressable ones) using <N> threads (everything is concurrent except writing the compressed file)\n"
		   "-S sort the item list by file size (leaving the largest files to the end may be beneficial if the target volume is almost full)\n"
		   "-RM <M> of the <N> workers will work the item list (must be sorted!) in reverse order, starting with the largest files\n"
#endif
		   "-T <compressor> Compression type to use: ZLIB (= types 3,4) or LZVN (= types 7,8)\n"
		   "-<level> Compression level to use when compressing (ZLIB only; ranging from 1 to 9, with 1 being the fastest and 9 being the best - default is 5)\n"
		  , AFSCTOOL_FULL_VERSION_STRING);
}

#ifndef SUPPORT_PARALLEL
int main (int argc, const char * argv[])
#else
int afsctool (int argc, const char * argv[])
#endif
{
	int i, j;
	struct stat fileinfo, dstfileinfo;
	struct folder_info folderinfo;
	struct filetype_info alltypesinfo;
	FTS *currfolder;
	FTSENT *currfile;
	char *folderarray[2], *fullpath = NULL, *fullpathdst = NULL, *cwd, *fileextension, *filetype = NULL;
	int compressionlevel = 5;
	compression_type compressiontype = ZLIB;
	double minSavings = 0.0;
	long long int filesize, filesize_rounded, maxSize = 0;
	bool printDir = FALSE, decomp = FALSE, createfile = FALSE, extractfile = FALSE, applycomp = FALSE,
		fileCheck = TRUE, argIsFile, hardLinkCheck = FALSE, dstIsFile, free_src = FALSE, free_dst = FALSE,
		invert_filetypelist = FALSE, allowLargeBlocks = FALSE, filetype_found, backupFile = FALSE;
	FILE *afscFile, *outFile;
	char *xattrnames, *curr_attr, header[4];
	ssize_t xattrnamesize, xattrsize, getxattrret, xattrPos;
	mode_t outFileMode;
	void *attr_buf;
	UInt16 big16;
	UInt64 big64;
	int nJobs = 0, nReverse = 0;
	bool sortQueue = false;

	folderinfo.filetypeslist = NULL;
	folderinfo.filetypeslistlen = 0;
	folderinfo.filetypeslistsize = 0;
	
	if (argc < 2)
	{
		printUsage();
		exit(EINVAL);
	}

	for (i = 1; i < argc && argv[i][0] == '-'; i++)
	{
		for (j = 1; j < strlen(argv[i]); j++)
		{
			switch (argv[i][j])
			{
				case 'l':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					printDir = TRUE;
					break;
				case 'L':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					allowLargeBlocks = TRUE;
					break;
				case 'v':
					printVerbose++;
					break;
				case 'd':
					if (printDir || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					decomp = TRUE;
					break;
				case 'a':
					if (printDir || extractfile || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					createfile = TRUE;
					break;
				case 'x':
					if (printDir || createfile || applycomp || hardLinkCheck)
					{
						printUsage();
						exit(EINVAL);
					}
					extractfile = TRUE;
					break;
				case 'c':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					applycomp = TRUE;
					break;
				case 'k':
					// this flag is obsolete and no longer does anything, but it is kept here for backward compatibility with scripts that use it
					break;
				case 'n':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					fileCheck = FALSE;
					break;
				case 'f':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					hardLinkCheck = TRUE;
					break;
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					if (createfile || extractfile || decomp)
					{
						printUsage();
						exit(EINVAL);
					}
					compressionlevel = argv[i][j] - '0';
					break;
				case 'm':
					if (createfile || extractfile || decomp || j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					sscanf(argv[i], "%lld", &maxSize);
					j = strlen(argv[i]) - 1;
					break;
				case 's':
					if (createfile || extractfile || decomp || j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					sscanf(argv[i], "%lf", &minSavings);
					if (minSavings > 99 || minSavings < 0)
					{
						fprintf(stderr, "Invalid minimum savings percentage; must be a number from 0 to 99\n");
						exit(EINVAL);
					}
					j = strlen(argv[i]) - 1;
					break;
				case 't':
					if (j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					if (folderinfo.filetypeslist == NULL)
					{
						folderinfo.filetypeslistsize = 1;
						folderinfo.filetypeslist = (char **) malloc(folderinfo.filetypeslistlen * sizeof(char *));
					}
					if (folderinfo.filetypeslistlen + 1 > folderinfo.filetypeslistsize)
					{
						folderinfo.filetypeslistsize *= 2;
						folderinfo.filetypeslist = (char **) realloc(folderinfo.filetypeslist, folderinfo.filetypeslistsize * sizeof(char *));
					}
					if (folderinfo.filetypeslist == NULL)
					{
						fprintf(stderr, "malloc error, out of memory\n");
						return ENOMEM;
					}
					folderinfo.filetypeslist[folderinfo.filetypeslistlen++] = (char *) argv[i];
					j = strlen(argv[i]) - 1;
					break;
				case 'T':
					if (j + 1 < strlen(argv[i]) || i + 2 > argc)
					{
						printUsage();
						exit(EINVAL);
					}
					i++;
					if (strcasecmp(argv[i], "zlib") == 0) {
						compressiontype = ZLIB;
					} else if (strcasecmp(argv[i], "lzvn") == 0) {
						compressiontype = LZVN;
					} else {
						fprintf(stderr, "Unsupported or unknown HFS compression requested (%s)\n", argv[i]);
						printUsage();
						exit(EINVAL);
					}
					j = strlen(argv[i]) - 1;
					break;
				case 'i':
					if (createfile || extractfile)
					{
						printUsage();
						exit(EINVAL);
					}
					invert_filetypelist = TRUE;
					break;
				case 'b':
					if (!applycomp)
					{
						printUsage();
						exit(EINVAL);
					}
					folderinfo.backup_file = backupFile = TRUE;
					break;
#ifdef SUPPORT_PARALLEL
				case 'j':
				case 'J':
				case 'R':
					if (!applycomp)
					{
						printUsage();
						exit(EINVAL);
					}
					if (argv[i][j] == 'J')
					{
						exclusive_io = false;
					}
					if (argv[i][j] == 'R')
					{
						nReverse = atoi(&argv[i][j+1]);
						if (nReverse <= 0)
						{
							fprintf( stderr, "Warning: reverse jobs must be a positive number (%s)\n", argv[i] );
							nReverse = 0;
						}
					}
					else
					{
						nJobs = atoi(&argv[i][j+1]);
						if (nJobs <= 0)
						{
							fprintf( stderr, "Warning: jobs must be a positive number (%s)\n", argv[i] );
							nJobs = 0;
						}
					}
					goto next_arg;
					break;
				case 'S':
					if (!applycomp)
					{
						printUsage();
						exit(EINVAL);
					}
					sortQueue = true;
					goto next_arg;
					break;
#endif
				default:
					printUsage();
					exit(EINVAL);
					break;
			}
		}
next_arg:;
	}
	
	if (i == argc || ((createfile || extractfile) && (argc - i < 2)))
	{
		printUsage();
		exit(EINVAL);
	}

#ifdef SUPPORT_PARALLEL
	if (nJobs > 0)
	{
		if (nReverse && !sortQueue)
		{
			fprintf( stderr, "Warning: reverse jobs are ignored when the item list is not sorted (-S)\n" );
			nReverse = 0;
		}
		PP = createParallelProcessor(nJobs, nReverse, printVerbose);
//		if (PP)
//		{
//			if (printVerbose)
//			{
//				printf( "Verbose mode switched off in parallel processing mode\n");
//			}
//			printVerbose = false;
//		}
	}
#endif

	// ignore signals due to exceeding CPU or file size limits
	signal(SIGXCPU, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);

	int N, step, n;
	if (createfile || extractfile)
	{
		N = argc - 1;
		step = 2;
	}
	else
	{
		N = argc;
		step = 1;
	}
	for ( n = 0 ; i < N ; i += step, ++n )
	{
		if (n && printVerbose > 0 && !nJobs)
		{
			printf("\n");
		}
		if (createfile || extractfile)
		{
			if (argv[i+1][0] != '/')
			{
				cwd = getcwd(NULL, 0);
				if (cwd == NULL)
				{
					fprintf(stderr, "Unable to get PWD, exiting...\n");
					exit(EACCES);
				}
				free_dst = TRUE;
				fullpathdst = (char *) malloc(strlen(cwd) + strlen(argv[i+1]) + 2);
				sprintf(fullpathdst, "%s/%s", cwd, argv[i+1]);
				free(cwd);
			}
			else
				fullpathdst = (char *) argv[i+1];
		}
		
		if (argv[i][0] != '/')
		{
			cwd = getcwd(NULL, 0);
			if (cwd == NULL)
			{
				fprintf(stderr, "Unable to get PWD, exiting...\n");
				exit(EACCES);
			}
			free_src = TRUE;
			fullpath = (char *) malloc(strlen(cwd) + strlen(argv[i]) + 2);
			sprintf(fullpath, "%s/%s", cwd, argv[i]);
			free(cwd);
		}
		else
		{
			free_src = FALSE;
			fullpath = (char *) argv[i];
		}
		
		if (lstat(fullpath, &fileinfo) < 0)
		{
			fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
			continue;
		}
		
		argIsFile = ((fileinfo.st_mode & S_IFDIR) == 0);
		
		if (!argIsFile)
		{
			folderarray[0] = fullpath;
			folderarray[1] = NULL;
		}
		
		if ((createfile || extractfile) && lstat(fullpathdst, &dstfileinfo) >= 0)
		{
			dstIsFile = ((dstfileinfo.st_mode & S_IFDIR) == 0);
			fprintf(stderr, "%s: %s already exists at this path\n", fullpath, dstIsFile ? "File" : "Folder");
			continue;
		}
		
		if (applycomp && argIsFile)
		{
			struct folder_info fi;
			fi.maxSize = maxSize;
			fi.compressionlevel = compressionlevel;
			fi.compressiontype = compressiontype;
			fi.allowLargeBlocks = allowLargeBlocks;
			fi.minSavings = minSavings;
			fi.check_files = fileCheck;
			fi.backup_file = backupFile;
#ifdef SUPPORT_PARALLEL
			if (PP)
			{
				if (fileIsCompressable(fullpath, &fileinfo, &fi.onAPFS))
				{
					addFileToParallelProcessor( PP, fullpath, &fileinfo, &fi, true );
				}
				else
				{
					process_file(fullpath, NULL, &fileinfo, getParallelProcessorJobInfo(PP));
				}
			}
			else
#endif
			{
				compressFile(fullpath, &fileinfo, &fi, NULL);
			}
			lstat(fullpath, &fileinfo);
		}
		
		if (createfile)
		{
			if (!argIsFile)
			{
				fprintf(stderr, "%s: File required, this is a folder\n", fullpath);
				return -1;
			}
			else if ((fileinfo.st_flags & UF_COMPRESSED) == 0)
			{
				fprintf(stderr, "%s: HFS+/APFS compressed file required, this file is not\n", fullpath);
				return -1;
			}
			
			afscFile = fopen(fullpathdst, "w");
			if (afscFile == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
				continue;
			}
			else
			{
				fprintf(afscFile, "afsc");
				big16 = EndianU16_NtoB(fileinfo.st_mode);
				if (fwrite(&big16, sizeof(mode_t), 1, afscFile) != 1)
				{
					fprintf(stderr, "%s: Error writing file\n", fullpathdst);
					return -1;
				}
				xattrnamesize = listxattr(fullpath, NULL, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
				
				if (xattrnamesize > 0)
				{
					xattrnames = (char *) malloc(xattrnamesize);
					if (xattrnames == NULL)
					{
						fprintf(stderr, "malloc error, unable to get file information\n");
						return -1;
					}
					if ((xattrnamesize = listxattr(fullpath, xattrnames, xattrnamesize, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW)) <= 0)
					{
						fprintf(stderr, "listxattr: %s\n", strerror(errno));
						free(xattrnames);
						return -1;
					}
					for (curr_attr = xattrnames; curr_attr < xattrnames + xattrnamesize; curr_attr += strlen(curr_attr) + 1)
					{
						xattrsize = getxattr(fullpath, curr_attr, NULL, 0, 0, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
						if (xattrsize < 0)
						{
							fprintf(stderr, "getxattr: %s\n", strerror(errno));
							free(xattrnames);
							return -1;
						}
						if (((strcmp(curr_attr, XATTR_RESOURCEFORK_NAME) == 0 && strlen(curr_attr) == 22) ||
							(strcmp(curr_attr, DECMPFS_XATTR_NAME) == 0 && strlen(curr_attr) == 17)) &&
							xattrsize != 0)
						{
							attr_buf = malloc(xattrsize);
							if (attr_buf == NULL)
							{
								fprintf(stderr, "malloc error, unable to get file information\n");
								return -1;
							}
							xattrPos = 0;
							do
							{
								getxattrret = getxattr(fullpath, curr_attr, attr_buf + xattrPos, xattrsize - xattrPos, xattrPos, XATTR_SHOWCOMPRESSION | XATTR_NOFOLLOW);
								if (getxattrret < 0)
								{
									fprintf(stderr, "getxattr: %s\n", strerror(errno));
									free(xattrnames);
									return -1;
								}
								xattrPos += getxattrret;
							} while (xattrPos < xattrsize && getxattrret > 0);
							fprintf(afscFile, "%s", curr_attr);
							putc('\0', afscFile);
							big64 = EndianU64_NtoB(xattrsize);
							if (fwrite(&big64, sizeof(ssize_t), 1, afscFile) != 1 ||
								fwrite(attr_buf, xattrsize, 1, afscFile) != 1)
							{
								fprintf(stderr, "%s: Error writing file\n", fullpathdst);
								return -1;
							}
							free(attr_buf);
						}
					}
					free(xattrnames);
				}
				fclose(afscFile);
				if (decomp) {
					if (printVerbose > 1) {
						printFileInfo(fullpath, &fileinfo, false, (bool)-1);
					}
					decompressFile(fullpath, &fileinfo, backupFile);
				}
			}
		}
		else if (extractfile)
		{
			if (!argIsFile)
			{
				fprintf(stderr, "%s: File required, this is a folder\n", fullpath);
				return -1;
			}
			afscFile = fopen(fullpath, "r");
			if (afscFile == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
				continue;
			}
			else
			{
				if (fread(header, 4, 1, afscFile) != 1)
				{
					fprintf(stderr, "%s: Error reading file\n", fullpath);
					return -1;
				}
				if (header[0] != 'a' ||
					header[1] != 'f' ||
					header[2] != 's' ||
					header[3] != 'c')
				{
					fprintf(stderr, "%s: Invalid header\n", fullpath);
					return -1;
				}
				outFile = fopen(fullpathdst, "w");
				if (outFile == NULL)
				{
					fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
					continue;
				}
				else
				{
					fclose(outFile);
					if (fread(&outFileMode, sizeof(mode_t), 1, afscFile) != 1)
					{
						fprintf(stderr, "%s: Error reading file\n", fullpath);
						return -1;
					}
					outFileMode = EndianU16_BtoN(outFileMode);
					xattrnames = (char *) malloc(23);
					while (1)
					{
						for (j = 0; j < 23; j++)
						{
							xattrnames[j] = getc(afscFile);
							if (xattrnames[j] == '\0') break;
							if (xattrnames[j] == EOF) goto decomp_check;
						}
						if (j == 23 ||
							fread(&xattrsize, sizeof(ssize_t), 1, afscFile) != 1)
						{
							fprintf(stderr, "%s: Error reading file\n", fullpath);
							return -1;
						}
						xattrsize = EndianU64_BtoN(xattrsize);
						attr_buf = malloc(xattrsize);
						if (attr_buf == NULL)
						{
							fprintf(stderr, "malloc error, unable to set file information\n");
							return -1;
						}
						if (fread(attr_buf, xattrsize, 1, afscFile) != 1)
						{
							fprintf(stderr, "%s: Error reading file\n", fullpath);
							return -1;
						}
						if (setxattr(fullpathdst, xattrnames, attr_buf, xattrsize, 0, XATTR_NOFOLLOW | XATTR_CREATE) < 0)
						{
							fprintf(stderr, "setxattr: %s\n", strerror(errno));
							return -1;
						}
						free(attr_buf);
					}
					fprintf(stderr, "%s: Error reading file\n", fullpath);
					return -1;
				decomp_check:
					if (chflags(fullpathdst, UF_COMPRESSED) < 0)
					{
						fprintf(stderr, "chflags: %s\n", strerror(errno));
						return -1;
					}
					if (chmod(fullpathdst, outFileMode) < 0)
					{
						fprintf(stderr, "chmod: %s\n", strerror(errno));
						return -1;
					}
					if (decomp)
					{
						if (lstat(fullpathdst, &dstfileinfo) < 0)
						{
							fprintf(stderr, "%s: %s\n", fullpathdst, strerror(errno));
							continue;
						}
						if (printVerbose > 1) {
							printFileInfo(fullpathdst, &dstfileinfo, false, (bool)-1);
						}
						decompressFile(fullpathdst, &dstfileinfo, backupFile);
					}
				}
			}
		}
		else if (decomp && argIsFile)
		{
			if (printVerbose > 1) {
				printFileInfo(fullpath, &fileinfo, false, (bool)-1);
			}
			decompressFile(fullpath, &fileinfo, backupFile);
		}
		else if (decomp)
		{
			if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
//				exit(EACCES);
				continue;
			}
			while ((currfile = fts_read(currfolder)) != NULL)
			{
				filetype_found = FALSE;
				if (folderinfo.filetypeslist != NULL)
				{
					filetype = getFileType(currfile->fts_path);
					if (filetype == NULL)
					{
						filetype = (char *) malloc(10);
						strcpy(filetype, "UNDEFINED");
					}
				}
				if (filetype != NULL)
				{
					fileextension = NULL;
					for (i = strlen(currfile->fts_path) - 1; i > 0; i--)
						if (currfile->fts_path[i] == '.')
							break;
					if (i != 0 && i != strlen(currfile->fts_path) - 1 && currfile->fts_path[i] != '/' && currfile->fts_path[i-1] != '/')
						fileextension = &currfile->fts_path[i+1];
					for (i = 0; i < folderinfo.filetypeslistlen; i++)
						if (strcmp(folderinfo.filetypeslist[i], filetype) == 0 ||
							strcmp("ALL", folderinfo.filetypeslist[i]) == 0 ||
							(fileextension != NULL && strcasecmp(fileextension, folderinfo.filetypeslist[i]) == 0))
							filetype_found = TRUE;
					if (invert_filetypelist)
					{
						if (filetype_found)
							filetype_found = FALSE;
						else
							filetype_found = TRUE;
					}
				}
				if ((currfile->fts_statp->st_mode & S_IFDIR) == 0 && (folderinfo.filetypeslist == NULL || filetype_found)) {
					if (printVerbose > 1) {
						printFileInfo(currfile->fts_path, currfile->fts_statp, false, (bool)-1);
					}
					decompressFile(currfile->fts_path, currfile->fts_statp, backupFile);
				}
				if (filetype != NULL) free(filetype);
			}
			fts_close(currfolder);
		}
		else if (argIsFile && printVerbose == 0)
		{
			if (applycomp)
			{
				if ((fileinfo.st_flags & UF_COMPRESSED) == 0)
					printf("Unable to compress file.\n");
			}
			else
			{
				if ((fileinfo.st_flags & UF_COMPRESSED) != 0)
					printf("File is HFS+/APFS compressed.\n");
				else
					printf("File is not HFS+/APFS compressed.\n");
			}
		}
		else if (argIsFile && printVerbose > 0)
		{
			bool onAPFS;
			fileIsCompressable(fullpath, &fileinfo, &onAPFS);
			printFileInfo(fullpath, &fileinfo, applycomp, onAPFS);
		}
		else if (!argIsFile)
		{
			if ((currfolder = fts_open(folderarray, FTS_PHYSICAL, NULL)) == NULL)
			{
				fprintf(stderr, "%s: %s\n", fullpath, strerror(errno));
//				exit(EACCES);
				continue;
			}
			folderinfo.uncompressed_size = 0;
			folderinfo.uncompressed_size_rounded = 0;
			folderinfo.compressed_size = 0;
			folderinfo.compressed_size_rounded = 0;
			folderinfo.compattr_size = 0;
			folderinfo.total_size = 0;
			folderinfo.num_compressed = 0;
			folderinfo.num_files = 0;
			folderinfo.num_hard_link_files = 0;
			folderinfo.num_folders = 0;
			folderinfo.num_hard_link_folders = 0;
			folderinfo.print_info = (nJobs)? false : printVerbose;
			folderinfo.print_files = (nJobs == 0)? printDir : 0;
			folderinfo.compress_files = applycomp;
			folderinfo.check_files = fileCheck;
			folderinfo.allowLargeBlocks = allowLargeBlocks;
			folderinfo.compressionlevel = compressionlevel;
			folderinfo.compressiontype = compressiontype;
			folderinfo.minSavings = minSavings;
			folderinfo.maxSize = maxSize;
			folderinfo.check_hard_links = hardLinkCheck;
			folderinfo.filetypes = NULL;
			folderinfo.numfiletypes = 0;
			folderinfo.filetypessize = 0;
			folderinfo.invert_filetypelist = invert_filetypelist;
			process_folder(currfolder, &folderinfo);
			folderinfo.num_folders--;
			if (printVerbose > 0 || !printDir)
			{
				if (!nJobs)
				{
					if (printDir) printf("\n");
					printf("%s:\n", fullpath);
				}
				else
				{
					printf("Adding %s to queue\n", fullpath);
				}
				if (folderinfo.filetypes != NULL)
				{
					alltypesinfo.filetype = NULL;
					alltypesinfo.uncompressed_size = 0;
					alltypesinfo.uncompressed_size_rounded = 0;
					alltypesinfo.compressed_size = 0;
					alltypesinfo.compressed_size_rounded = 0;
					alltypesinfo.compattr_size = 0;
					alltypesinfo.total_size = 0;
					alltypesinfo.num_compressed = 0;
					alltypesinfo.num_files = 0;
					alltypesinfo.num_hard_link_files = 0;
					for (i = 0; i < folderinfo.numfiletypes; i++)
					{
						alltypesinfo.uncompressed_size += folderinfo.filetypes[i].uncompressed_size;
						alltypesinfo.uncompressed_size_rounded += folderinfo.filetypes[i].uncompressed_size_rounded;
						alltypesinfo.compressed_size += folderinfo.filetypes[i].compressed_size;
						alltypesinfo.compressed_size_rounded += folderinfo.filetypes[i].compressed_size_rounded;
						alltypesinfo.compattr_size += folderinfo.filetypes[i].compattr_size;
						alltypesinfo.total_size += folderinfo.filetypes[i].total_size;
						alltypesinfo.num_compressed += folderinfo.filetypes[i].num_compressed;
						alltypesinfo.num_files += folderinfo.filetypes[i].num_files;
						alltypesinfo.num_hard_link_files += folderinfo.filetypes[i].num_hard_link_files;
						
						if (!folderinfo.invert_filetypelist)
							printf("\nFile content type: %s\n", folderinfo.filetypes[i].filetype);
						if (folderinfo.filetypes[i].numextensions > 0)
						{
							if (!folderinfo.invert_filetypelist)
								printf ("File extension(s): %s", folderinfo.filetypes[i].extensions[0]);
							free(folderinfo.filetypes[i].extensions[0]);
							for (j = 1; j < folderinfo.filetypes[i].numextensions; j++)
							{
								if (!folderinfo.invert_filetypelist)
									printf (", %s", folderinfo.filetypes[i].extensions[j]);
								free(folderinfo.filetypes[i].extensions[j]);
							}
							free(folderinfo.filetypes[i].extensions);
							if (!folderinfo.invert_filetypelist)
								printf("\n");
						}
						if (!folderinfo.invert_filetypelist)
							printf("Number of HFS+/APFS compressed files: %lld\n", folderinfo.filetypes[i].num_compressed);
						if (printVerbose > 0 && nJobs == 0 && (!folderinfo.invert_filetypelist))
						{
							printf("Total number of files: %lld\n", folderinfo.filetypes[i].num_files);
							if (hardLinkCheck)
								printf("Total number of file hard links: %lld\n", folderinfo.filetypes[i].num_hard_link_files);
							filesize = folderinfo.filetypes[i].uncompressed_size;
							filesize_rounded = folderinfo.filetypes[i].uncompressed_size_rounded;
							if (folderinfo.filetypes[i].num_hard_link_files == 0 || !hardLinkCheck)
								printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							else
								printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							filesize = folderinfo.filetypes[i].compressed_size;
							filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded;
							if (legacy_output
									&& (folderinfo.filetypes[i].num_hard_link_files == 0 || !hardLinkCheck)) {
								printf("File(s) size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							} else {
								printf("File(s) size (compressed - decmpfs xattr): %s\n",
									   getSizeStr(filesize, filesize_rounded, 0));
							}
							filesize = folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size;
							filesize_rounded = folderinfo.filetypes[i].compressed_size_rounded + folderinfo.filetypes[i].compattr_size;
							printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (folderinfo.filetypes[i].compressed_size + folderinfo.filetypes[i].compattr_size) / folderinfo.filetypes[i].uncompressed_size)) * 100.0);
							filesize = folderinfo.filetypes[i].total_size;
							printf("Approximate total file(s) size (files + file overhead): %s\n",
								   getSizeStr(filesize, filesize, 0));
						}
						free(folderinfo.filetypes[i].filetype);
					}
					if (folderinfo.invert_filetypelist)
					{
						alltypesinfo.uncompressed_size = folderinfo.uncompressed_size - alltypesinfo.uncompressed_size;
						alltypesinfo.uncompressed_size_rounded = folderinfo.uncompressed_size_rounded - alltypesinfo.uncompressed_size_rounded;
						alltypesinfo.compressed_size = folderinfo.compressed_size - alltypesinfo.compressed_size;
						alltypesinfo.compressed_size_rounded = folderinfo.compressed_size_rounded - alltypesinfo.compressed_size_rounded;
						alltypesinfo.compattr_size = folderinfo.compattr_size - alltypesinfo.compattr_size;
						alltypesinfo.total_size = folderinfo.total_size - alltypesinfo.total_size;
						alltypesinfo.num_compressed = folderinfo.num_compressed - alltypesinfo.num_compressed;
						alltypesinfo.num_files = folderinfo.num_files - alltypesinfo.num_files;
						alltypesinfo.num_hard_link_files = folderinfo.num_hard_link_files - alltypesinfo.num_hard_link_files;
					}
					if (folderinfo.numfiletypes > 1 || folderinfo.invert_filetypelist)
					{
						printf("\nTotals of file content types\n");
						printf("Number of HFS+/APFS compressed files: %lld\n", alltypesinfo.num_compressed);
						if (printVerbose > 0 && nJobs == 0)
						{
							printf("Total number of files: %lld\n", alltypesinfo.num_files);
							if (hardLinkCheck)
								printf("Total number of file hard links: %lld\n", alltypesinfo.num_hard_link_files);
							filesize = alltypesinfo.uncompressed_size;
							filesize_rounded = alltypesinfo.uncompressed_size_rounded;
							if (alltypesinfo.num_hard_link_files == 0 || !hardLinkCheck)
								printf("File(s) size (uncompressed; reported size by Mac OS 10.6+ Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							else
								printf("File(s) size (uncompressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							filesize = alltypesinfo.compressed_size;
							filesize_rounded = alltypesinfo.compressed_size_rounded;
							if (legacy_output
									 && (alltypesinfo.num_hard_link_files == 0 || !hardLinkCheck)) {
								printf("File(s) size (compressed - decmpfs xattr; reported size by Mac OS 10.0-10.5 Finder): %s\n",
									   getSizeStr(filesize, filesize_rounded, 1));
							} else {
								printf("File(s) size (compressed - decmpfs xattr): %s\n",
									   getSizeStr(filesize, filesize_rounded, 0));
							}
							filesize = alltypesinfo.compressed_size + alltypesinfo.compattr_size;
							filesize_rounded = alltypesinfo.compressed_size_rounded + alltypesinfo.compattr_size;
							printf("File(s) size (compressed): %s\n", getSizeStr(filesize, filesize_rounded, 0));
							printf("Compression savings: %0.1f%%\n", (1.0 - ((float) (alltypesinfo.compressed_size + alltypesinfo.compattr_size) / alltypesinfo.uncompressed_size)) * 100.0);
							filesize = alltypesinfo.total_size;
							printf("Approximate total file(s) size (files + file overhead): %s\n",
								   getSizeStr(filesize, filesize, 0));
						}
					}
					printf("\n");
				}
				if (nJobs == 0)
				{
					if (folderinfo.num_compressed == 0 && !applycomp)
						printf("Folder contains no compressed files\n");
					else if (folderinfo.num_compressed == 0 && applycomp)
						printf("No compressable files in folder\n");
					else
						printf("Number of HFS+/APFS compressed files: %lld\n", folderinfo.num_compressed);
					if (printVerbose > 0)
					{
						printFolderInfo( &folderinfo, hardLinkCheck );
					}
				}
			}
		}
		
		if (free_src)
		{
			xfree(fullpath);
			free_src = false;
		}
		if (free_dst)
		{
			xfree(fullpathdst);
			free_dst = false;
		}
	}
	if (folderinfo.filetypeslist != NULL)
		free(folderinfo.filetypeslist);
	
#ifdef SUPPORT_PARALLEL
	if (PP)
	{
		if (sortQueue)
		{
			sortFilesInParallelProcessorBySize(PP);
		}
		if (filesInParallelProcessor(PP))
		{
			signal(SIGINT, signal_handler);
			signal(SIGHUP, signal_handler);
			fprintf( stderr, "Starting %d worker threads to process queue with %lu items\n",
				nJobs, filesInParallelProcessor(PP) );
			int processed = runParallelProcessor(PP);
			fprintf( stderr, "Processed %d entries\n", processed );
			if (printVerbose > 0)
			{
				struct folder_info *fInfo = getParallelProcessorJobInfo(PP);
				if (fInfo->num_files > 0)
				{
					printFolderInfo( getParallelProcessorJobInfo(PP), hardLinkCheck );
				}
			}
		}
		else
		{
			fprintf( stderr, "No compressable files found.\n" );
		}
		releaseParallelProcessor(PP);
	}
#endif
	return 0;
}
