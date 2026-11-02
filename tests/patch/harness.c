// ROM patching check (tools/patch_test.sh): applies each case of <dir>/cases.txt (made by
// tools/make_patch_tests.py) with mGBA's patch code, the way source/rompatch.c does, and compares
// the result with the expected file, or checks that the patch is refused.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba-util/crc32.h>
#include <mgba-util/patch.h>
#include <mgba-util/vfs.h>

// What vfs-mem.c needs from the rest of mGBA.
void* anonymousMemoryMap(size_t size) {
	return calloc(1, size);
}

void mappedMemoryFree(void* memory, size_t size) {
	(void) size;
	free(memory);
}

ssize_t VFileReadline(struct VFile* vf, char* buffer, size_t size) {
	(void) vf, (void) buffer, (void) size;
	return -1; // vfs.c's, which the patch code never calls
}

static void* read_file(const char* dir, const char* name, size_t* size) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = malloc(*size ? *size : 1);
	if (data && fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

// The result, or NULL with the reason the patch was refused.
static void* apply(const void* rom, size_t rom_size, const void* patch_data, size_t patch_size, size_t* out_size,
                   const char** why) {
	struct VFile* vf = VFileFromConstMemory(patch_data, patch_size);
	struct Patch patch;
	void* out = NULL;
	*why = NULL;
	if (!loadPatch(vf, &patch)) {
		*why = "not a patch, or damaged";
	} else if (!(*out_size = patch.outputSize(&patch, rom_size))) {
		*why = "made for another size";
	} else if (!(out = calloc(1, *out_size))) { // UPS leaves bytes past the input to the caller
		*why = "out of memory";
	} else if (!patch.applyPatch(&patch, rom, rom_size, out, *out_size)) {
		*why = "checksum or format error";
		free(out);
		out = NULL;
	}
	vf->close(vf);
	return out;
}

int main(int argc, char** argv) {
	const char* dir = argc > 1 ? argv[1] : ".";
	char path[1024], line[1024];
	snprintf(path, sizeof(path), "%s/cases.txt", dir);
	FILE* cases = fopen(path, "r");
	if (!cases) {
		fprintf(stderr, "no %s\n", path);
		return 1;
	}
	int failed = 0, total = 0;
	while (fgets(line, sizeof(line), cases)) {
		char* rom_name = strtok(line, "\t\n");
		char* patch_name = strtok(NULL, "\t\n");
		char* expected_name = strtok(NULL, "\t\n");
		if (!rom_name || !patch_name || !expected_name) {
			continue;
		}
		++total;
		size_t rom_size, patch_size, out_size = 0, expected_size = 0;
		void* rom = read_file(dir, rom_name, &rom_size);
		void* patch = read_file(dir, patch_name, &patch_size);
		void* expected = strcmp(expected_name, "-") ? read_file(dir, expected_name, &expected_size) : NULL;
		const char* why = "";
		void* out = rom && patch ? apply(rom, rom_size, patch, patch_size, &out_size, &why) : NULL;
		char result[160];
		if (!rom || !patch || (strcmp(expected_name, "-") && !expected)) {
			snprintf(result, sizeof(result), "FAIL: missing file");
		} else if (!expected) {
			snprintf(result, sizeof(result), out ? "FAIL: accepted, should be refused" : "ok: refused (%s)", why);
		} else if (!out) {
			snprintf(result, sizeof(result), "FAIL: refused (%s)", why);
		} else if (out_size != expected_size || memcmp(out, expected, out_size) != 0) {
			snprintf(result, sizeof(result), "FAIL: wrong result (%zu bytes, crc32 %08x; expected %zu, %08x)", out_size,
			         doCrc32(out, out_size), expected_size, doCrc32(expected, expected_size));
		} else {
			snprintf(result, sizeof(result), "ok: %zu bytes, crc32 %08x", out_size, doCrc32(out, out_size));
		}
		failed += result[0] == 'F';
		printf("%s + %s: %s\n", rom_name, patch_name, result);
		free(rom);
		free(patch);
		free(expected);
		free(out);
	}
	fclose(cases);
	printf("%d of %d cases passed\n", total - failed, total);
	return failed || !total;
}
