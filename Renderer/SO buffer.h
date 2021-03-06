#pragma once

#include "stdafx.h"
#include "tracked resource.h"

namespace Renderer::Impl::CmdListPool
{
	class CmdList;
}

namespace Renderer::Impl::SOBuffer
{
	class Handle
	{
		friend class AllocatorBase;

	private:
		ID3D12Resource *buffer;
		unsigned long size = 0;
		D3D12_RESOURCE_STATES curUsage;	// consider moving it to template param (it would make usage static)
		mutable D3D12_RESOURCE_STATES prevUsage;

	public:
		Handle() = default;
		inline Handle(Handle &&src) noexcept;
		inline Handle &operator =(Handle &&srv) noexcept;

	private:
		Handle(ID3D12Resource *buffer, unsigned long size, D3D12_RESOURCE_STATES usage) noexcept : buffer(buffer), size(size), curUsage(usage) {}

	public:
		void Sync() const;

	public:
		void StartSO(CmdListPool::CmdList &target) const, UseSOResults(CmdListPool::CmdList &target) const, Finish(CmdListPool::CmdList &target) const;

	public:
		const unsigned long GetSize() const noexcept { return size; }
		const D3D12_STREAM_OUTPUT_BUFFER_VIEW GetSOView() const;	// valid after StartSO()
		const UINT64 GetGPUPtr() const;								// valid after UseSOResults() with usage specified at creation
	};

	class AllocatorBase
	{
		TrackedResource<ID3D12Resource> buffer;
		std::shared_mutex mtx;
		unsigned long version = 0;

	protected:
		AllocatorBase() = default;
		AllocatorBase(AllocatorBase &) = delete;
		AllocatorBase &operator =(AllocatorBase &) = delete;
		~AllocatorBase();

	protected:
		Handle Allocate(unsigned long size, D3D12_RESOURCE_STATES usage, LPCWSTR resourceName);
	};

	template<LPCWSTR resourceName>
	class Allocator : public AllocatorBase
	{
	public:
		Handle Allocate(unsigned long size, D3D12_RESOURCE_STATES usage) { return AllocatorBase::Allocate(size, usage, resourceName); }
	};

#pragma region inline impl
	inline Handle::Handle(Handle &&src) noexcept : buffer(src.buffer), size(src.size), curUsage(src.curUsage), prevUsage(src.prevUsage)
	{
		src.size = 0;
	}

	inline Handle &Handle::operator =(Handle &&src) noexcept
	{
		buffer = src.buffer;
		size = src.size;
		curUsage = src.curUsage;
		prevUsage = src.prevUsage;
		src.size = 0;
		return *this;
	}
#pragma endregion
}