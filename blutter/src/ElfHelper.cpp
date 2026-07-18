#include "pch.h"
#include "ElfHelper.h"
PRAGMA_WARNING(push, 0)
#include <platform/elf.h>
#if defined(DART_TARGET_OS_MACOS)
// old dart version has no mach_o.h
//#include <platform/mach_o.h>
#endif
PRAGMA_WARNING(pop)
#include <algorithm>
#include <limits>
#include <stdexcept>
#if defined(_WIN32) || defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
//#include <dlfcn.h>
#include <sys/mman.h>
#endif // #if defined(_WIN32) || defined(WIN32)

struct ElfIdent {
	uint8_t ei_magic[4];
	uint8_t ei_class;
	uint8_t ei_data;
	uint8_t ei_version;
	uint8_t ei_osabi;
	uint8_t ei_abiversion;
	uint8_t pad1[7];
};

using namespace dart::elf;

namespace {

struct MappedFile {
	uint8_t* data;
	size_t size;
};

struct SnapshotLocation {
	uint64_t offset;
	uint64_t size;
	bool found;
};

struct SnapshotOffsets {
	SnapshotLocation vm_snapshot_data;
	SnapshotLocation vm_snapshot_instructions;
	SnapshotLocation isolate_snapshot_data;
	SnapshotLocation isolate_snapshot_instructions;
};

// Flutter AOT libraries are normally far smaller than this. Keep malformed
// virtual addresses from reserving most of the scanner's address space.
constexpr uint64_t kMaxLoadImageSize = 2ULL * 1024 * 1024 * 1024;
constexpr uint64_t kMaxLoadImageExpansion = 256ULL * 1024 * 1024;
constexpr uint64_t kMinimumSnapshotSize = 64;

bool isRangeValid(uint64_t offset, uint64_t size, size_t containerSize)
{
	return offset <= containerSize && size <= containerSize - offset;
}

template<typename T>
const T* checkedTable(const uint8_t* data, size_t dataSize, uint64_t offset, uint64_t count, uint64_t entrySize, const char* name)
{
	if (entrySize != sizeof(T))
		throw std::invalid_argument(std::string("ELF: Invalid ") + name + " entry size");
	if (count > std::numeric_limits<uint64_t>::max() / entrySize || !isRangeValid(offset, count * entrySize, dataSize))
		throw std::invalid_argument(std::string("ELF: Invalid ") + name + " table range");
	return reinterpret_cast<const T*>(data + offset);
}

}

#ifdef _WIN32
static MappedFile load_map_file(const char* path)
{
	HANDLE hFile = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		throw std::runtime_error(std::string("Cannot open ") + path);

	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 || static_cast<uint64_t>(fileSize.QuadPart) > std::numeric_limits<size_t>::max()) {
		CloseHandle(hFile);
		throw std::runtime_error(std::string("Cannot get size of ") + path);
	}

	HANDLE hMapFile = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hMapFile == NULL) {
		CloseHandle(hFile);
		throw std::runtime_error(std::string("Cannot map ") + path);
	}

	void* mem = MapViewOfFile(hMapFile, FILE_MAP_COPY, 0, 0, 0);
	CloseHandle(hMapFile);
	CloseHandle(hFile);
	if (mem == NULL)
		throw std::runtime_error(std::string("Cannot map ") + path);
	return { static_cast<uint8_t*>(mem), static_cast<size_t>(fileSize.QuadPart) };
}

static void unmap_file(const MappedFile& file)
{
	UnmapViewOfFile(file.data);
}

static uint8_t* allocate_image(size_t size)
{
	auto image = static_cast<uint8_t*>(VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (image == NULL)
		throw std::runtime_error("ELF: Cannot allocate load image");
	return image;
}

static void free_image(uint8_t* image, size_t)
{
	VirtualFree(image, 0, MEM_RELEASE);
}
#else
static MappedFile load_map_file(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		throw std::runtime_error(std::string("Cannot open ") + path);

	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0 || static_cast<uint64_t>(st.st_size) > std::numeric_limits<size_t>::max()) {
		close(fd);
		throw std::runtime_error(std::string("Cannot get size of ") + path);
	}
	void* mem = mmap(NULL, static_cast<size_t>(st.st_size), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);

	close(fd);
	if (mem == MAP_FAILED)
		throw std::runtime_error(std::string("Cannot map ") + path);
	return { static_cast<uint8_t*>(mem), static_cast<size_t>(st.st_size) };
}

static void unmap_file(const MappedFile& file)
{
	munmap(file.data, file.size);
}

static uint8_t* allocate_image(size_t size)
{
	void* image = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (image == MAP_FAILED)
		throw std::runtime_error("ELF: Cannot allocate load image");
	return static_cast<uint8_t*>(image);
}

static void free_image(uint8_t* image, size_t size)
{
	munmap(image, size);
}
#endif

static SnapshotOffsets findSnapshotOffsets(const uint8_t* elf, size_t elfSize)
{
	if (elfSize < sizeof(ElfHeader))
		throw std::invalid_argument("ELF: File is too small");
	const auto* hdr = (const ElfHeader*)elf;
	const auto* sections = checkedTable<SectionHeader>(elf, elfSize, hdr->section_table_offset,
		hdr->num_section_headers, hdr->section_table_entry_size, "section");
	const auto* section = sections;
	const auto sh_num = hdr->num_section_headers;

	// find .dynstr and .dynsym sections, so we can map the section names
	const char* dynstr = nullptr;
	size_t dynstr_size = 0;
	const Symbol* dynsym = nullptr;
	const Symbol* dynsym_end = nullptr;
	for (uint16_t i = 0; i < sh_num; i++, section++) {
		if (section->type == SectionHeaderType::SHT_STRTAB && dynstr == nullptr) {
			if (!isRangeValid(section->file_offset, section->file_size, elfSize))
				throw std::invalid_argument("ELF: Invalid string table range");
			// we want only .dynstr for .dynsym
			const char* strtab = (const char*)elf + section->file_offset;
			const char* last = strtab + section->file_size;
			const char* s_first = kVmSnapshotDataAsmSymbol;
			const char* s_last = s_first + strlen(kVmSnapshotDataAsmSymbol) + 1;
			//if (memmem(strtab, section->s_size, kVmSnapshotDataAsmSymbol, strlen(kVmSnapshotDataAsmSymbol))) {
			if (std::search(strtab, last, s_first, s_last) != last) {
				// found it
				dynstr = strtab;
				dynstr_size = section->file_size;
			}
		}
		if (section->type == SectionHeaderType::SHT_DYNSYM) {
			if (section->entry_size != sizeof(Symbol))
				throw std::invalid_argument("ELF: Invalid DYNSYM entry size");
			if (!isRangeValid(section->file_offset, section->file_size, elfSize) || section->file_size % sizeof(Symbol) != 0)
				throw std::invalid_argument("ELF: Invalid DYNSYM table range");
			dynsym = (Symbol*)(elf + section->file_offset);
			dynsym_end = (Symbol*)(elf + section->file_offset + section->file_size);
		}
		if (dynsym != nullptr && dynstr != nullptr)
			break;
	}
	if (dynstr == nullptr || dynsym == nullptr)
		throw std::invalid_argument("ELF: Cannot find dynamic symbols");

	// find the required symbol addresses
	SnapshotOffsets offsets{};
	for (; dynsym < dynsym_end; dynsym++) {
		if (dynsym->info == 0)
			continue;
		if (dynsym->name >= dynstr_size || memchr(dynstr + dynsym->name, '\0', dynstr_size - dynsym->name) == nullptr)
			throw std::invalid_argument("ELF: Invalid dynamic symbol name");

		const char* name = dynstr + dynsym->name;
		if (strcmp(name, kVmSnapshotDataAsmSymbol) == 0) {
			offsets.vm_snapshot_data = { dynsym->value, dynsym->size, true };
		}
		else if (strcmp(name, kVmSnapshotInstructionsAsmSymbol) == 0) {
			offsets.vm_snapshot_instructions = { dynsym->value, dynsym->size, true };
		}
		else if (strcmp(name, kIsolateSnapshotDataAsmSymbol) == 0) {
			offsets.isolate_snapshot_data = { dynsym->value, dynsym->size, true };
		}
		else if (strcmp(name, kIsolateSnapshotInstructionsAsmSymbol) == 0) {
			offsets.isolate_snapshot_instructions = { dynsym->value, dynsym->size, true };
		}
	}

	if (!offsets.vm_snapshot_data.found)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Data");
	if (!offsets.vm_snapshot_instructions.found)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Instructions");
	if (!offsets.isolate_snapshot_data.found)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Data");
	if (!offsets.isolate_snapshot_instructions.found)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Instructions");

	return offsets;
}

LibAppInfo ElfHelper::MapLibAppSo(const char* path)
{
	const auto file = load_map_file(path);
	// quick and dirty parsing ELF to get symbol addresses
	uint8_t* elf = file.data;
#if defined(DART_TARGET_OS_MACOS)
	// Note: only new dart version getting snapshots from load command
	// <=2.17, use EXPORT name
	// <= v2.18,  load from sub SEGMENT_64, named "__CUSTOM" and section named "__dart_app_snap"
	// >= 2.19, LC_NOTE command is used
	auto header = (dart::mach_o::mach_header_64*)lib;
	switch (header->magic) {
	case dart::mach_o::MH_MAGIC:
	case dart::mach_o::MH_CIGAM:
		throw std::invalid_argument("Mach-O: Support only 64 bits");
	case dart::mach_o::MH_CIGAM_64:
		throw std::invalid_argument("Mach-O: Expected a host endian header");
	case dart::mach_o::MH_MAGIC_64:
		return size >= sizeof(mach_o::mach_header_64);
	default:
		throw std::invalid_argument("Mach-O: Invalid magic header");
	}
#else
	if (file.size < sizeof(ElfHeader)) {
		unmap_file(file);
		throw std::invalid_argument("ELF: File is too small");
	}
	const auto* hdr = (ElfHeader*)elf;
	const auto* ident = (ElfIdent*)hdr->ident;
	if (memcmp(ident->ei_magic, "\x7f" "ELF", 4) != 0) {
		unmap_file(file);
		throw std::invalid_argument("ELF: Invalid magic header"); // need ELF file
	}
	if (ident->ei_data != 1) {
		unmap_file(file);
		throw std::invalid_argument("ELF: Support only little endian"); // expect little-endian
	}

	if (ident->ei_class != ELFCLASS64) { // 1 is 32 bits, 2 is 64 bits
		unmap_file(file);
		throw std::invalid_argument("ELF: Support only 64 bits"); // support only 64 bits
	}
	// expected e_machine
	//   3: x86, 0x28: ARM
	//   0x3e: x86-64, 0xB7: Aarch64
	// EM_386, EM_ARM, EM_X86_64, EM_AARCH64
	//hdr->e_machine;
#endif

	try {
		const auto offsets = findSnapshotOffsets(elf, file.size);
		const auto* programs = checkedTable<ProgramHeader>(elf, file.size, hdr->program_table_offset,
			hdr->num_program_headers, hdr->program_table_entry_size, "program");

		uint64_t imageSize64 = 0;
		bool hasLoadSegment = false;
		bool canUseFileMapping = true;
		for (uint16_t i = 0; i < hdr->num_program_headers; i++) {
			const auto& program = programs[i];
			if (program.type != ProgramHeaderType::PT_LOAD)
				continue;
			hasLoadSegment = true;
			if (program.memory_size < program.file_size ||
				!isRangeValid(program.file_offset, program.file_size, file.size) ||
				program.memory_offset > std::numeric_limits<uint64_t>::max() - program.memory_size)
				throw std::invalid_argument("ELF: Invalid load segment range");
			imageSize64 = std::max(imageSize64, program.memory_offset + program.memory_size);
			if (program.file_offset != program.memory_offset || program.file_size != program.memory_size)
				canUseFileMapping = false;
		}
		if (!hasLoadSegment || imageSize64 == 0 || imageSize64 > std::numeric_limits<size_t>::max() ||
			imageSize64 > kMaxLoadImageSize ||
			(imageSize64 > file.size && imageSize64 - file.size > kMaxLoadImageExpansion))
			throw std::invalid_argument("ELF: Invalid load image size");

		auto checkSnapshotLocation = [programs, hdr](const SnapshotLocation& location) {
			const auto requiredSize = std::max(location.size, kMinimumSnapshotSize);
			for (uint16_t i = 0; i < hdr->num_program_headers; i++) {
				const auto& program = programs[i];
				if (program.type != ProgramHeaderType::PT_LOAD || location.offset < program.memory_offset)
					continue;
				const auto relativeOffset = location.offset - program.memory_offset;
				if (relativeOffset <= program.file_size && requiredSize <= program.file_size - relativeOffset)
					return;
			}
			throw std::invalid_argument("ELF: Snapshot symbol is outside a load segment");
		};
		checkSnapshotLocation(offsets.vm_snapshot_data);
		checkSnapshotLocation(offsets.vm_snapshot_instructions);
		checkSnapshotLocation(offsets.isolate_snapshot_data);
		checkSnapshotLocation(offsets.isolate_snapshot_instructions);

		if (canUseFileMapping && imageSize64 <= file.size) {
			return LibAppInfo{
				.lib = elf,
				.vm_snapshot_data = elf + offsets.vm_snapshot_data.offset,
				.vm_snapshot_instructions = elf + offsets.vm_snapshot_instructions.offset,
				.isolate_snapshot_data = elf + offsets.isolate_snapshot_data.offset,
				.isolate_snapshot_instructions = elf + offsets.isolate_snapshot_instructions.offset,
			};
		}

		// Symbol values are virtual addresses, not file offsets. Recreate the
		// loader's address layout so differently aligned text and BSS segments
		// remain at the relative addresses encoded in the Dart image header.
		const auto imageSize = static_cast<size_t>(imageSize64);
		auto image = allocate_image(imageSize);
		try {
			for (uint16_t i = 0; i < hdr->num_program_headers; i++) {
				const auto& program = programs[i];
				if (program.type == ProgramHeaderType::PT_LOAD && program.file_size != 0) {
					memcpy(image + program.memory_offset, elf + program.file_offset,
						static_cast<size_t>(program.file_size));
				}
			}
		}
		catch (...) {
			free_image(image, imageSize);
			throw;
		}

		unmap_file(file);
		return LibAppInfo{
			.lib = image,
			.vm_snapshot_data = image + offsets.vm_snapshot_data.offset,
			.vm_snapshot_instructions = image + offsets.vm_snapshot_instructions.offset,
			.isolate_snapshot_data = image + offsets.isolate_snapshot_data.offset,
			.isolate_snapshot_instructions = image + offsets.isolate_snapshot_instructions.offset,
		};
	}
	catch (...) {
		unmap_file(file);
		throw;
	}
}
