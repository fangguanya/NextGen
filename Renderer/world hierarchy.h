#pragma once

#define NOMINMAX

#include <cstddef>
#include <utility>
#include <type_traits>
#include <memory>
#include <vector>
#include <wrl/client.h>
#define DISABLE_MATRIX_SWIZZLES
#if !__INTELLISENSE__ 
#include "vector math.h"
#endif
#include "../occlusion query batch.h"

struct ID3D12Resource;

namespace Renderer::Impl
{
	template<unsigned int dimension>
	class FrustumCuller;
}

namespace Renderer::Impl::Hierarchy
{
	namespace WRL = Microsoft::WRL;
	namespace HLSL = Math::VectorMath::HLSL;

	enum TreeStructure
	{
		QUADTREE = 4,
		OCTREE = 8,
		ENNEATREE = 9,
		ICOSEPTREE = 27,
	};

	enum class SplitTechnique
	{
		REGULAR,
		MEAN,
	};

	enum class Axis : unsigned { X, Y, Z };

	template<class AABB>
	class AABBSizeSeparator
	{
		const std::remove_reference_t<decltype(std::declval<AABB>().Size())> size;

	public:
		AABBSizeSeparator(const decltype(size) &size) : size(size * .5f) {}

	public:
		inline bool operator ()(const AABB &aabb) const;
	};

	class AABBSplitterBase
	{
	protected:
		const float split;

	public:
		AABBSplitterBase(float split) : split(split) {}
	};

	template<Axis axis>
	class AAABBSplitter : public AABBSplitterBase
	{
		using AABBSplitterBase::AABBSplitterBase;

	public:
		template<class AABB>
		inline bool operator ()(const AABB &aabb) const;
	};

	// for ENNEATREE/ICOSEPTREE
	template<Axis axis>
	class AAABBInternalSplitter : public AABBSplitterBase
	{
		using AABBSplitterBase::AABBSplitterBase;

	public:
		template<class AABB>
		inline bool operator ()(const AABB &aabb) const;
	};

	template<Axis axis>
	class AABBAxisBounds
	{
		float &min, &max;

	public:
		AABBAxisBounds(float &min, float &max) : min(min), max(max) {}

	public:
		template<class AABB>
		inline void operator ()(const AABB &aabb) const;
	};

	template<class AABBHandler>
	struct Object2AABB : public AABBHandler
	{
		using AABBHandler::AABBHandler;

	public:
		template<class Object>
		inline auto operator ()(const Object &object) const;
	};

	template<TreeStructure treeStructure, class Object, class ...CustomNodeData>
	class View;

	// currently for static geometry only
	template<TreeStructure treeStructure, class Object, class ...CustomNodeData>
	class BVH
	{
		std::vector<Object> objects;

	public:
		typedef View<treeStructure, Object, CustomNodeData...> View;
		friend View;

	public:
		class Node : public CustomNodeData...
		{
			friend class BVH;
			friend View;

		private:
			std::decay_t<decltype(std::declval<Object>().GetAABB())> aabb;
			std::unique_ptr<Node> children[treeStructure];
			typename decltype(objects)::const_iterator objBegin, objExclusiveSeparator, objEnd;
			unsigned long int exclusiveTriCount, inclusiveTriCount;
			unsigned long idx;
			float occlusion;
			unsigned char childrenCount{};

		public:	// for make_unique
			Node(unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, SplitTechnique splitTechnique, ...);
			Node(Node &&) = delete;
			Node &operator =(Node &&) = delete;

		private:
			template<typename ...Params>
			void CreateChildNode(bool splitted, unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, SplitTechnique splitTechnique, unsigned int idxOffset, Params ...params);
			template<Axis axis, class F>
			void Split2(const F &action, bool &splitted, unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, decltype(aabb.Center()) splitPoint, double overlapThreshold, unsigned int idxOffset = 0);
			void SplitQuadtree(bool &splitted, unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, SplitTechnique splitTechnique, decltype(aabb.Center()) splitPoint, double overlapThreshold, unsigned int idxOffset = 0);
			void SplitOctree(bool &splitted, unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, SplitTechnique splitTechnique, decltype(aabb.Center()) splitPoint, double overlapThreshold);
			template<Axis axis, class F>
			void Split3(const F &action, bool &splitted, unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, decltype(aabb.Center()) splitPoint, unsigned int idxOffset = 0);
			void SplitEneaTree(bool &splitted, unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, SplitTechnique splitTechnique, decltype(aabb.Center()) splitPoint, unsigned int idxOffset = 0);
			void SplitIcoseptree(bool &splitted, unsigned long &nodeCounter, typename decltype(objects)::iterator begin, typename decltype(objects)::iterator end, SplitTechnique splitTechnique, decltype(aabb.Center()) splitPoint);

		public:
			inline const auto &GetAABB() const noexcept { return aabb; }
			inline auto GetExclusiveObjectsRange() const noexcept { return std::make_pair(objBegin, objExclusiveSeparator); }
			inline auto GetInclusiveObjectsRange() const noexcept { return std::make_pair(objBegin, objEnd); }
			inline unsigned long int GetExclusiveTriCount() const noexcept { return exclusiveTriCount; }
			inline unsigned long int GetInclusiveTriCount() const noexcept { return inclusiveTriCount; }
			inline float GetOcclusion() const noexcept { return occlusion; }	// exclusive

		private:
			template<typename ...Args, typename NodeHandler, typename ReorderProvider>
			void Traverse(NodeHandler &nodeHandler, ReorderProvider reorderProvider, Args ...args);
			template<bool enableEarlyOut, class Allocator>
			std::pair<unsigned long int, bool> Schedule(View &view, Allocator &GPU_AABB_allocator, const FrustumCuller<decltype(aabb.Center())::dimension> &frustumCuller, const HLSL::float4x4 &frustumXform, const HLSL::float4x3 *depthSortXform,
				bool parentInsideFrustum = false, float parentOcclusionCulledProjLength = INFINITY, float parentOcclusion = 0);
			template<bool enableEarlyOut>
			std::pair<unsigned long int, float> CollectOcclusionQueryBoxes(const View &view, const Node **boxesBegin, const Node **boxesEnd);
		};

	private:
		std::unique_ptr<Node> root;
		unsigned long nodeCount{};

	public:
		BVH() = default;
		template<typename Iterator>
		BVH(Iterator objBegin, Iterator objEnd, SplitTechnique splitTechnique, ...);	// overlapThreshold -> ... for QUADTREE/OCTREE
		// reference from View gets invalidated after move
		BVH(BVH &&) = default;
		BVH &operator =(BVH &&) = default;

	public:
		explicit operator bool() const noexcept { return root.operator bool(); }
		const auto &GetAABB() const noexcept { return root->GetAABB(); }
		unsigned long int GetTriCount() const noexcept { return root->GetInclusiveTriCount(); }

	public:
		template<typename ...Args, typename F>
		void Traverse(F &nodeHandler, const Args &...args);
		void FreeObjects(), Reset();
	};

	template<TreeStructure treeStructure, class Object, class ...CustomNodeData>
	class View
	{
		typedef BVH<treeStructure, Object, CustomNodeData...> BVH;
		friend BVH;

	public:
		class Node
		{
			friend class View;
			friend BVH;

		public:
			enum struct Visibility : unsigned char
			{
				Culled		= 0b10,	// completely culled by frustum
				Atomic		= 0b01,	// all children (and possibly node's exclusive objects) has the same visibility
				Composite	= 0b00,	// traverse for children required
			};
			struct OcclusionQueryGeometry
			{
				ID3D12Resource *VB;
#if 0
				volatile decltype(aabb) *VB_CPU_ptr;
#endif
				unsigned long int startIdx;
				unsigned int count;

			public:
				explicit operator bool() const noexcept { return VB; }
				void operator =(std::nullptr_t src) noexcept { VB = src; }
			};

		private:
			unsigned char childrenOrder[treeStructure];
			Visibility visibility{};	// need to init since it can be accessed in CollectOcclusionQueryBoxes()
			enum struct OcclusionCullDomain : unsigned char
			{
				WholeNode		= 0b1111,
				ChildrenOnly	= 0b0111,
				ForceComposite	= 0b0010,
			} occlusionCullDomain{};	// can be overridden by parent during tree traverse; need to init in order to eliminate possible UB due to uninit read in OverrideOcclusionCullDomain()
			OcclusionQueryGeometry occlusionQueryGeometry;

		public:
			Node();
			Node(Node &&) = default;
			Node &operator =(Node &&) = default;

		private:
			Visibility GetVisibility(OcclusionCullDomain override) const noexcept;
			void OverrideOcclusionCullDomain(OcclusionCullDomain &domain) const noexcept;

		private:
			template<typename IssueOcclusion, typename IssueObjects>
			bool Issue(const typename BVH::Node &bvhNode, const IssueOcclusion &issueOcclusion, const IssueObjects &issueObjects,
				std::remove_const_t<decltype(OcclusionCulling::QueryBatchBase::npos)> &occlusionProvider,
				std::remove_const_t<decltype(OcclusionCulling::QueryBatchBase::npos)> &coarseOcclusion,
				std::remove_const_t<decltype(OcclusionCulling::QueryBatchBase::npos)> &fineOcclusion,
				OcclusionCullDomain &occlusionCullDomainOverriden) const;
		};

	private:
		// bvh referenced should be valid during View lifetime\
		shared_ptr for bvh would be safer but it adds overhead
		const BVH *bvh;
		std::unique_ptr<Node []> nodes;

	public:
		View() = default;
		explicit View(const BVH &bvh);
		View(View &&) = default;
		View &operator =(View &&) = default;

	private:
		template<typename ...Args, typename F>
		void Traverse(F &nodeHandler, const Args &...args) const;

	public:
		template<bool enableEarlyOut, class Allocator>
		void Schedule(Allocator &GPU_AABB_allocator, const FrustumCuller<decltype(std::declval<Object>().GetAABB().Center())::dimension> &frustumCuller, const HLSL::float4x4 &frustumXform, const HLSL::float4x3 *depthSortXform = nullptr);
		template<typename IssueOcclusion, typename IssueObjects>
		void Issue(const IssueOcclusion &issueOcclusion, const IssueObjects &issueObjects, std::remove_const_t<decltype(OcclusionCulling::QueryBatchBase::npos)> &occlusionProvider) const;
		void Reset();
	};
}