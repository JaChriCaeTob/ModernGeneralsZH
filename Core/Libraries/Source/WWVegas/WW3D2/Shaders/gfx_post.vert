#version 450
// Native Vulkan backend: full screen triangle for the post processing passes.

layout(location = 0) out vec2 uv;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
