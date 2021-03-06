#include "stdafx.h"
#include "system.h"
#include "event handle.h"
#include "frame versioning.h"
#include "occlusion query batch.h"
#include "DMA engine.h"
#include "render output.hh"	// for tonemap reduction buffer
#include "viewport.hh"		// for tonemap root sig & PSOs
#include "world.hh"
#include "world render stages.h"
#include "terrain render stages.h"
#include "terrain materials.hh"
#include "object 3D.hh"
#include "tracked resource.inl"
#include "GPU stream buffer allocator.inl"

// auto init does not work with dll, hangs with Graphics Debugging
#define ENABLE_AUTO_INIT	0
#define ENABLE_GBV			0

using namespace std;
using Renderer::RenderOutput;
using Renderer::Impl::globalFrameVersioning;
using Renderer::Impl::Viewport;
using Renderer::Impl::World;
using Renderer::TerrainVectorQuad;
using Renderer::Impl::Object3D;
using Microsoft::WRL::ComPtr;
namespace TerrainMaterials = Renderer::TerrainMaterials;

static constexpr size_t maxD3D12NameLength = 256;

// set it as default?
pmr::synchronized_pool_resource globalTransientRAM;

void NameObject(ID3D12Object *object, LPCWSTR name) noexcept
{
	if (const HRESULT hr = object->SetName(name); FAILED(hr))
	{
		System::WideIOGuard IOGuard(stderr);
		wcerr << "Fail to set name " << quoted(name) << " for D3D12 object \'" << object << "\' (hr=" << hr << ")." << endl;
	}
}

void NameObjectF(ID3D12Object *object, LPCWSTR format, ...) noexcept
{
	WCHAR buf[maxD3D12NameLength];
	va_list args;
	va_start(args, format);
	const int length = vswprintf(buf, size(buf), format, args);
	if (length < 0)
		perror("Fail to compose name for D3D12 object");
	else
		NameObject(object, buf);
	assert(length >= 0 && length < maxD3D12NameLength);
	va_end(args);
}

// define it here to eliminate creation tiny dedicated .cpp for such a little stuff
Renderer::Impl::EventHandle::EventHandle() : Handle(CreateEvent(NULL, FALSE, FALSE, NULL))
{
}

ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &desc, LPCWSTR name)
{
	extern ComPtr<ID3D12Device2> device;
	ComPtr<ID3D12RootSignature> result;
	ComPtr<ID3DBlob> sig, error;
	const HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &error);
	if (error)
	{
		cerr.write((const char *)error->GetBufferPointer(), error->GetBufferSize()) << endl;
	}
	CheckHR(hr);
	CheckHR(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(result.GetAddressOf())));
	NameObject(result.Get(), name);
	return result;
}

static auto CreateFactory()
{
	UINT creationFlags = 0;

#ifdef _DEBUG
	ComPtr<ID3D12Debug1> debugController;
	const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()));
	if (SUCCEEDED(hr))
	{
		debugController->EnableDebugLayer();
		debugController->SetEnableGPUBasedValidation(ENABLE_GBV);
		creationFlags |= DXGI_CREATE_FACTORY_DEBUG;
	}
	else
	{
		System::WideIOGuard IOGuard(stderr);
		wcerr << "Fail to enable D3D12 debug layer: " << _com_error(hr).ErrorMessage() << endl;
	}
#endif

	ComPtr<IDXGIFactory5> factory;
	CheckHR(CreateDXGIFactory2(creationFlags, IID_PPV_ARGS(factory.GetAddressOf())));
	return factory;
}

static auto CreateDevice()
{
	ComPtr<ID3D12Device2> device;
	CheckHR(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));

#pragma region validate device features support
	string unsupportedFeatures;

	// resource binding tier
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS caps;
		CheckHR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &caps, sizeof caps));
		if (caps.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_1)
			unsupportedFeatures += "\n* limited shader accessible textures";
	}

	// shader model
	{
		D3D12_FEATURE_DATA_SHADER_MODEL SM = { D3D_SHADER_MODEL_6_0 };
		CheckHR(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &SM, sizeof SM));
		if (SM.HighestShaderModel != D3D_SHADER_MODEL_6_0)
			unsupportedFeatures += "\n* SM 6.0 not supported";
	}

	// HLSL SIMD ops
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS1 caps;
		CheckHR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &caps, sizeof caps));
		if (!caps.WaveOps)
			unsupportedFeatures += "\n* HLSL SIMD ops not supported";
	}

	// root signature\
	is it really needed, or D3D runtime will fall back to 1.0 ?
	{
		D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSig = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
		CheckHR(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rootSig, sizeof rootSig));
		if (rootSig.HighestVersion != D3D_ROOT_SIGNATURE_VERSION_1_1)
			unsupportedFeatures += "\n* root signature v1.1 not supported";
	}

	// write buffer immediate
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS3 caps;
		CheckHR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &caps, sizeof caps));
		if (!(caps.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT))
			unsupportedFeatures += "\n* immediate buffer writes not supported";
	}

	if (!unsupportedFeatures.empty())
		throw runtime_error("Sorry, but your GPU or driver is too old. Try to update driver to the latest version. If it doesn't help, you have to upgrade your graphics card." + unsupportedFeatures);
#pragma endregion

	// GBV device settings
#if _DEBUG && ENABLE_GBV
	{
		ComPtr<ID3D12DebugDevice1> debugDeviceController;
		if (const HRESULT hr = device.As(&debugDeviceController); FAILED(hr))
		{
			System::WideIOGuard IOGuard(stderr);
			wcerr << "Fail to setup device debug settings, defaults used: " << _com_error(hr).ErrorMessage() << endl;
		}
		else
		{
			const D3D12_DEBUG_DEVICE_GPU_BASED_VALIDATION_SETTINGS GBVSettings =
			{
				16,
				D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE_NONE,
				D3D12_GPU_BASED_VALIDATION_PIPELINE_STATE_CREATE_FLAG_NONE
			};
			if (const HRESULT hr = debugDeviceController->SetDebugParameter(D3D12_DEBUG_DEVICE_PARAMETER_GPU_BASED_VALIDATION_SETTINGS, &GBVSettings, sizeof GBVSettings); FAILED(hr))
			{
				System::WideIOGuard IOGuard(stderr);
				wcerr << "Fail to setup GBV settings: " << _com_error(hr).ErrorMessage() << endl;
			}
		}
	}
#endif

	return device;
}

static auto CreateGraphicsCommandQueue()
{
	extern ComPtr<ID3D12Device2> device;

	const D3D12_COMMAND_QUEUE_DESC desc =
	{
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
		D3D12_COMMAND_QUEUE_FLAG_NONE
	};
	ComPtr<ID3D12CommandQueue> cmdQueue;
	CheckHR(device->CreateCommandQueue(&desc, IID_PPV_ARGS(cmdQueue.GetAddressOf())));
	NameObject(cmdQueue.Get(), L"main GFX command queue");
	return cmdQueue;
}

static auto CreateDMACommandQueue()
{
	extern ComPtr<ID3D12Device2> device;

	ComPtr<ID3D12CommandQueue> cmdQueue;
	D3D12_FEATURE_DATA_ARCHITECTURE GPUArch{};
	CheckHR(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &GPUArch, sizeof GPUArch));
	assert(GPUArch.UMA || !GPUArch.CacheCoherentUMA);

	// init DMA engine for discrete GPUs only
	if (!GPUArch.UMA)
	{
		/*
		or CacheCoherentUMA ?
		or check device id ?
		https://docs.microsoft.com/en-us/windows/desktop/api/d3d12/ns-d3d12-d3d12_feature_data_architecture#how-to-use-uma-and-cachecoherentuma
		*/
		const D3D12_COMMAND_QUEUE_DESC desc =
		{
			D3D12_COMMAND_LIST_TYPE_COPY,
			D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
			D3D12_COMMAND_QUEUE_FLAG_NONE
		};
		CheckHR(device->CreateCommandQueue(&desc, IID_PPV_ARGS(cmdQueue.GetAddressOf())));
		NameObject(cmdQueue.Get(), L"DMA engine command queue");
	}
	return cmdQueue;
}

static void PrintCreateError(const exception_ptr &error, const char object[])
{
	try
	{
		rethrow_exception(error);
	}
	catch (const _com_error &error)
	{
		System::WideIOGuard IOGuard(stderr);
		wclog << "Fail to automatically create " << object << ", manual call to 'InitRenderer()' required: " << error.ErrorMessage() << endl;
	}
	catch (const exception &error)
	{
		clog << "Fail to automatically create " << object << ": " << error.what() << ". Manual call to 'InitRenderer()' required." << endl;
	}
	catch (...)
	{
		clog << "Fail to automatically create " << object << " (unknown error). Manual call to 'InitRenderer()' required." << endl;
	}
}

static inline ComPtr<IDXGIFactory5> TryCreateFactory()
{
#if ENABLE_AUTO_INIT
	try
	{
		return CreateFactory();
	}
	catch (...)
	{
		PrintCreateError(current_exception(), "DXGI factory");
	}
#endif
	return nullptr;
}

static inline ComPtr<ID3D12Device2> TryCreateDevice()
{
#if ENABLE_AUTO_INIT
	try
	{
		return CreateDevice();
	}
	catch (...)
	{
		PrintCreateError(current_exception(), "D3D12 device");
	}
#endif
	return nullptr;
}

template<typename Result>
static Result Try(Result Create(), const char object[])
{
	extern ComPtr<ID3D12Device2> device;
	if (device)
	{
		try
		{
			return Create();
		}
		catch (...)
		{
			PrintCreateError(current_exception(), object);
		}
	}
	device.Reset();	// force recreation everything in 'InitRenderer()'
	return {};
}

ComPtr<IDXGIFactory5> factory = TryCreateFactory();
ComPtr<ID3D12Device2> device = TryCreateDevice();
ComPtr<ID3D12CommandQueue> gfxQueue = Try(CreateGraphicsCommandQueue, "main GFX command queue"), dmaQueue = Try(CreateDMACommandQueue, "DMA engine command queue");

template<class Optional>
static inline Optional TryCreate(const char object[])
{
	if (device)
	{
		try
		{
			return Optional(in_place);
		}
		catch (...)
		{
			PrintCreateError(current_exception(), object);
		}
	}
	return nullopt;
}

namespace Renderer::Impl::Descriptors::TextureSamplers::Impl
{
	ComPtr<ID3D12DescriptorHeap> CreateHeap(), heap = Try(CreateHeap, "GPU texture sampler heap");
}

ComPtr<ID3D12Resource> RenderOutput::tonemapReductionBuffer = device ? RenderOutput::CreateTonemapReductionBuffer() : nullptr;

struct RetiredResource
{
	UINT64 frameID;
	ComPtr<IUnknown> resource;
};
static queue<RetiredResource> retireQueue;

// keeps resource alive while accessed by GPU
void RetireResource(ComPtr<IUnknown> resource)
{
	static mutex mtx;
	if (globalFrameVersioning->GetCurFrameID() > globalFrameVersioning->GetCompletedFrameID())
	{
		lock_guard lck(mtx);
		retireQueue.push({ globalFrameVersioning->GetCurFrameID(), move(resource) });
	}
}

// NOTE: not thread-safe
void OnFrameFinish()
{
	const UINT64 completedFrameID = globalFrameVersioning->GetCompletedFrameID();
	while (!retireQueue.empty() && retireQueue.front().frameID <= completedFrameID)
		retireQueue.pop();
}

#pragma region root sigs & PSOs
ComPtr<ID3D12RootSignature>
	Viewport::tonemapRootSig														= Try(Viewport::CreateTonemapRootSig, "tonemapping root signature"),
	TerrainVectorQuad::MainRenderStage::cullPassRootSig								= Try(TerrainVectorQuad::MainRenderStage::CreateCullPassRootSig, "terrain occlusion query root signature"),
	TerrainVectorQuad::DebugRenderStage::AABB_rootSig								= Try(TerrainVectorQuad::DebugRenderStage::CreateAABB_RootSig, "terrain AABB visualization root signature"),
	TerrainMaterials::Flat::rootSig													= Try(TerrainMaterials::Flat::CreateRootSig, "terrain flat material root signature"),
	TerrainMaterials::Masked::rootSig												= Try(TerrainMaterials::Masked::CreateRootSig, "terrain masked material root signature"),
	TerrainMaterials::Standard::rootSig												= Try(TerrainMaterials::Standard::CreateRootSig, "terrain standard material root signature"),
	TerrainMaterials::Extended::rootSig												= Try(TerrainMaterials::Extended::CreateRootSig, "terrain extended material root signature"),
	World::MainRenderStage::xformAABB_rootSig										= Try(World::MainRenderStage::CreateXformAABB_RootSig, "Xform 3D AABB root signature"),
	World::MainRenderStage::cullPassRootSig											= Try(World::MainRenderStage::CreateCullPassRootSig, "world objects occlusion query root signature"),
	World::DebugRenderStage::AABB_rootSig											= Try(World::DebugRenderStage::CreateAABB_RootSig, "world 3D objects AABB visualization root signature"),
	Object3D::rootSig																= Try(Object3D::CreateRootSig, "object 3D root signature");
ComPtr<ID3D12PipelineState>
	Viewport::tonemapTextureReductionPSO											= Try(Viewport::CreateTonemapTextureReductionPSO, "tonemap texture reduction PSO"),
	Viewport::tonemapBufferReductionPSO												= Try(Viewport::CreateTonemapBufferReductionPSO, "tonemap buffer reduction PSO"),
	Viewport::tonemapPSO															= Try(Viewport::CreateTonemapPSO, "tonemapping PSO"),
	TerrainVectorQuad::MainRenderStage::cullPassPSO									= Try(TerrainVectorQuad::MainRenderStage::CreateCullPassPSO, "terrain occlusion query PSO"),
	TerrainVectorQuad::DebugRenderStage::AABB_PSO									= Try(TerrainVectorQuad::DebugRenderStage::CreateAABB_PSO, "terrain AABB visualization PSO"),
	TerrainMaterials::Flat::PSO														= Try(TerrainMaterials::Flat::CreatePSO, "terrain flat material PSO"),
	TerrainMaterials::Masked::PSO													= Try(TerrainMaterials::Masked::CreatePSO, "terrain masked material PSO"),
	TerrainMaterials::Standard::PSO													= Try(TerrainMaterials::Standard::CreatePSO, "terrain standard material PSO"),
	TerrainMaterials::Extended::PSO													= Try(TerrainMaterials::Extended::CreatePSO, "terrain extended material PSO"),
	World::MainRenderStage::xformAABB_PSO											= Try(World::MainRenderStage::CreateXformAABB_PSO, "Xform 3D AABB PSO");
decltype(World::MainRenderStage::cullPassPSOs) World::MainRenderStage::cullPassPSOs	= Try(World::MainRenderStage::CreateCullPassPSOs, "world objects occlusion query passes PSOs");
decltype(World::DebugRenderStage::AABB_PSOs) World::DebugRenderStage::AABB_PSOs		= Try(World::DebugRenderStage::CreateAABB_PSOs, "world 3D objects AABB visualization PSOs");
decltype(Object3D::PSOs) Object3D::PSOs												= Try(Object3D::CreatePSOs, "object 3D PSOs");

// should be defined before globalFrameVersioning in order to be destroyed after waiting in globalFrameVersioning dtor completes
using Renderer::Impl::World;
ComPtr<ID3D12Resource> World::globalGPUBuffer = Try(World::CreateGlobalGPUBuffer, "global GPU buffer");
#if PERSISTENT_MAPS
volatile World::PerFrameData *World::globalGPUBuffer_CPU_ptr = World::TryMapGlobalGPUBuffer();
// define here to enable inline
inline volatile World::PerFrameData *World::TryMapGlobalGPUBuffer()
{
	return globalGPUBuffer ? MapGlobalGPUBuffer(&CD3DX12_RANGE(0, 0)) : nullptr;
}
#endif

/*
fence going to be waited on another queue in 'DMA::Sync()'
define it before globalFrameVersioning to get destruction order that honors dependencies:
	wait in globalFrameVersioning's dtor happens first
	now fence no longer used on GFX queue
	then destroy fence
more robust alternative is to extend its lifetime by means of additional ref tracking (such as 'SetPrivateDataInterface()')
*/
namespace Renderer::DMA::Impl
{
	decltype(cmdBuffers) cmdBuffers = Try(CreateCmdBuffers, "DMA engine command buffers");
	ComPtr<ID3D12Fence> fence = Try(CreateFence, "DMA engine fence");
}

namespace Renderer::Impl
{
	decltype(globalFrameVersioning) globalFrameVersioning = TryCreate<decltype(globalFrameVersioning)>("global frame versioning");
}

// tracked resource should be destroyed before globalFrameVersioning => should be defined after globalFrameVersioning
namespace Renderer::Impl::Descriptors::GPUDescriptorHeap::Impl
{
	TrackedResource<ID3D12DescriptorHeap> PreallocateHeap(), heap = Try(PreallocateHeap, "GPU descriptor heap (preallocation)");
}
namespace OcclusionCulling = Renderer::Impl::OcclusionCulling;
using OcclusionCulling::QueryBatchBase;
using OcclusionCulling::QueryBatch;
decltype(QueryBatchBase::heapPool) QueryBatchBase::heapPool;
decltype(QueryBatch<OcclusionCulling::TRANSIENT>::resultsPool) QueryBatch<OcclusionCulling::TRANSIENT>::resultsPool;

// allocators contains tracked resource (=> after globalFrameVersioning)
decltype(TerrainVectorQuad::MainRenderStage::GPU_AABB_allocator) TerrainVectorQuad::MainRenderStage::GPU_AABB_allocator = TryCreate<decltype(TerrainVectorQuad::MainRenderStage::GPU_AABB_allocator)>("GPU AABB allocator for terrain vector layers");
decltype(World::MainRenderStage::GPU_AABB_allocator) World::MainRenderStage::GPU_AABB_allocator = TryCreate<decltype(World::MainRenderStage::GPU_AABB_allocator)>("GPU AABB allocator for world 3D objects");
decltype(World::MainRenderStage::xformedAABBsStorage) World::MainRenderStage::xformedAABBsStorage;

bool enableDebugDraw;

extern void __cdecl InitRenderer()
{
	if (!factory || !device)
	{
		namespace TextureSamplers = Renderer::Impl::Descriptors::TextureSamplers;
		namespace GPUDescriptorHeap = Renderer::Impl::Descriptors::GPUDescriptorHeap;
		namespace DMAEngine = Renderer::DMA::Impl;
		factory												= CreateFactory();
		device												= CreateDevice();
		gfxQueue											= CreateGraphicsCommandQueue();
		dmaQueue											= CreateDMACommandQueue();
		TextureSamplers::Impl::heap							= TextureSamplers::Impl::CreateHeap();
		RenderOutput::tonemapReductionBuffer				= RenderOutput::CreateTonemapReductionBuffer();
		Viewport::tonemapRootSig							= Viewport::CreateTonemapRootSig();
		Viewport::tonemapTextureReductionPSO				= Viewport::CreateTonemapTextureReductionPSO();
		Viewport::tonemapBufferReductionPSO					= Viewport::CreateTonemapBufferReductionPSO();
		Viewport::tonemapPSO								= Viewport::CreateTonemapPSO();
		TerrainVectorQuad::MainRenderStage::cullPassRootSig	= TerrainVectorQuad::MainRenderStage::CreateCullPassRootSig();
		TerrainVectorQuad::DebugRenderStage::AABB_rootSig	= TerrainVectorQuad::DebugRenderStage::CreateAABB_RootSig();
		TerrainVectorQuad::MainRenderStage::cullPassPSO		= TerrainVectorQuad::MainRenderStage::CreateCullPassPSO();
		TerrainVectorQuad::DebugRenderStage::AABB_PSO		= TerrainVectorQuad::DebugRenderStage::CreateAABB_PSO();
		TerrainMaterials::Flat::rootSig						= TerrainMaterials::Flat::CreateRootSig();
		TerrainMaterials::Masked::rootSig					= TerrainMaterials::Masked::CreateRootSig();
		TerrainMaterials::Standard::rootSig					= TerrainMaterials::Standard::CreateRootSig();
		TerrainMaterials::Extended::rootSig					= TerrainMaterials::Extended::CreateRootSig();
		TerrainMaterials::Flat::PSO							= TerrainMaterials::Flat::CreatePSO();
		TerrainMaterials::Masked::PSO						= TerrainMaterials::Masked::CreatePSO();
		TerrainMaterials::Standard::PSO						= TerrainMaterials::Standard::CreatePSO();
		TerrainMaterials::Extended::PSO						= TerrainMaterials::Extended::CreatePSO();
		World::MainRenderStage::xformAABB_rootSig			= World::MainRenderStage::CreateXformAABB_RootSig();
		World::MainRenderStage::cullPassRootSig				= World::MainRenderStage::CreateCullPassRootSig();
		World::DebugRenderStage::AABB_rootSig				= World::DebugRenderStage::CreateAABB_RootSig();
		World::MainRenderStage::xformAABB_PSO				= World::MainRenderStage::CreateXformAABB_PSO();
		World::MainRenderStage::cullPassPSOs				= World::MainRenderStage::CreateCullPassPSOs();
		World::DebugRenderStage::AABB_PSOs					= World::DebugRenderStage::CreateAABB_PSOs();
		Object3D::rootSig									= Object3D::CreateRootSig();
		Object3D::PSOs										= Object3D::CreatePSOs();
		World::globalGPUBuffer								= World::CreateGlobalGPUBuffer();
#if PERSISTENT_MAPS
		World::globalGPUBuffer_CPU_ptr						= World::MapGlobalGPUBuffer();
#endif
		globalFrameVersioning.emplace();
		GPUDescriptorHeap::Impl::heap						= GPUDescriptorHeap::Impl::PreallocateHeap();
		TerrainVectorQuad::MainRenderStage::GPU_AABB_allocator.emplace();
		World::MainRenderStage::GPU_AABB_allocator.emplace();
		DMAEngine::cmdBuffers								= DMAEngine::CreateCmdBuffers();
		DMAEngine::fence									= DMAEngine::CreateFence();
	}
}