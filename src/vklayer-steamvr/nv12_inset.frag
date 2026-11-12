#version 450
// quest1_modifier.c: Steam Link VR's frame for vrcompositor, at its eye buffers' size, each wide view with its sharp
// inset blended in (quest1_views.h). `frame` is the frame as RGBA with smaller levels: the inset comes out smaller
// than it was rendered and is read at the level that fits. The blend's edge is the one of Valve's own shader for the
// Frame (distort_ps_achromatic_se2): full up to 80 % of the inset's half size, none at 99 %.
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(push_constant) uniform Inset
{
	vec4 place; // the inset over the wide view, in the atlas: u0 v0 u1 v1 (u1 <= u0: none)
	vec4 view;  // the inset's own view in the atlas
	float lod;  // its level here
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main()
{
	vec3 c = textureLod(frame, uv, 0.0).rgb;
	if (pc.place.z > pc.place.x) {
		vec2 t = (uv - pc.place.xy) / (pc.place.zw - pc.place.xy);
		vec2 e = abs(t * 2.0 - 1.0);
		float w = 1.0 - clamp((max(e.x, e.y) - 0.8) * 5.2631578, 0.0, 1.0);
		if (w > 0.0)
			c = mix(c, textureLod(frame, mix(pc.view.xy, pc.view.zw, t), pc.lod).rgb, w);
	}
	color = vec4(c, 1.0);
}
