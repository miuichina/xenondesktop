#version 450

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D source;
layout(std140, binding = 0) uniform Params {
	vec2 stepSize;
	float flip;
	float tapCount;
	vec4 taps[32];
};

void main() {
	vec2 uv = vec2(v_texcoord.x, mix(v_texcoord.y, 1.0 - v_texcoord.y, flip));
	vec4 color = texture(source, uv) * taps[0].z;
	for (int i = 0; i < 32; ++i) {
		if (float(i) >= tapCount) {
			break;
		}
		vec2 delta = stepSize * taps[i].x;
		color += (texture(source, uv + delta) + texture(source, uv - delta)) * taps[i].y;
	}
	fragColor = color;
}
