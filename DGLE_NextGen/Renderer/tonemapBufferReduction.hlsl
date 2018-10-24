/**
\author		Alexey Shaydurov aka ASH
\date		25.10.2018 (c)Korotkov Andrey

This file is a part of DGLE project and is distributed
under the terms of the GNU Lesser General Public License.
See "DGLE.h" for more details.
*/

#include "tonemapBufferReduction config.hlsli"

static const uint localDataSize = blockSize;
#include "tonemapLocalReduction.hlsli"

ByteAddressBuffer buffer : register(t1);
RWByteAddressBuffer tonemapParams : register(u2);
cbuffer LumAdaptation : register(b1)
{
	float lerpFactor;
}

inline float LinearizeLum(float src)
{
	return exp2(src) - 1;
}

static const float keyValue = LinearizeLum(.5f);

inline float2 CalcTonemapParams(float2 src)
{
	const float middleGray = LinearizeLum(src[0]), exposure = keyValue / middleGray, whitePoint = max(src[1] * exposure, 1);
	return float2(exposure, rcp(whitePoint * whitePoint));
}

[numthreads(blockSize, 1, 1)]
void main(in uint globalIdx : SV_DispatchThreadID, in uint localIdx : SV_GroupIndex)
{
	// global buffer loading combined with first level reduction
	const float4 batch = asfloat(buffer.Load4(globalIdx * 16));
	const float2 partialReduction = { batch[0] + batch[2], max(batch[1], batch[3]) };

	// bulk of reduction work
	const float2 finalReduction = LocalReduce(partialReduction, localIdx);

	// update tonemap params buffer
	if (localIdx == 0)
		tonemapParams.Store2(0, asuint(lerp(CalcTonemapParams(finalReduction), asfloat(tonemapParams.Load2(0)), lerpFactor)));
}