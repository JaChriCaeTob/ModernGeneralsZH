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

#if defined(PASS_AO) || defined(PASS_AOBLUR) || defined(PASS_SHADOW)

// view space depth of a depth buffer value; the sign of the projection's [2][3] element is in p2.w
float viewZ(float d) { return u.proj.w / (u.p2.w * d - u.proj.z); }

vec3 viewPos(vec2 p)
{
	float z = viewZ(texture(t0, p).r);
	vec2 ndc = vec2(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0);
	return vec3(ndc.x * u.p2.w * z / u.proj.x, ndc.y * u.p2.w * z / u.proj.y, z);
}

#endif

#if defined(PASS_SHADOW)

const vec2 kDisk[24] = vec2[](
	vec2(0.1, 0.2), vec2(-0.45, 0.12), vec2(0.38, -0.31), vec2(-0.12, -0.52), vec2(0.62, 0.21), vec2(-0.68, -0.18),
	vec2(0.22, 0.68), vec2(-0.31, 0.61), vec2(0.78, -0.35), vec2(-0.82, 0.31), vec2(0.05, -0.85), vec2(0.45, 0.58),
	vec2(-0.58, -0.62), vec2(0.88, 0.1), vec2(-0.1, 0.92), vec2(0.62, -0.7), vec2(-0.9, -0.1), vec2(0.3, 0.95),
	vec2(-0.5, 0.82), vec2(0.95, 0.38), vec2(-0.25, -0.95), vec2(0.7, 0.72), vec2(-0.95, 0.5), vec2(0.15, -0.5));

void main()
{
	float d = texture(t0, uv).r;
	if (d >= 0.99999) { outColor = vec4(1.0); return; }
	vec3 P = viewPos(uv);
	vec3 Pw = (u.invView * vec4(P, 1.0)).xyz;
	vec4 lc = u.lightVP * vec4(Pw, 1.0);
	vec2 suv = vec2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);
	if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 || lc.z > 1.0) { outColor = vec4(1.0); return; }

	float size = u.shadowParams.x;
	float range = u.shadowParams.z;
	float mapWorld = u.shadowParams.w;
	float texelWorld = mapWorld / size;
	float zr = lc.z - (texelWorld * 2.2 + 0.12) / range;
	float lightSize = u.shadowParams.y;
	float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	mat2 rot = mat2(cos(ang), sin(ang), -sin(ang), cos(ang));

	// blocker search: how far in front of this point is the geometry that shades it
	float searchWorld = lightSize * range * 0.35;
	float searchUV = clamp(searchWorld / mapWorld, 2.0 / size, 0.03);
	float sum = 0.0; float cnt = 0.0;
	for (int i = 0; i < 16; ++i)
	{
		vec2 o = rot * kDisk[i] * searchUV;
		float sd = texture(t1, suv + o).r;
		if (sd < zr) { sum += sd; cnt += 1.0; }
	}
	if (cnt < 0.5) { outColor = vec4(1.0); return; }
	float zb = sum / cnt;
	// penumbra: grows with the distance between caster and receiver
	float penWorld = (zr - zb) * range * lightSize;
	float rUV = clamp(penWorld / mapWorld, 1.4 / size, 0.02);
	float lit = 0.0;
	for (int i = 0; i < 24; ++i)
	{
		vec2 o = rot * kDisk[i] * rUV;
		lit += (texture(t1, suv + o).r < zr) ? 0.0 : 1.0;
	}
	outColor = vec4(lit / 24.0, 0.0, 0.0, 1.0);
}

#elif defined(PASS_AO)

void main()
{
	float d = texture(t0, uv).r;
	if (d >= 0.99999) { outColor = vec4(1.0); return; }
	vec2 px = u.texel.xy;
	vec3 P = viewPos(uv);
	vec3 Pr = viewPos(uv + vec2(px.x, 0.0)), Pl = viewPos(uv - vec2(px.x, 0.0));
	vec3 Pu = viewPos(uv - vec2(0.0, px.y)), Pd = viewPos(uv + vec2(0.0, px.y));
	vec3 dx = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
	vec3 dy = abs(Pu.z - P.z) < abs(P.z - Pd.z) ? Pu - P : P - Pd;
	vec3 n = normalize(cross(dx, dy));
	if (dot(n, -P) < 0.0) n = -n;

	float radius = u.p1.w;
	float dist = abs(P.z);
	float rpx = clamp(radius * abs(u.proj.x) * 0.5 * u.texel.z / dist, 3.0, 110.0);
	float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float occ = 0.0;
	const int N = 20;
	for (int i = 0; i < N; ++i)
	{
		float t = (float(i) + 0.5) / float(N);
		float a = ang + float(i) * 2.399963;
		vec2 o = vec2(cos(a), sin(a)) * rpx * (0.15 + 0.85 * t) * px;
		vec3 S = viewPos(uv + o);
		vec3 v = S - P;
		float len = length(v);
		float ndv = dot(n, v) / max(len, 1e-4);
		float fall = 1.0 - clamp(len / radius, 0.0, 1.0);
		occ += max(ndv - 0.10, 0.0) * fall;
	}
	occ = occ / float(N) * 2.4;
	outColor = vec4(1.0 - clamp(occ * u.p1.z, 0.0, 0.85), 0.0, 0.0, 1.0);
}

#elif defined(PASS_AOBLUR)

// depth aware blur along the direction in p2.xy (in uv units per tap), depth buffer in t1
void main()
{
	float zc = abs(viewZ(texture(t1, uv).r));
	float sum = texture(t0, uv).r, wsum = 1.0;
	for (int k = -4; k <= 4; ++k)
	{
		if (k == 0) continue;
		vec2 p = uv + u.p2.xy * float(k);
		float z = abs(viewZ(texture(t1, p).r));
		float w = (1.0 - abs(float(k)) / 5.0) * exp(-abs(z - zc) / max(zc, 1.0) * 120.0);
		sum += texture(t0, p).r * w;
		wsum += w;
	}
	outColor = vec4(sum / wsum, 0.0, 0.0, 1.0);
}

#elif defined(PASS_BLOOM_DOWN0) || defined(PASS_BLOOM_DOWN)

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
		if (u.p1.z > 0.0)
			c *= texture(t2, uv).r;
		if (u.sunDir.w > 0.0)
			c *= mix(1.0, 0.0 + texture(t3, uv).r, u.sunDir.w) * 1.0 + 0.0;
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
