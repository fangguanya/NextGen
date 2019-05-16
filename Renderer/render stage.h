#pragma once

#include <functional>
#include "GPU work item.h"

namespace Renderer::Impl::RenderPipeline
{
	class IRenderStage
	{
		virtual void Sync() const = 0;

	protected:
		static PipelineItem (IRenderStage::*phaseSelector)(unsigned int &length) const;

	protected:
		IRenderStage() = default;
		~IRenderStage() = default;
		IRenderStage(IRenderStage &) = delete;
		void operator =(IRenderStage &) = delete;

	protected:
		// helper function
		PipelineItem IterateRenderPass(unsigned int &length, const signed long int passLength,
			const std::function<void ()> &PassFinish, const std::function<RenderStageItem (unsigned long rangeBegin, unsigned long rangeEnd)> &GetRenderRange) const;

	public:
		void Sync(PipelineItem(IRenderStage::*startPhaseSelector)(unsigned int &length) const) const { phaseSelector = startPhaseSelector, Sync(); }
		PipelineItem GetNextWorkItem(unsigned int &length) const { return (this->*phaseSelector)(length); }
	};
}