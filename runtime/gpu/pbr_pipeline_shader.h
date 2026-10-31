#pragma once
// Separate stages for the dedicated fixed-geometry path; never appended to legacy final-color output.
#include <string_view>
namespace gpu::pbr::pipeline::shader {
inline constexpr std::string_view kCommon=R"HLSL(
cbuffer PbrGeometry : register(b0) {
    row_major float4x4 pbr_model_to_view;
    row_major float4x4 pbr_normal_to_view;
    row_major float4x4 pbr_model_to_clip;
    row_major float4x4 pbr_view_to_clip;
    float4 pbr_raster;
    float4 pbr_surface_size;
    uint4 pbr_geometry_options;
    float4 pbr_depth;
};
cbuffer PbrMaterial : register(b1) {
    float4 pbr_base_color;
    float4 pbr_emission_color;
    float4 pbr_surface;
    float4 pbr_overrides;
    float4 pbr_height;
    uint4 pbr_options;
    uint4 pbr_maps;
    uint4 pbr_coverage;
};
struct PbrLight {
    float4 position_range, direction_type, radiance, attenuation_falloff, cone;
};
cbuffer PbrScene : register(b2) {
    float4 pbr_irradiance_exposure;
    uint4 pbr_scene_info;
    PbrLight pbr_lights[8];
    float4 pbr_fog;       // start, end, density, table mode (0 none)
    float4 pbr_fog_color;
};
// Albedo/emission require sRGB SRVs; these samples are consequently ALREADY linear.
Texture2D<float4> pbr_albedo : register(t0);
Texture2D<float4> pbr_normal : register(t1);
Texture2D<float4> pbr_orm : register(t2);
Texture2D<float> pbr_height_map : register(t3);
Texture2D<float4> pbr_emission : register(t4);
SamplerState pbr_sampler : register(s0);
struct PbrVertex {
    float3 model_position : POSITION0;
    float3 model_normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float2 uv1 : TEXCOORD1;
};
struct PbrRaster {
    float4 clip_position : SV_Position;
    float3 view_position : TEXCOORD0;
    float3 view_normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
    float fog : TEXCOORD4;
    float2 uv1 : TEXCOORD5;
};
struct PbrControl {
    float3 view_position : POSITION0;
    float3 view_normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float2 uv1 : TEXCOORD1;
};
float4 PbrProject(float3 view_position) {
    return mul(float4(view_position,1),pbr_view_to_clip);
}
// The legacy fixed-function fogfactor(abs(cp.z)), per vertex, on the same composed clip position.
float PbrFogFactor(float d) {
    if (pbr_fog.w == 3) return (pbr_fog.y - d) / (pbr_fog.y - pbr_fog.x);
    if (pbr_fog.w == 1) return exp(-d * pbr_fog.z);
    if (pbr_fog.w == 2) return exp(-(d * pbr_fog.z) * (d * pbr_fog.z));
    return 1;
}
// The legacy ReversedClip on the same clip position, so both paths store the same depth.
float4 PbrReversedDepth(float4 cp) {
    if(pbr_depth.w==0)return cp;
    cp.z=pbr_depth.x!=0?pbr_depth.x*(pbr_depth.y-(cp.w+pbr_depth.z)):cp.w-cp.z-pbr_depth.z*cp.w;
    return cp;
}
float4 PbrFixedEdgeCorrection(float4 cp) {
    // Exact existing fixed-function far-edge policy, using the guest surface rather than host dimensions.
    float2 sp=(float2(cp.x,-cp.y)/cp.w+1)*0.5*pbr_surface_size.xy;
    if(abs(sp.x-(pbr_surface_size.x-0.5))<1e-3)cp.x=cp.w;
    if(abs(sp.y-(pbr_surface_size.y-0.5))<1e-3)cp.y=-cp.w;
    return cp;
}
)HLSL";
inline constexpr std::string_view kVertex=R"HLSL(
PbrRaster main(PbrVertex i) {
    PbrRaster o=(PbrRaster)0;
    // CPU contract already rejects nonaffine/singular transforms and malformed packed vertices.
    PbrGeoPosition(i.model_position,pbr_model_to_view,o.view_position);
    PbrGeoNormal(i.model_normal,pbr_normal_to_view,o.view_normal);
    // Preserve the original renderer's already-composed matrix and operation order for ordinary raster depth.
    float4 cp=mul(float4(i.model_position,1),pbr_model_to_clip);
    o.fog=PbrFogFactor(abs(cp.z));
    o.clip_position=PbrReversedDepth(PbrFixedEdgeCorrection(cp));o.uv=i.uv;o.color=i.color;o.uv1=i.uv1;return o;
}
)HLSL";
inline constexpr std::string_view kPatchVertex=R"HLSL(
PbrControl main(PbrVertex i) {
    PbrControl o=(PbrControl)0;
    PbrGeoPosition(i.model_position,pbr_model_to_view,o.view_position);
    PbrGeoNormal(i.model_normal,pbr_normal_to_view,o.view_normal);o.uv=i.uv;o.color=i.color;o.uv1=i.uv1;return o;
}
)HLSL";
inline constexpr std::string_view kHull=R"HLSL(
struct PbrPatch { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
float PbrEdgeFactor(float3 a,float3 b) {
    float limit=clamp(pbr_height.w,1,16);
    if(pbr_geometry_options.x==0) return limit;
    // Symmetric endpoints give identical shared-edge factors only for the separately proved continuous mesh.
    float4 ca=PbrProject(a),cb=PbrProject(b);
    if(!all(isfinite(ca))||!all(isfinite(cb))||ca.w<=1e-6||cb.w<=1e-6)return limit;
    float2 delta=(ca.xy/ca.w-cb.xy/cb.w)*0.5*pbr_raster.xy;
    float pixels=length(delta);
    if(!isfinite(pixels))return limit;
    return clamp(ceil(pixels/max(pbr_raster.z,1)),1,limit);
}
PbrPatch PbrPatchConstants(InputPatch<PbrControl,3> p,uint patch_id : SV_PrimitiveID) {
    PbrPatch o;
    o.edge[0]=PbrEdgeFactor(p[1].view_position,p[2].view_position);
    o.edge[1]=PbrEdgeFactor(p[2].view_position,p[0].view_position);
    o.edge[2]=PbrEdgeFactor(p[0].view_position,p[1].view_position);
    o.inside=max(o.edge[0],max(o.edge[1],o.edge[2]));return o;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PbrPatchConstants")]
[maxtessfactor(16.0)]
PbrControl main(InputPatch<PbrControl,3> p,uint id : SV_OutputControlPointID,uint patch_id : SV_PrimitiveID) {
    return p[id];
}
)HLSL";
inline constexpr std::string_view kDomain=R"HLSL(
struct PbrPatch { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
[domain("tri")]
PbrRaster main(PbrPatch patch,float3 bary : SV_DomainLocation,const OutputPatch<PbrControl,3> p) {
    PbrRaster o=(PbrRaster)0;
    o.view_position=p[0].view_position*bary.x+p[1].view_position*bary.y+p[2].view_position*bary.z;
    float3 n=p[0].view_normal*bary.x+p[1].view_normal*bary.y+p[2].view_normal*bary.z;
    PbrGeoUnit(n,o.view_normal);
    o.uv=p[0].uv*bary.x+p[1].uv*bary.y+p[2].uv*bary.z;
    o.color=p[0].color*bary.x+p[1].color*bary.y+p[2].color*bary.z;
    o.uv1=p[0].uv1*bary.x+p[1].uv1*bary.y+p[2].uv1*bary.z;
    if(pbr_options.w==2&&pbr_maps.z!=0) {
        float height=pbr_height_map.SampleLevel(pbr_sampler,o.uv,0);
        // This changes actual geometry, depth and silhouette. It is distinct from the PS UV-only parallax path.
        if(isfinite(height))o.view_position+=o.view_normal*((height-pbr_height.z)*pbr_height.y);
    }
    float4 cp=PbrProject(o.view_position);o.fog=PbrFogFactor(abs(cp.z));
    o.clip_position=PbrReversedDepth(PbrFixedEdgeCorrection(cp));return o;
}
)HLSL";
inline constexpr std::string_view kPixel=R"HLSL(
float3 PbrSrgbToLinear(float3 x) {
    x=saturate(x);return float3(x.x<=0.04045?x.x/12.92:pow((x.x+0.055)/1.055,2.4),
        x.y<=0.04045?x.y/12.92:pow((x.y+0.055)/1.055,2.4),x.z<=0.04045?x.z/12.92:pow((x.z+0.055)/1.055,2.4));
}
float3 PbrLinearToSrgb(float3 x) {
    x=max(x,0);return float3(x.x<=0.0031308?12.92*x.x:1.055*pow(x.x,1.0/2.4)-0.055,
        x.y<=0.0031308?12.92*x.y:1.055*pow(x.y,1.0/2.4)-0.055,
        x.z<=0.0031308?12.92*x.z:1.055*pow(x.z,1.0/2.4)-0.055);
}
// Converts a view-space distance along the tangent ray into actual UV units using this surface's derivatives.
bool PbrUvDistance(float3 dpdx,float3 dpdy,float2 dtdx,float2 dtdy,float3 direction,out float2 uv_direction) {
    uv_direction=0;
    float d=dtdx.x*dtdy.y-dtdx.y*dtdy.x;
    float uv_lengths=dot(dtdx,dtdx)*dot(dtdy,dtdy);
    if(!(uv_lengths>0)||abs(d)<=1e-6*sqrt(uv_lengths))return false;
    float3 dpdu=(dpdx*dtdy.y-dpdy*dtdx.y)/d,dpdv=(dpdy*dtdx.x-dpdx*dtdy.x)/d;
    float aa=dot(dpdu,dpdu),bb=dot(dpdv,dpdv),ab=dot(dpdu,dpdv),det=aa*bb-ab*ab;
    if(!(aa>0)||!(bb>0)||!isfinite(det)||det<=1e-12*aa*bb)return false;
    float da=dot(direction,dpdu),db=dot(direction,dpdv);
    uv_direction=float2((da*bb-db*ab)/det,(db*aa-da*ab)/det);
    return all(isfinite(uv_direction));
}
// Fine surface grain past the maps' resolution: two octaves of gradient noise at two grains per texel of the
// surface's own texture (so its size follows the art, not the tiling), as a tangent-space slope. It fades out where
// its frequency nears the pixel footprint, so it shows only where the texels are magnified and never shimmers.
float2 PbrNoiseGrad(float2 p) {
    float2 i=floor(p),f=frac(p),u=f*f*(3-2*f),du=6*f*(1-f);
    float2 g[4];
    [unroll]for(uint k=0;k<4;++k){float2 c=i+float2(k&1,k>>1);
        float h=frac(sin(dot(c,float2(127.1,311.7)))*43758.5453)*6.2831853;g[k]=float2(cos(h),sin(h));}
    float a=dot(g[0],f),b=dot(g[1],f-float2(1,0)),c=dot(g[2],f-float2(0,1)),d=dot(g[3],f-1);
    float2 grad=lerp(lerp(g[0],g[1],u.x),lerp(g[2],g[3],u.x),u.y);
    return grad+du*float2((b-a)+(a-b-c+d)*u.y,(c-a)+(a-b-c+d)*u.x);
}
float2 PbrDetail(float2 uv,float2 tx,float2 ty) {
    uint w,h;pbr_albedo.GetDimensions(w,h);
    const float frequency=2*float(max(max(w,h),16u));
    float footprint=max(length(tx),length(ty))*frequency;
    float fade=saturate(1.5-2*footprint);
    if(fade<=0)return 0;
    return (PbrNoiseGrad(uv*frequency)+0.5*PbrNoiseGrad(uv*frequency*2.13+17.0))*0.035*fade;
}
float2 PbrParallax(float2 uv,float3 view,float3 n,float3 dx,float3 dy,float2 tx,float2 ty) {
    float nv=dot(n,view);float2 ray;
    if(pbr_options.w!=1||pbr_maps.z==0||pbr_height.x<=0||nv<=0.05||
       !PbrUvDistance(dx,dy,tx,ty,view-n*nv,ray))return uv;
    ray*=pbr_height.x/nv;
    if(!all(isfinite(ray)))return uv;
    // The base plane is midpoint. March from the top toward the bottom, then interpolate the bracket.
    float2 top=uv+ray*(1-pbr_height.z);
    uint steps=(uint)lerp(32.0,8.0,saturate(nv));float lo=0,hi=1;
    float previous=0;
    [loop]for(uint k=0;k<=32;++k) {
        if(k>steps)break;float depth=(float)k/steps;
        float h=pbr_height_map.SampleGrad(pbr_sampler,top-ray*depth,tx,ty);
        if(!isfinite(h))return uv;
        if(depth>=1-saturate(h)){lo=previous;hi=depth;break;}previous=depth;
    }
    [unroll]for(uint j=0;j<4;++j){float middle=(lo+hi)*0.5;
        float h=pbr_height_map.SampleGrad(pbr_sampler,top-ray*middle,tx,ty);
        if(!isfinite(h))return uv;if(middle>=1-saturate(h))hi=middle;else lo=middle;}
    return top-ray*((lo+hi)*0.5);
}
float3 PbrLightAt(PbrLight light,float3 position,out float3 l) {
    l=0;float3 result=0;
    if(light.direction_type.w==0) {
        l=-Mc3PbrSafeNormalize(light.direction_type.xyz);
        result=max(light.radiance.xyz,0);
    } else {
        float3 delta=light.position_range.xyz-position;float dist=length(delta);
        if(dist>0&&isfinite(dist)&&dist<=light.position_range.w) {
            l=delta/dist;
            float3 a=light.attenuation_falloff.xyz;
            float attenuation=1/max(a.x+a.y*dist+a.z*dist*dist,1e-6);
            if(light.direction_type.w==2) {
                float rho=dot(-l,Mc3PbrSafeNormalize(light.direction_type.xyz));
                float cone=rho>=light.cone.x?1:rho<=light.cone.y?0:(rho-light.cone.y)/(light.cone.x-light.cone.y);
                attenuation*=cone>0?pow(saturate(cone),light.attenuation_falloff.w):0;
            }
            result=max(light.radiance.xyz,0)*attenuation;
        }
    }
    return result;
}
float4 main(PbrRaster i) : SV_Target0 {
    float3 dx=ddx(i.view_position),dy=ddy(i.view_position),n,view;
    float2 tx=ddx(i.uv),ty=ddy(i.uv);
    // A CPU-qualified triangle supplies valid interpolants; guards also keep exceptional pixels finite.
    bool normal_ok=PbrGeoUnit(i.view_normal,n),view_ok=PbrGeoUnit(-i.view_position,view);
    if(pbr_geometry_options.y==1) {
        // No authored normal: shade with this triangle's own plane, facing the viewer it is drawn to.
        // A near-zero-area sliver keeps the interpolated normal instead.
        float3 face;
        if(PbrGeoFaceNormal(dx,dy,false,face)){n=dot(face,view)<0?-face:face;normal_ok=true;}
    }
    if(!normal_ok||!view_ok)return float4(0,0,0,1);
    if(pbr_options.w==2&&pbr_geometry_options.y!=1) {
        float3 displaced_n;
        if(PbrGeoFaceNormal(dx,dy,false,displaced_n))n=dot(displaced_n,n)<0?-displaced_n:displaced_n;
    }
    float2 uv=PbrParallax(i.uv,view,n,dx,dy,tx,ty);
    float4 albedo=pbr_albedo.SampleGrad(pbr_sampler,uv,tx,ty);
    // The game's own texture holds gamma values; the legacy path filters them as they are, so decode after sampling.
    if(pbr_geometry_options.z==1)albedo.rgb=PbrSrgbToLinear(albedo.rgb);
    // The original tests the alpha it writes: in baked mode the texture's times the cascade's (vertex alpha).
    if(pbr_coverage.x!=0)clip((pbr_scene_info.z==1?albedo.a*saturate(i.color.a):albedo.a)-pbr_overrides.z);
    float3 base=max(albedo.rgb,0)*pbr_base_color.rgb;
    float3 orm=pbr_orm.SampleGrad(pbr_sampler,uv,tx,ty).rgb;
    float roughness=pbr_options.y!=0?pbr_overrides.x:saturate(orm.g*pbr_surface.x);
    float metallic=pbr_options.z!=0?pbr_overrides.y:saturate(orm.b*pbr_surface.y);
    float ao=saturate(1+(orm.r-1)*pbr_surface.w);
    const float3 n_geo=n; // the surface's own normal, before the normal map
    PbrGeoFrame frame;
    if(pbr_maps.x!=0&&pbr_surface.z>0&&PbrGeoCotangentFrame(dx,dy,tx,ty,n,frame)) {
        float3 map=saturate(pbr_normal.SampleGrad(pbr_sampler,uv,tx,ty).xyz)*2-1;
        map.xy*=pbr_surface.z;if(pbr_options.x==0)map.y=-map.y;
        if(pbr_scene_info.z==1)map.xy+=PbrDetail(uv,tx,ty)*pbr_surface.z; // baked city surfaces: fine grain
        float3 mapped; // Common-scale cotangents preserve skew UV metric; never normalize T/B separately.
        if(PbrGeoUnit(frame.tangent*map.x+frame.bitangent*map.y+frame.normal*map.z,mapped))n=mapped;
    }
    float3 radiance;
    if(pbr_scene_info.z==1) {
        // Baked: the vertex diffuse is display-referred like the original texture, so decode both and the
        // neutral-map product matches the original texture*diffuse. The baked light has no direction, so it is
        // taken to fall from above the surface toward the viewer: the normal map shades relative to the surface's own
        // normal (relief, exactly 1 for a flat map) and a glossy lobe of the same light picks out the surface where
        // roughness allows (none at roughness 1). Normal strength scales the relief, roughness the gloss.
        float3 light=PbrSrgbToLinear(i.color.rgb);
        float3 l=Mc3PbrSafeNormalize(n_geo*0.8+view*0.6),h=Mc3PbrSafeNormalize(l+view);
        float nl=saturate(dot(n,l)),nh=saturate(dot(n,h)),nv=max(dot(n,view),1e-4),vh=saturate(dot(view,h));
        float relief=clamp(nl/max(dot(n_geo,l),0.2),0.35,1.65);
        float a=max(roughness*roughness,0.002),a2=a*a,d=nh*nh*(a2-1)+1,k=a*0.5;
        float lobe=a2/(3.14159265*d*d)*0.25/((nl*(1-k)+k)*(nv*(1-k)+k))*nl;
        float fresnel=0.04+0.96*pow(1-vh,5);
        radiance=saturate(base)*saturate(ao)*light*relief
                +light*saturate(ao)*min(lobe*fresnel,4.0)*(1-roughness)*(1-metallic*0.5);
    } else {
        radiance=Mc3PbrIndirectDiffuse(base,metallic,ao,pbr_irradiance_exposure.xyz);
        [loop]for(uint k=0;k<min(pbr_scene_info.x,8u);++k) {
            float3 l=0;float3 incoming=PbrLightAt(pbr_lights[k],i.view_position,l);
            radiance+=Mc3PbrDirect(base,metallic,roughness,n,view,l,incoming);
        }
    }
    if(pbr_maps.w!=0)radiance+=max(pbr_emission.SampleGrad(pbr_sampler,uv,tx,ty).rgb,0)*pbr_emission_color.rgb*pbr_overrides.w;
    if(!all(isfinite(radiance)))radiance=0;
    radiance=max(radiance,0)*pbr_irradiance_exposure.w;
    // The original combiner saturates rather than tone maps; the baked mode keeps that.
    if(pbr_scene_info.y==1)radiance=PbrLinearToSrgb(pbr_scene_info.z==1?saturate(radiance):radiance/(1+radiance));
    // The window layer as the original cascade adds it, in display values: saturate(tex1*TFACTOR) times the base's
    // alpha (the window mask), over the encoded surface and before fog.
    if(pbr_scene_info.z==1&&pbr_geometry_options.w==1) {
        float3 glow=saturate(pbr_emission.SampleGrad(pbr_sampler,i.uv1,ddx(i.uv1),ddy(i.uv1)).rgb*pbr_emission_color.rgb);
        radiance=saturate(radiance+glow*saturate(albedo.a));
    }
    // The blend layer as the original program lays it, in display values: t1*v0 over the encoded surface by t1.a*v0.a.
    if(pbr_scene_info.z==1&&pbr_geometry_options.w==2) {
        float4 layer=pbr_emission.SampleGrad(pbr_sampler,i.uv1,ddx(i.uv1),ddy(i.uv1));
        radiance=lerp(radiance,saturate(layer.rgb*i.color.rgb),saturate(layer.a*i.color.a));
    }
    // Legacy order: the fog colour is blended over the final colour with saturate(fogfactor).
    if(pbr_fog.w!=0)radiance=lerp(pbr_fog_color.rgb,radiance,saturate(i.fog));
    // Baked: the vertex alpha carries the cascade's diffuse/TFACTOR alpha factors, as the original writes them.
    return float4(radiance,pbr_scene_info.z==1?albedo.a*saturate(i.color.a):albedo.a);
}
)HLSL";
} // namespace gpu::pbr::pipeline::shader
