// LoadAccel: runtime patches for two engine hot spots in the data load (README.md).
// Personal build for SkyrimSE.exe 1.6.1170.0 only; on any other runtime it logs one line and does nothing.
//
// Each target is built in stages (README.md): 1 counts, 2 keeps an index beside the engine and compares
// every call, 3 uses the index and keeps checking a sample. The stages built in are LOADACCEL_STAGE (source-file
// lists) and LOADACCEL_B_STAGE (large references); an optional SKSE\Plugins\LoadAccel.ini can lower or raise
// them without a rebuild.

#include "PCH.h"

#include "FileLists.h"
#include "LargeRefs.h"
#include "Timing.h"

namespace
{
	// The only runtime the offsets and code hashes were read from.
	constexpr REL::Version kRuntime{ 1, 6, 1170, 0 };

	std::filesystem::path IniPath()
	{
		HMODULE module = nullptr;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&IniPath), &module);
		std::array<wchar_t, 1024> buffer{};
		::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
		std::filesystem::path path(buffer.data());
		path.replace_extension(L".ini");
		return path;
	}

	struct Settings
	{
		FileLists::Settings fileLists;
		LargeRefs::Settings largeRefs;
	};

	Settings ReadSettings()
	{
		Settings settings;
		settings.fileLists.stage = LOADACCEL_STAGE;
		settings.fileLists.compareFirst = 4096;
		settings.fileLists.sampleEvery = 64;
		settings.largeRefs.stage = LOADACCEL_B_STAGE;
		settings.largeRefs.compareFirst = 4096;
		settings.largeRefs.sampleEvery = 64;

		const auto path = IniPath();
		std::error_code error;
		const bool found = std::filesystem::exists(path, error);
		if (found) {
			const auto read = [&](const wchar_t* a_section, const wchar_t* a_key, std::uint32_t a_default) {
				return static_cast<std::uint32_t>(::GetPrivateProfileIntW(a_section, a_key, static_cast<int>(a_default), path.c_str()));
			};
			settings.fileLists.stage = static_cast<int>(read(L"SourceFileLists", L"Stage", static_cast<std::uint32_t>(settings.fileLists.stage)));
			settings.fileLists.compareFirst = read(L"SourceFileLists", L"CompareFirst", settings.fileLists.compareFirst);
			settings.fileLists.sampleEvery = std::max(1u, read(L"SourceFileLists", L"SampleEvery", settings.fileLists.sampleEvery));
			settings.fileLists.digest = read(L"SourceFileLists", L"Digest", 0) != 0;
			settings.largeRefs.stage = static_cast<int>(read(L"LargeRefs", L"Stage", static_cast<std::uint32_t>(settings.largeRefs.stage)));
			settings.largeRefs.compareFirst = read(L"LargeRefs", L"CompareFirst", settings.largeRefs.compareFirst);
			settings.largeRefs.sampleEvery = std::max(1u, read(L"LargeRefs", L"SampleEvery", settings.largeRefs.sampleEvery));
			settings.largeRefs.dump = read(L"LargeRefs", L"Dump", 0) != 0;
		}
		logs::info("{}: [SourceFileLists] Stage={}, CompareFirst={}, SampleEvery={}, Digest={}; [LargeRefs] Stage={}, CompareFirst={}, SampleEvery={}, Dump={}",
			found ? "settings from LoadAccel.ini" : "no LoadAccel.ini, built-in settings", settings.fileLists.stage, settings.fileLists.compareFirst,
			settings.fileLists.sampleEvery, settings.fileLists.digest ? 1 : 0, settings.largeRefs.stage, settings.largeRefs.compareFirst,
			settings.largeRefs.sampleEvery, settings.largeRefs.dump ? 1 : 0);
		return settings;
	}

	void Summaries(const char* a_when)
	{
		FileLists::Summary(a_when, true);
		LargeRefs::Summary(a_when);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Summaries("kDataLoaded");
			LargeRefs::EndOfDataLoad();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Summaries("kPostLoadGame");
			break;
		case SKSE::MessagingInterface::kNewGame:
			Summaries("kNewGame");
			break;
		case SKSE::MessagingInterface::kSaveGame:
			// Every save (autosaves included) leaves a summary of what happened since: the gameplay evidence.
			Summaries("kSaveGame");
			break;
		default:
			break;
		}
	}

	void InitializeLog()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= "LoadAccel.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
		logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
		spdlog::set_default_logger(std::move(logger));
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	SKSE::Init(a_skse, false);
	loadaccel::InitClock();

	const auto runtime = a_skse->RuntimeVersion();
	logs::info("LoadAccel {} (built for stages {} / {}), runtime {}", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string(), LOADACCEL_STAGE,
		LOADACCEL_B_STAGE, runtime.string());
	if (runtime != kRuntime) {
		logs::info("runtime is not {}: nothing installed", kRuntime.string());
		return true;
	}

	const auto settings = ReadSettings();
	SKSE::AllocTrampoline(256);
	FileLists::Install(settings.fileLists);
	LargeRefs::Install(settings.largeRefs);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
