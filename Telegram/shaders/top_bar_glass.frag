#version 450

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D backdrop;
layout(binding = 2) uniform sampler2D foreground;
layout(binding = 3) uniform sampler2D original;
layout(std140, binding = 0) uniform Params {
	vec4 tint;
	vec2 crop;
	float flip;
	float backdropOpacity;
	float fade;
};

void main() {
	vec2 uv = vec2(v_texcoord.x, crop.x + v_texcoord.y * crop.y);
	vec4 sharp = texture(original, uv);
	sharp *= min(1.0, backdropOpacity / max(sharp.a, 0.00001));
	uv.y = mix(uv.y, 1.0 - uv.y, flip);
	vec4 bg = texture(backdrop, uv) * (1.0 - tint.a)
		+ vec4(tint.rgb * tint.a, tint.a);
	bg *= min(1.0, backdropOpacity / max(bg.a, 0.00001));
	vec4 fg = texture(foreground, v_texcoord);
	fragColor = mix(sharp, fg + bg * (1.0 - fg.a), fade);
}
