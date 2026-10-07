// Modern water surface pixel shader, ps_2_b. Compiled twice by scripts/compile-shaders.ps1:
//   /D RIVER      river polygons   (stage 3 = river alpha edge, shroud baked into the vertex color)
//   /D TRAPEZOID  area water, i.e. the sea (stage 3 = shroud texture)
// Needs a Direct3D layer that accepts shader model 2 on a Direct3D 8 device (the patched DXVK shipped with this port).
// If it cannot be created, or the scene copy is not available, the classic ps.1.1 shaders in W3DWater.cpp are used.
//
// The surface does not blend a flat texture over the scene. It rebuilds what is seen through the water:
//   * the terrain height of the whole map is stored in a texture (stage 5) and sampled in world space, so the water depth is exact
//     per pixel and the shoreline is smooth no matter how coarse the water mesh is
//   * the back buffer is copied before the water is drawn (stage 4) and read with a wave-distorted lookup: refraction
//   * Beer-Lambert absorption (red dies first) turns the refracted scene into turquoise shallows and navy deep water
//   * a multi-scale wave normal drives fresnel sky reflection and sun glints
//   * foam sits on the shoreline, travels in bands towards the beach and breaks up on the wave crests

sampler2D waterMap   : register(s0);	// water texture of the time of day
sampler2D sparkleMap : register(s1);	// white highlights on black
sampler2D noiseMap   : register(s2);	// noise, also used as the wave height field
sampler2D maskMap    : register(s3);	// RIVER: alpha edge, TRAPEZOID: shroud
sampler2D sceneMap   : register(s4);	// copy of the back buffer
sampler2D depthMap   : register(s5);	// terrain height of the map, world space
sampler2D reflMap    : register(s6);	// the scene mirrored at the water plane, same screen mapping as the view

float4 g_wave   : register(c1);	// x = refraction strength, y = foam strength, z = reflection strength, w = wave amplitude
float4 g_misc   : register(c2);	// x = vertex alpha without shroud, y = glint strength, z = glint sharpness, w = depth scale (shallow bays read as deeper)
float4 g_world  : register(c3);	// x = water height, y = time in seconds, zw = world size of the height texture
float4 g_height : register(c4);	// x = terrain height at texel 0, y = height range, zw = world position of the texture origin
float4 g_screen : register(c5);	// xy = scale, zw = offset from the projected position to back buffer uv
float4 g_refl   : register(c6);	// x = 1 when reflMap holds this frame's mirrored scene
float3 g_axisX  : register(c8);	// world x, y and z axis in camera space (rows of the view matrix)
float3 g_axisY  : register(c9);
float3 g_axisZ  : register(c10);
float3 g_sun    : register(c11);	// world space direction towards the sun
float4 g_obj[6] : register(c12);	// units on the water: xy = world position, z = radius, w = speed in world units per second
float4 g_dir[6] : register(c18);	// xy = heading, z = 1 when the entry is used

float4 main(float2 uvWater : TEXCOORD0, float2 uvSparkle : TEXCOORD1, float2 uvNoise : TEXCOORD2, float2 uvMask : TEXCOORD3,
	float4 projPos : TEXCOORD4, float2 uvDepth : TEXCOORD5, float3 camPos : TEXCOORD6, float4 vertexColor : COLOR0) : COLOR0
{
	float t = g_world.y;
	float2 world = uvDepth * g_world.zw - g_height.zw;

	// ---- depth below the water surface, in world units
	float terrain = tex2D(depthMap, uvDepth).r * g_height.y + g_height.x;
	float depth = g_world.x - terrain;

	// ---- waves: a sum of five directional waves (wave length 60 down to 8 units) with a noise-warped phase so the pattern
	// does not repeat in an obvious lattice. The slope of each wave is analytic.
	float4 nA = tex2D(noiseMap, world * 0.0110 + float2(t * 0.017, t * 0.011));
	float4 nB = tex2D(noiseMap, world * 0.0270 + float2(-t * 0.023, t * 0.019));
	float4 nC = tex2D(noiseMap, world * 0.0710 + float2(t * 0.041, -t * 0.033));
	float2 warped = world + (nA.rg - 0.5) * 14.0 + (nB.gb - 0.5) * 5.0;
	float2 slope = float2(0.0, 0.0);
	float crestSum = 0.0;
	{
		// direction, wave number, slope amplitude, speed
		const float2 d0 = float2(0.86, 0.51);   const float k0 = 0.105; const float a0 = 0.095; const float w0 = 0.70;
		const float2 d1 = float2(0.31, 0.95);   const float k1 = 0.185; const float a1 = 0.080; const float w1 = 0.95;
		const float2 d2 = float2(-0.55, 0.84);  const float k2 = 0.300; const float a2 = 0.065; const float w2 = 1.25;
		const float2 d3 = float2(0.97, -0.24);  const float k3 = 0.480; const float a3 = 0.050; const float w3 = 1.60;
		const float2 d4 = float2(-0.80, -0.60); const float k4 = 0.800; const float a4 = 0.030; const float w4 = 2.10;
		float c0 = cos(k0 * dot(d0, warped) - w0 * t);
		float c1 = cos(k1 * dot(d1, warped) - w1 * t + 1.3);
		float c2 = cos(k2 * dot(d2, warped) - w2 * t + 2.1);
		float c3 = cos(k3 * dot(d3, warped) - w3 * t + 0.4);
		float c4 = cos(k4 * dot(d4, warped) - w4 * t + 4.2);
		slope = d0 * (a0 * c0) + d1 * (a1 * c1) + d2 * (a2 * c2) + d3 * (a3 * c3) + d4 * (a4 * c4);
		crestSum = c0 * 0.40 + c1 * 0.32 + c2 * 0.28;
	}
	slope += (nA.rg - 0.5) * 0.06 + (nB.rg - 0.5) * 0.035;
	slope *= g_wave.w;

	// ---- units on the water: rings spreading from the hull and a V shaped wake with foam behind moving ones
	float wakeFoam = 0.0;
	[unroll] for (int i = 0; i < 6; i++)
	{
		float4 ob = g_obj[i];
		float3 od = g_dir[i];
		float2 dv = world - ob.xy;
		float dist = length(dv);
		float radius = ob.z + 1.0;
		float spd = saturate(ob.w * 0.045);
		float ringFade = saturate(1.0 - dist / (radius * 3.5 + 6.0)) * saturate(dist / (radius * 0.5));
		float ring = sin(dist * 1.6 - t * 4.0) * ringFade * (1.0 - spd) * 0.6 * od.z;	// idle hulls only: nothing rings ahead of a moving one
		slope += (dv / max(dist, 0.001)) * ring * 0.45;
		float behind = -dot(dv, od.xy);
		float across = dot(dv, float2(-od.y, od.x));
		float behindMask = saturate(behind / (radius * 0.6)) * saturate(1.0 - behind / (radius * 7.0 + 40.0));
		float armOffset = (abs(across) - (radius * 0.5 + behind * 0.32)) / (radius * 0.22 + 0.7 + behind * 0.04);
		float coreOffset = across / (radius * 0.55 + 0.6 + behind * 0.10);
		wakeFoam += (exp(-armOffset * armOffset) * 0.9 + exp(-coreOffset * coreOffset) * 0.6) * behindMask * spd * od.z;
	}

	// normal and view vector in camera space
	float3 normal = normalize(slope.x * g_axisX + slope.y * g_axisY + g_axisZ);
	float3 incident = normalize(camPos);
	float cosView = saturate(dot(normal, -incident));

	// ---- refraction: the scene seen through the wavy surface, less distorted close to the shore
	float2 screenUV = (projPos.xy / projPos.w) * g_screen.xy + g_screen.zw;
	float2 refractUV = screenUV + slope * g_wave.x * saturate(depth * 0.6);
	float3 scene = tex2D(sceneMap, refractUV).rgb;

	// ---- absorption: red is gone after a few units, blue survives
	float3 absorb = float3(0.34, 0.095, 0.060);
	float optical = max(depth, 0.0) * g_misc.w;
	float3 transmit = exp(-absorb * optical);
	float depthMix = saturate(optical * (1.0 / 9.0));
	float3 shallowCol = float3(0.040, 0.420, 0.470);
	float3 deepCol = float3(0.010, 0.060, 0.230);
	float3 waterTint = tex2D(waterMap, uvWater + slope * 0.02).rgb;
	float3 inscatter = lerp(shallowCol, deepCol, depthMix) * lerp(float3(1.0, 1.0, 1.0), waterTint * 1.7, 0.22) * (0.55 + 0.45 * saturate(g_sun.z));

	// light of the day / ambient shading from the vertex color (the sea) and shroud
	float3 lightTint = saturate(vertexColor.rgb * 1.9);
#ifdef RIVER
	float shroud = saturate(vertexColor.a / max(g_misc.x, 0.001));
	float4 edge = tex2D(maskMap, uvMask);
#else
	float shroud = 1.0;
#endif

	// facets of the waves facing the sun are lighter: this is what makes the waves readable from above
	float3 sunCam0 = g_sun.x * g_axisX + g_sun.y * g_axisY + g_sun.z * g_axisZ;
	float waveShade = 0.72 + 0.55 * saturate(dot(normal, sunCam0));
	float3 color = scene * transmit + inscatter * lightTint * waveShade * (1.0 - transmit);

	// ---- fresnel sky reflection
	float fresnel = 0.16 + 0.84 * pow(1.0 - cosView, 3.0);
	float3 reflected = reflect(incident, normal);
	float skyUp = saturate(dot(reflected, g_axisZ));
	float3 skyColor = lerp(float3(0.62, 0.74, 0.86), float3(0.20, 0.42, 0.80), pow(skyUp, 0.6));
	float2 reflUV = projPos.xy / projPos.w + slope * 0.030;
	float3 mirror = tex2D(reflMap, reflUV).rgb;
	float3 envColor = lerp(skyColor * lightTint, mirror, g_refl.x);
	color = lerp(color, envColor, saturate(fresnel * g_wave.z * saturate(depth * 2.0)));

	// ---- sun glints
	float3 sunCam = g_sun.x * g_axisX + g_sun.y * g_axisY + g_sun.z * g_axisZ;
	float3 halfVec = normalize(sunCam - incident);
	float spec = pow(saturate(dot(normal, halfVec)), g_misc.z) * g_misc.y;
	float2 sparkleUV = uvSparkle * 2.7 + slope * 0.05;
	float sparkle = tex2D(sparkleMap, sparkleUV).r * tex2D(sparkleMap, sparkleUV * 2.31 + 0.47).r;
	float sunFacing = saturate(dot(normal, sunCam));
	color += (spec + sparkle * 2.2 * pow(sunFacing, 14.0)) * float3(1.0, 0.96, 0.88) * saturate(depth * 0.8);

	// ---- foam
	float foamPattern = smoothstep(0.30, 0.78, nA.b * 0.55 + nB.g * 0.55 + nC.r * 0.30);
	float shoreWidth = 0.55 + 0.28 * sin(t * 0.85 + nA.r * 6.28);
	float shoreFoam = saturate(1.0 - depth / shoreWidth) * (0.55 + 0.45 * foamPattern);
	float bandPhase = depth * 3.0 - t * 0.85 + nB.r * 2.0;
	float bands = pow(saturate(sin(bandPhase)), 5.0) * saturate(1.0 - depth * (1.0 / 1.8)) * foamPattern;
	float crest = saturate((crestSum - 0.72) * 4.0) * foamPattern * saturate(depth * 0.3);
	float foam = saturate((shoreFoam + bands * 0.8 + crest * 0.6 + wakeFoam * foamPattern) * g_wave.y);
	color = lerp(color, float3(0.93, 0.97, 1.0) * lightTint, foam);

	// ---- opacity: the shore fades into the dry land, foam stays solid just off the beach
	float alpha = saturate(depth * 4.0 + 0.05);
	alpha = max(alpha, foam * saturate(depth * 10.0 + 0.3));
#ifdef RIVER
	alpha *= edge.a;
	color *= shroud;
#else
	color *= tex2D(maskMap, uvMask).rgb;
#endif
	return float4(color, alpha);
}
