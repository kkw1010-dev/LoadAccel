// Target A core: an index over the engine's table of distinct source-file lists.
//
// No engine or SKSE dependency: the plugin (FileLists.cpp) and the offline test (tests/filelist_test.cpp)
// both drive this file, the test against a model of the engine functions written from the disassembly.
//
// What the engine does (SkyrimSE.exe 1.6.1170, IDs 14580 / 14569): the table is a BSTArray<TESFileArray*>.
// "Find this list or add it" walks the whole table and compares every list element by element; the first
// entry with the same content is returned, and when there is none a new entry is appended.
//
// What this does instead: a hash of the list content finds the candidate entry; the candidate's current
// content is compared element by element before it is returned. On a miss the ENGINE's own function runs
// and does the append, so nothing here ever writes engine memory. Entries appended by code that does not
// pass through the wrapped call sites (ID 14570 carries its own inlined search and append) are picked up
// by Sync(), which indexes the table's tail before every lookup.
//
// Why a hit is the engine's answer: the engine returns the first table entry whose content equals the
// key. A hit returns an entry of the table whose content equals the key right now. The two can differ
// only when two table entries have equal content, which find-or-add never produces; Audit() counts such
// duplicates. An entry the index does not know (changed in place after it was added) turns into a miss,
// and a miss is answered by the engine itself.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace loadaccel
{
	struct FileArray  // TESFileArray = BSStaticArray<TESFile*>: { TESFile** data; std::uint32_t size; }
	{
		void** data;
		std::uint32_t size;
	};

	// A list given either whole (ID 14580: files, count) or as "this list plus one file" (ID 14569).
	struct Key
	{
		void* const* head = nullptr;
		std::uint32_t headCount = 0;
		void* tail = nullptr;
		bool hasTail = false;

		[[nodiscard]] std::uint32_t Length() const { return headCount + (hasTail ? 1u : 0u); }
	};

	namespace detail
	{
		[[nodiscard]] inline std::uint64_t Mix(std::uint64_t a_hash, std::uint64_t a_value)
		{
			a_hash ^= a_value;
			a_hash *= 0x9E3779B97F4A7C15ull;
			a_hash ^= a_hash >> 32;
			return a_hash;
		}

		[[nodiscard]] inline std::uint64_t Finish(std::uint64_t a_hash, std::uint32_t a_length)
		{
			a_hash ^= a_length;
			a_hash *= 0xBF58476D1CE4E5B9ull;
			a_hash ^= a_hash >> 29;
			return a_hash;
		}

		[[nodiscard]] inline std::uint64_t Hash(void* const* a_values, std::uint32_t a_count, std::uint64_t a_hash = 0x51ED270B0C3A4D2Full)
		{
			for (std::uint32_t i = 0; i < a_count; ++i) {
				a_hash = Mix(a_hash, reinterpret_cast<std::uint64_t>(a_values[i]));
			}
			return a_hash;
		}
	}

	[[nodiscard]] inline std::uint64_t HashKey(const Key& a_key)
	{
		auto hash = detail::Hash(a_key.head, a_key.headCount);
		if (a_key.hasTail) {
			hash = detail::Mix(hash, reinterpret_cast<std::uint64_t>(a_key.tail));
		}
		return detail::Finish(hash, a_key.Length());
	}

	[[nodiscard]] inline std::uint64_t HashEntry(const FileArray& a_entry)
	{
		return detail::Finish(detail::Hash(a_entry.data, a_entry.size), a_entry.size);
	}

	[[nodiscard]] inline bool Equal(const FileArray& a_entry, const Key& a_key)
	{
		if (a_entry.size != a_key.Length()) {
			return false;
		}
		if (a_key.headCount != 0 && std::memcmp(a_entry.data, a_key.head, static_cast<std::size_t>(a_key.headCount) * sizeof(void*)) != 0) {
			return false;
		}
		return !a_key.hasTail || a_entry.data[a_key.headCount] == a_key.tail;
	}

	[[nodiscard]] inline bool Equal(const FileArray& a_left, const FileArray& a_right)
	{
		return a_left.size == a_right.size &&
			   (a_left.size == 0 || std::memcmp(a_left.data, a_right.data, static_cast<std::size_t>(a_left.size) * sizeof(void*)) == 0);
	}

	// First part of ID 14569: the file is already in the list, so the list itself is the answer.
	[[nodiscard]] inline bool Contains(const FileArray& a_list, const void* a_file)
	{
		for (std::uint32_t i = 0; i < a_list.size; ++i) {
			if (a_list.data[i] == a_file) {
				return true;
			}
		}
		return false;
	}

	// Hash index over the table's first Synced() entries. For each distinct content it holds the first
	// table entry with that content (the one the engine's scan would return).
	class Index
	{
	public:
		enum class SyncResult
		{
			kOk,
			kShrank,       // the table has fewer entries than were indexed
			kPrefixChanged // the last indexed slot no longer holds the entry that was indexed from it
		};

		struct AuditResult
		{
			std::uint32_t entries = 0;    // table entries looked at
			std::uint32_t first = 0;      // entries the index returns for their own content
			std::uint32_t duplicates = 0; // entries whose content equals an earlier entry's (the index returns the earlier one)
			std::uint32_t wrongFirst = 0; // the index returns a LATER entry for this content: the engine would return this one
			std::uint32_t missing = 0;    // not reachable through the index by their current content (changed in place)
			std::uint32_t stale = 0;      // indexed entries whose content no longer matches the hash they were stored under
			std::uint32_t nulls = 0;      // null table slots

			[[nodiscard]] bool Clean() const { return duplicates == 0 && wrongFirst == 0 && missing == 0 && stale == 0 && nulls == 0; }
		};

		[[nodiscard]] SyncResult Sync(FileArray* const* a_table, std::uint32_t a_count)
		{
			if (a_count < synced_) {
				return SyncResult::kShrank;
			}
			if (synced_ != 0 && a_table[synced_ - 1] != lastSynced_) {
				return SyncResult::kPrefixChanged;
			}
			for (; synced_ < a_count; ++synced_) {
				auto* entry = a_table[synced_];
				lastSynced_ = entry;
				if (!entry) {
					++nulls_;
				} else if (!Insert(entry)) {
					++duplicates_;
				}
			}
			return SyncResult::kOk;
		}

		[[nodiscard]] FileArray* Find(const Key& a_key) const
		{
			if (slots_.empty()) {
				return nullptr;
			}
			const auto hash = HashKey(a_key);
			const auto mask = slots_.size() - 1;
			for (auto i = hash & mask;; i = (i + 1) & mask) {
				const auto& slot = slots_[i];
				if (!slot.entry) {
					return nullptr;
				}
				if (slot.hash == hash && Equal(*slot.entry, a_key)) {
					return slot.entry;
				}
			}
		}

		// Walks the whole table and checks it against the index. Linear in the table; call it rarely.
		[[nodiscard]] AuditResult Audit(FileArray* const* a_table, std::uint32_t a_count) const
		{
			AuditResult result;
			std::unordered_map<const FileArray*, std::uint32_t> position;
			position.reserve(a_count);
			for (std::uint32_t i = 0; i < a_count; ++i) {
				++result.entries;
				const auto* entry = a_table[i];
				if (!entry) {
					++result.nulls;
					continue;
				}
				position.emplace(entry, i);
				Key key;
				key.head = entry->data;
				key.headCount = entry->size;
				const auto* found = Find(key);
				if (!found) {
					++result.missing;
				} else if (found == entry) {
					++result.first;
				} else {
					const auto it = position.find(found);
					if (it != position.end() && it->second < i) {
						++result.duplicates;
					} else {
						++result.wrongFirst;
					}
				}
			}
			for (const auto& slot : slots_) {
				if (slot.entry && HashEntry(*slot.entry) != slot.hash) {
					++result.stale;
				}
			}
			return result;
		}

		[[nodiscard]] std::uint32_t Synced() const { return synced_; }
		[[nodiscard]] std::size_t Distinct() const { return used_; }
		[[nodiscard]] std::uint32_t Duplicates() const { return duplicates_; }
		[[nodiscard]] std::uint32_t Nulls() const { return nulls_; }

	private:
		struct Slot
		{
			std::uint64_t hash = 0;
			FileArray* entry = nullptr;
		};

		// False when an entry with the same content is already indexed (the earlier one is kept).
		bool Insert(FileArray* a_entry)
		{
			if ((used_ + 1) * 2 > slots_.size()) {
				Grow();
			}
			const auto hash = HashEntry(*a_entry);
			const auto mask = slots_.size() - 1;
			for (auto i = hash & mask;; i = (i + 1) & mask) {
				auto& slot = slots_[i];
				if (!slot.entry) {
					slot.hash = hash;
					slot.entry = a_entry;
					++used_;
					return true;
				}
				if (slot.hash == hash && Equal(*slot.entry, *a_entry)) {
					return false;
				}
			}
		}

		void Grow()
		{
			std::vector<Slot> old;
			old.swap(slots_);
			slots_.resize(old.empty() ? 1024 : old.size() * 2);
			const auto mask = slots_.size() - 1;
			for (const auto& slot : old) {
				if (!slot.entry) {
					continue;
				}
				// The stored hash is kept: an entry changed in place stays where it was first indexed.
				auto i = slot.hash & mask;
				while (slots_[i].entry) {
					i = (i + 1) & mask;
				}
				slots_[i] = slot;
			}
		}

		std::vector<Slot> slots_;
		std::size_t used_ = 0;
		std::uint32_t synced_ = 0;
		FileArray* lastSynced_ = nullptr;
		std::uint32_t duplicates_ = 0;
		std::uint32_t nulls_ = 0;
	};

	enum class Mode
	{
		kCount = 1,  // call the engine, count
		kShadow = 2, // call the engine on every call and compare the index's prediction with its answer
		kReplace = 3 // answer hits from the index; call the engine on misses and on a sample of hits
	};

	enum class MismatchKind
	{
		kOwnList,       // ID 14569 returned something else than its own list although the file is in it
		kHit,           // the index predicted an entry, the engine returned another one or changed the table
		kMiss,          // the index predicted "not in the table", but the engine did not append exactly this list
		kTableShrank,   // Sync: fewer entries than indexed
		kPrefixChanged, // Sync: an indexed slot holds another entry now
	};

	struct Mismatch
	{
		MismatchKind kind;
		std::size_t site;
		std::uint64_t call;
		std::uint32_t keyLength;
		const FileArray* predicted;
		const FileArray* returned;
		std::uint32_t countBefore;
		std::uint32_t countAfter;
	};

	struct SiteStats
	{
		std::atomic<std::uint64_t> calls{ 0 };
		std::atomic<std::uint64_t> ownList{ 0 };       // ID 14569: the file was already in the list
		std::atomic<std::uint64_t> hits{ 0 };          // the list was in the table
		std::atomic<std::uint64_t> misses{ 0 };        // the engine appended it
		std::atomic<std::uint64_t> engineCalls{ 0 };   // calls passed on to the engine's function
		std::atomic<std::uint64_t> ticks{ 0 };         // whole wrapper
		std::atomic<std::uint64_t> engineTicks{ 0 };   // inside the engine's function
		std::atomic<std::uint64_t> checked{ 0 };       // answers known without the engine that were checked against it anyway
		std::atomic<std::uint64_t> checkedTicks{ 0 };  // engine time of those: in kShadow, what kReplace would not spend
		std::atomic<std::uint64_t> sampledTicks{ 0 };  // kReplace: engine time of the one-in-N sampled answers, the basis of the saving estimate
		std::atomic<std::uint64_t> sampled{ 0 };       // kReplace: answers sampled one in N (the compare-first stretch is not counted here)
		std::atomic<std::uint64_t> keyElements{ 0 };   // list elements hashed
		std::atomic<std::uint64_t> scannable{ 0 };     // table size at entry, summed: what a linear scan could visit
	};

	template <std::size_t Sites>
	class Core
	{
	public:
		using Clock = std::int64_t (*)();
		using Report = void (*)(const Mismatch&);
		// A number that names a file the same way in every launch (the pointer does not). Optional: with it the
		// core keeps a digest of every call's list, in call order, and Inspect() digests the table. Two launches
		// with equal digests asked for the same lists in the same order and ended with the same table.
		using Identity = std::uint64_t (*)(const void* a_file);

		// a_compareFirst: in kReplace, this many answers taken from the index are all checked against the engine
		// before sampling starts; afterwards one in a_sampleEvery is.
		Core(FileArray** const* a_tableData, const std::uint32_t* a_tableCount, Mode a_mode, std::uint32_t a_compareFirst, std::uint32_t a_sampleEvery,
			Clock a_clock, Report a_report, Identity a_identity = nullptr) :
			tableData_(a_tableData), tableCount_(a_tableCount), mode_(a_mode), compareFirst_(a_compareFirst), sampleEvery_(a_sampleEvery ? a_sampleEvery : 1),
			clock_(a_clock), report_(a_report), identity_(a_identity)
		{}

		// ID 14580: a_original() must call the engine's function with the same arguments.
		template <class Original>
		FileArray* FindOrAdd(std::size_t a_site, void** a_files, std::uint32_t a_count, Original&& a_original)
		{
			Key key;
			key.head = a_files;
			key.headCount = a_count;
			return Run(a_site, key, nullptr, a_original);
		}

		// ID 14569.
		template <class Original>
		FileArray* WithFileAdded(std::size_t a_site, FileArray* a_list, void* a_file, Original&& a_original)
		{
			Key key;
			key.head = a_list->data;
			key.headCount = a_list->size;
			key.tail = a_file;
			key.hasTail = true;
			return Run(a_site, key, a_list, a_original);
		}

		// ID 14570 (the list without one file) searches the table and appends by itself. It is not answered from the
		// index, but from stage 2 on it runs under the index lock like every other access to the table, so no two
		// threads can be in the table's code at once.
		template <class Original>
		FileArray* Guarded(std::size_t a_site, Original&& a_original)
		{
			auto& site = sites_[a_site];
			const auto start = clock_();
			site.calls.fetch_add(1, std::memory_order_relaxed);
			FileArray* result;
			if (mode_ == Mode::kCount || FellBack()) {
				result = CallEngine(site, a_original);
			} else {
				std::unique_lock lock(mutex_, std::try_to_lock);
				if (!lock.owns_lock()) {
					contended_.fetch_add(1, std::memory_order_relaxed);
					lock.lock();
				}
				result = CallEngine(site, a_original);
				(void)SyncLocked(a_site, 0);
			}
			site.ticks.fetch_add(static_cast<std::uint64_t>(clock_() - start), std::memory_order_relaxed);
			return result;
		}

		[[nodiscard]] std::uint32_t TableCount() const { return *tableCount_; }
		[[nodiscard]] Mode GetMode() const { return mode_; }
		[[nodiscard]] bool FellBack() const { return fellBack_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t Mismatches() const { return mismatches_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t Contended() const { return contended_.load(std::memory_order_relaxed); }
		[[nodiscard]] const SiteStats& Site(std::size_t a_site) const { return sites_[a_site]; }

		struct Snapshot
		{
			std::uint32_t tableCount = 0;
			std::uint32_t synced = 0;
			std::size_t distinct = 0;
			std::uint32_t syncDuplicates = 0;
			std::uint32_t foreignEntries = 0; // entries indexed that no wrapped call appended
			Index::AuditResult audit;
			bool audited = false;
			std::uint64_t callDigest = 0;  // with an Identity: every call's site and list, in order
			std::uint64_t tableDigest = 0; // with an Identity: every table entry, in order
		};

		// Brings the index up to date and, when asked, audits the whole table. Takes the lock.
		Snapshot Inspect(bool a_audit)
		{
			Snapshot snapshot;
			std::lock_guard lock(mutex_);
			snapshot.tableCount = *tableCount_;
			if (mode_ != Mode::kCount && !FellBack()) {
				if (SyncLocked(Sites, 0)) {
					if (a_audit) {
						snapshot.audit = index_.Audit(*tableData_, *tableCount_);
						snapshot.audited = true;
					}
				}
			}
			if (identity_) {
				snapshot.callDigest = callDigest_;
				const auto count = *tableCount_;
				auto digest = detail::Mix(0, count);
				for (std::uint32_t i = 0; i < count; ++i) {
					const auto* entry = (*tableData_)[i];
					digest = detail::Mix(digest, entry ? entry->size : 0xFFFFFFFFull);
					for (std::uint32_t k = 0; entry && k < entry->size; ++k) {
						digest = detail::Mix(digest, identity_(entry->data[k]));
					}
				}
				snapshot.tableDigest = digest;
			}
			snapshot.synced = index_.Synced();
			snapshot.distinct = index_.Distinct();
			snapshot.syncDuplicates = index_.Duplicates();
			snapshot.foreignEntries = foreign_;
			return snapshot;
		}

	private:
		template <class Original>
		FileArray* Run(std::size_t a_site, const Key& a_key, FileArray* a_ownList, Original& a_original)
		{
			auto& site = sites_[a_site];
			const auto start = clock_();
			site.calls.fetch_add(1, std::memory_order_relaxed);
			site.scannable.fetch_add(*tableCount_, std::memory_order_relaxed);

			FileArray* result;
			if (mode_ == Mode::kCount || FellBack()) {
				// Exactly the engine: no lock, no index.
				const auto before = *tableCount_;
				result = CallEngine(site, a_original);
				if (*tableCount_ != before) {
					site.misses.fetch_add(1, std::memory_order_relaxed);
				}
			} else {
				std::unique_lock lock(mutex_, std::try_to_lock);
				if (!lock.owns_lock()) {
					contended_.fetch_add(1, std::memory_order_relaxed);
					lock.lock();
				}
				result = Indexed(a_site, site, a_key, a_ownList, a_original);
			}
			site.ticks.fetch_add(static_cast<std::uint64_t>(clock_() - start), std::memory_order_relaxed);
			return result;
		}

		template <class Original>
		FileArray* CallEngine(SiteStats& a_site, Original& a_original, std::uint64_t* a_ticks = nullptr)
		{
			const auto start = clock_();
			auto* result = a_original();
			const auto ticks = static_cast<std::uint64_t>(clock_() - start);
			a_site.engineCalls.fetch_add(1, std::memory_order_relaxed);
			a_site.engineTicks.fetch_add(ticks, std::memory_order_relaxed);
			if (a_ticks) {
				*a_ticks = ticks;
			}
			return result;
		}

		// With the lock held.
		template <class Original>
		FileArray* Indexed(std::size_t a_siteIndex, SiteStats& a_site, const Key& a_key, FileArray* a_ownList, Original& a_original)
		{
			const auto call = ++calls_;
			const auto before = *tableCount_;
			const bool own = a_ownList && Contains(*a_ownList, a_key.tail);
			if (identity_) {
				auto digest = detail::Mix(callDigest_, (static_cast<std::uint64_t>(a_siteIndex) << 32) | a_key.Length());
				for (std::uint32_t i = 0; i < a_key.headCount; ++i) {
					digest = detail::Mix(digest, identity_(a_key.head[i]));
				}
				if (a_key.hasTail) {
					digest = detail::Mix(digest, identity_(a_key.tail));
				}
				callDigest_ = digest;
			}

			FileArray* predicted = nullptr;
			if (own) {
				predicted = a_ownList;
			} else {
				if (!SyncLocked(a_siteIndex, call)) {
					return CallEngine(a_site, a_original);
				}
				a_site.keyElements.fetch_add(a_key.Length(), std::memory_order_relaxed);
				predicted = index_.Find(a_key);
			}

			if (predicted) {
				(own ? a_site.ownList : a_site.hits).fetch_add(1, std::memory_order_relaxed);
				const auto answered = ++answered_;
				const bool warmUp = answered <= compareFirst_;
				const bool sample = !warmUp && (answered - compareFirst_) % sampleEvery_ == 0;
				if (mode_ == Mode::kReplace && !warmUp && !sample) {
					return predicted;
				}
				// The engine's function changes nothing when it finds the list, so running it as well is safe.
				std::uint64_t ticks = 0;
				auto* result = CallEngine(a_site, a_original, &ticks);
				a_site.checked.fetch_add(1, std::memory_order_relaxed);
				a_site.checkedTicks.fetch_add(ticks, std::memory_order_relaxed);
				if (mode_ == Mode::kReplace && sample) {
					a_site.sampled.fetch_add(1, std::memory_order_relaxed);
					a_site.sampledTicks.fetch_add(ticks, std::memory_order_relaxed);
				}
				const auto after = *tableCount_;
				if (result != predicted || after != before) {
					Fail({ own ? MismatchKind::kOwnList : MismatchKind::kHit, a_siteIndex, call, a_key.Length(), predicted, result, before, after });
					(void)SyncLocked(a_siteIndex, call);
				}
				return result;
			}

			// Not in the table as far as the index knows: the engine searches and appends.
			a_site.misses.fetch_add(1, std::memory_order_relaxed);
			auto* result = CallEngine(a_site, a_original);
			const auto after = *tableCount_;
			const bool appended = after == before + 1 && result && (*tableData_)[after - 1] == result && Equal(*result, a_key);
			if (!appended) {
				Fail({ MismatchKind::kMiss, a_siteIndex, call, a_key.Length(), nullptr, result, before, after });
			}
			if (after > before) {
				own_ += after - before;
			}
			(void)SyncLocked(a_siteIndex, call);
			return result;
		}

		// With the lock held. False: the index cannot be trusted any more (reported, fallen back).
		bool SyncLocked(std::size_t a_siteIndex, std::uint64_t a_call)
		{
			const auto count = *tableCount_;
			const auto before = index_.Synced();
			const auto result = index_.Sync(*tableData_, count);
			if (result == Index::SyncResult::kOk) {
				// Entries that appeared without a wrapped call appending them (ID 14570's inlined append).
				const auto indexed = index_.Synced();
				if (indexed - before > own_) {
					foreign_ += indexed - before - own_;
				}
				own_ = 0;
				return true;
			}
			Fail({ result == Index::SyncResult::kShrank ? MismatchKind::kTableShrank : MismatchKind::kPrefixChanged, a_siteIndex, a_call, 0, nullptr, nullptr, before, count });
			fellBack_.store(true, std::memory_order_relaxed); // also in shadow mode: nothing left to compare against
			return false;
		}

		void Fail(const Mismatch& a_mismatch)
		{
			mismatches_.fetch_add(1, std::memory_order_relaxed);
			if (mode_ == Mode::kReplace) {
				fellBack_.store(true, std::memory_order_relaxed);
			}
			if (report_) {
				report_(a_mismatch);
			}
		}

		FileArray** const* tableData_;
		const std::uint32_t* tableCount_;
		const Mode mode_;
		const std::uint32_t compareFirst_;
		const std::uint32_t sampleEvery_;
		const Clock clock_;
		const Report report_;
		const Identity identity_;
		std::uint64_t callDigest_ = 0;

		// Recursive: if the engine's function ever came back into a wrapped call on the same thread, the inner
		// call runs to completion and the outer one reports the table change as a mismatch instead of hanging.
		std::recursive_mutex mutex_;
		Index index_;
		std::uint64_t calls_ = 0;
		std::uint64_t answered_ = 0; // lists found without the engine's search (own list or index hit)
		std::uint32_t own_ = 0;     // entries appended by wrapped calls since the last sync
		std::uint32_t foreign_ = 0; // entries indexed that were appended by other code
		std::atomic<bool> fellBack_{ false };
		std::atomic<std::uint64_t> mismatches_{ 0 };
		std::atomic<std::uint64_t> contended_{ 0 };
		std::array<SiteStats, Sites> sites_{};
	};
}
