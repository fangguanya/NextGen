/**
\author		Alexey Shaydurov aka ASH
\date		29.10.2017 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#pragma once

#include <algorithm>
#include <vector>
#include <optional>
#include <wrl/client.h>

struct ID3D12Fence;
struct ID3D12CommandAllocator;

namespace Renderer::Impl
{
	namespace WRL = Microsoft::WRL;

	static constexpr unsigned short maxFrameLatency = 3;

	template<class Data>
	class FrameVersioning : public FrameVersioning<void>
	{
		Data ringBuffer[maxFrameLatency];

	public:
		void OnFrameStart();
		Data &GetCurFrameDataVersion() noexcept { return ringBuffer[GetRingBufferIdx()]; }
	};

	template<>
	class FrameVersioning<void>
	{
		UINT64 frameID = 0;
		class EventHandle
		{
			const HANDLE handle;

		public:
			EventHandle();
			~EventHandle();
			EventHandle(EventHandle &) = delete;
			void operator =(EventHandle &) = delete;

		public:
			operator HANDLE () const noexcept { return handle; }
		} fenceEvent;
		WRL::ComPtr<ID3D12Fence> fence;
		unsigned short frameLatency = 1, ringBufferIdx = 0;

	protected:
		FrameVersioning();
		FrameVersioning(FrameVersioning &) = delete;
		FrameVersioning &operator =(FrameVersioning &) = delete;
		~FrameVersioning();

	public:
		UINT64 GetCurFrameID() const noexcept { return frameID; }
		UINT64 GetCompletedFrameID() const;
		void WaitForGPU() const { WaitForGPU(frameID); }

	private:
		void WaitForGPU(UINT64 waitFrameID) const;

	protected:
		unsigned short GetRingBufferIdx() const noexcept { return ringBufferIdx; }
		unsigned short OnFrameStart();

	public:
		void OnFrameFinish();
	};

	// Data here is a cmd list pool
	extern std::optional<FrameVersioning<std::vector<WRL::ComPtr<ID3D12CommandAllocator>>>> globalFrameVersioning;

	// template impl
	template<class Data>
	void FrameVersioning<Data>::OnFrameStart()
	{
		if (const unsigned short rangeEndIdx = FrameVersioning<void>::OnFrameStart())
		{
			const auto rangeBegin = ringBuffer + GetRingBufferIdx(), rangeEnd = ringBuffer + rangeEndIdx;
			std::move_backward(rangeBegin, rangeEnd, rangeEnd + 1);
		}
	}
}