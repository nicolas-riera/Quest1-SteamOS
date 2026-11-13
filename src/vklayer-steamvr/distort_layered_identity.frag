#version 450
// quest1_compat.c: stands in for vrcompositor's distort_ps_layered when its distortion is an identity (Monado does the
// lens and its chromatic aberration: /etc/quest1-warp-lens "1" or "2"). That shader reads the app's eye and the
// overlays' layer three times each, once per colour channel at the channel's own distorted place, and the three
// places are the same here (driver_quest1's ComputeDistortion gives (u, v) to all three). This one reads each once
// and otherwise does what it does: the app's eye masked to its bounds, from sRGB when asked, under the overlays
// (premultiplied), scaled, to sRGB, scaled, floored. Same interface (bindings, inputs, entry point PS_DISTORT).
struct EyeParams
{
	mat4 m0;
	mat4 m1;
	mat4 m2;
	mat4 m3;
	mat4 m4;
	mat4 m5;
	mat4 bounds; // column 2: the eye's bounds, centre and half size
};
struct Layer
{
	uint u0, u1, u2, u3, u4, u5, u6, u7;
	EyeParams eye[2];
};
layout(set = 0, binding = 4, std140) uniform Frame
{
	layout(row_major) mat4 m0;
	layout(row_major) mat4 m1;
	vec4 v2;
	vec4 v3;
	vec4 scale;      // before the sRGB encoding
	vec3 scale_srgb; // after it
	float f6;
	float floor_;
	float app_srgb; // the app's eye holds sRGB
	vec2 v9;
	float f10;
	float f11;
	vec2 v12;
	vec2 v13;
	vec2 v14;
	float f15;
	float f16;
	float f17;
	float f18;
	uint eye; // which of each layer's two eyes
	float f20;
	uint u21;
} frame;
layout(set = 0, binding = 6, std140) uniform Layers
{
	uint count;
	layout(row_major) Layer layer[3];
} layers;
layout(set = 0, binding = 9) uniform texture2D app[3];
layout(set = 0, binding = 25) uniform sampler smp;
layout(set = 0, binding = 13) uniform texture2D overlays;

layout(location = 0) noperspective in vec4 red;
layout(location = 1) noperspective in vec4 green; // xy: the app's eye, zw: the overlays
layout(location = 2) noperspective in vec4 blue;
layout(location = 3) flat in uint index;
layout(location = 0) out vec4 color;

void main()
{
	vec2 uv = green.xy;
	vec3 c;
	switch (index) {
	case 0u:
		c = texture(sampler2D(app[0], smp), uv).rgb;
		break;
	case 1u:
		c = texture(sampler2D(app[1], smp), uv).rgb;
		break;
	default:
		c = texture(sampler2D(app[2], smp), uv).rgb;
		break;
	}
	mat4 b = frame.eye == 0u ? layers.layer[index].eye[0].bounds : layers.layer[index].eye[1].bounds;
	vec2 inside = step(abs(uv - b[2].xy), b[2].zw);
	c *= inside.x * inside.y;
	if (frame.app_srgb != 0.0)
		c = mix(pow(c * 0.947867334 + 0.0521327, vec3(2.4)), c * 0.0773993805, lessThanEqual(c, vec3(0.04045)));
	vec4 o = texture(sampler2D(overlays, smp), green.zw);
	vec3 r = (o.rgb + c * (1.0 - o.a)) * frame.scale.xyz;
	r = mix(pow(r, vec3(0.41666667)) * 1.055 - 0.055, r * 12.92, lessThanEqual(r, vec3(0.0031308)));
	color = vec4(max(r * frame.scale_srgb, vec3(frame.floor_)), 1.0);
}
