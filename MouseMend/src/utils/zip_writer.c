#include "zip_writer.h"
#include <string.h>
#include <strsafe.h>

// CRC-32, reflected polynomial 0xEDB88320, nibble at a time
//
// The nibble table is 64 bytes instead of the usual 1 KB, which keeps the whole
// writer small enough to live on a thread's stack during a crash.
static const uint32_t CRC_NIBBLE[16] = {
	0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
	0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
	0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
	0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};

uint32_t zip_crc32_start(void)
{
	return 0xFFFFFFFFu;
}

uint32_t zip_crc32_update(uint32_t crc, const void *data, uint32_t size)
{
	const unsigned char *bytes = (const unsigned char *)data;

	if (!bytes || size == 0)
		return crc;

	for (uint32_t i = 0; i < size; i++)
	{
		crc = (crc >> 4) ^ CRC_NIBBLE[(crc ^ bytes[i]) & 0x0F];
		crc = (crc >> 4) ^ CRC_NIBBLE[(crc ^ (bytes[i] >> 4)) & 0x0F];
	}

	return crc;
}

uint32_t zip_crc32_finish(uint32_t crc)
{
	return crc ^ 0xFFFFFFFFu;
}

uint32_t zip_crc32(const void *data, uint32_t size)
{
	return zip_crc32_finish(zip_crc32_update(zip_crc32_start(), data, size));
}

// Little endian helpers. The format is defined on bytes, not on the host, so
// nothing here may be written as a struct.
static void put_u16(unsigned char *buffer, uint16_t value)
{
	buffer[0] = (unsigned char)(value & 0xFF);
	buffer[1] = (unsigned char)((value >> 8) & 0xFF);
}

static void put_u32(unsigned char *buffer, uint32_t value)
{
	buffer[0] = (unsigned char)(value & 0xFF);
	buffer[1] = (unsigned char)((value >> 8) & 0xFF);
	buffer[2] = (unsigned char)((value >> 16) & 0xFF);
	buffer[3] = (unsigned char)((value >> 24) & 0xFF);
}

static bool write_all(HANDLE file, const void *data, uint32_t size)
{
	const unsigned char *cursor = (const unsigned char *)data;

	while (size > 0)
	{
		DWORD written = 0;

		if (!WriteFile(file, cursor, size, &written, NULL) || written == 0)
			return false;

		cursor += written;
		size -= written;
	}

	return true;
}

// Move the file pointer and record the new logical position
static bool seek_to(ZipWriter *writer, uint64_t offset)
{
	LARGE_INTEGER distance;
	distance.QuadPart = (LONGLONG)offset;

	if (!SetFilePointerEx(writer->file, distance, NULL, FILE_BEGIN))
		return false;

	writer->position = offset;
	return true;
}

// MS-DOS timestamp: zip predates 64 bit time and stores two packed words
static void build_dos_stamp(uint16_t *dos_time, uint16_t *dos_date)
{
	SYSTEMTIME now;
	GetLocalTime(&now);

	if (now.wYear < 1980)
	{
		// Before the epoch the format cannot represent the date
		*dos_time = 0;
		*dos_date = (uint16_t)((1 << 5) | 1);
		return;
	}

	*dos_time = (uint16_t)((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
	*dos_date = (uint16_t)(((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay);
}

// 30 byte local file header, immediately followed by the name and the data
static bool write_local_header(ZipWriter *writer, const char *name, size_t name_length,
							   uint32_t crc, uint32_t size)
{
	unsigned char header[30];

	put_u32(header + 0, 0x04034B50u); // local file header signature
	put_u16(header + 4, 20);		  // version needed to extract
	put_u16(header + 6, 0x0800);	  // general purpose flags: names are UTF-8
	put_u16(header + 8, 0);			  // compression: stored
	put_u16(header + 10, writer->dos_time);
	put_u16(header + 12, writer->dos_date);
	put_u32(header + 14, crc);
	put_u32(header + 18, size); // compressed size
	put_u32(header + 22, size); // uncompressed size
	put_u16(header + 26, (uint16_t)name_length);
	put_u16(header + 28, 0); // extra field length

	if (!write_all(writer->file, header, sizeof(header)))
		return false;

	writer->position += sizeof(header);

	if (!write_all(writer->file, name, (uint32_t)name_length))
		return false;

	writer->position += name_length;
	return true;
}

bool zip_begin(ZipWriter *writer, const wchar_t *path)
{
	if (!writer || !path || !path[0])
		return false;

	SecureZeroMemory(writer, sizeof(*writer));

	writer->file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
							   FILE_ATTRIBUTE_NORMAL, NULL);

	if (writer->file == INVALID_HANDLE_VALUE)
	{
		writer->file = NULL;
		return false;
	}

	// Remembered so a failure can undo the empty file this call just created
	StringCchCopyW(writer->path, ARRAYSIZE(writer->path), path);

	build_dos_stamp(&writer->dos_time, &writer->dos_date);
	writer->ok = true;
	return true;
}

// Common tail of both add paths: remember the entry for the central directory
static bool remember_entry(ZipWriter *writer, const char *name, size_t name_length,
						   uint32_t crc, uint32_t size, uint64_t offset)
{
	if (writer->entry_count >= ZIP_MAX_ENTRIES)
		return false;

	if (name_length == 0 || name_length >= ZIP_NAME_MAX)
		return false;

	char *destination = writer->entries[writer->entry_count].name;
	memcpy(destination, name, name_length);
	destination[name_length] = '\0';

	writer->entries[writer->entry_count].crc = crc;
	writer->entries[writer->entry_count].size = size;
	writer->entries[writer->entry_count].offset = (uint32_t)offset;
	writer->entry_count++;
	return true;
}

bool zip_add_bytes(ZipWriter *writer, const char *name, const void *data, uint32_t size)
{
	if (!writer || !writer->ok || !name || (!data && size > 0))
		return false;

	size_t name_length = 0;
	if (FAILED(StringCchLengthA(name, ZIP_NAME_MAX, &name_length)))
		return false;

	uint32_t crc = zip_crc32(data, size);
	uint64_t offset = writer->position;

	if (!write_local_header(writer, name, name_length, crc, size))
		return false;

	if (size > 0)
	{
		if (!write_all(writer->file, data, size))
			return false;

		writer->position += size;
	}

	return remember_entry(writer, name, name_length, crc, size, offset);
}

// Chunk used while copying a file into the archive.
//
// Static on purpose: this runs inside the exception filter, and 16 KB of stack
// in a process that may already be dying is exactly the wrong place to spend
// stack. The consequence is that zip_add_file is not reentrant, which is fine
// because only one bundle is ever built at a time.
static unsigned char g_copy_chunk[16384];

bool zip_add_file(ZipWriter *writer, const char *name, const wchar_t *source)
{
	if (!writer || !writer->ok || !name || !source)
		return false;

	size_t name_length = 0;
	if (FAILED(StringCchLengthA(name, ZIP_NAME_MAX, &name_length)))
		return false;

	HANDLE input = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL,
							   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (input == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER file_size;
	bool have_size = GetFileSizeEx(input, &file_size) != 0;

	// The format stores 32 bit sizes, and a dump anywhere near 4 GB is a bug of
	// its own rather than something worth streaming
	if (!have_size || file_size.QuadPart < 0 || file_size.QuadPart > 0x7FFFFFFF)
	{
		CloseHandle(input);
		return false;
	}

	uint64_t offset = writer->position;
	uint32_t total = (uint32_t)file_size.QuadPart;

	// Written empty first: the checksum is only known after the data was read,
	// and patching the header afterwards is cheaper than buffering the file.
	if (!write_local_header(writer, name, name_length, 0, 0))
	{
		CloseHandle(input);
		return false;
	}

	unsigned char *chunk = g_copy_chunk;
	uint32_t crc = zip_crc32_start();
	uint32_t copied = 0;

	while (copied < total)
	{
		DWORD want = (DWORD)((total - copied > sizeof(g_copy_chunk))
								 ? sizeof(g_copy_chunk)
								 : (total - copied));
		DWORD read = 0;

		if (!ReadFile(input, chunk, want, &read, NULL) || read == 0)
			break;

		crc = zip_crc32_update(crc, chunk, read);

		if (!write_all(writer->file, chunk, read))
			break;

		writer->position += read;
		copied += read;
	}

	CloseHandle(input);

	if (copied != total)
		return false;

	crc = zip_crc32_finish(crc);

	// Patch the checksum and both sizes into the local header
	unsigned char patch[12];
	put_u32(patch + 0, crc);
	put_u32(patch + 4, total);
	put_u32(patch + 8, total);

	uint64_t end_of_data = writer->position;

	if (!seek_to(writer, offset + 14) || !write_all(writer->file, patch, sizeof(patch)))
		return false;

	if (!seek_to(writer, end_of_data))
		return false;

	return remember_entry(writer, name, name_length, crc, total, offset);
}

bool zip_end(ZipWriter *writer)
{
	if (!writer || !writer->ok)
	{
		zip_abort(writer);
		return false;
	}

	writer->central_offset = (uint32_t)writer->position;

	// Central directory: one 46 byte header plus the name, per entry
	for (uint32_t i = 0; i < writer->entry_count; i++)
	{
		size_t name_length = 0;
		if (FAILED(StringCchLengthA(writer->entries[i].name, ZIP_NAME_MAX, &name_length)))
		{
			zip_abort(writer);
			return false;
		}

		unsigned char header[46];
		put_u32(header + 0, 0x02014B50u); // central directory signature
		put_u16(header + 4, 20);		  // version made by
		put_u16(header + 6, 20);		  // version needed
		put_u16(header + 8, 0x0800);	  // UTF-8 names
		put_u16(header + 10, 0);		  // stored
		put_u16(header + 12, writer->dos_time);
		put_u16(header + 14, writer->dos_date);
		put_u32(header + 16, writer->entries[i].crc);
		put_u32(header + 20, writer->entries[i].size);
		put_u32(header + 24, writer->entries[i].size);
		put_u16(header + 28, (uint16_t)name_length);
		put_u16(header + 30, 0);				 // extra
		put_u16(header + 32, 0);				 // comment
		put_u16(header + 34, 0);				 // disk number
		put_u16(header + 36, 0);				 // internal attributes
		put_u32(header + 38, 0);				 // external attributes
		put_u32(header + 42, writer->entries[i].offset);

		if (!write_all(writer->file, header, sizeof(header)) ||
			!write_all(writer->file, writer->entries[i].name, (uint32_t)name_length))
		{
			zip_abort(writer);
			return false;
		}

		writer->position += sizeof(header) + name_length;
	}

	writer->central_size = (uint32_t)(writer->position - writer->central_offset);

	// End of central directory
	unsigned char end[22];
	put_u32(end + 0, 0x06054B50u);
	put_u16(end + 4, 0); // this disk
	put_u16(end + 6, 0); // disk with the central directory
	put_u16(end + 8, (uint16_t)writer->entry_count);
	put_u16(end + 10, (uint16_t)writer->entry_count);
	put_u32(end + 12, writer->central_size);
	put_u32(end + 16, writer->central_offset);
	put_u16(end + 20, 0); // comment length

	if (!write_all(writer->file, end, sizeof(end)))
	{
		zip_abort(writer);
		return false;
	}

	writer->position += sizeof(end);

	bool complete = FlushFileBuffers(writer->file) != 0 && writer->entry_count > 0;
	CloseHandle(writer->file);
	writer->file = NULL;
	writer->ok = false;

	if (!complete)
	{
		// A zip that no tool can open is worse than no zip: the user would attach
		// it and we would spend a round trip discovering it is empty.
		DeleteFileW(writer->path);
		writer->path[0] = L'\0';
		return false;
	}

	writer->path[0] = L'\0'; // Committed; a later abort must not delete it
	return true;
}

void zip_abort(ZipWriter *writer)
{
	if (!writer)
		return;

	if (writer->file)
	{
		CloseHandle(writer->file);
		writer->file = NULL;
	}

	if (writer->path[0] != L'\0')
	{
		DeleteFileW(writer->path);
		writer->path[0] = L'\0';
	}

	writer->ok = false;
}
