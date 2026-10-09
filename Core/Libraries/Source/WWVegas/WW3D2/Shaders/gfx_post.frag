#version 450
// Native Vulkan backend: post processing of the 3D scene. One source file, built once per pass with -DPASS_x.
//   BLOOM_DOWN0  bright pass and first down sample of the HDR scene
//   BLOOM_DOWN   down sample of a bloom level
//   BLOOM_UP     up sample, added onto the next larger level
//   COMPOSITE    scene + bloom, soft highlight roll-off and colour grading
//   FXAA         edge anti-aliasing of the finished image

layout(std140, set = 0, binding = 0) uniform Post
{
	vec4 texel;         // 1 / width, 1 / height, width, height of the source image of the pass
	vec4 p0;            // bloom threshold, bloom knee, bloom intensity, unused
	vec4 p1;            // saturation, contrast, ambient occlusion strength, shadow strength
	vec4 p2;            // fxaa on, 3D scene present, time, unused
	vec4 proj;          // projection: [0][0], [1][1], [2][2], [3][2]
	mat4 invView;       // camera space to world space
	mat4 lightVP;       // world space to shadow map clip space
	vec4 sunDir;        // world space direction towards the sun
	vec4 shadowParams;  // shadow map size, light size, bias, unused
} u;

layout(set = 0, binding = 1) uniform sampler2D t0;
layout(set = 0, binding = 2) uniform sampler2D t1;
layout(set = 0, binding = 3) uniform sampler2D t2;
layout(set = 0, binding = 4) uniform sampler2D t3;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

#if defined(PASS_BLOOM_DOWN0) || defined(PASS_BLOOM_DOWN)

vec3 prefilter(vec3 c)
{
#ifdef PASS_BLOOM_DOWN0
	float br = max(c.r, max(c.g, c.b));
	float knee = max(u.p0.y, 1e-4);
	float soft = clamp(br - u.p0.x + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee);
	float contrib = max(soft, br - u.p0.x) / max(br, 1e-4);
	return c * contrib;
#else
	return c;
#endif
}

vec3 tap(vec2 p) { return prefilter(min(texture(t0, p).rgb, vec3(16.0))); }

void main()
{
	vec2 d = u.texel.xy;
	vec3 a = tap(uv + d * vec2(-2.0, -2.0));
	vec3 b = tap(uv + d * vec2( 0.0, -2.0));
	vec3 c = tap(uv + d * vec2( 2.0, -2.0));
	vec3 dd = tap(uv + d * vec2(-2.0,  0.0));
	vec3 e = tap(uv);
	vec3 f = tap(uv + d * vec2( 2.0,  0.0));
	vec3 g = tap(uv + d * vec2(-2.0,  2.0));
	vec3 h = tap(uv + d * vec2( 0.0,  2.0));
	vec3 i = tap(uv + d * vec2( 2.0,  2.0));
	vec3 j = tap(uv + d * vec2(-1.0, -1.0));
	vec3 k = tap(uv + d * vec2( 1.0, -1.0));
	vec3 l = tap(uv + d * vec2(-1.0,  1.0));
	vec3 m = tap(uv + d * vec2( 1.0,  1.0));
	vec3 col = e * 0.125 + (a + c + g + i) * 0.03125 + (b + dd + f + h) * 0.0625 + (j + k + l + m) * 0.125;
	outColor = vec4(col, 1.0);
}

#elif defined(PASS_BLOOM_UP)

void main()
{
	vec2 d = u.texel.xy;
	vec3 a = texture(t0, uv + d * vec2(-1.0, -1.0)).rgb;
	vec3 b = texture(t0, uv + d * vec2( 0.0, -1.0)).rgb;
	vec3 c = texture(t0, uv + d * vec2( 1.0, -1.0)).rgb;
	vec3 dd = texture(t0, uv + d * vec2(-1.0,  0.0)).rgb;
	vec3 e = texture(t0, uv).rgb;
	vec3 f = texture(t0, uv + d * vec2( 1.0,  0.0)).rgb;
	vec3 g = texture(t0, uv + d * vec2(-1.0,  1.0)).rgb;
	vec3 h = texture(t0, uv + d * vec2( 0.0,  1.0)).rgb;
	vec3 i = texture(t0, uv + d * vec2( 1.0,  1.0)).rgb;
	vec3 col = (e * 4.0 + (b + dd + f + h) * 2.0 + (a + c + g + i)) * (1.0 / 16.0);
	outColor = vec4(col * 0.9, 1.0);
}

#elif defined(PASS_COMPOSITE)

vec3 rolloff(vec3 x)
{
	// identity up to 0.9, then a smooth shoulder: values that additive effects push above 1 compress instead of clipping
	vec3 over = max(x - 0.9, vec3(0.0));
	vec3 comp = 0.9 + 0.1 * tanh(over / 0.1);
	return mix(x, comp, step(vec3(0.9), x));
}

void main()
{
	vec3 c = texture(t0, uv).rgb;
	if (u.p2.y > 0.5)
	{
		vec3 bloom = texture(t1, uv).rgb;
		c += bloom * u.p0.z;
		c = rolloff(c);
		// colour grading: a little more saturation and contrast around the mid grey
		float l = luma(c);
		c = mix(vec3(l), c, u.p1.x);
		c = (c - 0.5) * u.p1.y + 0.5;
	}
	outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}

#elif defined(PASS_FXAA)

void main()
{
	if (u.p2.x < 0.5) { outColor = vec4(texture(t0, uv).rgb, 1.0); return; }
	vec2 px = u.texel.xy;
	vec3 rgbNW = texture(t0, uv + vec2(-1.0, -1.0) * px).rgb;
	vec3 rgbNE = texture(t0, uv + vec2( 1.0, -1.0) * px).rgb;
	vec3 rgbSW = texture(t0, uv + vec2(-1.0,  1.0) * px).rgb;
	vec3 rgbSE = texture(t0, uv + vec2( 1.0,  1.0) * px).rgb;
	vec3 rgbM  = texture(t0, uv).rgb;
	float lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE), lM = luma(rgbM);
	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
	vec2 dir;
	dir.x = -((lNW + lNE) - (lSW + lSE));
	dir.y =  ((lNW + lSW) - (lNE + lSE));
	float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
	float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * px;
	vec3 rgbA = 0.5 * (texture(t0, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(t0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
	vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(t0, uv + dir * -0.5).rgb + texture(t0, uv + dir * 0.5).rgb);
	float lB = luma(rgbB);
	outColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}

#endif
