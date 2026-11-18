// uam2_batch: ouncebrew/uam2's compiler compiling many shaders per process, as uam2rt does on the console.
// make_packs.py build-uam2 links it with uam2's own objects from an existing uam2 build tree, and a copy of its
// glsl_frontend.cpp carrying the changes uam2rt (mesa-nx/src/nouveau/uam2rt) has over it.
//
//   uam2_batch <job file>
//
// Each line of the job file is "<stage>\t<input.glsl>\t<output.dksh>\t<log file>" (stage: vert, geom, frag).
// For every job the program prints "begin\t<n>" before compiling and "end\t<n>\tok|fail" after, flushed, so that
// make_packs.py can tell which shader a crash happened on. The compiler's messages go to the job's log file,
// which is removed when the shader compiles.
#include "uam2.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

static bool read_file(const std::string& path, std::vector<char>& data)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	data.assign(size + 1, 0);
	bool ok = fread(data.data(), 1, size, f) == (size_t)size;
	fclose(f);
	return ok;
}

static bool parse_stage(const std::string& name, uam2_stage& stage)
{
	if (name == "vert")
		stage = Uam2Stage_Vertex;
	else if (name == "geom")
		stage = Uam2Stage_Geometry;
	else if (name == "frag")
		stage = Uam2Stage_Fragment;
	else
		return false;
	return true;
}

int main(int argc, char* argv[])
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <job file>\n", argv[0]);
		return EXIT_FAILURE;
	}
	std::vector<char> jobs;
	if (!read_file(argv[1], jobs))
	{
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	uam2_init();
	const int savedStderr = _dup(2);
	unsigned index = 0;
	for (char* line = strtok(jobs.data(), "\r\n"); line; line = strtok(nullptr, "\r\n"), index++)
	{
		std::vector<std::string> fields;
		for (char *field = line, *tab; field; field = tab ? tab + 1 : nullptr)
		{
			tab = strchr(field, '\t');
			fields.emplace_back(field, tab ? tab - field : strlen(field));
		}
		printf("begin\t%u\n", index);
		fflush(stdout);
		bool ok = false;
		uam2_stage stage;
		if (fields.size() == 4 && parse_stage(fields[0], stage))
		{
			// (the compiler reports errors on stderr: the job's log file collects them)
			fflush(stderr);
			const int log = _open(fields[3].c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
			if (log >= 0)
			{
				_dup2(log, 2);
				_close(log);
			}
			std::vector<char> source;
			if (!read_file(fields[1], source))
				fprintf(stderr, "Could not read input file: %s\n", fields[1].c_str());
			else
			{
				uam2_program prog;
				if (uam2_compile_glsl(source.data(), stage, false, &prog))
				{
					ok = uam2_write_dksh(fields[2].c_str(), &prog);
					uam2_program_free(&prog);
				}
			}
			fflush(stderr);
			_dup2(savedStderr, 2);
			if (ok)
				remove(fields[3].c_str());
		}
		printf("end\t%u\t%s\n", index, ok ? "ok" : "fail");
		fflush(stdout);
	}
	uam2_exit();
	return EXIT_SUCCESS;
}
