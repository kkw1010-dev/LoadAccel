// Target B: TESObjectREFR::InitItemImpl (ID 19507) calls, for every exterior reference whose file is not a
// master-type file,
//   ID 18216  void Remove(BGSLargeRefData* this, FormID)       walks both cell -> FormID[] maps (ID 18242 twice)
//   ID 18218  void Readd(BGSLargeRefData* this, TESObjectREFR*) lists the reference again through ID 18215
// with this = worldspace + 0x250. All read off the disassembly of SkyrimSE.exe 1.6.1170. The call to 18216
// is wrapped here, and the three calls that change the lists by other means are watched (18215 append, 18214
// RNAM load, 18213 destructor); what happens inside the wrappers is LargeRefCore.h (tested offline by tests/).

#include "LargeRefs.h"

#include "LargeRefCore.h"
#include "Timing.h"

namespace LargeRefs
{
	namespace
	{
		using loadaccel::LargeRefData;
		using loadaccel::LargeRefMode;
		using loadaccel::Now;
		using loadaccel::Seconds;

		constexpr std::uint64_t kRemove = 18216;
		constexpr std::uint64_t kAppend = 18215;
		constexpr std::uint64_t kLoad = 18214;
		constexpr std::uint64_t kDestroy = 18213;

		// TESWorldSpace::largeRefData (the 'this' of all four functions is worldspace + 0x250).
		constexpr std::ptrdiff_t kLargeRefDataOffset = 0x250;

		// The code the analysis read: Address Library ID, offset and size of the stretch, FNV-1a of its bytes in
		// 1.6.1170. If another plugin has patched one of them (or the image differs), nothing is installed.
		// ID 18218 is hashed around its first call, which another plugin hooks (the bytes of that call differ per run).
		struct Code
		{
			std::uint64_t id;
			std::ptrdiff_t offset;
			std::size_t size;
			std::uint64_t hash;
			const char* what;
		};
		constexpr std::array<Code, 6> kCode{ {
			{ 18216, 0, 103, 0x3ABC282E71BDA3CDull, "Remove: the two walks" },
			{ 18242, 0, 253, 0x0D23C00649ABBDC1ull, "the walk over one map" },
			{ 18215, 0, 544, 0x363D2E70771BD423ull, "Append" },
			{ 19507, 2139, 30, 0xB9516772E6788B40ull, "InitItemImpl: formID and worldspace + 0x250 into Remove, then Readd" },
			{ 18218, 0, 86, 0x7887C322ACEB350Eull, "Readd, up to its first call" },
			{ 18218, 90, 282, 0xAFBD41C8E2C44CABull, "Readd, after its first call" },
		} };

		struct Site
		{
			std::uint64_t id;      // Address Library ID of the calling function
			std::ptrdiff_t offset; // offset of the E8 call inside it
			std::uint64_t callee;  // Address Library ID it must call
		};

		// Every direct call to the four functions in the 1.6.1170 image (E8/E9 scan of the whole text section; no
		// stored pointers to them). The destructor is also reached by a jump from an unwind funclet (ID 116391),
		// which only runs when a constructor throws.
		constexpr std::array<Site, 4> kSites{ {
			{ 19507, 2149, kRemove },
			{ 18218, 303, kAppend },
			{ 20453, 1561, kLoad },
			{ 20447, 967, kDestroy },
		} };

		using Remove_t = void(LargeRefData* a_data, std::uint32_t a_formID);
		using Append_t = std::uintptr_t(LargeRefData* a_data, std::uint32_t a_formID, std::uint32_t a_cell, std::uint32_t a_listedCell);
		using Load_t = std::uintptr_t(LargeRefData* a_data, void* a_file);
		using Destroy_t = std::uintptr_t(LargeRefData* a_data);

		REL::Relocation<Remove_t> origRemove;
		REL::Relocation<Append_t> origAppend;
		REL::Relocation<Load_t> origLoad;
		REL::Relocation<Destroy_t> origDestroy;
		std::unique_ptr<loadaccel::LargeRefCore> core;
		Settings settings;
		loadaccel::ThreadPicture threads;

		std::atomic<std::int64_t> firstCallTick{ 0 };
		std::atomic<std::int64_t> lastCallTick{ 0 };
		std::atomic<std::int64_t> nextReportTick{ 0 };
		std::atomic<std::uint32_t> mismatchLines{ 0 };
		// Every removal and append the engine asked for, in order (worldspace FormID, FormID): equal in two launches
		// when the engine did the same work in both. Written by the calling thread only (one thread during the load).
		std::atomic<std::uint64_t> callDigest{ 0 };

		void Digest(const LargeRefData* a_data, std::uint32_t a_formID, std::uint64_t a_kind)
		{
			const auto* worldspace = reinterpret_cast<const RE::TESWorldSpace*>(reinterpret_cast<std::uintptr_t>(a_data) - kLargeRefDataOffset);
			auto digest = callDigest.load(std::memory_order_relaxed);
			digest = (digest ^ ((a_kind << 32) | worldspace->GetFormID())) * 0x9E3779B97F4A7C15ull;
			digest ^= digest >> 32;
			digest = (digest ^ a_formID) * 0x9E3779B97F4A7C15ull;
			digest ^= digest >> 32;
			callDigest.store(digest, std::memory_order_relaxed);
		}

		const char* StageName(int a_stage)
		{
			switch (a_stage) {
			case 1:
				return "count only";
			case 2:
				return "shadow set: the engine walks on every call, the set's answer is compared with a full walk first";
			case 3:
				return "replace: the engine's walk is skipped when the set says the FormID is in no list";
			default:
				return "off";
			}
		}

		std::string Describe(const LargeRefData* a_data)
		{
			const auto* worldspace = reinterpret_cast<const RE::TESWorldSpace*>(reinterpret_cast<std::uintptr_t>(a_data) - kLargeRefDataOffset);
			const char* name = worldspace->GetFormEditorID();
			return std::format("{:08X} {}", worldspace->GetFormID(), name && *name ? name : "(no editor id)");
		}

		void Report(const loadaccel::LargeRefMismatch& a_mismatch)
		{
			if (mismatchLines.fetch_add(1, std::memory_order_relaxed) >= 40) {
				return;
			}
			logs::error("MISMATCH #{} at call {}: FormID {:08X} is in a large-reference list of worldspace {} but not in the set{}", core->Mismatches(),
				a_mismatch.call, a_mismatch.formID, Describe(a_mismatch.data),
				core->GetMode() == LargeRefMode::kReplace ? "; the set is switched off for the rest of the session, the engine walks on every call" : "");
		}

		void MaybeReport(std::int64_t a_now)
		{
			std::int64_t due = nextReportTick.load(std::memory_order_relaxed);
			if (a_now < due || !nextReportTick.compare_exchange_strong(due, a_now + 5 * loadaccel::tickFrequency)) {
				return;
			}
			const auto& stats = core->Stats();
			logs::info("large refs progress +{:.1f} s: calls {}, skipped {}, time in the engine's walk {:.2f} s, in the wrapper {:.2f} s, mismatches {}",
				Seconds(a_now - loadaccel::loadTick), stats.calls.load(), stats.skipped.load(), Seconds(stats.engineTicks.load()), Seconds(stats.ticks.load()),
				core->Mismatches());
		}

		void RemoveThunk(LargeRefData* a_data, std::uint32_t a_formID)
		{
			loadaccel::ThreadPicture::Call call(threads);
			const auto start = Now();
			std::int64_t zero = 0;
			if (firstCallTick.compare_exchange_strong(zero, start)) {
				nextReportTick.store(start + 5 * loadaccel::tickFrequency, std::memory_order_relaxed);
				logs::info("large refs: first call +{:.1f} s after plugin load, thread {}, worldspace {}", Seconds(start - loadaccel::loadTick),
					::GetCurrentThreadId(), Describe(a_data));
			}
			Digest(a_data, a_formID, 1);
			core->Remove(a_data, a_formID, [=] { origRemove(a_data, a_formID); });
			const auto end = Now();
			lastCallTick.store(end, std::memory_order_relaxed);
			MaybeReport(end);
		}

		std::uintptr_t AppendThunk(LargeRefData* a_data, std::uint32_t a_formID, std::uint32_t a_cell, std::uint32_t a_listedCell)
		{
			Digest(a_data, a_formID, 2);
			core->NoteAppend(a_data, a_formID);
			return origAppend(a_data, a_formID, a_cell, a_listedCell);
		}

		std::uintptr_t LoadThunk(LargeRefData* a_data, void* a_file)
		{
			const auto result = origLoad(a_data, a_file);
			core->NoteBulkLoad(a_data);
			return result;
		}

		std::uintptr_t DestroyThunk(LargeRefData* a_data)
		{
			core->Forget(a_data);
			return origDestroy(a_data);
		}

		bool ImageIsTheAnalysedOne(std::array<std::uintptr_t, kSites.size()>& a_sites)
		{
			bool ok = true;
			for (const auto& code : kCode) {
				const auto hash = loadaccel::CodeHash(REL::ID(code.id).address() + code.offset, code.size);
				if (hash != code.hash) {
					logs::error("ID {} + {} ({}) is not the code that was analysed (hash {:016X}, expected {:016X}): another plugin patched it, or the image differs",
						code.id, code.offset, code.what, hash, code.hash);
					ok = false;
				}
			}
			for (std::size_t i = 0; i < kSites.size(); ++i) {
				a_sites[i] = REL::ID(kSites[i].id).address() + kSites[i].offset;
				if (!loadaccel::IsCallTo(a_sites[i], REL::ID(kSites[i].callee).address())) {
					logs::error("call site ID {} + {} is not the expected call to ID {}", kSites[i].id, kSites[i].offset, kSites[i].callee);
					ok = false;
				}
			}
			return ok;
		}
	}

	bool Install(const Settings& a_settings)
	{
		settings = a_settings;
		if (settings.stage < 1 || settings.stage > 3) {
			logs::info("large refs: stage {} = off, nothing installed", settings.stage);
			return true;
		}
		std::array<std::uintptr_t, kSites.size()> address{};
		if (!ImageIsTheAnalysedOne(address)) {
			logs::error("large refs: nothing installed");
			return false;
		}

		core = std::make_unique<loadaccel::LargeRefCore>(static_cast<LargeRefMode>(settings.stage), settings.compareFirst, settings.sampleEvery, &Now, &Report);
		auto& trampoline = SKSE::GetTrampoline();
		origRemove = trampoline.write_call<5>(address[0], RemoveThunk);
		origAppend = trampoline.write_call<5>(address[1], AppendThunk);
		origLoad = trampoline.write_call<5>(address[2], LoadThunk);
		origDestroy = trampoline.write_call<5>(address[3], DestroyThunk);
		logs::info("large refs: stage {} ({}); wrapped the call to ID 18216 in InitItemImpl, watching ID 18215 (append), 18214 (RNAM load), 18213 (destructor){}",
			settings.stage, StageName(settings.stage),
			settings.stage == 3 ? std::format("; first {} skips all checked by a full walk, then 1 in {}", settings.compareFirst, settings.sampleEvery) :
			settings.stage == 1 ? std::format("; 1 call in {} is walked for the statistics", settings.sampleEvery) :
								  std::string());
		return true;
	}

	namespace
	{
		// Every list of every worldspace seen, as text: one line per list, worldspaces by FormID, the filtered map
		// first, cells by key, the FormIDs in list order (0 = emptied slot). Two launches that left the engine's
		// large-reference data in the same state write the same file.
		void Dump(const char* a_when, std::vector<loadaccel::LargeRefCore::WorldReport> a_worlds)
		{
			auto path = SKSE::log::log_directory();
			if (!path) {
				return;
			}
			*path /= std::format("LoadAccel-largerefs-{}.txt", a_when);
			std::ofstream out(*path, std::ios::binary | std::ios::trunc);
			std::sort(a_worlds.begin(), a_worlds.end(), [](const auto& a_left, const auto& a_right) {
				return reinterpret_cast<const RE::TESWorldSpace*>(reinterpret_cast<std::uintptr_t>(a_left.data) - kLargeRefDataOffset)->GetFormID() <
					   reinterpret_cast<const RE::TESWorldSpace*>(reinterpret_cast<std::uintptr_t>(a_right.data) - kLargeRefDataOffset)->GetFormID();
			});
			std::size_t lists = 0;
			std::string line;
			for (const auto& world : a_worlds) {
				const auto name = Describe(world.data);
				int which = 0;
				for (const auto* map : { &world.data->filtered, &world.data->full }) {
					std::vector<const loadaccel::LargeRefMap::Entry*> entries;
					for (std::uint32_t i = 0; map->entries && i < map->capacity; ++i) {
						if (map->entries[i].next && map->entries[i].value) {
							entries.push_back(&map->entries[i]);
						}
					}
					std::sort(entries.begin(), entries.end(), [](const auto* a_left, const auto* a_right) { return a_left->key < a_right->key; });
					for (const auto* entry : entries) {
						line = std::format("{} {} {:08X}:", name, which == 0 ? "filtered" : "full", entry->key);
						for (std::uint32_t k = 1; k <= entry->value[0]; ++k) {
							line += std::format(" {:08X}", entry->value[k]);
						}
						line += '\n';
						out.write(line.data(), static_cast<std::streamsize>(line.size()));
						++lists;
					}
					++which;
				}
			}
			logs::info("  dump: {} lists of {} worldspaces written to {}", lists, a_worlds.size(), path->string());
		}
	}

	void EndOfDataLoad()
	{
		if (core) {
			core->EndOfDataLoad();
			if (core->GetMode() == LargeRefMode::kReplace) {
				logs::info("large refs: data load over; from here on the engine walks on every call, 1 call in {} tests the set", settings.sampleEvery);
			}
		}
	}

	void Summary(const char* a_when)
	{
		if (!core) {
			return;
		}
		const auto worlds = core->Inspect();
		const auto& stats = core->Stats();
		const auto calls = stats.calls.load();
		const auto mismatches = core->Mismatches();
		std::size_t missing = 0;
		for (const auto& world : worlds) {
			missing += world.missing;
		}
		const double set = Seconds(stats.ticks.load() - stats.engineTicks.load() - stats.walkTicks.load());

		switch (core->GetMode()) {
		case LargeRefMode::kCount:
			logs::info("LARGE REFS SUMMARY at {}: stage 1, calls {}, time in the engine's walk {:.2f} s; of {} sampled calls {} had their FormID in a list", a_when,
				calls, Seconds(stats.engineTicks.load()), stats.walked.load(), stats.occurred.load());
			break;
		case LargeRefMode::kShadow:
			logs::info("LARGE REFS SUMMARY at {}: stage 2, calls {}, MISMATCHES {}, time in the engine's walk {:.2f} s of which {:.2f} s on calls the set would "
					   "have skipped (stage 3 would save about that); set overhead {:.2f} s, checking walks {:.2f} s",
				a_when, calls, mismatches, Seconds(stats.engineTicks.load()), Seconds(stats.skippableTicks.load()), set, Seconds(stats.walkTicks.load()));
			break;
		case LargeRefMode::kReplace: {
			const auto sampled = stats.sampled.load();
			const auto skipped = stats.skipped.load();
			const double saved = sampled ? Seconds(stats.sampledTicks.load()) / static_cast<double>(sampled) * static_cast<double>(skipped) : 0.0;
			logs::info("LARGE REFS SUMMARY at {}: stage 3{}, calls {}, engine walks skipped {}, MISMATCHES {}, time spent {:.2f} s (engine {:.2f} s, set {:.2f} s, "
					   "checking walks {:.2f} s), engine time saved about {:.2f} s (extrapolated from {} sampled walks)",
				a_when, core->FellBack() ? " FELL BACK TO THE ENGINE" : "", calls, skipped, mismatches, Seconds(stats.ticks.load()),
				Seconds(stats.engineTicks.load()), set, Seconds(stats.walkTicks.load()), saved, sampled);
			break;
		}
		}

		logs::info("  calls {}: set said \"may be listed\" {} (engine walked), \"in no list\" {}; full walks done to check {} of which {} found the FormID; "
				   "set a superset by {} (said \"may be\", was not)",
			calls, stats.inSet.load(), stats.notInSet.load(), stats.walked.load(), stats.occurred.load(), stats.setWasSuperset.load());
		logs::info("  list changes seen: ID 18215 appends {} ({} into an existing set), ID 18214 RNAM loads {} ({} after the set existed); list walks to fill "
				   "sets {} in {:.3f} s",
			stats.appends.load(), stats.appendsNoted.load(), stats.loads.load(), stats.rewalks.load(), stats.builds.load(), Seconds(stats.buildTicks.load()));
		if (const auto late = stats.lateCalls.load()) {
			logs::info("  after the data load the engine walks on every call: {} calls; {} tested, the set would have skipped {}, wrongly {}", late,
				stats.lateSampled.load(), stats.lateWouldSkip.load(), stats.lateWrong.load());
		}
		const auto first = firstCallTick.load();
		const auto last = lastCallTick.load();
		logs::info("  window +{:.1f} s to +{:.1f} s after plugin load; {} worldspaces", first ? Seconds(first - loadaccel::loadTick) : 0.0,
			last ? Seconds(last - loadaccel::loadTick) : 0.0, worlds.size());
		logs::info("  digest of every removal and append the engine asked for, in order: {:016X}", callDigest.load());
		{
			// The whole of it in one line, then the busiest worldspaces.
			std::size_t cells = 0, listed = 0, zeros = 0, setSize = 0;
			for (const auto& world : worlds) {
				cells += world.cells + world.cellsFiltered;
				listed += world.listed;
				zeros += world.zeros;
				setSize += world.setSize;
			}
			logs::info("  all {} worldspaces: lists now {} (both maps), {} FormIDs listed, {} emptied slots; sets {} FormIDs", worlds.size(), cells, listed, zeros,
				setSize);
			auto busiest = worlds;
			std::sort(busiest.begin(), busiest.end(), [](const auto& a_left, const auto& a_right) { return a_left.calls > a_right.calls; });
			for (std::size_t i = 0; i < busiest.size() && i < 8; ++i) {
				const auto& world = busiest[i];
				logs::info("  worldspace {}: calls {}, \"in no list\" {}, set {} FormIDs; lists now: {} cells ({} filtered), {} FormIDs listed, {} emptied slots, "
						   "{} listed FormIDs missing from the set",
					Describe(world.data), world.calls, world.notInSet, world.setSize, world.cells, world.cellsFiltered, world.listed, world.zeros, world.missing);
			}
		}
		if (settings.dump) {
			Dump(a_when, worlds);
		}
		if (core->GetMode() != LargeRefMode::kCount) {
			logs::info("  audit of every list: {}", missing == 0 ? "CLEAN (every listed FormID is in its worldspace's set)" :
																	"NOT CLEAN (a listed FormID was missing from a set: some code changes the lists unseen; see README)");
		}
		logs::info("  threads: {}; calls that waited for the set lock {}", threads.Describe(calls), core->Contended());
		if (mismatches > 40) {
			logs::info("  {} mismatches in total; only the first 40 are listed above", mismatches);
		}
	}
}
