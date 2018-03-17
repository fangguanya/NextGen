/**
\author		Alexey Shaydurov aka ASH
\date		17.03.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#pragma once

#include <climits>
#include <string>
#include "tracked resource.h"

struct ID3D12QueryHeap;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList1;

namespace Renderer::Impl::OcclusionCulling
{
	class QueryBatchBase
	{
		static Impl::TrackedResource<ID3D12QueryHeap> heapPool;
		static unsigned long heapPoolSize;

	public:
		static constexpr unsigned long npos = ULONG_MAX;

	protected:
		ID3D12QueryHeap *batchHeap;
		unsigned long count = 0;

	protected:
		QueryBatchBase() = default;
		QueryBatchBase(QueryBatchBase &) = delete;
		QueryBatchBase &operator =(QueryBatchBase &) = delete;
		~QueryBatchBase() = default;

	private:
		virtual void FinalSetup() = 0;

	public:
		void Setup(unsigned long count);

	protected:
		void Set(unsigned long queryIdx, ID3D12Resource *batchResults, bool visible, ID3D12GraphicsCommandList1 *target) const;

	public:
		void Start(unsigned long queryIdx, ID3D12GraphicsCommandList1 *target) const, Stop(unsigned long queryIdx, ID3D12GraphicsCommandList1 *target) const;
	};

	template<bool preserving>
	class QueryBatch;

	template<>
	class QueryBatch<false> final : public QueryBatchBase
	{
		static Impl::TrackedResource<ID3D12Resource> resultsPool;

	private:
		ID3D12Resource *batchResults;
		mutable bool fresh;

	private:
		void FinalSetup() override;

	public:
		void Sync() const;
		void Set(unsigned long queryIdx, ID3D12GraphicsCommandList1 *target, bool visible = true) const { QueryBatchBase::Set(queryIdx, batchResults, visible, target); }
		void Resolve(ID3D12GraphicsCommandList1 *target) const, Finish(ID3D12GraphicsCommandList1 *target) const;
	};

	template<>
	class QueryBatch<true> final : public QueryBatchBase
	{
		Impl::TrackedResource<ID3D12Resource> batchResults;
		const std::string &name;
		unsigned long version = 0;	// increments within lifetime of batch object, resets to 0 on new object construction

	public:
		// holds reference to the string, thus it's lifetime must be appropriate; a better and safer approach would be using immutable Name class
		QueryBatch(const std::string &name);
		~QueryBatch();

	private:
		void FinalSetup() override;

	public:
		void Set(unsigned long queryIdx, ID3D12GraphicsCommandList1 *target, bool visible = true) const { QueryBatchBase::Set(queryIdx, batchResults.Get(), visible, target); }
		void Resolve(ID3D12GraphicsCommandList1 *target, long int/*instead of D3D12_RESOURCE_STATES to eliminate dependency in d3d12.h here*/ usage = 0) const;
		UINT64 GetGPUPtr() const;	// valid after Reslove if 'usage' was specified accordingly
	};
}