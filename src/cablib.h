/*
 * Standalone cabinet container reader/writer for the cabarc
 * reconstruction.  No cabinet.dll dependency.
 *
 * C89 / MSVC6 compatible.
 */
#ifndef CABLIB_H
#define CABLIB_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CABLIB_COMP_NONE    0
#define CABLIB_COMP_MSZIP   1
#define CABLIB_COMP_QUANTUM 2
#define CABLIB_COMP_LZX     3

typedef struct {
  char           name[300];
  unsigned long  size;
  unsigned long  uoff;
  unsigned short folder;
  unsigned short date;
  unsigned short time;
  unsigned short attribs;
  unsigned char *data;
} CABLIB_FILE;

typedef struct {
  CABLIB_FILE *files;
  int          nfiles;
  unsigned short setID;
  unsigned short iCab;
} CABLIB;

/* Read and fully decompress a cabinet into memory.
   Returns 1 on success, 0 on error (message copied into err, if given). */
int cablib_read(const char *path, CABLIB *cab, char *err, int errsz);

void cablib_free(CABLIB *cab);

/* Write files into a single cabinet.  folder[] gives the folder index
   per file; nfolders is the number of folders. */
int cablib_write(const char *path, CABLIB_FILE *files, int nfiles,
                 int nfolders, int comp_type, int window_bits, int level,
                 unsigned short setID, unsigned int reserve);

const char *cablib_compname(unsigned short type);

#ifdef __cplusplus
}
#endif

#endif
