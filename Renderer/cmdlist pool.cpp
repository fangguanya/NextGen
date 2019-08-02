#include "stdafx.h"
#include "cmdlist pool.h"
#include "cmd ctx.h"

using namespace std;
using namespace Renderer;
using namespace CmdListPool;
using Impl::globalFrameVersioning;
using Microsoft::WRL::ComPtr;

static remove_reference_t<decltype(globalFrameVersioning->GetCurFrameDataVersion())>::size_type firstFreePoolIdx;

CmdList::CmdList() : poolIdx(firstFreePoolIdx)
{
	auto &curFramePool = globalFrameVersioning->GetCurFrameDataVersion();
	cmdCtx = curFramePool.size() <= firstFreePoolIdx ? &curFramePool.emplace_back() : &curFramePool[firstFreePoolIdx];
	firstFreePoolIdx++;	// increment after insertion improves exception safety guarantee
	assert(cmdCtx->pendingBarriers.empty());
}

void CmdList::ResourceBarrier(const D3D12_RESOURCE_BARRIER &barrier)
{
	assert(cmdCtx);
	assert(cmdCtx->list);
	cmdCtx->pendingBarriers.push_back(barrier);
}

void CmdList::ResourceBarrier(initializer_list<D3D12_RESOURCE_BARRIER> barriers)
{
	assert(cmdCtx);
	assert(cmdCtx->list);
	cmdCtx->pendingBarriers.insert(cmdCtx->pendingBarriers.cend(), barriers);
}

template<bool force>
void CmdList::FlushBarriers()
{
	assert(cmdCtx);
	assert(cmdCtx->list);
	if (force || !cmdCtx->pendingBarriers.empty())
	{
		if constexpr (force)
			assert(!cmdCtx->pendingBarriers.empty());
		cmdCtx->list->ResourceBarrier(cmdCtx->pendingBarriers.size(), cmdCtx->pendingBarriers.data());
		cmdCtx->pendingBarriers.clear();
	}
}

void CmdList::Init(ID3D12PipelineState *PSO)
{
	extern ComPtr<ID3D12Device2> device;
	void NameObjectF(ID3D12Object *object, LPCWSTR format, ...) noexcept;

	const unsigned short ringIdx = globalFrameVersioning->GetFrameLatency() - 1;

	// allocator
	if (cmdCtx->allocator)
	{
		// reset
		CheckHR(cmdCtx->allocator->Reset());
	}
	else
	{
		// create
		CheckHR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmdCtx->allocator.GetAddressOf())));
		NameObjectF(cmdCtx->allocator.Get(), L"pool command allocator [%hu][%zu]", ringIdx, poolIdx);
	}

	// cmd list
	if (cmdCtx->list && !cmdCtx->suspended/*workaround - recreate cmd list if it was suspended on previous frame (D3D runtime bug?)*/)
	{
		// reset
		CheckHR(cmdCtx->list->Reset(cmdCtx->allocator.Get(), PSO));
	}
	else
	{
		// create
		CheckHR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdCtx->allocator.Get(), PSO, IID_PPV_ARGS(cmdCtx->list.ReleaseAndGetAddressOf())));
		NameObjectF(cmdCtx->list.Get(), L"pool command list [%hu][%zu][%lu]", ringIdx, poolIdx, cmdCtx->listVersion++);
	}

	setup = &CmdList::Update;
	setupSimple = &CmdList::NOP;
}

void CmdList::Update(ID3D12PipelineState *PSO)
{
	operator ID3D12GraphicsCommandList4 *()->ClearState(PSO);
}

void CmdListPool::OnFrameFinish()
{
	firstFreePoolIdx = 0;
}

template void CmdList::FlushBarriers<false>();
template void CmdList::FlushBarriers<true>();