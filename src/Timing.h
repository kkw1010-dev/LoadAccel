// Clock and thread bookkeeping shared by the targets.

#pragma once

#include "PCH.h"

namespace loadaccel
{
	inline std::int64_t tickFrequency = 1;
	inline std::int64_t loadTick = 0; // when the plugin was loaded

	[[nodiscard]] inline std::int64_t Now()
	{
		LARGE_INTEGER value;
		::QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	[[nodiscard]] inline double Seconds(std::int64_t a_ticks)
	{
		return static_cast<double>(a_ticks) / static_cast<double>(tickFrequency);
	}

	[[nodiscard]] inline double Seconds(std::uint64_t a_ticks)
	{
		return static_cast<double>(a_ticks) / static_cast<double>(tickFrequency);
	}

	inline void InitClock()
	{
		LARGE_INTEGER frequency;
		::QueryPerformanceFrequency(&frequency);
		tickFrequency = frequency.QuadPart;
		loadTick = Now();
	}

	// Which threads call a wrapped function, and whether two calls are ever in progress at once.
	// The engine functions wrapped here take no lock of their own, so this is the evidence for
	// (or against) "one thread at a time".
	class ThreadPicture
	{
	public:
		class Call
		{
		public:
			explicit Call(ThreadPicture& a_picture) :
				picture_(a_picture)
			{
				picture_.Note(::GetCurrentThreadId());
				const auto depth = picture_.inFlight_.fetch_add(1, std::memory_order_relaxed) + 1;
				if (depth > 1) {
					picture_.overlapped_.fetch_add(1, std::memory_order_relaxed);
					auto seen = picture_.maxInFlight_.load(std::memory_order_relaxed);
					while (depth > seen && !picture_.maxInFlight_.compare_exchange_weak(seen, depth)) {}
				}
			}

			~Call() { picture_.inFlight_.fetch_sub(1, std::memory_order_relaxed); }

			Call(const Call&) = delete;
			Call& operator=(const Call&) = delete;

		private:
			ThreadPicture& picture_;
		};

		[[nodiscard]] std::string Describe(std::uint64_t a_calls) const
		{
			std::string others;
			std::size_t distinct = 0;
			for (const auto& slot : threads_) {
				if (const auto id = slot.load()) {
					others += std::format(" {}", id);
					++distinct;
				}
			}
			return std::format("first thread {}, calls from other threads {} ({} other thread ids:{}{}), calls that overlapped another call {}, most calls in progress at once {}",
				firstThread_.load(), otherThreadCalls_.load(), distinct, others.empty() ? " none" : others,
				distinct == threads_.size() ? " ...list full" : "", overlapped_.load(), std::max(maxInFlight_.load(), a_calls ? 1 : 0));
		}

		[[nodiscard]] bool SingleThread() const { return otherThreadCalls_.load() == 0 && overlapped_.load() == 0; }

	private:
		void Note(std::uint32_t a_id)
		{
			std::uint32_t expected = 0;
			if (firstThread_.compare_exchange_strong(expected, a_id) || expected == a_id) {
				return;
			}
			otherThreadCalls_.fetch_add(1, std::memory_order_relaxed);
			for (auto& slot : threads_) {
				std::uint32_t seen = slot.load(std::memory_order_relaxed);
				if (seen == a_id) {
					return;
				}
				if (seen == 0 && slot.compare_exchange_strong(seen, a_id)) {
					return;
				}
				if (seen == a_id) {
					return;
				}
			}
		}

		std::atomic<std::uint32_t> firstThread_{ 0 };
		std::atomic<std::uint64_t> otherThreadCalls_{ 0 };
		std::array<std::atomic<std::uint32_t>, 16> threads_{};
		std::atomic<std::int32_t> inFlight_{ 0 };
		std::atomic<std::int32_t> maxInFlight_{ 0 };
		std::atomic<std::uint64_t> overlapped_{ 0 };
	};

	// FNV-1a over a_size bytes of code: the installed image must be the one the analysis was done on.
	[[nodiscard]] inline std::uint64_t CodeHash(std::uintptr_t a_address, std::size_t a_size)
	{
		std::uint64_t hash = 0xcbf29ce484222325ull;
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
		for (std::size_t i = 0; i < a_size; ++i) {
			hash = (hash ^ bytes[i]) * 0x100000001b3ull;
		}
		return hash;
	}

	// The call at a_address is a plain E8 call to a_target.
	[[nodiscard]] inline bool IsCallTo(std::uintptr_t a_address, std::uintptr_t a_target)
	{
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
		if (bytes[0] != 0xE8) {
			return false;
		}
		std::int32_t relative;
		std::memcpy(&relative, bytes + 1, sizeof(relative));
		return a_address + 5 + relative == a_target;
	}

	// Target of a RIP-relative operand whose 32-bit displacement ends the instruction at a_address.
	[[nodiscard]] inline std::uintptr_t RipTarget(std::uintptr_t a_address, std::size_t a_length)
	{
		std::int32_t relative;
		std::memcpy(&relative, reinterpret_cast<const std::uint8_t*>(a_address) + a_length - 4, sizeof(relative));
		return a_address + a_length + relative;
	}
}
