#ifndef FRESNEL_INCLUDED
#define FRESNEL_INCLUDED

// currently for non-metals only

inline float Pow5(float x)
{
	float p = x * x;
	p *= p;
	return p * x;
}

inline float FresnelShlick(float F0, float LdotN)
{
	return lerp(Pow5(1 - LdotN), 1, F0);
}

float FresnelFull(float IOR, float LdotN);
/*
	from https://seblagarde.wordpress.com/2013/03/19/water-drop-3a-physically-based-wet-surfaces/
	inline to allow for #include in C++
*/
inline float FresnelFull(float IOR, float c)
{
	const float G = IOR * IOR + c * c - 1;

	if (G < 0)
		return 1;

	const float g = sqrt(G), p = (g - c) / (g + c), q = ((g + c) * c - 1) / ((g - c) * c + 1);
	return .5f * p * p * (1 + q * q);
}

inline float F0(float IOR)
{
	const float p = (IOR - 1) / (IOR + 1);
	return p * p;
}

inline float F0(float IOR1, float IOR2)
{
	const float p = (IOR2 - IOR1) / (IOR2 + IOR1);
	return p * p;
}

#endif	// FRESNEL_INCLUDED