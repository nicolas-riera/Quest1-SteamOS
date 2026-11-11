#version 450
// quest1_modifier.c: a triangle over the whole target, uv 0..1 on it (build_nv12_shaders.sh)
layout(location = 0) out vec2 uv;
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
