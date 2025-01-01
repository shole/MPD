// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "output/Interface.hxx"
#include "output/Registry.hxx"
#include "output/OutputPlugin.hxx"
#include "ConfigGlue.hxx"
#include "lib/fmt/AudioFormatFormatter.hxx"
#include "lib/fmt/RuntimeError.hxx"
#include "event/Thread.hxx"
#include "fs/Path.hxx"
#include "fs/NarrowPath.hxx"
#include "pcm/AudioParser.hxx"
#include "pcm/AudioFormat.hxx"
#include "cmdline/OptionDef.hxx"
#include "cmdline/OptionParser.hxx"
#include "io/FileDescriptor.hxx"
#include "util/StringBuffer.hxx"
#include "util/ScopeExit.hxx"
#include "util/StaticFifoBuffer.hxx"
#include "util/PrintException.hxx"
#include "LogBackend.hxx"

#include <cassert>
#include <memory>

#include <string.h>
#include <unistd.h>
#include <stdlib.h>

struct CommandLine {
	FromNarrowPath config_path;

	const char *output_name = nullptr;

	AudioFormat audio_format{44100, SampleFormat::S16, 2};

	bool verbose = false;
};

enum Option {
	OPTION_VERBOSE,
};

static constexpr OptionDef option_defs[] = {
	{"verbose", 'v', false, "Verbose logging"},
};

static CommandLine
ParseCommandLine(int argc, char **argv)
{
	CommandLine c;

	OptionParser option_parser(option_defs, argc, argv);
	while (auto o = option_parser.Next()) {
		switch (Option(o.index)) {
		case OPTION_VERBOSE:
			c.verbose = true;
			break;
		}
	}

	auto args = option_parser.GetRemaining();
	if (args.size() < 2 || args.size() > 3)
		throw std::runtime_error("Usage: run_output CONFIG NAME [FORMAT] <IN");

	c.config_path = args[0];
	c.output_name = args[1];

	if (args.size() > 2)
		c.audio_format = ParseAudioFormat(args[2], false);

	return c;
}

static std::unique_ptr<AudioOutput>
LoadAudioOutput(const ConfigData &config, EventLoop &event_loop,
		const char *name)
{
	const auto *block = config.FindBlock(ConfigBlockOption::AUDIO_OUTPUT,
					     "name", name);
	if (block == nullptr)
		throw FmtRuntimeError("No such configured audio output: {}",
				      name);

	const char *plugin_name = block->GetBlockValue("type");
	if (plugin_name == nullptr)
		throw std::runtime_error("Missing \"type\" configuration");

	const auto *plugin = GetAudioOutputPluginByName(plugin_name);
	if (plugin == nullptr)
		throw FmtRuntimeError("No such audio output plugin: {}",
				      plugin_name);

	return std::unique_ptr<AudioOutput>(ao_plugin_init(event_loop, *plugin,
							   *block));
}

static void
RunOutput(AudioOutput &ao, AudioFormat audio_format,
	  FileDescriptor in_fd)
{
	in_fd.SetBinaryMode();

	/* open the audio output */

	ao.Enable();
	AtScopeExit(&ao) { ao.Disable(); };

	ao.Open(audio_format);
	AtScopeExit(&ao) { ao.Close(); };

	fmt::print(stderr, "audio_format={}\n", audio_format);

	const size_t in_frame_size = audio_format.GetFrameSize();

	/* play */

	StaticFifoBuffer<std::byte, 4096> buffer;

	while (true) {
		{
			const auto dest = buffer.Write();
			assert(!dest.empty());

			ssize_t nbytes = in_fd.Read(dest);
			if (nbytes <= 0)
				break;

			buffer.Append(nbytes);
		}

		auto src = buffer.Read();
		assert(!src.empty());

		src = src.first(src.size() - src.size() % in_frame_size);
		if (src.empty())
			continue;

		size_t consumed = ao.Play(src);

		assert(consumed <= src.size());
		assert(consumed % in_frame_size == 0);

		buffer.Consume(consumed);
	}

	ao.Drain();
}

int main(int argc, char **argv)
try {
	const auto c = ParseCommandLine(argc, argv);
	SetLogThreshold(c.verbose ? LogLevel::DEBUG : LogLevel::INFO);

	/* read configuration file (mpd.conf) */

	const auto config = AutoLoadConfigFile(c.config_path);

	EventThread io_thread;
	io_thread.Start();

	/* initialize the audio output */

	auto ao = LoadAudioOutput(config, io_thread.GetEventLoop(),
				  c.output_name);

	/* do it */

	RunOutput(*ao, c.audio_format, FileDescriptor(STDIN_FILENO));

	/* cleanup and exit */

	return EXIT_SUCCESS;
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
