#pragma once

#include <wrl/client.h>

struct ID3D12DescriptorHeap;
struct ID3D12Resource;
struct IDXGISwapChain4;
struct D3D12_GPU_DESCRIPTOR_HANDLE;

namespace Renderer::Impl::Descriptors
{
	namespace WRL = Microsoft::WRL;

	class TonemapResourceViewsStage;

	namespace GPUDescriptorHeap
	{
		namespace Impl
		{
			WRL::ComPtr<ID3D12DescriptorHeap> CreateHeap();
		}
		D3D12_GPU_DESCRIPTOR_HANDLE SetCurFrameTonemapReductionDescs(const TonemapResourceViewsStage &src, UINT backBufferIdx);
	}

	class TonemapResourceViewsStage
	{
		friend WRL::ComPtr<ID3D12DescriptorHeap> GPUDescriptorHeap::Impl::CreateHeap();
		friend D3D12_GPU_DESCRIPTOR_HANDLE GPUDescriptorHeap::SetCurFrameTonemapReductionDescs(const TonemapResourceViewsStage &src, UINT backBufferIdx);

		enum
		{
			SrcSRV,
			DstUAV,
			ReductionBufferUAV,
			ReductionBufferSRV,
			ViewCount
		};
		WRL::ComPtr<ID3D12DescriptorHeap> allocation;	// CPU heap does not require lifetime tracking

	public:
		TonemapResourceViewsStage();
		void Fill(ID3D12Resource *src, ID3D12Resource *dst, ID3D12Resource *reductionBuffer, UINT reductionBufferLength);
	};
}