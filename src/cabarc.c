/*
 * cabarc -- Microsoft Cabinet Tool, C89 reconstruction.
 *
 * Reconstructed from CABARC.EXE (5.1.2600.1106) with Quantum support
 * from DIAMOND.EXE.  This build is standalone: the cabinet container
 * and all codecs are compiled in, with no cabinet.dll dependency.
 *
 * Build with MSVC6: see build\build.bat.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <conio.h>
#include <sys/stat.h>
#include "cablib.h"

/* ------------------------------------------------------------ options */

static long    g_disk_size = 0x7FFFFFFF;
static int     g_comp = 1;             /* 0=NONE 1=MSZIP 2=LZX 3=QUANTUM */
static int     g_memory = 0;
static int     g_confirm = 0;
static int     g_overwrite = 0;
static int     g_reserve = 0;
static int     g_set_id = 0;
static int     g_preserve = 0;
static int     g_recurse = 0;
static char    g_destdir[260];
static int     g_qlevel = 7;
static int     g_qwindow = 15;

typedef struct _SLIST {
  struct _SLIST *next;
  char           name[1];
} SLIST;

static SLIST g_strip, g_files;

static void slist_init(SLIST *l) { l->next = NULL; l->name[0] = '\0'; }

static int slist_add(SLIST *l, const char *name)
{
  SLIST *n = (SLIST *)malloc(sizeof(SLIST) + strlen(name));
  if (!n) return 0;
  n->next = NULL;
  strcpy(n->name, name);
  while (l->next) l = l->next;
  l->next = n;
  return 1;
}

static void slist_free(SLIST *l)
{
  SLIST *p = l->next;
  while (p) { SLIST *q = p->next; free(p); p = q; }
  l->next = NULL;
}

static void usage(void)
{
  printf(
    "Usage: CABARC [options] command cabfile [@list] [files] [dest_dir]\n"
    "\n"
    "Commands:\n"
    "   L   List contents of cabinet (e.g. cabarc l test.cab)\n"
    "   N   Create new cabinet (e.g. cabarc n test.cab *.c app.mak *.h)\n"
    "   X   Extract file(s) from cabinet (e.g. cabarc x test.cab foo*.c)\n"
    "\n"
    "Options:\n"
    "  -c   Confirm files to be operated on\n"
    "  -o   When extracting, overwrite without asking for confirmation\n"
    "  -m   Set compression type [LZX:<15..21>|MSZIP|NONE|QUANTUM[:l[:w]]]\n"
    "  -p   Preserve path names (absolute paths not allowed)\n"
    "  -P   Strip specified prefix from files when added\n"
    "  -r   Recurse into subdirectories when adding files (see -p also)\n"
    "  -s   Reserve space in cabinet for signing (e.g. -s 6144)\n"
    "  -i   Set cabinet set ID when creating cabinets (default is 0)\n"
    "  -d   Set diskette size (default is no limit/single CAB)\n"
    "\n"
    "Quantum: -m QUANTUM[:level[:window]]  level 1..7, window 10..21\n"
    "\n"
    "Notes\n"
    "-----\n"
    "When creating a cabinet, the plus sign (+) may be used as a filename\n"
    "to force a folder boundary; e.g. cabarc n test.cab *.c test.h + *.bmp\n"
    "\n"
    "When extracting files to disk, the <dest_dir>, if provided, must end in\n"
    "a backslash; e.g. cabarc x test.cab bar*.cpp *.h d:\\test\\\n"
    "\n"
    "The -P (strip prefix) option can be used to strip out path information\n"
    "e.g. cabarc -r -p -P myproj\\ n test.cab myproj\\balloon\\*.*\n"
    "The -P option can be used multiple times to strip out multiple paths\n"
    "\n"
    "When creating cabinet sets using -d, the cabinet name should contain\n"
    "a single '*' character where the cabinet number will be inserted.\n"
    "\n");
}

/* ------------------------------------------------------------ helpers */

static int wild_match(const char *pat, const char *name)
{
  const char *p = pat, *n = name, *star = NULL, *mark = NULL;
  while (*n) {
    if (*p == '?' || *p == *n) { p++; n++; continue; }
    if (*p == '*') { star = p++; mark = n; continue; }
    if (star) { p = star + 1; n = ++mark; continue; }
    return 0;
  }
  while (*p == '*') p++;
  return *p == '\0';
}

static int pattern_match(const char *name)
{
  SLIST *p;
  const char *base;
  if (!g_files.next) return 1;
  base = strrchr(name, '\\');
  base = base ? base + 1 : name;
  for (p = g_files.next; p; p = p->next)
    if (wild_match(p->name, name) || wild_match(p->name, base)) return 1;
  return 0;
}

static void make_stored_name(const char *src, char *dst, size_t dstlen)
{
  const char *p = src, *colon = strchr(src, ':');
  SLIST *l;
  if (colon) p = colon + 1;
  if (*p == '\\') p++;
  for (l = g_strip.next; l; l = l->next) {
    size_t plen = strlen(l->name);
    if (plen && _strnicmp(p, l->name, plen) == 0 && strlen(p) > plen) {
      p += plen;
      break;
    }
  }
  if (!g_preserve) {
    const char *slash = strrchr(p, '\\');
    const char *fslash = strrchr(p, '/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    if (slash) p = slash + 1;
  }
  strncpy(dst, p, dstlen - 1);
  dst[dstlen - 1] = '\0';
}

static void attr_string(unsigned short a, char *buf)
{
  buf[0] = (a & 1) ? 'r' : '-';
  buf[1] = (a & 0x20) ? 'a' : '-';
  buf[2] = (a & 2) ? 'h' : '-';
  buf[3] = (a & 4) ? 's' : '-';
  buf[4] = '\0';
}

static int ask_yesno(const char *prompt)
{
  int c;
  printf("%s", prompt);
  for (;;) {
    c = toupper(getch());
    if (c == 'Y') { printf("y\n"); return 1; }
    if (c == 'N') { printf("n\n"); return 0; }
    if (c == 'A') { printf("a\n"); return 2; }
    if (c == 'Q') { printf("q\n"); return -1; }
  }
}

/* ------------------------------------------------------------ list */

static int do_list(const char *cabpath)
{
  CABLIB cab;
  int i;
  char err[256];
  if (!cablib_read(cabpath, &cab, err, sizeof(err))) {
    if (err[0]) printf("%s\n", err);
    return 0;
  }
  {
    FILE *fp = fopen(cabpath, "rb");
    long sz = 0;
    if (fp) { fseek(fp, 0, SEEK_END); sz = ftell(fp); fclose(fp); }
    printf("Listing of cabinet file '%s' (size %ld):\n"
           "   %d file(s), %d folder(s), set ID %d, cabinet #%d\n\n",
           cabpath, sz, cab.nfiles, 1, cab.setID, cab.iCab);
  }
  printf("File name                      File size     Date      Time   Attrs\n"
         "-----------------------------  ---------- ---------- -------- -----\n");
  for (i = 0; i < cab.nfiles; i++) {
    CABLIB_FILE *f = &cab.files[i];
    char date[32], timebuf[32], attrs[8];
    sprintf(date, "%04d/%02d/%02d", (f->date >> 9) + 1980,
            (f->date >> 5) & 0xF, f->date & 0x1F);
    sprintf(timebuf, "%02d:%02d:%02d", f->time >> 11,
            (f->time >> 5) & 0x3F, (f->time & 0x1F) * 2);
    attr_string(f->attribs, attrs);
    printf("   %-29s %8lu %s %s  %s\n", f->name, f->size,
           date, timebuf, attrs);
  }
  printf("\n");
  cablib_free(&cab);
  return 1;
}

/* ------------------------------------------------------------ extract */

static int do_extract(const char *cabpath)
{
  CABLIB cab;
  char err[256];
  int i, any = 0;

  if (!cablib_read(cabpath, &cab, err, sizeof(err))) {
    if (err[0]) printf("%s\n", err);
    return 0;
  }
  printf("Extracting file(s) from cabinet '%s':\n", cabpath);
  for (i = 0; i < cab.nfiles; i++) {
    CABLIB_FILE *f = &cab.files[i];
    const char *base;
    char path[260];
    int ans;
    FILE *fp;
    FILETIME ft, lt;
    USHORT a;

    if (strchr(f->name, ':')) {
      printf("   File '%s' contains illegal character (colon) -- skipping\n", f->name);
      continue;
    }
    if (!pattern_match(f->name)) continue;
    base = g_preserve ? f->name
           : (strrchr(f->name, '\\') ? strrchr(f->name, '\\') + 1 : f->name);
    sprintf(path, "%s%s", g_destdir, base);

    if (g_confirm) {
      ans = ask_yesno("   -- Extract? (Yes/No/All/Quit): ");
      if (ans == 0) continue;
      if (ans == -1) break;
      if (ans == 2) g_confirm = 0;
    }
    if (!g_overwrite && GetFileAttributesA(path) != (DWORD)-1) {
      char prompt[512];
      sprintf(prompt, "   -- File '%s' already exists; overwrite? (Yes/No/All/Quit): ", path);
      ans = ask_yesno(prompt);
      if (ans == 0) continue;
      if (ans == -1) break;
      if (ans == 2) g_overwrite = 1;
    }
    fp = fopen(path, "wb");
    if (!fp) {
      printf("   -- Error opening '%s' for output\n", path);
      continue;
    }
    if (f->size)
      fwrite(f->data, 1, f->size, fp);
    fclose(fp);
    printf("   extracting: %s\n", path);

    a = (USHORT)(f->attribs & 0xFFD8);
    if (!a) a = 0x80;
    if (DosDateTimeToFileTime(f->date, f->time, &ft) &&
        LocalFileTimeToFileTime(&ft, &lt)) {
      HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, &lt, NULL, &lt);
        CloseHandle(h);
      }
    }
    SetFileAttributesA(path, a);
    any++;
  }
  cablib_free(&cab);
  return 1;
}

/* ------------------------------------------------------------ create */

typedef struct {
  char src[260];
  char stored[260];
  int  folder;
} ADDENT;

static int add_one(const char *path, ADDENT **list, int *n, int *cap)
{
  ADDENT *e;
  if (*n >= *cap) {
    ADDENT *q;
    *cap = *cap ? *cap * 2 : 16;
    q = (ADDENT *)realloc(*list, (size_t)*cap * sizeof(ADDENT));
    if (!q) return 0;
    *list = q;
  }
  e = &(*list)[*n];
  strcpy(e->src, path);
  make_stored_name(path, e->stored, sizeof(e->stored));
  e->folder = 0;
  (*n)++;
  return 1;
}

/* Add a literal name or expand a wildcard to the files that match it.
   The literal pattern itself is never added as a file (as in cabarc.exe). */
static int add_spec(const char *spec, ADDENT **list, int *n, int *cap)
{
  char path[_MAX_PATH], dirpart[_MAX_PATH], sub[_MAX_PATH];
  char drive[_MAX_DRIVE], dir[_MAX_DIR];
  char fname[_MAX_FNAME], ext[_MAX_EXT], pat[_MAX_PATH];
  HANDLE hfind;
  WIN32_FIND_DATAA fd;

  if (!strchr(spec, '*') && !strchr(spec, '?'))
    return add_one(spec, list, n, cap);

  _splitpath(spec, drive, dir, fname, ext);
  sprintf(pat, "%s%s%s%s", drive, dir, fname, ext);
  sprintf(dirpart, "%s%s", drive, dir);

  hfind = FindFirstFileA(pat, &fd);
  if (hfind == INVALID_HANDLE_VALUE)
    return 1;                 /* no match: original cabarc adds nothing */
  do {
    if (fd.cFileName[0] == '.')   /* skips "." and ".." */
      continue;
    sprintf(path, "%s%s", dirpart, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (g_recurse) {
        /* apply the same filename pattern inside the subdirectory */
        sprintf(sub, "%s\\%s%s", path, fname, ext);
        if (!add_spec(sub, list, n, cap)) {
          FindClose(hfind);
          return 0;
        }
      }
    }
    else {
      if (!add_one(path, list, n, cap)) {
        FindClose(hfind);
        return 0;
      }
    }
  } while (FindNextFileA(hfind, &fd));
  FindClose(hfind);
  return 1;
}

static int do_create(const char *cabpath)
{
  ADDENT *ents = NULL;
  CABLIB_FILE *files;
  SLIST *p;
  int n = 0, cap = 0, nfolders = 1, i, k, ok = 0, cur = 0;
  int comp_type;
  FILE *fp;

  for (p = g_files.next; p; p = p->next) {
    ADDENT *e;
    if (strcmp(p->name, "+") == 0) {
      if (n >= cap) {
        ADDENT *q;
        cap = cap ? cap * 2 : 16;
        q = (ADDENT *)realloc(ents, (size_t)cap * sizeof(ADDENT));
        if (!q) goto done;
        ents = q;
      }
      e = &ents[n];
      strcpy(e->src, "+");
      strcpy(e->stored, "+");
      e->folder = nfolders++;
      n++;
      continue;
    }
    if (!add_spec(p->name, &ents, &n, &cap)) goto done;
  }

  for (i = 0; i < n; i++) {
    if (strcmp(ents[i].stored, "+") == 0) { cur++; continue; }
    ents[i].folder = cur;
  }

  files = (CABLIB_FILE *)calloc((size_t)n + 1, sizeof(CABLIB_FILE));
  if (!files) goto done;
  k = 0;
  for (i = 0; i < n; i++) {
    HANDLE hf;
    BY_HANDLE_FILE_INFORMATION fi;
    FILETIME lft;
    DWORD got;
    if (strcmp(ents[i].stored, "+") == 0) continue;
    hf = CreateFileA(ents[i].src, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
      printf("   -- Error opening '%s' for input\n", ents[i].src);
      goto donefiles;
    }
    files[k].size = GetFileSize(hf, NULL);
    files[k].data = (unsigned char *)malloc((size_t)files[k].size + 1);
    if (!files[k].data || !ReadFile(hf, files[k].data, files[k].size, &got, NULL)) {
      CloseHandle(hf);
      goto donefiles;
    }
    if (GetFileInformationByHandle(hf, &fi)) {
      FileTimeToLocalFileTime(&fi.ftLastWriteTime, &lft);
      FileTimeToDosDateTime(&lft, &files[k].date, &files[k].time);
      files[k].attribs = (USHORT)(fi.dwFileAttributes & 0x27);
    }
    else {
      files[k].date = files[k].time = 0;
      files[k].attribs = 0x20;
    }
    CloseHandle(hf);
    strcpy(files[k].name, ents[i].stored);
    files[k].folder = ents[i].folder;
    k++;
    printf("  -- adding %s\n", ents[i].src);
  }

  if (g_comp == 0) comp_type = CABLIB_COMP_NONE;
  else if (g_comp == 1) comp_type = CABLIB_COMP_MSZIP;
  else if (g_comp == 2) comp_type = CABLIB_COMP_LZX;
  else comp_type = CABLIB_COMP_QUANTUM;

  if (g_comp == 3) {
    printf("Creating new cabinet '%s' with compression 'QUANTUM:%d:%d':\n",
           cabpath, g_qlevel, g_qwindow);
    if (!cablib_write(cabpath, files, k, nfolders, CABLIB_COMP_QUANTUM,
                      g_qwindow, g_qlevel, (unsigned short)g_set_id,
                      (unsigned)g_reserve)) {
      printf("   -- Quantum encoder could not encode this data; "
             "falling back to NONE\n");
      comp_type = CABLIB_COMP_NONE;
    }
  }
  else {
    char name[80];
    if (comp_type == CABLIB_COMP_NONE) strcpy(name, "NONE");
    else if (comp_type == CABLIB_COMP_LZX) sprintf(name, "LZX:%d", g_memory);
    else strcpy(name, "MSZIP");
    printf("Creating new cabinet '%s' with compression '%s':\n", cabpath, name);
  }

  if (!cablib_write(cabpath, files, k, nfolders, comp_type,
                    g_comp == 3 ? g_qwindow : g_comp == 2 ? g_memory : 15, g_qlevel,
                    (unsigned short)g_set_id, (unsigned)g_reserve))
    goto donefiles;
  printf("Completed successfully\n");
  ok = 1;

donefiles:
  for (i = 0; i < k; i++)
    if (files[i].data) free(files[i].data);
  free(files);
done:
  free(ents);
  return ok;
}

/* ------------------------------------------------------------ main */

int main(int argc, char **argv)
{
  int i = 1, ret = 0;
  char command = 0;
  const char *cab = NULL;

  slist_init(&g_strip);
  slist_init(&g_files);
  g_disk_size = 0x7FFFFFFF;
  g_comp = 1;
  g_reserve = 0;
  g_set_id = 0;
  g_destdir[0] = '\0';

  printf("\nMicrosoft (R) Cabinet Tool - Version 5.1.2600.1106\n"
         "Copyright (c) Microsoft Corporation. All rights reserved..\n"
         "\n");
  if (argc < 2) { usage(); return 0; }

  while (i < argc) {
    char *arg = argv[i];
    if (arg[0] == '-' || arg[0] == '/') {
      char opt = (char)toupper((unsigned char)arg[1]);
      switch (opt) {
        case 'M':
          if (++i >= argc) { printf("-m option requires a parameter\n"); goto done; }
          if (_stricmp(argv[i], "MSZIP") == 0) g_comp = 1;
          else if (_stricmp(argv[i], "NONE") == 0) g_comp = 0;
          else if (_strnicmp(argv[i], "LZX:", 4) == 0) {
            int m = atoi(argv[i] + 4);
            if (m < 15 || m > 21) {
              printf("compression memory must be in range 15..21\n"); goto done;
            }
            g_comp = 2; g_memory = m;
          }
          else if (_strnicmp(argv[i], "QUANTUM", 7) == 0) {
            const char *q = argv[i] + 7;
            g_comp = 3;
            g_qlevel = 7; g_qwindow = 15;
            if (*q == ':') {
              g_qlevel = atoi(++q);
              if (g_qlevel < 1 || g_qlevel > 7) {
                printf("Quantum level must be in range 1..7\n"); goto done;
              }
              if ((q = strchr(q, ':')) != NULL) {
                g_qwindow = atoi(q + 1);
                if (g_qwindow < 10 || g_qwindow > 21) {
                  printf("Quantum window must be in range 10..21\n"); goto done;
                }
              }
            }
          }
          else {
            printf("-m option supports LZX:<15..21>, MSZIP, NONE or QUANTUM[:level[:window]]\n");
            goto done;
          }
          break;
        case 'O': g_overwrite = 1; break;
        case 'P':
          if (arg[1] == 'p') { g_preserve = 1; break; }
          if (++i >= argc) { printf("-P option requires a parameter\n"); goto done; }
          slist_add(&g_strip, argv[i]);
          break;
        case 'R': g_recurse = 1; break;
        case 'S':
          if (++i >= argc) { printf("-s option requires a parameter\n"); goto done; }
          g_reserve = atoi(argv[i]);
          if (g_reserve <= 0 || g_reserve >= 65536) {
            printf("-s parameter must be in range 1...65535\n"); goto done;
          }
          break;
        case 'C': g_confirm = 1; break;
        case 'D':
          if (++i >= argc) { printf("-d option requires a parameter\n"); goto done; }
          if (!isdigit((unsigned char)argv[i][0])) {
            printf("-d option requires a numeric parameter\n"); goto done;
          }
          g_disk_size = atol(argv[i]);
          if (g_disk_size < 50000) {
            printf("-d value must be at least 50000\n"); goto done;
          }
          break;
        case 'I':
          if (++i >= argc) { printf("-i option requires a parameter\n"); goto done; }
          if (!isdigit((unsigned char)argv[i][0])) {
            printf("-i option requires a numeric parameter\n"); goto done;
          }
          g_set_id = atoi(argv[i]);
          if (g_set_id < 0 || g_set_id >= 65536) {
            printf("Allowable values for set ID are 0...65535\n"); goto done;
          }
          break;
        case 'H':
        case '?': usage(); goto done;
        default:
          printf("Unknown option -%c\n", opt);
          ret = 1; goto done;
      }
      i++;
      continue;
    }

    command = (char)toupper((unsigned char)arg[0]);
    if (++i >= argc) {
      printf("Insufficient parameters\n");
      ret = 1; goto done;
    }
    cab = argv[i];
    i++;
    break;
  }

  switch (command) {
    case 'L':
      ret = !do_list(cab);
      break;
    case 'X':
      for (; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (len && argv[i][len - 1] == '\\') {
          strncpy(g_destdir, argv[i], sizeof(g_destdir) - 1);
          g_destdir[sizeof(g_destdir) - 1] = '\0';
        }
        else if (argv[i][0] == '@') {
          printf("Error, @files not supported for extract\n");
          ret = 1; goto done;
        }
        else slist_add(&g_files, argv[i]);
      }
      ret = !do_extract(cab);
      break;
    case 'N':
      for (; i < argc; i++) {
        if (strcmp(argv[i], "+") == 0) slist_add(&g_files, "+");
        else if (argv[i][0] == '@') {
          FILE *fp = fopen(argv[i] + 1, "rt");
          char line[512];
          if (!fp) {
            printf("Error reading list of files from '%s'\n", argv[i] + 1);
            ret = 1; goto done;
          }
          while (fgets(line, sizeof(line), fp)) {
            char *p = line;
            size_t l;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            l = strlen(p);
            while (l && (p[l-1] == '\r' || p[l-1] == '\n' ||
                         p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = '\0';
            if (l && *p != '#') slist_add(&g_files, p);
          }
          fclose(fp);
        }
        else slist_add(&g_files, argv[i]);
      }
      ret = !do_create(cab);
      break;
    default:
      printf("Unknown command %c\n", command);
      ret = 1;
      break;
  }

done:
  slist_free(&g_strip);
  slist_free(&g_files);
  return ret;
}
