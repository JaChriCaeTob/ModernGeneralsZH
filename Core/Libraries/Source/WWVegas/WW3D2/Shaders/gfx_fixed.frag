#version 450
// Native Vulkan backend: fragment stage that stands in for the Direct3D 8 texture stage combiner (up to 4 stages) and the
// alpha test. Blending, depth and culling are pipeline state.

layout(std140, set = 0, binding = 0) uniform Draw
{
	mat4 wvp;
	vec4 viewport;
	vec4 textureFactor;
	uvec4 flags;
	vec4 alphaRef;
	uvec4 stage[8];
	mat4 world;            // for lighting: world space position and normal
	vec4 matDiffuse;
	vec4 matAmbient;
	vec4 matEmissive;
	vec4 sceneAmbient;
	uvec4 lightFlags;      // x: 1 = fixed-function lighting, y/z/w: diffuse / ambient / emissive material source (0 material, 1 vertex color)
	uvec4 lightInfo;       // x: number of lights in use
	vec4 lightDiffuse[4];
	vec4 lightAmbient[4];
	vec4 lightPosType[4];  // xyz: position, w: type (1 point, 2 spot, 3 directional)
	vec4 lightDirRange[4]; // xyz: direction, w: range
	vec4 lightAtten[4];    // attenuation 0, 1, 2
	mat4 worldView;        // camera space position and normal for texture coordinate generation
	mat4 texMatrix[8];     // texture transform of each stage (D3D row-major, read like wvp)
	uvec4 texGen[8];       // per stage: x = source (0 vertex set, 1 camera normal, 2 camera position, 3 reflection), y = vertex set, z = transform count (0 off), w = 1 projected
	vec4 pointParams;      // size, min size, max size, 1 when point sprites are on
	vec4 pointScale;       // attenuation A, B, C, 1 when scaling is on
	vec4 softA;            // soft particles: projection [2][2], [3][2], fade distance, mode (0 off, 1 fade alpha, 2 fade colour)
	vec4 softB;            // size in pixels of the depth image
	vec4 camPos;           // xyz: camera in world space, w: strength of the per pixel highlight (0 = off)
} draw;

layout(set = 0, binding = 1) uniform sampler2D tex0;
layout(set = 0, binding = 2) uniform sampler2D tex1;
layout(set = 0, binding = 3) uniform sampler2D tex2;
layout(set = 0, binding = 4) uniform sampler2D tex3;
layout(set = 0, binding = 8) uniform sampler2D depthCopy;

layout(location = 0) in vec4 vDiffuse;
layout(location = 1) in vec4 vUv[8];
layout(location = 9) in vec4 vWorldPos;
layout(location = 10) in vec3 vWorldNormal;
layout(location = 11) in vec4 vRawDiffuse;
layout(location = 0) out vec4 outColor;

// Direct3D 8 fixed-function lighting (same as the vertex shader's), evaluated per pixel with the interpolated normal.
// hl receives the sun highlight and rim light that go on top of the textured colour.
vec4 litPixel(vec3 worldPos, vec3 worldNormal, out vec3 hl)
{
	vec4 diffuse = draw.lightFlags.y != 0u ? vRawDiffuse : draw.matDiffuse;
	vec4 ambient = draw.lightFlags.z != 0u ? vRawDiffuse : draw.matAmbient;
	vec4 emissive = draw.lightFlags.w != 0u ? vRawDiffuse : draw.matEmissive;
	vec3 n = normalize(worldNormal);
	vec3 v = normalize(draw.camPos.xyz - worldPos);
	vec3 sum = draw.sceneAmbient.rgb * ambient.rgb;
	hl = vec3(0.0);
	for (uint i = 0u; i < draw.lightInfo.x && i < 4u; ++i)
	{
		vec3 l;
		float att = 1.0;
		if (draw.lightPosType[i].w > 2.5)
			l = -normalize(draw.lightDirRange[i].xyz);
		else
		{
			vec3 d = draw.lightPosType[i].xyz - worldPos;
			float dist = length(d);
			l = d / max(dist, 1e-5);
			float a = draw.lightAtten[i].x + draw.lightAtten[i].y * dist + draw.lightAtten[i].z * dist * dist;
			att = (dist <= draw.lightDirRange[i].w || draw.lightDirRange[i].w <= 0.0) ? 1.0 / max(a, 1e-5) : 0.0;
		}
		float ndl = max(dot(n, l), 0.0);
		sum += (draw.lightAmbient[i].rgb * ambient.rgb + draw.lightDiffuse[i].rgb * diffuse.rgb * ndl) * att;
		// soft highlight of the strongest directional light (the sun), only where the surface faces it
		if (i == 0u && ndl > 0.0)
		{
			vec3 h = normalize(l + v);
			float s = pow(max(dot(n, h), 0.0), 36.0);
			hl += draw.lightDiffuse[i].rgb * s * draw.camPos.w * smoothstep(0.0, 0.25, ndl);
		}
	}
	// rim light: a faint sky coloured edge on surfaces seen at a grazing angle, stronger on the side away from the sun
	float rim = pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 3.0);
	hl += draw.sceneAmbient.rgb * rim * 0.35 * draw.camPos.w * 2.0;
	return vec4(clamp(emissive.rgb + sum, 0.0, 1.0), diffuse.a);
}

vec4 sampleStage(uint i, vec4 uv)
{
	vec2 c = (uv.w != 0.0 && uv.w != 1.0) ? uv.xy / uv.w : uv.xy;   // projective coordinates are rare, handled loosely
	if (i == 0u) return texture(tex0, c);
	if (i == 1u) return texture(tex1, c);
	if (i == 2u) return texture(tex2, c);
	return texture(tex3, c);
}

vec4 argument(uint code, vec4 current, vec4 tex, vec4 diffuse)
{
	uint a = code & 0xFu;
	vec4 v = diffuse;
	if (a == 1u) v = current;
	else if (a == 2u) v = tex;
	else if (a == 3u) v = draw.textureFactor;
	else if (a == 4u) v = vec4(0.0);          // specular is not carried by this backend yet
	if ((code & 0x10u) != 0u) v = vec4(1.0) - v;                 // complement
	if ((code & 0x20u) != 0u) v = vec4(v.a);                     // alpha replicate
	return v;
}

vec4 operate(uint op, vec4 a0, vec4 a1, vec4 a2, vec4 current, vec4 tex, vec4 diffuse)
{
	if (op == 2u) return a1;                                                 // select arg1
	if (op == 3u) return a2;                                                 // select arg2
	if (op == 4u) return a1 * a2;                                            // modulate
	if (op == 5u) return a1 * a2 * 2.0;                                      // modulate 2x
	if (op == 6u) return a1 * a2 * 4.0;                                      // modulate 4x
	if (op == 7u) return a1 + a2;                                            // add
	if (op == 8u) return a1 + a2 - 0.5;                                      // add signed
	if (op == 9u) return (a1 + a2 - 0.5) * 2.0;                              // add signed 2x
	if (op == 10u) return a1 - a2;                                           // subtract
	if (op == 11u) return a1 + a2 - a1 * a2;                                 // add smooth
	if (op == 12u) return mix(a2, a1, diffuse.a);                            // blend diffuse alpha
	if (op == 13u) return mix(a2, a1, tex.a);                                // blend texture alpha
	if (op == 14u) return mix(a2, a1, draw.textureFactor.a);                 // blend factor alpha
	if (op == 15u) return a1 + a2 * (1.0 - tex.a);                           // blend texture alpha, premultiplied
	if (op == 16u) return mix(a2, a1, current.a);                            // blend current alpha
	if (op == 17u) return vec4(a1.rgb * tex.rgb, a1.a);                      // premodulate (approximation)
	if (op == 18u) return vec4(a1.rgb + a1.a * a2.rgb, a1.a + a1.a * a2.a);  // modulate alpha, add color
	if (op == 19u) return vec4(a1.rgb * a2.rgb + a1.a, a1.a * a2.a);         // modulate color, add alpha
	if (op == 20u) return vec4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a * a2.a); // modulate inverse alpha, add color
	if (op == 21u) return vec4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a * a2.a); // modulate inverse color, add alpha
	if (op == 25u) return a0 + a1 * a2;                                      // multiply add
	if (op == 24u) { float d = 4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5); return vec4(d); } // dot product 3
	return a1 * a2;
}

void main()
{
	vec4 current = vDiffuse;
	vec4 diffuse = vDiffuse;
	vec3 highlight = vec3(0.0);
	const bool pixelLit = vWorldPos.w > 0.5;
	if (pixelLit)
	{
		diffuse = litPixel(vWorldPos.xyz, vWorldNormal, highlight);
		current = diffuse;
	}

	for (uint i = 0u; i < 4u; ++i)
	{
		uvec4 s0 = draw.stage[2u * i];
		uvec4 s1 = draw.stage[2u * i + 1u];
		uint colorOp = s0.x;
		if (colorOp == 1u)
			break;                                    // stage disabled: this and all later stages are skipped

		vec4 tex = vec4(1.0);
		if (s1.w != 0u)
			tex = sampleStage(i, draw.pointParams.w > 0.5 ? vec4(gl_PointCoord, 0.0, 1.0) : vUv[i]);

		vec4 c1 = argument(s0.y, current, tex, diffuse);
		vec4 c2 = argument(s0.z, current, tex, diffuse);
		vec4 c0 = argument(s1.z, current, tex, diffuse);
		vec4 color = operate(colorOp, c0, c1, c2, current, tex, diffuse);

		float alpha = current.a;
		uint alphaOp = s0.w;
		if (alphaOp != 1u)
		{
			vec4 a1 = argument(s1.x, current, tex, diffuse);
			vec4 a2 = argument(s1.y, current, tex, diffuse);
			alpha = operate(alphaOp, a1, a1, a2, current, tex, diffuse).a;
		}
		current = vec4(color.rgb, alpha);
	}

	if (draw.flags.w != 0u)
	{
		float a = current.a;
		float ref = draw.alphaRef.x;
		uint f = draw.flags.z;
		bool pass = true;
		if (f == 1u) pass = false;
		else if (f == 2u) pass = a < ref;
		else if (f == 3u) pass = a == ref;
		else if (f == 4u) pass = a <= ref;
		else if (f == 5u) pass = a > ref;
		else if (f == 6u) pass = a != ref;
		else if (f == 7u) pass = a >= ref;
		if (!pass) discard;
	}

	if (draw.softA.w > 0.5)
	{
		// particles fade out where they meet the geometry behind them
		float sceneDepth = texture(depthCopy, gl_FragCoord.xy / draw.softB.xy).r;
		float sceneZ = draw.softA.y / (sceneDepth + draw.softA.x);		// the game's projection is right handed: depth = -[2][2] + [3][2] / distance
		float fragZ = draw.softA.y / (gl_FragCoord.z + draw.softA.x);
		float f = clamp((sceneZ - fragZ) / draw.softA.z, 0.0, 1.0);
		f = f * f * (3.0 - 2.0 * f);
		if (sceneDepth >= 0.99999) f = 1.0;
		if (draw.softA.w > 1.5) current.rgb *= f; else current.a *= f;
	}

	if (pixelLit)
	{
		// the highlight is scaled by the surface colour so dark materials and shadowed areas do not glow; it may exceed 1 (HDR scene, feeds the bloom)
		float lum = dot(current.rgb, vec3(0.299, 0.587, 0.114));
		current.rgb += highlight * (0.25 + 0.75 * clamp(lum * 1.6, 0.0, 1.0)) * current.a;
		outColor = vec4(clamp(current.rgb, 0.0, 2.5), clamp(current.a, 0.0, 1.0));
		return;
	}
	outColor = clamp(current, 0.0, 1.0);
}
