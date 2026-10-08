#version 450
// Native Vulkan backend: vertex stage that stands in for the Direct3D 8 fixed-function vertex pipeline.
// Two modes, chosen by flags.x: pre-transformed vertices (XYZRHW, screen space, used by the 2D interface) and transformed
// vertices (world*view*projection supplied by the engine, used by the 3D scene).

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inDiffuse;
layout(location = 3) in vec4 inSpecular;
layout(location = 4) in vec4 inUv0;
layout(location = 5) in vec4 inUv1;
layout(location = 6) in vec4 inUv2;
layout(location = 7) in vec4 inUv3;

layout(std140, set = 0, binding = 0) uniform Draw
{
	mat4 wvp;              // D3D row-major matrix, read as is: GLSL's M * v equals D3D's v * M
	vec4 viewport;         // x, y, width, height of the viewport in pixels
	vec4 textureFactor;
	uvec4 flags;           // x: 1 = pre-transformed vertices, y: unused, z: alpha test function, w: alpha test enable
	vec4 alphaRef;         // x: reference value 0..1
	uvec4 stage[8];        // per stage i: [2*i] = colorop, colorarg1, colorarg2, alphaop   [2*i+1] = alphaarg1, alphaarg2, texcoord index, 1 if a texture is bound
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
} draw;

layout(location = 0) out vec4 vDiffuse;
layout(location = 1) out vec4 vUv[4];

// Direct3D 8 fixed-function vertex lighting (diffuse and ambient, no specular)
vec4 lit(vec3 worldPos, vec3 worldNormal)
{
	vec4 diffuse = draw.lightFlags.y != 0u ? inDiffuse : draw.matDiffuse;
	vec4 ambient = draw.lightFlags.z != 0u ? inDiffuse : draw.matAmbient;
	vec4 emissive = draw.lightFlags.w != 0u ? inDiffuse : draw.matEmissive;
	vec3 n = normalize(worldNormal);
	vec3 sum = draw.sceneAmbient.rgb * ambient.rgb;
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
	}
	return vec4(clamp(emissive.rgb + sum, 0.0, 1.0), diffuse.a);
}

void main()
{
	vDiffuse = inDiffuse;
	vUv[0] = inUv0; vUv[1] = inUv1; vUv[2] = inUv2; vUv[3] = inUv3;

	if (draw.flags.x != 0u)
	{
		// XYZRHW: x and y are pixels relative to the viewport, z is the depth, w holds 1/w
		float w = (inPos.w != 0.0) ? 1.0 / inPos.w : 1.0;
		vec2 ndc = vec2((inPos.x - draw.viewport.x) * 2.0 / draw.viewport.z - 1.0,
		                (inPos.y - draw.viewport.y) * 2.0 / draw.viewport.w - 1.0);
		gl_Position = vec4(ndc * w, inPos.z * w, w);
	}
	else
	{
		vec4 p = draw.wvp * vec4(inPos.xyz, 1.0);
		p.y = -p.y;     // Direct3D clip space has y up, Vulkan's points down
		gl_Position = p;
		if (draw.lightFlags.x != 0u)
			vDiffuse = lit((draw.world * vec4(inPos.xyz, 1.0)).xyz, mat3(draw.world) * inNormal.xyz);
	}
}
