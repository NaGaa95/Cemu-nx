// drako_shader_tool: the PC half of tools/drako3d_shader_packs (see ../../README.md).
//
//   drako_shader_tool decompile --title <id> --cache <id>_shaders.bin --work <dir> [--jobs N] [--dump-source]
//     Loads a Cemu transferable shader cache the way the Switch 2 drako3d core loads it at boot (Cemu's own
//     loader, decompiler and options), computes each shader's Deko3D cache key, and translates the GLSL for uam2
//     as the renderer does. Writes <dir>/records.tsv and <dir>/glsl/<key>_<stage>.glsl.
//
//   drako_shader_tool assemble --title <id> --cache <id>_shaders.bin --work <dir> --driver-out <file>
//                              --transferable-out <file>
//     Takes the DKSH images uam2 left in <dir>/dksh/, validates them as the renderer does, and writes the native
//     cache (Cemu's FileCache with the renderer's CDKC records) and the pack's transferable cache (the input
//     without the shaders that have no native code, removed as Cemu removes cache entries).

#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cemu/FileCache/FileCache.h"
#include "util/helpers/StringBuf.h"
#include "tool.h"

#include <glslang/Public/ShaderLang.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>

namespace
{
	struct Options
	{
		std::string mode;
		uint64 titleId = 0;
		bool hasTitle = false;
		fs::path cache;
		fs::path work;
		fs::path driverOut;
		fs::path transferableOut;
		unsigned jobs = 0;
		bool dumpSource = false;
	};

	[[noreturn]] void Fail(const std::string& message)
	{
		std::fprintf(stderr, "drako_shader_tool: %s\n", message.c_str());
		std::exit(1);
	}

	Options ParseOptions(int argc, char** argv)
	{
		Options options;
		if (argc < 2)
			Fail("usage: drako_shader_tool decompile|assemble --title <id> --cache <file> --work <dir> ...");
		options.mode = argv[1];
		for (int i = 2; i < argc; i++)
		{
			const std::string arg = argv[i];
			auto value = [&]() -> std::string {
				if (i + 1 >= argc)
					Fail("missing value for " + arg);
				return argv[++i];
			};
			if (arg == "--title")
			{
				options.titleId = std::stoull(value(), nullptr, 16);
				options.hasTitle = true;
			}
			else if (arg == "--cache")
				options.cache = _utf8ToPath(value());
			else if (arg == "--work")
				options.work = _utf8ToPath(value());
			else if (arg == "--driver-out")
				options.driverOut = _utf8ToPath(value());
			else if (arg == "--transferable-out")
				options.transferableOut = _utf8ToPath(value());
			else if (arg == "--jobs")
				options.jobs = static_cast<unsigned>(std::stoul(value()));
			else if (arg == "--dump-source")
				options.dumpSource = true;
			else
				Fail("unknown option " + arg);
		}
		if (!options.hasTitle || options.cache.empty() || options.work.empty())
			Fail("--title, --cache and --work are required");
		if (options.jobs == 0)
			options.jobs = std::max(1u, std::thread::hardware_concurrency());
		return options;
	}

	std::string Hex64(uint64 value)
	{
		return fmt::format("{:016x}", value);
	}

	std::string HexBytes(const std::vector<uint8>& bytes)
	{
		std::string out;
		out.reserve(bytes.size() * 2);
		for (uint8 b : bytes)
			out += fmt::format("{:02x}", b);
		return out;
	}

	std::vector<uint8> ParseHexBytes(std::string_view text)
	{
		std::vector<uint8> bytes(text.size() / 2);
		for (size_t i = 0; i < bytes.size(); i++)
			bytes[i] = static_cast<uint8>(std::stoul(std::string(text.substr(i * 2, 2)), nullptr, 16));
		return bytes;
	}

	std::string OneLine(std::string text)
	{
		for (char& c : text)
			if (c == '\t' || c == '\r' || c == '\n')
				c = ' ';
		while (!text.empty() && text.back() == ' ')
			text.pop_back();
		return text.empty() ? "-" : text;
	}

	const char* TypeName(RendererShader::ShaderType type)
	{
		switch (type)
		{
		case RendererShader::ShaderType::kVertex: return "vs";
		case RendererShader::ShaderType::kGeometry: return "gs";
		default: return "ps";
		}
	}

	bool WriteFile(const fs::path& path, const void* data, size_t size)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
		return static_cast<bool>(file);
	}

	std::optional<std::vector<uint8>> ReadFile(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return std::nullopt;
		return std::vector<uint8>(std::istreambuf_iterator<char>(file), {});
	}

	// The console opens the transferable cache in LatteShaderCache_Load: with the legacy extra version first, then
	// with the title's. A file matching neither is replaced by an empty cache there, so nothing of it is usable.
	FileCache* OpenTransferableCache(const fs::path& path, uint64 titleId, uint32& extraVersion)
	{
		extraVersion = g_toolGenericExtraVersion;
		if (FileCache* cache = FileCache::Open(path, false, extraVersion))
			return cache;
		extraVersion = LatteShaderCache_getShaderCacheExtraVersion(titleId);
		return FileCache::Open(path, false, extraVersion);
	}

	struct Record
	{
		uint32 index = 0;
		uint64 name1 = 0;
		uint64 name2 = 0;
		std::string type = "-";
		uint64 baseHash = 0;
		uint64 auxHash = 0;
		std::string status;
		uint64 key = 0;
		int stage = -1;
		std::vector<uint8> bindings;
		std::string detail;
		// (decompile only)
		RendererShader::ShaderType shaderType{};
		std::string source;
	};

	std::string GlslName(uint64 key, int stage)
	{
		return fmt::format("{:016x}_{}", key, stage);
	}

	int Decompile(const Options& options)
	{
		ToolInitEmulatorState(options.titleId);
		glslang::InitializeProcess();

		uint32 extraVersion = 0;
		std::unique_ptr<FileCache> cache(OpenTransferableCache(options.cache, options.titleId, extraVersion));
		if (!cache)
			Fail("not a transferable shader cache of this title: " + _pathToUtf8(options.cache));
		cache->UseCompression(false);

		// LatteShaderCache_Load's loading loop, without its UI
		std::vector<Record> records;
		LatteShaderCache_initCompileQueue();
		for (uint32 loadIndex = 0; loadIndex < (uint32)cache->GetMaximumFileIndex(); loadIndex++)
		{
			LatteShaderCache_updateCompileQueue(g_toolCompileQueueSize - 2);
			uint64 name1;
			uint64 name2;
			std::vector<uint8> fileData;
			if (!cache->GetFileByIndex(loadIndex, &name1, &name2, fileData))
				continue;
			Record record;
			record.index = loadIndex;
			record.name1 = name1;
			record.name2 = name2;
			if (fileData.size() >= 17)
			{
				static const char* const kTypes[] = {"vs", "gs", "ps"};
				const uint32 type = (fileData[0] >> 4) & 0xF;
				record.type = type < 3 ? kTypes[type] : "-";
				for (int i = 0; i < 8; i++)
				{
					record.baseHash = (record.baseHash << 8) | fileData[1 + i];
					record.auxHash = (record.auxHash << 8) | fileData[9 + i];
				}
			}
			g_toolCapture = {};
			if (!LatteShaderCache_readSeparableShader(fileData.data(), (sint32)fileData.size()))
			{
				// (the console deletes such an entry from the transferable cache)
				record.status = "invalid";
				record.detail = "LatteShaderCache_readSeparableShader rejected the entry";
			}
			else if (!g_toolCapture.created)
			{
				// (decompiler error or one of LatteShader_CreateRendererShader's filtered shaders)
				record.status = "no_shader";
				record.detail = "no renderer shader is created for this entry";
				// for the record: the key its GLSL would have had (the decompiler's output is kept in that case)
				LatteDecompilerShader* shader = nullptr;
				RendererShader::ShaderType type = RendererShader::ShaderType::kVertex;
				if (record.type == "vs")
					shader = LatteSHRC_FindVertexShader(record.baseHash, record.auxHash);
				else if (record.type == "gs")
				{
					shader = LatteSHRC_FindGeometryShader(record.baseHash, record.auxHash);
					type = RendererShader::ShaderType::kGeometry;
				}
				else if (record.type == "ps")
				{
					shader = LatteSHRC_FindPixelShader(record.baseHash, record.auxHash);
					type = RendererShader::ShaderType::kFragment;
				}
				if (shader && shader->strBuf_shaderSource && shader->strBuf_shaderSource->getLen())
				{
					const std::string source = shader->strBuf_shaderSource->c_str();
					record.detail += fmt::format(" (unused key {:016x}:{})", Deko3D::ToolSourceHash(source, type),
						Deko3D::ToolUamStage(type));
				}
			}
			else
			{
				record.status = "ok";
				record.shaderType = g_toolCapture.type;
				record.type = TypeName(g_toolCapture.type);
				record.baseHash = g_toolCapture.baseHash;
				record.auxHash = g_toolCapture.auxHash;
				record.source = std::move(g_toolCapture.source);
				// (RendererShaderDK -> Compile: the renderer's cache key)
				record.key = Deko3D::ToolSourceHash(record.source, record.shaderType);
				record.stage = Deko3D::ToolUamStage(record.shaderType);
			}
			records.push_back(std::move(record));
		}
		LatteShaderCache_updateCompileQueue(0);

		// a key compiled once serves every entry with the same source (the renderer finds it in its disk cache)
		std::map<std::pair<uint64, int>, size_t> firstByKey;
		std::vector<size_t> unique;
		for (size_t i = 0; i < records.size(); i++)
		{
			Record& record = records[i];
			if (record.status != "ok")
				continue;
			auto [it, inserted] = firstByKey.emplace(std::make_pair(record.key, record.stage), i);
			if (inserted)
				unique.push_back(i);
			else
			{
				record.status = "duplicate";
				record.detail = fmt::format("same key as entry {}", records[it->second].index);
			}
		}

		fs::create_directories(options.work / "glsl");
		if (options.dumpSource)
			fs::create_directories(options.work / "source");

		// Translate (glslang + SPIRV-Cross) in parallel
		std::atomic_size_t next{0};
		auto worker = [&]() {
			for (;;)
			{
				const size_t n = next.fetch_add(1);
				if (n >= unique.size())
					return;
				Record& record = records[unique[n]];
				const std::string name = GlslName(record.key, record.stage);
				if (options.dumpSource)
					WriteFile(options.work / "source" / (name + ".glsl"), record.source.data(), record.source.size());
				std::string glsl, error;
				if (!Deko3D::ToolTranslate(record.shaderType, record.source, glsl, record.bindings, error))
				{
					record.status = "translate_error";
					record.detail = error;
					continue;
				}
				if (!WriteFile(options.work / "glsl" / (name + ".glsl"), glsl.data(), glsl.size()))
				{
					record.status = "translate_error";
					record.detail = "cannot write the translated GLSL";
				}
			}
		};
		std::vector<std::thread> threads;
		for (unsigned i = 0; i < options.jobs; i++)
			threads.emplace_back(worker);
		for (auto& thread : threads)
			thread.join();

		std::string tsv = fmt::format("# drako_shader_tool records v1\n# title={:016x} extra_version={:#x} entries={}\n",
			options.titleId, extraVersion, records.size());
		tsv += "index\tname1\tname2\ttype\tbase\taux\tstatus\tkey\tstage\tbindings\tdetail\n";
		std::map<std::string, size_t> counts;
		for (const Record& record : records)
		{
			const bool keyed = record.stage >= 0;
			tsv += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", record.index, Hex64(record.name1),
				Hex64(record.name2), record.type, Hex64(record.baseHash), Hex64(record.auxHash), record.status,
				keyed ? Hex64(record.key) : "-", keyed ? std::to_string(record.stage) : "-",
				record.bindings.empty() ? "-" : HexBytes(record.bindings), OneLine(record.detail));
			counts[record.status]++;
		}
		if (!WriteFile(options.work / "records.tsv", tsv.data(), tsv.size()))
			Fail("cannot write records.tsv");
		std::string summary = fmt::format("{{\"title\": \"{:016x}\", \"entries\": {}", options.titleId, records.size());
		for (const auto& [status, count] : counts)
			summary += fmt::format(", \"{}\": {}", status, count);
		std::printf("%s}\n", summary.c_str());
		return 0;
	}

	std::vector<Record> ReadRecords(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			Fail("cannot read " + _pathToUtf8(path));
		std::vector<Record> records;
		std::string line;
		bool header = true;
		while (std::getline(file, line))
		{
			if (line.empty() || line[0] == '#')
				continue;
			if (header)
			{
				header = false;
				continue;
			}
			std::vector<std::string> fields;
			size_t start = 0;
			for (;;)
			{
				const size_t tab = line.find('\t', start);
				fields.push_back(line.substr(start, tab - start));
				if (tab == std::string::npos)
					break;
				start = tab + 1;
			}
			if (fields.size() != 11)
				Fail("malformed records.tsv line: " + line);
			Record record;
			record.index = static_cast<uint32>(std::stoul(fields[0]));
			record.name1 = std::stoull(fields[1], nullptr, 16);
			record.name2 = std::stoull(fields[2], nullptr, 16);
			record.type = fields[3];
			record.baseHash = std::stoull(fields[4], nullptr, 16);
			record.auxHash = std::stoull(fields[5], nullptr, 16);
			record.status = fields[6];
			if (fields[7] != "-")
			{
				record.key = std::stoull(fields[7], nullptr, 16);
				record.stage = std::stoi(fields[8]);
			}
			if (fields[9] != "-")
				record.bindings = ParseHexBytes(fields[9]);
			record.detail = fields[10];
			records.push_back(std::move(record));
		}
		return records;
	}

	int Assemble(const Options& options)
	{
		if (options.driverOut.empty() || options.transferableOut.empty())
			Fail("--driver-out and --transferable-out are required");
		ToolInitEmulatorState(options.titleId);
		std::vector<Record> records = ReadRecords(options.work / "records.tsv");

		// native records, in the order the console compiles (and stores) them: the transferable cache's order
		std::map<std::pair<uint64, int>, bool> nativeByKey;
		std::vector<std::pair<std::pair<uint64, int>, std::vector<uint8>>> nativeRecords;
		for (Record& record : records)
		{
			if (record.status != "ok")
				continue;
			const auto key = std::make_pair(record.key, record.stage);
			const auto dksh = ReadFile(options.work / "dksh" / (GlslName(record.key, record.stage) + ".dksh"));
			std::vector<uint8> data;
			std::string error;
			if (!dksh)
			{
				record.status = "compile_error";
				const auto log = ReadFile(options.work / "dksh" / (GlslName(record.key, record.stage) + ".log"));
				record.detail = log ? std::string(log->begin(), log->end()) : std::string("uam2 produced no DKSH");
			}
			else if (!Deko3D::ToolBuildRecord(record.key, record.stage, record.bindings, *dksh, data, error))
			{
				record.status = "invalid_dksh";
				record.detail = error;
			}
			else
			{
				record.status = "native";
				nativeRecords.emplace_back(key, std::move(data));
			}
			nativeByKey[key] = record.status == "native";
		}
		for (Record& record : records)
		{
			if (record.status == "duplicate")
				record.status = nativeByKey[std::make_pair(record.key, record.stage)] ? "native" : "duplicate_excluded";
		}

		// the native cache, written as the renderer writes it (Cemu's FileCache, no compression, CACHE_VERSION)
		std::error_code ec;
		fs::remove(options.driverOut, ec);
		fs::create_directories(options.driverOut.parent_path(), ec);
		{
			std::unique_ptr<FileCache> driver(FileCache::Create(options.driverOut, Deko3D::ToolCacheVersion()));
			if (!driver)
				Fail("cannot create " + _pathToUtf8(options.driverOut));
			driver->UseCompression(false);
			for (const auto& [key, data] : nativeRecords)
				driver->AddFile({key.first, static_cast<uint64>(key.second)}, data.data(), static_cast<sint32>(data.size()));
		}

		// the transferable cache without the entries that have no native code (FileCache::DeleteFile, as the
		// console deletes entries)
		fs::remove(options.transferableOut, ec);
		fs::create_directories(options.transferableOut.parent_path(), ec);
		fs::copy_file(options.cache, options.transferableOut, ec);
		if (ec)
			Fail("cannot copy the transferable cache: " + ec.message());
		size_t kept = 0;
		{
			std::unique_ptr<FileCache> transferable(FileCache::Open(options.transferableOut));
			if (!transferable)
				Fail("cannot open " + _pathToUtf8(options.transferableOut));
			for (const Record& record : records)
			{
				if (record.status == "native")
					kept++;
				else if (!transferable->DeleteFile({record.name1, record.name2}))
					Fail(fmt::format("cannot remove entry {} from the transferable cache", record.index));
			}
		}

		std::string tsv = "index\tname1\tname2\ttype\tbase\taux\tresult\tkey\tstage\tdetail\n";
		std::map<std::string, size_t> counts;
		for (const Record& record : records)
		{
			const bool keyed = record.stage >= 0;
			tsv += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", record.index, Hex64(record.name1),
				Hex64(record.name2), record.type, Hex64(record.baseHash), Hex64(record.auxHash), record.status,
				keyed ? Hex64(record.key) : "-", keyed ? std::to_string(record.stage) : "-", OneLine(record.detail));
			counts[record.status]++;
		}
		if (!WriteFile(options.work / "assembled.tsv", tsv.data(), tsv.size()))
			Fail("cannot write assembled.tsv");
		std::string summary = fmt::format("{{\"title\": \"{:016x}\", \"transferable_in\": {}, \"transferable_out\": {}, "
			"\"native\": {}, \"excluded\": {}", options.titleId, records.size(), kept, nativeRecords.size(),
			records.size() - kept);
		for (const auto& [status, count] : counts)
			summary += fmt::format(", \"{}\": {}", status, count);
		std::printf("%s}\n", summary.c_str());
		return 0;
	}
} // namespace

int main(int argc, char** argv)
{
	const Options options = ParseOptions(argc, argv);
	if (options.mode == "decompile")
		return Decompile(options);
	if (options.mode == "assemble")
		return Assemble(options);
	Fail("unknown mode " + options.mode);
}
