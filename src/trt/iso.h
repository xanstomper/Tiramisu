// tiramisu/src/trt/iso.h
// Disc-image filesystem: ISO9660 (+Joliet), UDF 1.02/2.01, and raw 2352/2336
// sector dumps (.bin). A disc game's files live inside the image; the VFS
// mounts the image as a read-only drive so the guest's own path requests
// (D:\data\... , FindFirstFile, mmap) reach the real bytes.
//
// Read-only by construction: a disc is a disc. Writes answer
// ERROR_WRITE_PROTECT, never a silent success.
#ifndef TRT_ISO_H
#define TRT_ISO_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct trt_iso trt_iso;

struct trt_iso_info {
  char format[16];        // "iso9660", "joliet", "udf", "udf+iso"
  uint32_t sector_size;   // 2048, or the raw sector stride
  uint32_t user_offset;   // where the 2048 user bytes start inside a sector
  uint64_t image_bytes;
  uint32_t file_count;
  uint32_t dir_count;
};

// Open an image. Detects the layout from the volume descriptors.
trt_iso* trt_iso_open(const char* host_path);
void trt_iso_close(trt_iso* iso);
void trt_iso_get_info(const trt_iso* iso, struct trt_iso_info* out);

// Look a guest path up ("/", "/data", "data/level1.pak"). Case-insensitive,
// accepts '\\' or '/'. Returns 0 on success.
int trt_iso_stat(const trt_iso* iso, const char* path, int* is_dir,
                 uint64_t* size);

// Enumerate one directory. cb is called once per entry (never "." / "..").
// Names are UTF-8. Returns 0 on success, negative if the path is not a dir.
typedef void (*trt_iso_enum_cb)(void* user, const char* name, int is_dir,
                                uint64_t size);
int trt_iso_enum(const trt_iso* iso, const char* path, trt_iso_enum_cb cb,
                 void* user);

// Read `len` bytes at `off` from a file inside the image. Returns bytes read.
uint32_t trt_iso_read(const trt_iso* iso, const char* path, uint64_t off,
                      void* buf, uint32_t len);

// Extract a whole file to a host path (the VFS cache). Returns 0 on success.
int trt_iso_extract(const trt_iso* iso, const char* path, const char* host_out);

// Extract every file directly inside an image directory (not recursive) into
// host_dir. Used once for the exe's own folder so the loader finds the game's
// DLLs beside it. Returns the number of files extracted.
int trt_iso_extract_dir(const trt_iso* iso, const char* rel_dir,
                        const char* host_dir);

// Find a mountable disc image in a host directory (up to two levels deep): a
// .iso, a raw .bin/.img dump, or the track a .cue sheet names. Candidates that
// do not open as an image are skipped. out receives the full host path.
// Returns 1 when found.
int trt_iso_find_image_in_dir(const char* host_dir, char* out, size_t out_cap);

// Resolve a .cue sheet's first FILE track (the mountable image). Returns 1 and
// fills out_path when the sheet names a track.
int trt_iso_cue_track(const char* cue_path, char* out_path, size_t out_cap);

#if defined(__cplusplus)
}
#endif

#endif  // TRT_ISO_H
