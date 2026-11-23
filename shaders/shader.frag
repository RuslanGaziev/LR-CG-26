#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragPos;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform UBO {
    mat4 mvp;
    mat4 model;
    vec4 camera_pos;
    vec4 light_direction;
    vec4 light_color;
    vec4 ambient_light;
    vec4 albedo;
    vec4 specular;
    vec4 material;        // x = shininess, y = use_texture
    mat4 light_view_proj;
    vec4 shadow_params;   // x = depth_bias, y = ortho_size, z = shadow_enabled
} ubo;

layout(binding = 1) uniform sampler2D albedo_texture;
layout(binding = 2) uniform sampler2DShadow shadow_map;

float calculateShadow(vec3 fragPosWorld, float NdotL) {
    if (ubo.shadow_params.z < 0.5) return 1.0;

    vec4 fragPosLightSpace = ubo.light_view_proj * vec4(fragPosWorld, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 1.0;
    }

    float currentDepth = projCoords.z - ubo.shadow_params.x;

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadow_map, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(x, y) * texelSize;
            shadow += texture(shadow_map, vec3(projCoords.xy + offset, currentDepth));
        }
    }
    shadow /= 9.0;

    return shadow;
}

void main() {
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(-ubo.light_direction.xyz);
    vec3 V = normalize(ubo.camera_pos.xyz - fragPos);
    vec3 H = normalize(L + V);

    vec3 baseColor;
    if (ubo.material.y > 0.5) {
        baseColor = texture(albedo_texture, fragUV).rgb;
    } else {
        baseColor = ubo.albedo.rgb;
    }

    vec3 ambient = ubo.ambient_light.rgb * baseColor;
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = ubo.light_color.rgb * NdotL * baseColor;
    
    float spec = pow(max(dot(N, H), 0.0), ubo.material.x);
    vec3 specular = ubo.light_color.rgb * spec * ubo.specular.rgb;

    float shadow = calculateShadow(fragPos, NdotL);

    // Блик (specular) НЕ умножается на тень, чтобы он не пропадал
    vec3 finalColor = ambient + (diffuse * shadow) + specular;

    outColor = vec4(finalColor, 1.0);
}