/**
\author		Alexey Shaydurov aka ASH
\date		14.01.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#include "stdafx.h"
#include "frame versioning.h"
#include "occlusion query batch.h"
#include "world.hh"
#include "terrain.hh"
#include "tracked resource.inl"
#include "GPU stream buffer allocator.inl"

// auto init does not work with dll, hangs with Graphics Debugging
#define ENABLE_AUTO_INIT 0

using namespace std;
using Renderer::Impl::globalFrameVersioning;
using Microsoft::WRL::ComPtr;

static auto CreateFactory()
{
	UINT creationFlags = 0;

#ifdef _DEBUG
	ComPtr<ID3D12Debug> debugController;
	const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()));
	if (SUCCEEDED(hr))
	{
		debugController->EnableDebugLayer();
		creationFlags |= DXGI_CREATE_FACTORY_DEBUG;
	}
	else
		cerr << "Fail to enable D3D12 debug layer (hr=" << hr << ")." << endl;
#endif

	ComPtr<IDXGIFactory5> factory;
	CheckHR(CreateDXGIFactory2(creationFlags, IID_PPV_ARGS(factory.GetAddressOf())));
	return factory;
}

static auto CreateDevice()
{
	ComPtr<ID3D12Device2> device;
	CheckHR(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
	return device;
}

static auto CreateCommandQueue()
{
	extern ComPtr<ID3D12Device2> device;
	if (!device)
		throw E_FAIL;

	const D3D12_COMMAND_QUEUE_DESC desc =
	{
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
		D3D12_COMMAND_QUEUE_FLAG_NONE
	};
	ComPtr<ID3D12CommandQueue> cmdQueue;
	CheckHR(device->CreateCommandQueue(&desc, IID_PPV_ARGS(cmdQueue.GetAddressOf())));
	return cmdQueue;
}

static inline ComPtr<IDXGIFactory5> TryCreateFactory()
{
#if ENABLE_AUTO_INIT
	try
	{
		return CreateFactory();
	}
	catch (HRESULT hr)
	{
		clog << "Fail to automatically create DXGI factory, manual call to 'InitRenderer()' required (hr=" << hr << ")." << endl;
		return nullptr;
	}
#else
	return nullptr;
#endif
}

static inline ComPtr<ID3D12Device2> TryCreateDevice()
{
#if ENABLE_AUTO_INIT
	try
	{
		return CreateDevice();
	}
	catch (HRESULT hr)
	{
		clog << "Fail to automatically create D3D12 device, manual call to 'InitRenderer()' required (hr=" << hr << ")." << endl;
		return nullptr;
	}
#else
	return nullptr;
#endif
}

ComPtr<IDXGIFactory5> factory = TryCreateFactory();
ComPtr<ID3D12Device2> device = TryCreateDevice();
ComPtr<ID3D12CommandQueue> cmdQueue = device ? CreateCommandQueue() : nullptr;

struct RetiredResource
{
	UINT64 frameID;
	ComPtr<IUnknown> resource;
};
static queue<RetiredResource> retiredResources;

// keeps resource alive while accessed by GPU
void RetireResource(ComPtr<IUnknown> resource)
{
	static mutex mtx;
	lock_guard<decltype(mtx)> lck(mtx);
	retiredResources.push({ globalFrameVersioning->GetCurFrameID(), move(resource) });
}

// NOTE: not thread-safe
void OnFrameFinish()
{
	const UINT64 completedFrameID = globalFrameVersioning->GetCompletedFrameID();
	while (!retiredResources.empty() && retiredResources.front().frameID <= completedFrameID)
		retiredResources.pop();
}

// should be defined before globalFrameVersioning in order to be destroyed after waiting in globalFrameVersioning dtor completes
using Renderer::Impl::World;
ComPtr<ID3D12Resource> World::perFrameCB = World::TryCreatePerFrameCB();
// define Try...() functions here to enable inline
inline ComPtr<ID3D12Resource> World::TryCreatePerFrameCB()
{
	return device ? CreatePerFrameCB() : nullptr;
}
#if PERSISTENT_MAPS
volatile World::PerFrameData *World::perFrameCB_CPU_ptr = World::TryMapPerFrameCB();
inline volatile World::PerFrameData *World::TryMapPerFrameCB()
{
	return perFrameCB ? MapPerFrameCB(&CD3DX12_RANGE(0, 0)) : nullptr;
}
#endif

namespace Renderer::Impl
{
#if defined _MSC_VER && _MSC_VER <= 1912
	decltype(globalFrameVersioning) globalFrameVersioning;
#else
	// guaranteed copy elision required
	decltype(globalFrameVersioning) globalFrameVersioning(device ? decltype(globalFrameVersioning)(in_place) : nullopt);
#endif
}

// tracked resource should be destroyed before globalFrameVersioning => should be defined after globalFrameVersioning
using Renderer::Impl::OcclusionCulling::QueryBatch;
decltype(QueryBatch::heapPool) QueryBatch::heapPool;
decltype(QueryBatch::resultsPool) QueryBatch::resultsPool;

// allocator contains tracked resource
using Renderer::TerrainVectorLayer;
#if defined _MSC_VER && _MSC_VER <= 1912
decltype(TerrainVectorLayer::GPU_AABB_allocator) TerrainVectorLayer::GPU_AABB_allocator;
#else
// guaranteed copy elision required
decltype(TerrainVectorLayer::GPU_AABB_allocator) TerrainVectorLayer::GPU_AABB_allocator(device ? decltype(TerrainVectorLayer::GPU_AABB_allocator)(in_place) : nullopt);
#endif

extern void __cdecl InitRenderer()
{
	if (!factory)
		factory = CreateFactory();

	if (!device)
	{
		device = CreateDevice();
		cmdQueue = CreateCommandQueue();
		World::perFrameCB = World::CreatePerFrameCB();
#if PERSISTENT_MAPS
		World::perFrameCB_CPU_ptr = World::MapPerFrameCB();
#endif
		globalFrameVersioning.emplace();
		TerrainVectorLayer::GPU_AABB_allocator.emplace();
	}
}