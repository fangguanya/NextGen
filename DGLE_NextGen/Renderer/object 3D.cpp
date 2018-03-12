/**
\author		Alexey Shaydurov aka ASH
\date		08.03.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#include "stdafx.h"
#include "object 3D.hh"
#include "tracked resource.inl"
#ifdef _MSC_VER
#include <codecvt>
#include <locale>
#endif

#include "object3D_VS.csh"
#include "object3D_PS.csh"

using namespace std;
using namespace Renderer;
using WRL::ComPtr;

extern ComPtr<ID3D12Device2> device;
void NameObject(ID3D12Object *object, LPCWSTR name) noexcept, NameObjectF(ID3D12Object *object, LPCWSTR format, ...) noexcept;

/*
	it seems that INTEL fails to set root constants inside bundle so use cbuffer instead
	constider GPU/driver detection in runtume instead of current fixed compiletime approach
	btw, using static data flag in root signature v1.1 allows driver to place cbuffer content directly in root signature, thus this workaround can have no impact on shader performance
		(although buffer storage still need to be allocated)
*/
#define INTEL_WORKAROUND 1

#if INTEL_WORKAROUND
#include "CB register.h"
namespace
{
	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) MaterialData
	{
		Impl::CBRegister::AlignedRow<3> color;
	};
}
#endif

WRL::ComPtr<ID3D12RootSignature> Impl::Object3D::CreateRootSig()
{
	ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &desc, LPCWSTR name);
	CD3DX12_ROOT_PARAMETER1 rootParams[3];
	rootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX);	// per-frame data
	rootParams[1].InitAsConstantBufferView(1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX);	// instance data
#if INTEL_WORKAROUND
	rootParams[2].InitAsConstantBufferView(0, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_PIXEL);
#else
	rootParams[2].InitAsConstants(3, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
#endif
	const CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC sigDesc(size(rootParams), rootParams, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	return CreateRootSignature(sigDesc, L"object 3D root signature");
}

auto Impl::Object3D::CreatePSOs() -> decltype(PSOs)
{
	decltype(PSOs) result;

	const CD3DX12_RASTERIZER_DESC rasterDesc
	(
		D3D12_FILL_MODE_SOLID,
		D3D12_CULL_MODE_BACK,
		TRUE,										// front CCW
		D3D12_DEFAULT_DEPTH_BIAS,
		D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
		D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
		TRUE,										// depth clip
		FALSE,										// MSAA
		FALSE,										// AA line
		0,											// force sample count
		D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF
	);

	const CD3DX12_DEPTH_STENCIL_DESC dsDesc
	(
		TRUE,																									// depth
		D3D12_DEPTH_WRITE_MASK_ALL,
		D3D12_COMPARISON_FUNC_LESS,
		TRUE,																									// stencil
		D3D12_DEFAULT_STENCIL_READ_MASK,																		// stencil read mask
		D3D12_DEFAULT_STENCIL_WRITE_MASK,																		// stencil write mask
		D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_COMPARISON_FUNC_ALWAYS,		// front
		D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_COMPARISON_FUNC_ALWAYS		// back
	);

	const D3D12_INPUT_ELEMENT_DESC VB_decl[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC PSO_desc =
	{
		rootSig.Get(),													// root signature
		CD3DX12_SHADER_BYTECODE(object3D_VS, sizeof object3D_VS),		// VS
		CD3DX12_SHADER_BYTECODE(object3D_PS, sizeof object3D_PS),		// PS
		{},																// DS
		{},																// HS
		{},																// GS
		{},																// SO
		CD3DX12_BLEND_DESC(D3D12_DEFAULT),								// blend
		UINT_MAX,														// sample mask
		rasterDesc,														// rasterizer
		dsDesc,															// depth stencil
		{ VB_decl, size(VB_decl) },										// IA
		D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED,					// restart primtive
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,							// primitive topology
		1,																// render targets
		{ DXGI_FORMAT_R8G8B8A8_UNORM },									// RT formats
		DXGI_FORMAT_D24_UNORM_S8_UINT,									// depth stencil format
		{1}																// MSAA
	};

	CheckHR(device->CreateGraphicsPipelineState(&PSO_desc, IID_PPV_ARGS(result[false].GetAddressOf())));
	NameObject(result[false].Get(), L"object 3D PSO");

	PSO_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	CheckHR(device->CreateGraphicsPipelineState(&PSO_desc, IID_PPV_ARGS(result[true].GetAddressOf())));
	NameObject(result[true].Get(), L"doublesided object 3D PSO");

	return move(result);
}

Impl::Object3D::Object3D(unsigned int subobjCount, const function<SubobjectData __cdecl(unsigned int subobjIdx)> &getSubobjectData, string name) : VB_size(), IB_size()
{
	if (!subobjCount)
		throw logic_error("Attempt to create empty 3D object");

	subobjects.reserve(subobjCount);
	for (unsigned i = 0; i < subobjCount; i++)
	{
		const auto curSubobjData = getSubobjectData(i);
		subobjects.push_back({ curSubobjData.color, curSubobjData.aabb, VB_size, IB_size, curSubobjData.tricount, curSubobjData.doublesided });
		VB_size += curSubobjData.vcount;	// VB_size currently holds vcount, need multipy by vertex size later
		IB_size += curSubobjData.tricount;	// IB_size currently holds tricount, multiply needed as well
	}

	VB_size *= sizeof *SubobjectData::verts;
	IB_size *= sizeof *SubobjectData::tris;

	// create VIB
	CheckHR(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
#if INTEL_WORKAROUND
		&CD3DX12_RESOURCE_DESC::Buffer(sizeof(MaterialData) * subobjCount + VB_size + IB_size),
#else
		&CD3DX12_RESOURCE_DESC::Buffer(VB_size + IB_size),
#endif
		D3D12_RESOURCE_STATE_GENERIC_READ,
		NULL,	// clear value
		IID_PPV_ARGS(VIB.GetAddressOf())));
#ifdef _MSC_VER
	// same workaround as for terrain quad
#if 0
	wstring_convert<codecvt_utf8<WCHAR>> converter;
	wstring convertedName = converter.from_bytes(name);
#else
	wstring convertedName(name.cbegin(), name.cend());
#endif
	NameObjectF(VIB.Get(), L"\"%ls\" geometry (contains %u subobjects)", convertedName.c_str(), subobjCount);
#else
	NameObjectF(VIB.Get(), L"\"%s\" geometry (contains %u subobjects)", name.c_str(), subobjCount);
#endif

	// fill VIB
	{
#if INTEL_WORKAROUND
		MaterialData *matPtr;
		CheckHR(VIB->Map(0, &CD3DX12_RANGE(0, 0), reinterpret_cast<void **>(&matPtr)));
		float (*VB_ptr)[3] = reinterpret_cast<float (*)[3]>(matPtr + subobjCount);
#else
		float (*VB_ptr)[3];
		CheckHR(VIB->Map(0, &CD3DX12_RANGE(0, 0), reinterpret_cast<void **>(&VB_ptr)));
#endif
		uint16_t (*IB_ptr)[3] = reinterpret_cast<uint16_t (*)[3]>(reinterpret_cast<unsigned char *>(VB_ptr) + VB_size);

		for (unsigned i = 0; i < subobjCount; i++)
		{
			const auto curSubobjData = getSubobjectData(i);

#if 0
			// calc AABB if it is not provided
			if (any(subobjects[i].aabb.Size() < 0.f))
				for (unsigned idx = 0; idx < curSubobjData.vcount; idx++)
					subobjects[i].aabb.Refit(curSubobjData.verts[idx]);
#endif

#if INTEL_WORKAROUND
			matPtr++->color = curSubobjData.color;
#endif
			memcpy(VB_ptr, curSubobjData.verts, curSubobjData.vcount * sizeof *curSubobjData.verts);
			memcpy(IB_ptr, curSubobjData.tris, curSubobjData.tricount * sizeof *curSubobjData.tris);
			VB_ptr += curSubobjData.vcount;
			IB_ptr += curSubobjData.tricount;
		}

		VIB->Unmap(0, NULL);
	}

	// start bundle creation
#ifdef _MSC_VER
	bundle = async(CreateBundle, subobjects, ComPtr<ID3D12Resource>(VIB), VB_size, IB_size, move(convertedName));
#else
	bundle = async(CreateBundle, subobjects, ComPtr<ID3D12Resource>(VIB), VB_size, IB_size, move(name));
#endif
}

Impl::Object3D::Object3D() = default;
Impl::Object3D::Object3D(const Object3D &) = default;
Impl::Object3D::Object3D(Object3D &&) = default;
Impl::Object3D &Impl::Object3D::operator =(const Object3D &) = default;
Impl::Object3D &Impl::Object3D::operator =(Object3D &&) = default;
Impl::Object3D::~Object3D() = default;

AABB<3> Impl::Object3D::GetXformedAABB(const HLSL::float4x3 &xform) const
{
	AABB<3> result;

	/*
		transform every individual suboject AABB and then refit
		it somewhat slower than refitting in object space and transforming once for entire object but may provide tighter AABB
	*/
	for (const auto &subobject : subobjects)
	{
		AABB<3> subobjAABB = subobject.aabb;
		subobjAABB.Transform(xform);
		result.Refit(subobjAABB);
	}

	return result;
}

const ComPtr<ID3D12PipelineState> &Impl::Object3D::GetStartPSO() const
{
	return PSOs[subobjects.front().doublesided];
}

const void Impl::Object3D::Render(ID3D12GraphicsCommandList1 *cmdList) const
{
	cmdList->ExecuteBundle(bundle.get().second.Get());
}

// need to copy subobjects to avoid dangling reference as the function can be executed in another thread
#ifdef _MSC_VER
auto Impl::Object3D::CreateBundle(decltype(subobjects) subobjects, ComPtr<ID3D12Resource> VIB, unsigned long int VB_size, unsigned long int IB_size, wstring &&objectName) -> decay_t<decltype(bundle.get())>
#else
auto Impl::Object3D::CreateBundle(decltype(subobjects) subobjects, ComPtr<ID3D12Resource> VIB, unsigned long int VB_size, unsigned long int IB_size, string &&objectName) -> decay_t<decltype(bundle.get())>
#endif
{
	decay_t<decltype(bundle.get())> bundle;	// to be retunred

	// context
	ID3D12PipelineState *curPSO = PSOs[subobjects.front().doublesided].Get();
#if !INTEL_WORKAROUND
	HLSL::float3 curColor = NAN;	// ensures first compare to trigger
#endif

	// create command allocator
	CheckHR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(bundle.first.GetAddressOf())));
#ifdef _MSC_VER
	NameObjectF(bundle.first.Get(), L"\"%ls\" bundle allocator", objectName.c_str());
#else
	NameObjectF(bundle.first.Get(), L"\"%s\" bundle allocator", objectName.c_str());
#endif

	// create command list
	CheckHR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle.first.Get(), curPSO, IID_PPV_ARGS(bundle.second.GetAddressOf())));
#ifdef _MSC_VER
	NameObjectF(bundle.second.Get(), L"\"%ls\" bundle", objectName.c_str());
#else
	NameObjectF(bundle.second.Get(), L"\"%s\" bundle", objectName.c_str());
#endif

	// record bundle
	{
		bundle.second->SetGraphicsRootSignature(rootSig.Get());
		bundle.second->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// setup VB/IB
		{
			const D3D12_VERTEX_BUFFER_VIEW VB_view =
			{
#if INTEL_WORKAROUND
				subobjects.size() * sizeof(MaterialData) +
#endif
				VIB->GetGPUVirtualAddress(),
				VB_size,
				sizeof(float [3])
			};
			const D3D12_INDEX_BUFFER_VIEW IB_view =
			{
				VB_view.BufferLocation + VB_view.SizeInBytes,
				IB_size,
				DXGI_FORMAT_R16_UINT
			};
			bundle.second->IASetVertexBuffers(0, 1, &VB_view);
			bundle.second->IASetIndexBuffer(&IB_view);
		}

#if INTEL_WORKAROUND
		// TODO: use C++20 initializer in range-based for
		auto material_GPU_ptr = VIB->GetGPUVirtualAddress();
#endif
		for (const auto &curSubobj : subobjects)
		{
			if (ID3D12PipelineState *const subobjPSO = PSOs[curSubobj.doublesided].Get(); curPSO != subobjPSO)
				bundle.second->SetPipelineState(curPSO = subobjPSO);
#if INTEL_WORKAROUND
			bundle.second->SetGraphicsRootConstantBufferView(2, material_GPU_ptr), material_GPU_ptr += sizeof(MaterialData);
#else
			if (any(curColor != curSubobj.color))
				bundle.second->SetGraphicsRoot32BitConstants(2, decltype(curColor)::dimension, &(curColor = curSubobj.color), 0);
#endif
			bundle.second->DrawIndexedInstanced(curSubobj.tricount * 3, 1, curSubobj.IB_offset * 3, curSubobj.VB_offset, 0);
		}

		CheckHR(bundle.second->Close());
	}

	return move(bundle);
}