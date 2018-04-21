/**
\author		Alexey Shaydurov aka ASH
\date		17.04.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#include "stdafx.h"
#include "SO buffer.h"
#include "tracked resource.inl"

using namespace std;
using namespace Renderer::Impl::SOBuffer;
using Microsoft::WRL::ComPtr;

// D3D12 debug layer emits out of bounds access error on SOSetTargets without this (bug in D3D12 runtime/debug layer?)
#define SO_BUFFER_PADDING 4u

#pragma region Handle
void Handle::Sync() const
{
	if (size)
	{
		// {BECEE8C0-64A3-466E-83D7-318030A879D9}
		static const GUID usageGUID = 
		{ 0xbecee8c0, 0x64a3, 0x466e, { 0x83, 0xd7, 0x31, 0x80, 0x30, 0xa8, 0x79, 0xd9 } };

		UINT usageSize = sizeof prevUsage;
		if (const HRESULT hr = buffer->GetPrivateData(usageGUID, &usageSize, &prevUsage); hr == DXGI_ERROR_NOT_FOUND)
			prevUsage = -1;
		else
			CheckHR(hr);

		CheckHR(buffer->SetPrivateData(usageGUID, sizeof curUsage, &curUsage));
	}
}

void Handle::StartSO(ID3D12GraphicsCommandList2 *cmdList) const
{
	if (size)
	{
		// complete transition barrier if needed
		if (prevUsage != -1)
			cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer, D3D12_RESOURCE_STATES(prevUsage), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_BARRIER_FLAG_END_ONLY));

		// clear SO counter (is current approach safe?)
		cmdList->WriteBufferImmediate(1, &D3D12_WRITEBUFFERIMMEDIATE_PARAMETER{buffer->GetGPUVirtualAddress() + size, 0}, NULL);

		// transition to SO target state
		cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT));
	}
}

void Handle::UseSOResults(ID3D12GraphicsCommandList2 *cmdList) const
{
	if (size)
		cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATES(curUsage)));
}

void Handle::Finish(ID3D12GraphicsCommandList2 *cmdList) const
{
	// !: optimization opportunity: do not perform transition barrier on last buffer usage (rely on implicit state transition for next frame), it woud require providing this info in Sync() or at creation
	if (size)
		cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer, D3D12_RESOURCE_STATES(curUsage), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY));
}

const D3D12_STREAM_OUTPUT_BUFFER_VIEW Handle::GetSOView() const
{
	assert(size);
	// C++20 designated init?
	const auto GPUPtr = GetGPUPtr();
	return { GPUPtr, size, GPUPtr + size };
}

const UINT64 Handle::GetGPUPtr() const
{
	assert(size);
	return buffer->GetGPUVirtualAddress();
}
#pragma endregion

#pragma region Allocator
AllocatorBase::~AllocatorBase() = default;

// !: currently allocates exact requested size without reserving so frequent reallocs on first several frames possible
Handle AllocatorBase::Allocate(unsigned long payloadSize, long int usage, LPCWSTR resourceName)
{
	extern ComPtr<ID3D12Device2> device;
	void NameObjectF(ID3D12Object *object, LPCWSTR format, ...) noexcept;

	shared_lock<decltype(mtx)> sharedLock(mtx, defer_lock);

	if (payloadSize)
	{
		const unsigned long totalSize = payloadSize + /*counter*/D3D12_STANDARD_COMPONENT_BIT_COUNT / CHAR_BIT + SO_BUFFER_PADDING;

		sharedLock.lock();

		if (!buffer || totalSize > buffer->GetDesc().Width)
		{
			sharedLock.unlock();
			{
				lock_guard<decltype(mtx)> exclusiveLock(mtx);
				if (!buffer || totalSize > buffer->GetDesc().Width)
				{
					const CD3DX12_RESOURCE_DESC bufferDesc(
						D3D12_RESOURCE_DIMENSION_BUFFER,
						D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
						totalSize,
						1,		// height
						1,		// depth
						1,		// mips
						DXGI_FORMAT_UNKNOWN,
						1, 0,	// MSAA
						D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
						D3D12_RESOURCE_FLAG_NONE);
					CheckHR(device->CreateCommittedResource(
						&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
						D3D12_HEAP_FLAG_NONE,
						&bufferDesc,
						D3D12_RESOURCE_STATE_COPY_DEST,
						NULL,	// clear value
						IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())));
					NameObjectF(buffer.Get(), L"SO buffer for %ls [%lu]", resourceName, version++);
				}
			}
			sharedLock.lock();
		}
	}

	return { buffer.Get(), payloadSize, usage };
}
#pragma endregion