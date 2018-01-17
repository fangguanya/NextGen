/**
\author		Alexey Shaydurov aka ASH
\date		17.01.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#pragma once

#include "stdafx.h"
#include "GPU stream buffer allocator.h"

namespace Renderer::Impl::GPUStreamBuffer
{
	inline AllocatorBase::AllocatorBase(unsigned long allocGranularity, LPCWSTR resourceName)
	{
		AllocateChunk(CD3DX12_RESOURCE_DESC::Buffer(allocGranularity), resourceName);
	}

	template<unsigned itemSize, LPCWSTR resourceName>
	inline const unsigned long Allocator<itemSize, resourceName>::allocGranularity = std::lcm(itemSize, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

	template<unsigned itemSize, LPCWSTR resourceName>
	inline Allocator<itemSize, resourceName>::Allocator() : AllocatorBase(allocGranularity, resourceName)
	{}

	template<unsigned itemSize, LPCWSTR resourceName>
	inline std::pair<ID3D12Resource *, unsigned long> Allocator<itemSize, resourceName>::Allocate(unsigned long count)
	{
		return AllocatorBase::Allocate(count, itemSize, allocGranularity, resourceName);
	}

	template<unsigned itemSize, LPCWSTR resourceName>
	inline std::pair<ID3D12Resource *, unsigned long> CountedAllocatorWrapper<itemSize, resourceName>::Allocate(unsigned long count)
	{
		const auto result = allocator.Allocate(count);
		// place after Allocate() for exception safety (fetch_add is noexcept => if Allocate() succeeds it will be guaranteed reflected in allocatedItemsCount)
		allocatedItemsCount.fetch_add(1, std::memory_order_relaxed);
		return result;
	}
}