// Included at the end of gen_dkshader.cpp, inside namespace Deko3D: the tool's entry points into the definitions
// extracted from DKShader.cpp (which live in its anonymous namespace there).

static_assert(sizeof(CacheHeader) == 416, "CDKC record header layout changed");

uint64 ToolSourceHash(std::string_view source, RendererShader::ShaderType type)
{
	return SourceHash(source, type);
}

int ToolUamStage(RendererShader::ShaderType type)
{
	return UamStageOf(type);
}

uint32 ToolCacheVersion()
{
	return CACHE_VERSION;
}

size_t ToolBindingsSize()
{
	return sizeof(ShaderBindings);
}

bool ToolTranslate(RendererShader::ShaderType type, const std::string& source, std::string& glsl,
				   std::vector<uint8>& bindingsOut, std::string& error)
{
	try
	{
		ShaderBindings bindings;
		glsl = Translate(type, source, bindings);
		bindingsOut.resize(sizeof(bindings));
		std::memcpy(bindingsOut.data(), &bindings, sizeof(bindings));
		return true;
	} catch (const std::exception& exception)
	{
		error = exception.what();
		return false;
	}
}

// The record StoreCachedBinary writes for a compiled shader, checked as Compile() checks the compiler's output and
// as LoadCachedBinary checks the record when the console reads it back.
bool ToolBuildRecord(uint64 hash, int stage, const std::vector<uint8>& bindingBytes, const std::vector<uint8>& binary,
					 std::vector<uint8>& record, std::string& error)
{
	ShaderBindings bindings;
	if (bindingBytes.size() != sizeof(bindings))
	{
		error = "binding table has the wrong size";
		return false;
	}
	std::memcpy(&bindings, bindingBytes.data(), sizeof(bindings));
	// (Compile)
	u32 scratchSize = 0;
	if (!ValidateBinary(binary, stage, &scratchSize))
	{
		error = scratchSize > PER_WARP_SCRATCH_MEMORY_SIZE
					? fmt::format("Shader needs {} bytes of scratch per warp; the queue has {}", scratchSize, PER_WARP_SCRATCH_MEMORY_SIZE)
					: std::string("Invalid DKSH shader binary");
		return false;
	}
	// (StoreCachedBinary)
	CacheHeader header{};
	header.stage = stage;
	header.size = static_cast<u32>(binary.size());
	header.sourceHash = hash;
	header.bindings = bindings;
	record.assign(sizeof(header) + binary.size(), 0);
	std::memcpy(record.data(), &header, sizeof(header));
	// (the bytes after the binding table are structure padding: zero, as in the Switch packs)
	const size_t used = offsetof(CacheHeader, bindings) + sizeof(ShaderBindings);
	std::memset(record.data() + used, 0, sizeof(header) - used);
	std::memcpy(record.data() + sizeof(header), binary.data(), binary.size());
	// (LoadCachedBinary)
	CacheHeader stored;
	std::memcpy(&stored, record.data(), sizeof(stored));
	const std::span<const u8> storedBinary(record.data() + sizeof(stored), record.size() - sizeof(stored));
	if (stored.magic != CacheHeader{}.magic || stored.version != CACHE_VERSION ||
		stored.stage != static_cast<u32>(stage) || stored.sourceHash != hash ||
		!stored.size || stored.size > MAX_BINARY_SIZE || stored.size != storedBinary.size() ||
		!ValidateBinary(storedBinary, stage) || !ValidateBindings(stored.bindings))
	{
		error = "the record fails LoadCachedBinary's checks";
		return false;
	}
	return true;
}
