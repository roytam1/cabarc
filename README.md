# cabarc — standalone C89 reconstruction

A standalone reconstruction of Microsoft's `CABARC.EXE` (5.1.2600.1106),
built from `CABARC.EXE.lst` / `CABARC.EXE.c`, with **Quantum** support from
`DIAMOND.EXE`.  Cabins are read and written in-process: **no cabinet.dll
dependency** (`dumpbin /imports` shows only KERNEL32).

## Build

```
cd build
build.bat
```

Requires MSVC6 (`VCVARS32.BAT` is called by the script).  Output:
`build\cabarc_standalone.exe`.

All sources are now plain C89 (`/TC`) — no C++ compiler is needed.  The LZX
decoder was ported from the supplied `CabLzx.cpp` to `src/lzxdec.c` (malloc
instead of new, int instead of bool, plain structs), and the old C++ bridge
(`lzxwrap.*`) is gone.  No third-party libraries (e.g. zlib) are needed.

## Source map

| File | Purpose |
|------|---------|
| `src/cabarc.c` | CLI, list/create/extract (C89) |
| `src/cablib.c` | Cabinet container parse/decompress/write, no DLLs |
| `src/inflate.c` | Standalone raw DEFLATE decoder (reference / fallback) |
| `src/msinflate.c` | Shared-window MSZIP inflater from `mscompress-expand/` (memory-output adapted) |
| `src/mszip_comp.c`, `src/lzencode.c` | Real MSZIP dynamic-Huffman compressor, from `mscompress-expand/compress.c` |
| `src/quantum.c` | Quantum LZ77 + arithmetic codec; encoder ported from `paq/qpack.c`, decoder validated against DIAMOND cabinets |
| `src/qcabinet.c` | Cabinet container writer |
| `src/lzxdec.c` | LZX decoder, C89 port of the supplied `CabLzx.cpp` |
| `src/lzxenc.c` | LZX encoder (verbatim blocks with Huffman trees, validated fallback to uncompressed) |

## Verified behaviour

* `L` and `X` read cabinets produced by the original `CABARC.EXE` (MSZIP),
  by `DIAMOND.EXE` (Quantum), and LZX/NONE cabinets — data hashes match.
* `N -m MSZIP` produces real dynamic-Huffman MSZIP data, switching to a
  stored DEFLATE block when the dynamic tree would make a frame larger
  than its raw bytes.  Original `CABARC.EXE` lists and extracts these
  cabinets correctly, including multi-block folders (history window
  shared across blocks) and incompressible data.
* `N -m LZX:<15..21>` works at every window level.  `src/lzxenc.c` implements
  real LZX verbatim blocks (LZ77 position slots, persistent R0/R1/R2 repeats,
  frequency-based 20-symbol pre-tree with RLE of unchanged lengths, and
  cost-aware match selection with window-dependent weights).  History is
  shared across all blocks of a folder, so larger windows find matches in
  earlier CFDATA blocks.  Up to eight 32 KB frames share one LZX block
  (one set of Huffman trees), with word-aligned CFDATA split points.  x86
  E8 call translation is enabled when an opcode-aware scan finds a real
  x86 call site (the stream header carries the file size), so stray 0xE8
  bytes in text or random data do not pay for the header.  The decoder
  honours it even when every 0xE8 was consumed by a match.  Verbatim blocks are compared
  against raw size and replaced with uncompressed LZX blocks when larger
  (also the Huffman failure fallback), keeping every cabinet readable by
  both this tool and the original `CABARC.EXE`.
* `N -m QUANTUM` now produces real Quantum-compressed cabinets.  The
  encoder was ported from `paq/qpack.c` (the verified C89 reconstruction of
  the original DOS Quantum packer): carry-less arithmetic coding, the
  delayed raw-bit queue, and persistent models across the CFDATA frames of
  a folder.  Our single-frame output for the test data is **byte-identical
  to DIAMOND.EXE's**, and the original `CABARC.EXE` extracts our Quantum
  cabinets (including multi-frame ones) correctly.
* CLI options/messages reproduce the original tool (`-c -o -m -p -P -r -s
  -i -d`, `+` folder boundaries, wildcards, `@list`).

## Known limitations

* The LZX encoder uses greedy/lazy parsing rather than the original's full
  cost-based parser, so sizes on varied data are typically 0.5–5% larger
  than `CABARC.EXE` on larger inputs (tiny files pay a fixed tree/header
  overhead).  E8 translation is applied to every 0xE8 byte in the data area,
  not only to genuine x86 call opcodes.
* Quantum matchfinder tie-breaking may differ from DIAMOND's in rare cases,
  so compressed sizes for other inputs may vary slightly (the streams are
  still valid).
* Multi-cabinet sets (`-d`) and reserve areas are parsed/created only for
  single-cabinet output.
