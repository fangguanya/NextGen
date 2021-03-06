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
}