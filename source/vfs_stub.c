// mGBA needs a file backend to build, but its POSIX one relies on mmap, which lv2 lacks.
// The app does its own file I/O and hands mGBA memory VFiles, so path-based opens just fail.
// shortcut: no real file VFiles or VDirs, add an lv2-backed one if mGBA ever needs to open paths itself (e.g. BIOS).
#include <mgba-util/vfs.h>

struct VFile* VFileOpenFD(const char* path, int flags) {
	(void) path;
	(void) flags;
	return NULL;
}

struct VDir* VDirOpen(const char* path) {
	(void) path;
	return NULL;
}

bool VDirCreate(const char* path) {
	(void) path;
	return false;
}
