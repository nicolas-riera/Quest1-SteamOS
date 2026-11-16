#version 450
// streambench.c: a region of a texture at level 0 (the mip step as render passes, and the reference read of the output)
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(push_constant) uniform Region
{
	vec4 region; // u0 v0 u1 v1 of the texture that the whole target shows
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main()
{
	color = vec4(textureLod(frame, mix(pc.region.xy, pc.region.zw, uv), 0.0).rgb, 1.0);
}
