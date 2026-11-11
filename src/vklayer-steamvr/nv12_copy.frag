#version 450
// quest1_modifier.c: an NV12 frame as RGB, converted by the sampler (BT.709, full range). vrcompositor shows what it
// samples as it is: raw YCbCr (the identity model, or re-encoded here) came out magenta (2026-11-11).
layout(set = 0, binding = 0) uniform sampler2D nv12;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main()
{
	color = vec4(texture(nv12, uv).rgb, 1.0);
}
