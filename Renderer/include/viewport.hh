#pragma once

#include <limits>
#include <memory>
#include "../tracked resource.h"
#include "../tracked ref.h"
#include "../frame versioning.h"
#include "../cmd buffer.h"
#include "../world view context.h"
#include "../render pipeline.h"

struct ID3D12GraphicsCommandList4;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct D3D12_GPU_DESCRIPTOR_HANDLE;

extern void __cdecl InitRenderer();

namespace Renderer
{
	class Viewport;
	class World;

	namespace WRL = Microsoft::WRL;

	namespace Impl
	{
		class World;

		using std::numeric_limits;
		using std::shared_ptr;

		static_assert(numeric_limits<double>::has_infinity);

		class Viewport : protected TrackedRef::Target<Renderer::Viewport>
		{
			enum
			{
				ROOT_PARAM_DESC_TABLE,
				ROOT_PARAM_CBV,
				ROOT_PARAM_UAV,
				ROOT_PARAM_LERP_FACTOR,
				ROOT_PARAM_COUNT
			};

		private:
			template<class Cmd>
			struct PrePostCmds
			{
				Cmd pre, post;
			};
			static constexpr const WCHAR viewportName[] = L"viewport";
			mutable class CmdListsManager : FrameVersioning<PrePostCmds<CmdBuffer<>>, viewportName>
			{
			public:
				PrePostCmds<ID3D12GraphicsCommandList4 *> OnFrameStart();
				using FrameVersioning::OnFrameFinish;
			} cmdListsManager;

		private:
			const shared_ptr<const class Renderer::World> world;
			mutable WorldViewContext ctx;
			float viewXform[4][3], projXform[4][4];
			typedef std::chrono::steady_clock Clock;
			mutable Clock::time_point time = Clock::now();
			TrackedResource<ID3D12Resource> tonemapParamsBuffer;
			mutable bool fresh = true;

		private:
			friend extern void __cdecl ::InitRenderer();
			static WRL::ComPtr<ID3D12RootSignature> tonemapRootSig, CreateTonemapRootSig();
			static WRL::ComPtr<ID3D12PipelineState> tonemapTextureReductionPSO, CreateTonemapTextureReductionPSO();
			static WRL::ComPtr<ID3D12PipelineState> tonemapBufferReductionPSO, CreateTonemapBufferReductionPSO();
			static WRL::ComPtr<ID3D12PipelineState> tonemapPSO, CreateTonemapPSO();

		private:
			RenderPipeline::PipelineStage
				Pre(ID3D12GraphicsCommandList4 *cmdList, ID3D12Resource *output, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT width, UINT height) const,
				Post(ID3D12GraphicsCommandList4 *cmdList, ID3D12Resource *output, ID3D12Resource *rendertarget, ID3D12Resource *HDRSurface, ID3D12Resource *LDRSurface, ID3D12Resource *tonemapReductionBuffer, D3D12_GPU_DESCRIPTOR_HANDLE tonemapDescriptorTable, float tonemapLerpFactor, UINT width, UINT height) const;

		protected:
		public:
			explicit Viewport(shared_ptr<const Renderer::World> world);
			~Viewport() = default;
			Viewport(Viewport &) = delete;
			void operator =(Viewport &) = delete;

		public:
			void SetViewTransform(const float (&matrix)[4][3]);
			void SetProjectionTransform(double fovy, double zn, double zf = numeric_limits<double>::infinity());

		protected:
			void UpdateAspect(double invAspect);
			void Render(ID3D12Resource *output, ID3D12Resource *rendertarget, ID3D12Resource *ZBuffer, ID3D12Resource *HDRSurface, ID3D12Resource *LDRSurface, ID3D12Resource *tonemapReductionBuffer,
				const D3D12_CPU_DESCRIPTOR_HANDLE rtv, const D3D12_CPU_DESCRIPTOR_HANDLE dsv, const D3D12_GPU_DESCRIPTOR_HANDLE tonemapDescriptorTable, UINT width, UINT height) const;
			void OnFrameFinish() const;
		};
	}

	class Viewport final : public Impl::Viewport
	{
		friend class Impl::World;
		friend class RenderOutput;
		friend class Impl::TrackedRef::Ref<Viewport>;

		using Impl::Viewport::Viewport;
	};
}