#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

// Minimal ZIP writer: stored (uncompressed) entries only.
//
// A support bundle has to be one file the user can attach to a bug report, and
// Windows offers no public API for creating a zip. The stored format is a
// handful of fixed headers, so it needs no allocation and no CRT - which is
// exactly what a handler running inside an already damaged process can afford.
//
// Compression is deliberately absent. A minidump is small next to the text
// report, and deflate would add a subsystem that can itself fail at the moment
// it matters most.

#define ZIP_MAX_ENTRIES 4
#define ZIP_NAME_MAX 64

// One archive. Sized for the stack, and reused as is when the caller has no heap.
typedef struct
{
	HANDLE file;
	wchar_t path[MAX_PATH];		// Name the archive is published under
	wchar_t part_path[MAX_PATH]; // Name it is written under until it is complete
	uint64_t position;		// Bytes written so far
	uint32_t entry_count;
	uint32_t central_offset;
	uint32_t central_size;
	uint16_t dos_time;
	uint16_t dos_date;
	bool ok;

	// Remembered so the central directory can be written at the end without
	// seeking back to the individual local headers
	struct
	{
		char name[ZIP_NAME_MAX];
		uint32_t crc;
		uint32_t size;
		uint32_t offset;
	} entries[ZIP_MAX_ENTRIES];
} ZipWriter;

// Standard CRC-32, the polynomial every zip tool uses
// The three step form exists so a file can be checksummed while it is streamed.
uint32_t zip_crc32_start(void);
uint32_t zip_crc32_update(uint32_t crc, const void *data, uint32_t size);
uint32_t zip_crc32_finish(uint32_t crc);

// One shot checksum, for tests and small buffers
uint32_t zip_crc32(const void *data, uint32_t size);

// Create the archive
// Returns:
//   false when the path cannot be created, so the caller can try another folder
bool zip_begin(ZipWriter *writer, const wchar_t *path);

// Append an entry that is already in memory
bool zip_add_bytes(ZipWriter *writer, const char *name, const void *data, uint32_t size);

// Append an entry by copying a file in fixed size chunks
// A dump never has to fit in memory, and the copy happens before the archive is
// finalised, so a half written entry can still be reported as a clean failure.
bool zip_add_file(ZipWriter *writer, const char *name, const wchar_t *source);

// Write the central directory and close
// Returns:
//   false when anything failed; the partial archive is deleted, because a zip
//   that cannot be opened is worse than no zip at all
bool zip_end(ZipWriter *writer);

// Close and delete, for a caller that gave up
void zip_abort(ZipWriter *writer);
