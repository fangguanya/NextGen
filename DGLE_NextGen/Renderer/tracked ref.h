/**
\author		Alexey Shaydurov aka ASH
\date		27.01.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#pragma once

#include <memory>

namespace Renderer::Impl::TrackedRef
{
	template<class Class>
	class Ref : public std::shared_ptr<Class>
	{
		typedef std::shared_ptr<Class> Base;

	private:
		void AttachTarget(Base &target) noexcept, DetachTarget() noexcept, TrackRefMove() noexcept;

	public:
		inline Ref() = default;
		inline Ref(Ref &&src) noexcept;
		inline void operator =(Ref &&src) noexcept;
		inline ~Ref();

	public:
		template<class Src>
		inline auto &operator =(Src &&src) noexcept;
		inline void swap(Base &target) noexcept;

	private:
		// hide from public
		using Base::reset;
	};

	template<class Class>
	struct Target
	{
		friend class Ref<Class>;

		Target() = default;
		Target(Target &) = delete;
		void operator =(Target &) = delete;

	private:
		std::shared_ptr<Class> *refPtr = nullptr;
	};
}