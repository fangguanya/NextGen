﻿#pragma once

#include "luminance.hlsli"
#include "fresnel.hlsli"

#define SMOOTH_PARALLAX 1
#define PRESERVE_PARALLAX_PLANE 1

#define DXC_NAMESPACE_WORKAROUND 1

namespace Lighting
{
	// 'static' causes DXC crash\
	static const float2 parallaxCurvatureParams = float2(.1f, 1.3f);

	/*
	fadeout on glancing incident angles to prevent hard cutoff by surface backlight blocking (simulates soft selfshadowing)
	normally it should be achieved automatically by BRDF but normal mapping introduces discrepancy between N and n
		causing brightly highlighted pixels even at grazing macrosurface angles
	it appears that it helps in other scenarios too (no normal mapping) - BRDF itself fades out not fast enough
		and exposure can boost dark parts to appear brighter

	research needed to estimate quality vs perf tradeoff of smoothstep() vs cheaper alternatives
	*/
	float SoftBackshadow(float LdotN)
	{
		static const float backshadowSoftness = 1e-1f;
		return smoothstep(0, backshadowSoftness, LdotN);
	}

	namespace GGX
	{
		// evaluate Smith Λ() function for GGX NDF
		float SmithIntegral(float a2, float NdotDir)
		{
			const float cos2 = NdotDir * NdotDir;
			return .5f * sqrt((a2 - a2 * cos2) / cos2 + 1) - .5f;
		}

		// denominator of height-direction-correlated GGX Smith masking & shadowing (from http://jcgt.org/published/0003/02/03/paper.pdf)
		const float G_rcp(float a2, float VdotN, float LdotN, float VdotL)
		{
			const float phi = acos(VdotL);
			float lambda = 4.41 * phi;
			lambda /= lambda + 1;
			const float LambdaV = SmithIntegral(a2, VdotN), LambdaL = SmithIntegral(a2, LdotN);
			return isfinite(LambdaV) ? 1 + max(LambdaV, LambdaL) + lambda * min(LambdaV, LambdaL) : LambdaV * 0/*generate NaN*/;
		}
	}

	// evaluate rendering equation for punctual light source, non-metal material\
	!: no low-level optimizations yet (such as avoiding H calculation in https://twvideo01.ubm-us.net/o1/vault/gdc2017/Presentations/Hammon_Earl_PBR_Diffuse_Lighting.pdf)
	float3 Lit(float3 albedo, float roughness, float F0, float3 N, float3 viewDir, float3 lightDir, float3 lightIrradiance)
	{
		// TODO: move outside
		static const float PI_rcp = .318309886183790671538f;

		const float LdotN = dot(lightDir, N);

		/*
			early out (backlighting)
			?: <= or <
		*/
		[branch]
		if (LdotN <= 0)
			return 0;

		const float VdotN = dot(viewDir, N);

		const float a2 = roughness * roughness, VdotL = dot(viewDir, lightDir);
		const float3 H = normalize(viewDir + lightDir);
		const float NdotH = dot(N, H);

		float GGX_denom = NdotH * NdotH * (a2 - 1) + 1;	// without PI term
		GGX_denom *= GGX_denom;

		// optimization opportunity: merge NDF and G denoms
		float spec = Fresnel::Shlick(F0, dot(lightDir, H)) * (.25f * PI_rcp) * a2, specDenom = GGX::G_rcp(a2, VdotN, LdotN, VdotL) * abs(VdotN);

		/*
			handle 'VdotN == 0' case: 'G_rcp(_, 0, _, _) == inf' giving 'specDenom = inf * 0 == NaN'
			analyzing 'G_rcp() * abs(VdotN)' gives '.5 * a' in limit 'VdotN -> 0' (a2 = a * a, a = roughness)

			fp precision issues can get 'G_rcp == inf' when 'VdotN ~ 0' (but not exactly 0): VdotN squared inside G_rcp thus near-to-zero can became zero (which causes inf ultimately)
			this case would give 'inf * ~0 == inf' for specDenom while NaN is desired
			to handle it G_rcp() have special mend which translates inf induced by VdotN param into NaN which propagates in specDenom which in turn triggers special 'VdotN == 0' case handling
		*/
		[flatten]
		if (isnan(specDenom))
			specDenom = .5f * roughness;
		specDenom *= GGX_denom;
		spec /= specDenom;

		// GGX diffuse approximation from https://twvideo01.ubm-us.net/o1/vault/gdc2017/Presentations/Hammon_Earl_PBR_Diffuse_Lighting.pdf \
		!: potential mad optimizations via manual '(1 - x) * y' transformations
		const float
			facing = .5f * VdotL + .5f,
			rough = facing * (.9f - .4f * facing) * (.5f / NdotH + 1),
			smooth = (1 - Fresnel::Shlick(F0, LdotN)) * 1.05f * (1 - Fresnel::Pow5(1 - abs(VdotN))),
			single = PI_rcp * lerp(smooth, rough, roughness),
			multi = .1159f * roughness;

		/*
			LdotN factor here from rendering equation
			it is absent for specular since it is canceled with specular microfacet BRDF
			placed inside '()' in order to do more math in scalar rather than vector
		*/
		const float3 diffuse = albedo * ((single + albedo * multi) * LdotN);

		return (spec + diffuse) * lightIrradiance * SoftBackshadow(LdotN);
	}

	/*
		makes n frontfacing so that it won't appear dark (with proper light direction)
		backface ideally should be never visible and advanced techniques such as POM can ensure it
			by picking n at view ray intersection point
		here is approximation which simulates parallax for micro normal (normal mapping) relative to macro normal
		assume single-sided/symmetric bump (blended via parallaxCurvatureParams.x)
	*/
	void FixNormal(in float3 N, inout float3 n, in float3 viewDir)
	{
		static const float2 parallaxCurvatureParams = float2(.1f, 1.3f);
		const float VdotN = dot(viewDir, n);	// recompute later to reduce GPR pressure?
		if (VdotN < 0)
		{
#if 1	// groove
			const float3 reflected = lerp(N, reflect(n, N), parallaxCurvatureParams.x);
#if SMOOTH_PARALLAX && PRESERVE_PARALLAX_PLANE
#define FP_DEGENERATE_POSSIBLE
			const float3 perp = cross(cross(N, n)/*could be 0 in degenerate case*/, viewDir);
#endif
#else	// cone
			// reflect off plane holding N and orthogonal to <N, viewDir> plane
#define FP_DEGENERATE_POSSIBLE
			const float3 plane = normalize(cross(cross(viewDir, N), N));	// could be 0 in degenerate case
			const float3 reflected = n - (1 + parallaxCurvatureParams.x) * dot(n, plane) * plane;
#if SMOOTH_PARALLAX && PRESERVE_PARALLAX_PLANE
			const float3 perp = cross(cross(reflected, n), viewDir);
#endif
#endif

#if SMOOTH_PARALLAX
#if !PRESERVE_PARALLAX_PLANE
			const float3 perp = n - dot(n, viewDir) * viewDir;	// do not normalize to prevent degenerate cases (+ perf boost as bonus)
#endif
			const float3 fixed = normalize(lerp(perp, reflected, saturate(VdotN * VdotN * parallaxCurvatureParams.y)));
#ifdef FP_DEGENERATE_POSSIBLE
#undef FP_DEGENERATE_POSSIBLE
			[flatten]
			if (all(isfinite(fixed)))	// extra check for degenerate case, should never happened according to math but possible due to floating point precision issues
#endif
				n = fixed;
#else
			n = reflected;
#endif
		}
	}

	// simulates parallax for surface macro normal (interpolated vertex normal), viewDir assumed to be unit length
	void FixNormal(inout float3 N, in float3 viewDir)
	{
#if 1
		N -= 2 * min(dot(viewDir, N), 0) * viewDir;
#else
		/*
		micro optimization, assumes N is unit length
		relies on following GPU behavior
			free source negation (that's why '-' applied to dot`s source vector, not scalar dot result)
			free result saturate
		*/
		N += 2 * saturate(dot(-viewDir, N)) * viewDir;
#endif
	}

	// simulates parallax for interpolated TBN frame, viewDir assumed to be unit length
	void FixTBN(inout float3x3 TBN, in float3 viewDir)
	{
		const float VdotN = dot(viewDir, TBN[2]) < 0;
		if (VdotN)
		{
			const float3 N = TBN[2];	// old normal
			TBN[2] -= 2 * VdotN * viewDir;

			/*
				rotation matrix from quaternion
				!:	consider alternative rot matrix construction from {axis, angle}, compare ALU stress / GPR pressure
					GPR pressure probably more important, particular considering that it will run for quite few pixels
			*/
			const float3 H = normalize(N + TBN[2]);
			float4 q, q2;
			q.xyz = cross(N, H);
			q2.xyz = q * q;
			q2.w = 1 - q2.xyz;
			q.w = sqrt(q2.w);
			const float3x3 rot =
			{
				q2.w + q2.x - q2.y - q2.z,		2 * (q.x * q.y - q.z * q.w),	2 * (q.x * q.z + q.y * q.w),
				2 * (q.x * q.y + q.z * q.w),	q2.w - q2.x + q2.y - q2.z,		2 * (q.y * q.z - q.x * q.w),
				2 * (q.x * q.z - q.y * q.w),	2 * (q.y * q.z + q.x * q.w),	q2.w - q2.x - q2.y + q2.z
			};

			// rotate T & B
			TBN[0] = mul(TBN[0], rot);
			TBN[1] = mul(TBN[1], rot);
		}
	}

	/*
	N - macro normal
	n - micro normal
	*/
	inline float3 Lit(float3 albedo, float roughness, float F0, float3 N, float3 n, float3 viewDir, float3 lightDir, float3 lightIrradiance)
	{
		const float LdotN = dot(lightDir, N);

		// blocks light leaking from under the macro surface caused by normal maps
		[branch]
		if (LdotN <= 0)
			return 0;

		FixNormal(N, n, viewDir);

		return Lit(albedo, roughness, F0, n, viewDir, lightDir, lightIrradiance) * SoftBackshadow(LdotN);
	}
}