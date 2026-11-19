/*
 * Minimal cabinet writer used for Quantum compression.
 * C89 / MSVC6 compatible.
 */
#ifndef QCABINET_H
#define QCABINET_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char          *name;       /* stored name (may contain backslashes) */
  unsigned char *data;
  unsigned long  size;
  unsigned short date;
  unsigned short time;
  unsigned short attribs;
  int            folder;     /* folder index (0-based) */
} QCAB_FILE;

/* Write a single cabinet containing the given files.
   Returns 1 on success, 0 on failure. */
int qcab_write(const char *path, QCAB_FILE *files, int nfiles,
               int nfolders, int window_bits, int level,
               unsigned short setID, unsigned int reserve,
               int comp_type);   /* 0 = NONE, 2 = QUANTUM */

#ifdef __cplusplus
}
#endif

#endif /* QCABINET_H */
