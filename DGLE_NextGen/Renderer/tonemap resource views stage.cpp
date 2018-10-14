/**
\author		Alexey Shaydurov aka ASH
\date		15.10.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#include "stdafx.h"
#include "tonemap resource views stage.h"

using namespace Renderer::Impl::Descriptors;
using WRL::ComPtr;

extern ComPtr<ID3D12Device2> device;
	
TonemapResourceViewsStage::TonemapResourceViewsStage(unsigned int backBufferCount)
{
	void NameObjectF(ID3D12Object *object, LPCWSTR format, ...) noexcept;

	const D3D12_DESCRIPTOR_HEAP_DESC heapDesc =
	{
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,	// type
		OffscreenBuffersCount + backBufferCount,
		D3D12_DESCRIPTOR_HEAP_FLAG_NONE			// CPU visible
	};
	CheckHR(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(allocation.GetAddressOf())));
	NameObjectF(allocation.Get(), L"CPU descriptor stage for tonemap reduction resources (D3D object: %p, heap start CPU address: %p)", allocation.Get(), allocation->GetCPUDescriptorHandleForHeapStart());
}

void TonemapResourceViewsStage::Fill(ID3D12Resource *src, ID3D12Resource *reductionBuffer, UINT reductionBufferLength, IDXGISwapChain4 *dst)
{
	const auto descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const auto heapStart = allocation->GetCPUDescriptorHandleForHeapStart();

	// offscreen buffer descriptors
	device->CreateShaderResourceView(src, NULL, CD3DX12_CPU_DESCRIPTOR_HANDLE(heapStart, SrcSRV, descriptorSize));
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVdesc =
		{
			DXGI_FORMAT_R32_TYPELESS,	// RAW buffer
			D3D12_UAV_DIMENSION_BUFFER
		};
		UAVdesc.Buffer =
		{
			0,							// FirstElement
			reductionBufferLength,		// NumElements
			0,							// StructureByteStride
			0,							// CounterOffsetInBytes
			D3D12_BUFFER_UAV_FLAG_RAW
		};
		device->CreateUnorderedAccessView(reductionBuffer, NULL, &UAVdesc, CD3DX12_CPU_DESCRIPTOR_HANDLE(heapStart, ReductionBufferUAV, descriptorSize));
	}

	// backbuffer descriptors
	{
		DXGI_SWAP_CHAIN_DESC dstDesc;
		CheckHR(dst->GetDesc(&dstDesc));
		CD3DX12_CPU_DESCRIPTOR_HANDLE descritor(heapStart, DstUAVArray, descriptorSize);
		for (UINT idx = 0; idx < dstDesc.BufferCount; idx++, descritor.Offset(descriptorSize))
		{
			ComPtr<ID3D12Resource> backbuffer;
			CheckHR(dst->GetBuffer(idx, IID_PPV_ARGS(backbuffer.GetAddressOf())));
			device->CreateUnorderedAccessView(backbuffer.Get(), NULL, NULL, descritor);
		}
	}
}