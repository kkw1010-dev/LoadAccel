// Target A: the engine keeps one global table of distinct source-file lists (TESFileArray, the thing
// TESForm::sourceFiles points at) and searches it linearly in two functions:
//   ID 14580  TESFileArray* FindOrAdd(TESFile** files, std::uint32_t count)
//   ID 14569  TESFileArray* WithFileAdded(TESFileArray* list, TESFile* file)
// Both were read off the disassembly of SkyrimSE.exe 1.6.1170 (research/disasm.py). Their five call sites
// are wrapped here; what happens inside the wrapper is FileListCore.h (tested offline by tests/).

#include "FileLists.h"

#include "FileListCore.h"
#include "Timing.h"

namespace FileLists
{
	namespace
	{
		using loadaccel::FileArray;
		using loadaccel::Mode;
		using loadaccel::Now;
		using loadaccel::Seconds;
		using loadaccel::SiteStats;

		// BSTArray<TESFileArray*>: data pointer at +0x00, element count at +0x10 (read off both functions,
		// and checked against the code at install time).
		constexpr std::uintptr_t kTableData = 0x20FBB68;
		constexpr std::uintptr_t kTableCount = 0x20FBB78;

		constexpr std::uint64_t kFindOrAdd = 14580;
		constexpr std::uint64_t kWithFileAdded = 14569;
		constexpr std::uint64_t kWithoutFile = 14570;

		// The functions the analysis read: Address Library ID, offset and size of the stretch, FNV-1a of its
		// bytes in 1.6.1170. If another plugin has patched one of them (or the image differs), nothing is installed.
		struct Code
		{
			std::uint64_t id;
			std::ptrdiff_t offset;
			std::size_t size;
			std::uint64_t hash;
			const char* what;
		};
		constexpr std::array<Code, 3> kCode{ {
			{ 14580, 0, 360, 0xBCA9CDEDDF7BA66Aull, "FindOrAdd" },
			{ 14569, 0, 391, 0x5295487682E7F584ull, "WithFileAdded" },
			{ 14570, 0, 631, 0xD6EE899FDCD52EA1ull, "WithoutFile (its own inlined search and append)" },
		} };

		struct Site
		{
			std::uint64_t id;      // Address Library ID of the calling function
			std::ptrdiff_t offset; // offset of the E8 call inside it
			std::uint64_t callee;  // Address Library ID it must call
		};

		// Every direct call to the three functions in the 1.6.1170 image (linear sweep of the whole image; no jumps
		// to them, no stored pointers). The first five are answered through the index; ID 14570 does its own search
		// and append and is only put under the same lock.
		constexpr std::size_t kSiteCount = 7;
		constexpr std::size_t kLookupSites = 5;
		constexpr std::array<Site, kSiteCount> kSites{ {
			{ 14570, 230, kFindOrAdd },
			{ 14571, 155, kFindOrAdd },
			{ 14572, 19, kFindOrAdd },
			{ 14593, 469, kWithFileAdded },
			{ 14623, 144, kWithFileAdded },
			{ 14593, 420, kWithoutFile },
			{ 14623, 182, kWithoutFile },
		} };

		using FindOrAdd_t = FileArray*(void** a_files, std::uint32_t a_count);
		using WithFileAdded_t = FileArray*(FileArray* a_list, void* a_file);
		using Core = loadaccel::Core<kSiteCount>;

		std::array<REL::Relocation<FindOrAdd_t>, kSiteCount> origFind;
		std::array<REL::Relocation<WithFileAdded_t>, kSiteCount> origAdd;
		std::array<REL::Relocation<WithFileAdded_t>, kSiteCount> origWithout; // ID 14570 has the same signature as 14569
		std::unique_ptr<Core> core;
		Settings settings;
		loadaccel::ThreadPicture threads;

		std::atomic<std::int64_t> firstCallTick{ 0 };
		std::atomic<std::int64_t> lastCallTick{ 0 };
		std::atomic<std::int64_t> nextReportTick{ 0 };
		std::uint32_t tableAtInstall = 0;
		std::atomic<std::uint32_t> mismatchLines{ 0 };

		const char* StageName(int a_stage)
		{
			switch (a_stage) {
			case 1:
				return "count only";
			case 2:
				return "shadow index: the engine answers every call, the index is compared with it";
			case 3:
				return "replace: the index answers, the engine is asked on misses and on a sample";
			default:
				return "off";
			}
		}

		struct Totals
		{
			std::uint64_t calls = 0, ownList = 0, hits = 0, misses = 0, engineCalls = 0, ticks = 0, engineTicks = 0, checked = 0, checkedTicks = 0, sampled = 0,
						  sampledTicks = 0, keyElements = 0, scannable = 0;
		};

		Totals Sum()
		{
			Totals total;
			for (std::size_t i = 0; i < kLookupSites; ++i) {
				const auto& site = core->Site(i);
				total.calls += site.calls.load(std::memory_order_relaxed);
				total.ownList += site.ownList.load(std::memory_order_relaxed);
				total.hits += site.hits.load(std::memory_order_relaxed);
				total.misses += site.misses.load(std::memory_order_relaxed);
				total.engineCalls += site.engineCalls.load(std::memory_order_relaxed);
				total.ticks += site.ticks.load(std::memory_order_relaxed);
				total.engineTicks += site.engineTicks.load(std::memory_order_relaxed);
				total.checked += site.checked.load(std::memory_order_relaxed);
				total.checkedTicks += site.checkedTicks.load(std::memory_order_relaxed);
				total.sampled += site.sampled.load(std::memory_order_relaxed);
				total.sampledTicks += site.sampledTicks.load(std::memory_order_relaxed);
				total.keyElements += site.keyElements.load(std::memory_order_relaxed);
				total.scannable += site.scannable.load(std::memory_order_relaxed);
			}
			return total;
		}

		// Engine time that the index made unnecessary. Stage 2 measures it directly (every answered call
		// still ran the engine); stage 3 extrapolates from the sampled calls.
		double SecondsAvoidable(const Totals& a_total)
		{
			if (core->GetMode() == Mode::kShadow) {
				return Seconds(a_total.checkedTicks);
			}
			if (core->GetMode() == Mode::kReplace && a_total.sampled) {
				const auto answered = a_total.ownList + a_total.hits;
				const auto unchecked = answered > a_total.checked ? answered - a_total.checked : 0;
				return Seconds(a_total.sampledTicks) / static_cast<double>(a_total.sampled) * static_cast<double>(unchecked);
			}
			return 0.0;
		}

		// The file's name, hashed: the same in every launch. Called with the index lock held.
		std::uint64_t FileIdentity(const void* a_file)
		{
			static std::unordered_map<const void*, std::uint64_t> known;
			if (!a_file) {
				return 0;
			}
			const auto it = known.find(a_file);
			if (it != known.end()) {
				return it->second;
			}
			std::uint64_t hash = 0xcbf29ce484222325ull;
			for (const char* c = static_cast<const RE::TESFile*>(a_file)->fileName; *c; ++c) {
				hash = (hash ^ static_cast<std::uint8_t>(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c)) * 0x100000001b3ull;
			}
			known.emplace(a_file, hash);
			return hash;
		}

		std::string Describe(const FileArray* a_list)
		{
			if (!a_list) {
				return "null";
			}
			std::string text = std::format("{} [{}:", static_cast<const void*>(a_list), a_list->size);
			const auto shown = std::min<std::uint32_t>(a_list->size, 6);
			for (std::uint32_t i = 0; i < shown; ++i) {
				const auto* file = static_cast<const RE::TESFile*>(a_list->data[i]);
				text += std::format(" {}", file ? file->fileName : "null");
			}
			if (shown < a_list->size) {
				text += " ...";
			}
			return text + "]";
		}

		void Report(const loadaccel::Mismatch& a_mismatch)
		{
			static constexpr std::array kKinds{ "own list", "hit", "miss", "table shrank", "indexed slot changed" };
			if (mismatchLines.fetch_add(1, std::memory_order_relaxed) >= 40) {
				return;
			}
			logs::error("MISMATCH #{} ({}) at call {} site {}: list of {} files, index said {}, engine returned {}, table {} -> {}{}",
				core->Mismatches(), kKinds[static_cast<std::size_t>(a_mismatch.kind)], a_mismatch.call, a_mismatch.site, a_mismatch.keyLength,
				Describe(a_mismatch.predicted), Describe(a_mismatch.returned), a_mismatch.countBefore, a_mismatch.countAfter,
				core->GetMode() == Mode::kReplace ? "; the index is switched off for the rest of the session, the engine answers every call" : "");
		}

		// One progress line every five seconds while calls keep coming; written from the calling thread.
		void MaybeReport(std::int64_t a_now)
		{
			std::int64_t due = nextReportTick.load(std::memory_order_relaxed);
			if (a_now < due || !nextReportTick.compare_exchange_strong(due, a_now + 5 * loadaccel::tickFrequency)) {
				return;
			}
			const auto total = Sum();
			logs::info("progress +{:.1f} s: calls {}, table {}, appended {}, time in the engine's two functions {:.2f} s, in the wrappers {:.2f} s, mismatches {}",
				Seconds(a_now - loadaccel::loadTick), total.calls, core->TableCount(), total.misses, Seconds(total.engineTicks), Seconds(total.ticks),
				core->Mismatches());
		}

		struct Scope
		{
			Scope() :
				call(threads)
			{
				const auto start = Now();
				std::int64_t zero = 0;
				if (firstCallTick.compare_exchange_strong(zero, start)) {
					nextReportTick.store(start + 5 * loadaccel::tickFrequency, std::memory_order_relaxed);
					logs::info("first call +{:.1f} s after plugin load, thread {}, table {}", Seconds(start - loadaccel::loadTick), ::GetCurrentThreadId(),
						core->TableCount());
				}
			}

			~Scope()
			{
				const auto end = Now();
				lastCallTick.store(end, std::memory_order_relaxed);
				MaybeReport(end);
			}

			loadaccel::ThreadPicture::Call call;
		};

		template <std::size_t N>
		FileArray* FindThunk(void** a_files, std::uint32_t a_count)
		{
			Scope scope;
			return core->FindOrAdd(N, a_files, a_count, [=] { return origFind[N](a_files, a_count); });
		}

		template <std::size_t N>
		FileArray* AddThunk(FileArray* a_list, void* a_file)
		{
			Scope scope;
			return core->WithFileAdded(N, a_list, a_file, [=] { return origAdd[N](a_list, a_file); });
		}

		template <std::size_t N>
		FileArray* WithoutThunk(FileArray* a_list, void* a_file)
		{
			Scope scope;
			return core->Guarded(N, [=] { return origWithout[N](a_list, a_file); });
		}

		bool ImageIsTheAnalysedOne(std::array<std::uintptr_t, kSiteCount>& a_sites)
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
			for (std::size_t i = 0; i < kSiteCount; ++i) {
				a_sites[i] = REL::ID(kSites[i].id).address() + kSites[i].offset;
				if (!loadaccel::IsCallTo(a_sites[i], REL::ID(kSites[i].callee).address())) {
					logs::error("call site {} (ID {} + {}) is not the expected call to ID {}", i, kSites[i].id, kSites[i].offset, kSites[i].callee);
					ok = false;
				}
			}
			// ID 14580 loads the table's count (+0x29, 6 bytes) and its data pointer (+0x2F, 7 bytes).
			const auto base = REL::Module::get().base();
			const auto find = REL::ID(kFindOrAdd).address();
			if (loadaccel::RipTarget(find + 0x29, 6) != base + kTableCount || loadaccel::RipTarget(find + 0x2F, 7) != base + kTableData) {
				logs::error("ID {} does not read the table at +{:X} / +{:X}", kFindOrAdd, kTableData, kTableCount);
				ok = false;
			}
			return ok;
		}
	}

	bool Install(const Settings& a_settings)
	{
		settings = a_settings;
		if (settings.stage < 1 || settings.stage > 3) {
			logs::info("source-file lists: stage {} = off, nothing installed", settings.stage);
			return true;
		}
		std::array<std::uintptr_t, kSiteCount> address{};
		if (!ImageIsTheAnalysedOne(address)) {
			logs::error("source-file lists: nothing installed");
			return false;
		}

		const auto base = REL::Module::get().base();
		core = std::make_unique<Core>(reinterpret_cast<FileArray** const*>(base + kTableData), reinterpret_cast<const std::uint32_t*>(base + kTableCount),
			static_cast<Mode>(settings.stage), settings.compareFirst, settings.sampleEvery, &Now, &Report, settings.digest ? &FileIdentity : nullptr);
		tableAtInstall = core->TableCount();

		auto& trampoline = SKSE::GetTrampoline();
		origFind[0] = trampoline.write_call<5>(address[0], FindThunk<0>);
		origFind[1] = trampoline.write_call<5>(address[1], FindThunk<1>);
		origFind[2] = trampoline.write_call<5>(address[2], FindThunk<2>);
		origAdd[3] = trampoline.write_call<5>(address[3], AddThunk<3>);
		origAdd[4] = trampoline.write_call<5>(address[4], AddThunk<4>);
		origWithout[5] = trampoline.write_call<5>(address[5], WithoutThunk<5>);
		origWithout[6] = trampoline.write_call<5>(address[6], WithoutThunk<6>);
		logs::info("source-file lists: stage {} ({}); 7 call sites wrapped (3 of ID 14580, 2 of ID 14569, 2 of ID 14570), table {} entries at install{}",
			settings.stage,
			StageName(settings.stage), tableAtInstall,
			settings.stage == 3 ? std::format("; first {} index answers all checked, then 1 in {}", settings.compareFirst, settings.sampleEvery) : "");
		return true;
	}

	void Summary(const char* a_when, bool a_audit)
	{
		if (!core) {
			return;
		}
		const auto snapshot = core->Inspect(a_audit);
		const auto total = Sum();
		const auto first = firstCallTick.load();
		const auto last = lastCallTick.load();
		const auto mismatches = core->Mismatches();
		const auto avoidable = SecondsAvoidable(total);

		// The one line to read.
		switch (core->GetMode()) {
		case Mode::kCount:
			logs::info("SUMMARY at {}: stage 1, calls {}, table {} entries, time in the engine's two functions {:.2f} s", a_when, total.calls, snapshot.tableCount,
				Seconds(total.engineTicks));
			break;
		case Mode::kShadow:
			logs::info("SUMMARY at {}: stage 2, calls {}, MISMATCHES {}, time in the engine's two functions {:.2f} s of which {:.2f} s on calls the index "
					   "could have answered (stage 3 would save about that), index overhead {:.2f} s",
				a_when, total.calls, mismatches, Seconds(total.engineTicks), avoidable, Seconds(total.ticks - total.engineTicks));
			break;
		case Mode::kReplace:
			logs::info("SUMMARY at {}: stage 3{}, calls {}, MISMATCHES {}, time spent {:.2f} s (engine {:.2f} s, index {:.2f} s), engine time saved about {:.2f} s "
					   "(extrapolated from {} sampled calls)",
				a_when, core->FellBack() ? " FELL BACK TO THE ENGINE" : "", total.calls, mismatches, Seconds(total.ticks), Seconds(total.engineTicks),
				Seconds(total.ticks - total.engineTicks), avoidable, total.sampled);
			break;
		}

		logs::info("  calls {}: own list returned {}, found in the table {}, appended {}; engine function ran {} times, {} of them only to check an answer; "
				   "list elements hashed {}, table entries a linear search could visit {}",
			total.calls, total.ownList, total.hits, total.misses, total.engineCalls, total.checked, total.keyElements, total.scannable);
		// Entries that no wrapped call appended: ID 14570 carries its own inlined search and append.
		const std::int64_t outside = core->GetMode() == Mode::kCount ?
										 static_cast<std::int64_t>(snapshot.tableCount) - tableAtInstall - static_cast<std::int64_t>(total.misses) :
										 static_cast<std::int64_t>(snapshot.foreignEntries);
		logs::info("  window +{:.1f} s to +{:.1f} s after plugin load; table {} entries ({} at install), {} of them appended by code outside the wrapped calls; "
				   "index: {} entries read, {} distinct",
			first ? Seconds(first - loadaccel::loadTick) : 0.0, last ? Seconds(last - loadaccel::loadTick) : 0.0, snapshot.tableCount, tableAtInstall, outside,
			snapshot.synced, snapshot.distinct);
		if (settings.digest && core->GetMode() != Mode::kCount) {
			logs::info("  digest of every call's site and list (by file name, in call order) {:016X}; digest of the table {:016X}", snapshot.callDigest,
				snapshot.tableDigest);
		}
		if (snapshot.audited) {
			const auto& audit = snapshot.audit;
			logs::info("  audit of the whole table: {} entries, {} answered by the index with themselves, duplicates {}, wrong-first {}, missing {}, stale {}, "
					   "nulls {}: {}",
				audit.entries, audit.first, audit.duplicates, audit.wrongFirst, audit.missing, audit.stale, audit.nulls,
				audit.Clean() ? "CLEAN" : "NOT CLEAN (the index and the table disagree; see README, Audit)");
		} else if (core->GetMode() != Mode::kCount && a_audit) {
			logs::info("  audit skipped: the index is switched off (fell back)");
		}
		for (std::size_t i = 0; i < kSiteCount; ++i) {
			const auto& site = core->Site(i);
			logs::info("  site {} (ID {} + {}, calls {}): calls {}, own list {}, found {}, appended {}, engine {:.2f} s, wrapper {:.2f} s", i, kSites[i].id,
				kSites[i].offset, kSites[i].callee, site.calls.load(), site.ownList.load(), site.hits.load(), site.misses.load(),
				Seconds(site.engineTicks.load()), Seconds(site.ticks.load()));
		}
		logs::info("  threads: {}; calls that waited for the index lock {}",
			threads.Describe(total.calls + core->Site(5).calls.load() + core->Site(6).calls.load()), core->Contended());
		if (mismatches > 40) {
			logs::info("  {} mismatches in total; only the first 40 are listed above", mismatches);
		}
	}
}
