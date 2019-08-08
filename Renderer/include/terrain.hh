#pragma once

#define NOMINMAX

#include <type_traits>
#include <memory>
#include <string>
#include <vector>
#include <list>
#include <functional>
#include <optional>
#include <variant>
#include <wrl/client.h>
#include "../tracked resource.h"
#include "../AABB.h"
#include "../world hierarchy.h"
#include "../render stage.h"
#include "../render pipeline.h"
#include "../render passes.h"
#include "../GPU stream buffer allocator.h"
#include "../occlusion query batch.h"
#include "allocator adaptors.h"
#define DISABLE_MATRIX_SWIZZLES
#if !__INTELLISENSE__ 
#include "vector math.h"
#endif

struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12GraphicsCommandList4;
struct ID3D12Resource;

extern void __cdecl InitRenderer();

namespace Renderer
{
	namespace WRL = Microsoft::WRL;
	namespace HLSL = Math::VectorMath::HLSL;

	class World;

	namespace Impl
	{
		class World;
		class TerrainVectorLayer;

		template<unsigned int dimension>
		class FrustumCuller;

		using Misc::AllocatorProxy;
	}

	namespace TerrainMaterials
	{
		class Interface;
	}

	class TerrainVectorQuad final
	{
		template<class>
		friend class Misc::AllocatorProxyAdaptor;
		friend class Impl::TerrainVectorLayer;

	private:
		struct ObjectData
		{
			unsigned long int triCount;
			const void *tris;
			const AABB<2> &aabb;
		};

		struct Object
		{
			AABB<2> aabb;
			unsigned long int triCount;
			unsigned int idx;

			// interface for BVH
		public:
#if defined _MSC_VER && _MSC_VER <= 1922
			const AABB<2> &GetAABB() const noexcept { return aabb; }
#else
			const auto &GetAABB() const noexcept { return aabb; }
#endif
			unsigned long int GetTriCount() const noexcept { return triCount; }
			float GetOcclusion() const noexcept { return .7f; }
		};

		struct NodeCluster
		{
			unsigned long int startIdx;
		};

	private:
		const std::shared_ptr<class TerrainVectorLayer> layer;
		Impl::Hierarchy::BVH<Impl::Hierarchy::QUADTREE, Object, NodeCluster> subtree;
		mutable decltype(subtree)::View subtreeView;
		Impl::TrackedResource<ID3D12Resource> VIB;	// Vertex/Index Buffer
		const bool IB32bit;
		const unsigned long int VB_size, IB_size;

	private:
		TerrainVectorQuad(std::shared_ptr<class TerrainVectorLayer> &&layer, unsigned long int vcount, const std::function<void (volatile float verts[][2])> &fillVB, unsigned int objCount, bool IB32bit, const std::function<ObjectData (unsigned int objIdx)> &getObjectData);
		~TerrainVectorQuad();
		TerrainVectorQuad(TerrainVectorQuad &) = delete;
		void operator =(TerrainVectorQuad &) = delete;

	private:
		static constexpr const WCHAR AABB_VB_name[] = L"terrain occlusion query quads";
		void Schedule(Impl::GPUStreamBuffer::Allocator<sizeof AABB<2>, AABB_VB_name> &GPU_AABB_allocator, const Impl::FrustumCuller<2> &frustumCuller, const HLSL::float4x4 &frustumXform) const, Issue(std::remove_const_t<decltype(Impl::OcclusionCulling::QueryBatchBase::npos)> &occlusionProvider) const;
	};

	namespace Impl
	{
		namespace RenderPasses = RenderPipeline::RenderPasses;

		class TerrainVectorLayer : public std::enable_shared_from_this<Renderer::TerrainVectorLayer>, RenderPipeline::IRenderStage
		{
			friend class TerrainVectorQuad;
			friend extern void __cdecl ::InitRenderer();

		protected:
			const std::shared_ptr<class Renderer::World> world;
			const unsigned int layerIdx;

		private:
			const std::string layerName;
			const std::shared_ptr<TerrainMaterials::Interface> layerMaterial;
			std::list<class TerrainVectorQuad, AllocatorProxy<class TerrainVectorQuad>> quads;

		private:
			typedef decltype(TerrainVectorQuad::subtree)::Node TreeNode;
			typedef decltype(TerrainVectorQuad::subtreeView)::Node ViewNode;

		private:
			mutable UINT64 tonemapParamsGPUAddress;
			mutable struct
			{
				std::optional<RenderPasses::StageRTBinding> RT;
				std::optional<RenderPasses::StageZBinding> ZBuffer;
				std::optional<RenderPasses::StageOutput> output;
			} ROPBindingsMain, ROPBidingsDebug;

#pragma region occlusion query pass
		private:
			static WRL::ComPtr<ID3D12RootSignature> cullPassRootSig, CreateCullPassRootSig();
			static WRL::ComPtr<ID3D12PipelineState> cullPassPSO, CreateCullPassPSO();

		private:
			struct OcclusionQueryGeometry
			{
				ID3D12Resource *VB;
				unsigned long int startIdx;
				unsigned int count;
			};
			mutable std::vector<OcclusionQueryGeometry> queryStream;

		private:
			inline void CullPassPre(CmdListPool::CmdList &target) const, CullPassPost(CmdListPool::CmdList &target) const;
			void CullPassRange(CmdListPool::CmdList &target, unsigned long rangeBegin, unsigned long rangeEnd, const RenderPasses::RenderPass &renderPass) const;

		private:
			void SetupCullPass() const;
			void IssueOcclusion(ViewNode::OcclusionQueryGeometry occlusionQueryGeometry);
#pragma endregion

#pragma region main pass
		private:
			struct RenderData
			{
				unsigned long int startIdx, triCount;
				decltype(OcclusionCulling::QueryBatchBase::npos) occlusion;
				unsigned long int startQuadIdx;
			};
			struct Quad
			{
				unsigned long int streamEnd;
				HLSL::float2 center;
				ID3D12Resource *VIB;	// no reference/lifetime tracking required, it would induce unnecessary overhead (lifetime tracking leads to mutex locks)
				unsigned long int VB_size, IB_size;
				bool IB32bit;
			};
			mutable std::vector<RenderData> renderStream;
			mutable std::vector<Quad> quadStram;

		private:
			inline void MainPassPre(CmdListPool::CmdList &target) const, MainPassPost(CmdListPool::CmdList &target) const;
			void MainPassRange(CmdListPool::CmdList &target, unsigned long rangeBegin, unsigned long rangeEnd, const RenderPasses::RenderPass &renderPass) const;

		private:
			void SetupMainPass() const;
			void IssueCluster(unsigned long int startIdx, unsigned long int triCount, decltype(OcclusionCulling::QueryBatchBase::npos) occlusion);
			void IssueExclusiveObjects(const TreeNode &node, decltype(OcclusionCulling::QueryBatchBase::npos) occlusion);
			void IssueChildren(const TreeNode &node, decltype(OcclusionCulling::QueryBatchBase::npos) occlusion);
			void IssueWholeNode(const TreeNode &node, decltype(OcclusionCulling::QueryBatchBase::npos) occlusion);
			bool IssueNodeObjects(const TreeNode &node, decltype(OcclusionCulling::QueryBatchBase::npos) coarseOcclusion,  decltype(OcclusionCulling::QueryBatchBase::npos) fineOcclusion, ViewNode::Visibility visibility);
			void IssueQuad(HLSL::float2 quadCenter, ID3D12Resource *VIB, unsigned long int VB_size, unsigned long int IB_size, bool IB32bit);
#pragma endregion

#pragma region visualize occlusion pass
		private:
			static WRL::ComPtr<ID3D12RootSignature> AABB_rootSig, CreateAABB_RootSig();
			static WRL::ComPtr<ID3D12PipelineState> AABB_PSO, CreateAABB_PSO();

		private:
			void AABBPassPre(CmdListPool::CmdList &target) const, AABBPassRange(CmdListPool::CmdList &target, unsigned long rangeBegin, unsigned long rangeEnd, const RenderPasses::RenderPass &renderPass, const float (&color)[3], bool visible) const;
#pragma endregion

		private:
			void StagePre(CmdListPool::CmdList &target) const, StagePost(CmdListPool::CmdList &target) const;
			void CullPass2MainPass(CmdListPool::CmdList &target) const;

		private:
			// order is essential (TRANSIENT, then PERSISTENT), index based access used
			mutable std::variant<OcclusionCulling::QueryBatch<OcclusionCulling::TRANSIENT>, OcclusionCulling::QueryBatch<OcclusionCulling::PERSISTENT>> occlusionQueryBatch;

		private:
			// Inherited via IRenderStage
			virtual void Sync() const override final;

		private:
			RenderPipeline::PipelineItem
				GetStagePre(unsigned int &length) const, GetStagePost(unsigned int &length) const,
				GetCullPassRange(unsigned int &length) const, GetMainPassRange(unsigned int &length) const,
				GetCullPass2MainPass(unsigned int &length) const,
				GetAABBPassPre(unsigned int &length) const,
				GetVisiblePassRange(unsigned int &length) const, GetCulledPassRange(unsigned int &length) const;

		private:
			void Setup(UINT64 tonemapParamsGPUAddress) const, SetupOcclusionQueryBatch(decltype(OcclusionCulling::QueryBatchBase::npos) maxOcclusion) const;

		private:
			static std::optional<GPUStreamBuffer::Allocator<sizeof(AABB<2>), TerrainVectorQuad::AABB_VB_name>> GPU_AABB_allocator;

		private:
			class QuadDeleter final
			{
				decltype(quads)::const_iterator quadLocation;

			public:
				QuadDeleter() = default;
				explicit QuadDeleter(decltype(quadLocation) quadLocation) : quadLocation(quadLocation) {}

			public:
				void operator ()(const class TerrainVectorQuad *quadToRemove) const;
			};

		protected:
			TerrainVectorLayer(std::shared_ptr<class Renderer::World> &&world, std::shared_ptr<TerrainMaterials::Interface> &&layerMaterial, unsigned int layerIdx, std::string &&layerName);
			~TerrainVectorLayer();
			TerrainVectorLayer(TerrainVectorLayer &) = delete;
			void operator =(TerrainVectorLayer &) = delete;

		public:
			typedef TerrainVectorQuad::ObjectData ObjectData;
			typedef std::unique_ptr<class TerrainVectorQuad, QuadDeleter> QuadPtr;
			QuadPtr AddQuad(unsigned long int vcount, const std::function<void __cdecl(volatile float verts[][2])> &fillVB, unsigned int objCount, bool IB32bit, const std::function<ObjectData __cdecl(unsigned int objIdx)> &getObjectData);

		private:
			RenderPipeline::RenderStage BuildRenderStage(const FrustumCuller<2> &frustumCuller, const HLSL::float4x4 &frustumXform, UINT64 tonemapParamsGPUAddress) const;
			RenderPipeline::PipelineStage GetDebugDrawRenderStage() const;

		protected:
			void ScheduleRenderStage(const FrustumCuller<2> &frustumCuller, const HLSL::float4x4 &frustumXform, UINT64 tonemapParamsGPUAddress, const RenderPasses::PipelineOutputTargets &outputTargets) const;
			void ScheduleDebugDrawRenderStage(const RenderPasses::PipelineOutputTargets &outputTargets) const;	// must be after ScheduleRenderStage()
			static void OnFrameFinish() { GPU_AABB_allocator->OnFrameFinish(); }
		};
	}

	class TerrainVectorLayer final : public Impl::TerrainVectorLayer
	{
		template<class>
		friend class Misc::AllocatorProxyAdaptor;
		friend class Impl::World;

	private:
		using Impl::TerrainVectorLayer::TerrainVectorLayer;
		~TerrainVectorLayer() = default;
#if defined _MSC_VER && _MSC_VER <= 1922 && !defined __clang__
		// this workaround makes '&TerrainVectorLayer::ScheduleRenderStage' accessible from 'Impl::World'\
		somewhat strange as the problem does not reproduce for simple synthetic experiment
		using Impl::TerrainVectorLayer::ScheduleRenderStage;
		using Impl::TerrainVectorLayer::ScheduleDebugDrawRenderStage;
#endif
	};
}