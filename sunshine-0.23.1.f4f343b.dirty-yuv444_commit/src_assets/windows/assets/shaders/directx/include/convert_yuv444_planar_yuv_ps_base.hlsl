Texture2D image : register(t0);
SamplerState def_sampler : register(s0);

cbuffer color_matrix_cbuffer : register(b0) {
    float4 color_vec_y;
    float4 color_vec_u;
    float4 color_vec_v;
    float2 range_y;
    float2 range_uv;
};

cbuffer yuv_order_cbuffer : register(b2) {
    int yuv_order;
};

#include "include/base_vs_types.hlsl"

float3 main_ps(vertex_t input) : SV_Target
{
    float3 rgb = CONVERT_FUNCTION(image.Sample(def_sampler, input.tex_coord, 0).rgb);

    float y = dot(color_vec_y.xyz, rgb) + color_vec_y.w;
    float u = dot(color_vec_u.xyz, rgb) + color_vec_u.w;
    float v = dot(color_vec_v.xyz, rgb) + color_vec_v.w;

    y = y * range_y.x + range_y.y;
    u = u * range_uv.x + range_uv.y;
    v = v * range_uv.x + range_uv.y;

    if (yuv_order == 0)
      return float3(v, u, y);
    else if (yuv_order == 1)
      return float3(u, y, v);
    else
      return float3(y, u, v);
}
