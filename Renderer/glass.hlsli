#ifndef GLASS_INCLUDED
#define GLASS_INCLUDED

#include "object3D material.hlsli"
#include "samplers.hlsli"
#include "fresnel.hlsli"

void ApplyGlassMask(in float2 uv, inout float roughness, inout float f0)
{
	static const float threshold = .8f, glassRough = 1e-2f, glassIOR = 1.51714f;

	const float glass = SelectTexture(GLASS_MASK).Sample(obj3DGlassMaskSampler, uv);
	if (glass >= threshold)
	{
		// just replace rough & IOR for now, consider multilayered material instead
		roughness = glassRough;
		f0 = F0(glassIOR);
	}
	else
	{
		// not actually glass, interpret glass mask other way: bump up rough & IOR in some manner
		roughness *= 1 - glass;
		f0 = lerp(f0, 1, glass);
	}
}

#endif	// GLASS_INCLUDED