#include "NFSMW_ReShade.fxh"
texture2D CurrentDepth : DEPTH;
sampler2D RawDepthSampler {Texture=CurrentDepth;MagFilter=POINT;MinFilter=POINT;};
texture2D MaterialNormal : RTX_MATERIAL_NORMAL;
texture2D MaterialSpecular : RTX_MATERIAL_SPECULAR;
sampler2D MaterialNormalSampler {Texture=MaterialNormal;MagFilter=POINT;MinFilter=POINT;};
sampler2D MaterialSpecularSampler {Texture=MaterialSpecular;MagFilter=POINT;MinFilter=POINT;};
texture2D RTXLighting : RTX_GPU_LIGHTING;
sampler2D LightingSampler {Texture=RTXLighting;MagFilter=LINEAR;MinFilter=LINEAR;};
uniform float LocalLightStrength <ui_type="slider";ui_min=0.0;ui_max=1.0;ui_label="Luzes locais RTX";> = .35;
texture2D RTXMask : RTX_GPU_MASK;
texture2D RTXReflectionUV : RTX_GPU_REFLECTION;
texture2D RTXIndirectUV : RTX_GPU_INDIRECT;
texture2D RTXWeights : RTX_GPU_WEIGHTS;
texture2D CurrentCamera : RTX_CAMERA_CURRENT;
texture2D CapturedCamera : RTX_CAMERA_CAPTURED;
sampler2D MaskSampler {Texture=RTXMask;MagFilter=POINT;MinFilter=POINT;};
sampler2D ReflectionUVSampler {Texture=RTXReflectionUV;MagFilter=POINT;MinFilter=POINT;};
sampler2D IndirectUVSampler {Texture=RTXIndirectUV;MagFilter=POINT;MinFilter=POINT;};
sampler2D WeightsSampler {Texture=RTXWeights;MagFilter=POINT;MinFilter=POINT;};
sampler2D CurrentCameraSampler {Texture=CurrentCamera;MagFilter=POINT;MinFilter=POINT;};
sampler2D CapturedCameraSampler {Texture=CapturedCamera;MagFilter=POINT;MinFilter=POINT;};
// Every displayed DXR result is from this frame; historical images are unused.
texture2D ReferenceA {Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16F;MipLevels=10;};
sampler2D ReferenceASampler {Texture=ReferenceA;MagFilter=LINEAR;MinFilter=LINEAR;MipFilter=LINEAR;AddressU=CLAMP;AddressV=CLAMP;};
uniform bool RTXReady <hidden=true;> = false;
uniform float2 MaterialUVScale <hidden=true;> = float2(1,1);
uniform bool MaterialReady <hidden=true;> = false;
uniform bool DepthReady <hidden=true;> = false;
uniform bool CameraReady <hidden=true;> = false;
uniform bool CaptureReference <hidden=true;> = false;
uniform bool NewResult <hidden=true;> = false;
uniform float Strength <ui_type="slider";ui_min=0.0;ui_max=1.0;ui_label="Sombras e AO RTX";> = .65;
uniform float ReflectionSpatialStrength <ui_type="slider";ui_min=0.0;ui_max=1.0;ui_label="Estabilidade espacial dos reflexos";> = .45;
uniform float ReflectionStrength <ui_type="slider";ui_min=0.0;ui_max=1.0;ui_label="Reflexos DXR";> = .45;
uniform float IndirectStrength <ui_type="slider";ui_min=0.0;ui_max=1.0;ui_label="Rebote DXR";> = .35;
uniform int EffectsDebug <ui_type="combo";ui_items="Imagem final\0Reflexos\0Rebote\0Validade geometrica\0Resposta do material\0Disponibilidade dos reflexos\0";> = 0;
float4 AcceptReference(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target{return float4(pow(max(0,tex2D(ReShade::BackBuffer,uv).rgb),2.2),1);}
float4 CameraRow(sampler2D source,int row){return tex2Dlod(source,float4((row+.5)/4.0,.5,0,0));}
bool SourceCoordinate(float2 uv,out float2 sourceUV,out float sourceDepth,out bool identicalCamera){
 sourceUV=0;sourceDepth=0;identicalCamera=false;
 float4 x=CameraRow(CurrentCameraSampler,0),y=CameraRow(CurrentCameraSampler,1),z=CameraRow(CurrentCameraSampler,2),terms=CameraRow(CurrentCameraSampler,3);
 // Raw D3D depth: output.z = viewZ*A+B, output.w = viewZ*C+D.
 // Do not apply ReShade's generic reversed-depth/far-plane approximation.
 float depth=tex2Dlod(RawDepthSampler,float4(uv,0,0)).r;
 float divisor=depth*terms.z-terms.x;
 if(abs(divisor)<1e-8)return false;
 float viewZ=(terms.y-depth*terms.w)/divisor;
 if(viewZ<=0||viewZ>1e6)return false;
 float4 sx=CameraRow(CapturedCameraSampler,0),sy=CameraRow(CapturedCameraSampler,1),sz=CameraRow(CapturedCameraSampler,2),st=CameraRow(CapturedCameraSampler,3);
 // Compare the actual GPU snapshots before using a pixel-aligned shortcut.
 identicalCamera=all(x==sx)&&all(y==sy)&&all(z==sz)&&all(terms==st);
 if(identicalCamera){sourceUV=uv;sourceDepth=viewZ;return all(uv>0)&&all(uv<1);}
 float clipW=viewZ*terms.z+terms.w;
 float3 rhs=float3((uv.x*2-1)*clipW-x.w,(1-uv.y*2)*clipW-y.w,viewZ-z.w);
 float3 yz=cross(y.xyz,z.xyz),zx=cross(z.xyz,x.xyz),xy=cross(x.xyz,y.xyz);
 float determinant=dot(x.xyz,yz);if(abs(determinant)<1e-5)return false;
 float3 world=(rhs.x*yz+rhs.y*zx+rhs.z*xy)/determinant;
 float4 worldPosition=float4(world,1);
 sourceDepth=dot(sz,worldPosition);float w=sourceDepth*st.z+st.w;
 if(sourceDepth<=0||w<=0)return false;
 sourceUV=float2(dot(sx,worldPosition)/w*.5+.5,.5-dot(sy,worldPosition)/w*.5);
 return all(sourceUV>0)&&all(sourceUV<1);
}
texture2D SkyPalette {Width=1;Height=1;Format=RGBA16F;};
sampler2D SkyPaletteSampler {Texture=SkyPalette;MagFilter=LINEAR;MinFilter=LINEAR;};
// Conservative fixed HUD exclusion for the current 16:9 game layout.
// Source pixels behind these overlays are unavailable; never mirror the UI.
bool IsHUD(float2 uv){return (uv.x<.25&&uv.y<.14)||(uv.x<.23&&uv.y>.65)||(uv.x>.83&&uv.y>.80);}
void CollectSky(float2 uv,inout float3 radiance,inout float count){
 if(!IsHUD(uv)&&tex2Dlod(RawDepthSampler,float4(uv,0,0)).r>.999995){radiance+=max(0,tex2D(ReferenceASampler,uv).rgb);count+=1;}
}
float4 BuildSky(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target{
 float3 radiance=0;float count=0;
 CollectSky(float2(.3,.035),radiance,count);CollectSky(float2(.45,.035),radiance,count);CollectSky(float2(.6,.035),radiance,count);CollectSky(float2(.75,.035),radiance,count);CollectSky(float2(.9,.035),radiance,count);
 CollectSky(float2(.3,.12),radiance,count);CollectSky(float2(.45,.12),radiance,count);CollectSky(float2(.6,.12),radiance,count);CollectSky(float2(.75,.12),radiance,count);CollectSky(float2(.9,.12),radiance,count);
 CollectSky(float2(.3,.22),radiance,count);CollectSky(float2(.45,.22),radiance,count);CollectSky(float2(.6,.22),radiance,count);CollectSky(float2(.75,.22),radiance,count);CollectSky(float2(.9,.22),radiance,count);
 float3 fallback=pow(float3(.48,.55,.64),2.2);
 return float4(count>0?radiance/max(count,1):fallback,count>0?1:0);
}
float3 DecodeDirection(float2 uv){
 float2 f=uv*2-1;float3 d=float3(f,1-abs(f.x)-abs(f.y));
 if(d.z<0)d.xy=(1-abs(d.yx))*float2(d.x>=0?1:-1,d.y>=0?1:-1);
 return normalize(d);
}
float3 SkyEnvironment(float3 direction){
 float3 sky=tex2D(SkyPaletteSampler,float2(.5,.5)).rgb;
 float horizon=1-saturate(direction.y);float3 result=sky*lerp(.82,1.08,horizon);
 float4 x=CameraRow(CapturedCameraSampler,0),y=CameraRow(CapturedCameraSampler,1),z=CameraRow(CapturedCameraSampler,2);
 float3 view=float3(dot(x.xyz,direction),dot(y.xyz,direction),dot(z.xyz,direction));
 if(view.z>.001){
  float2 uv=float2(view.x/view.z*.5+.5,.5-view.y/view.z*.5);
  if(!IsHUD(uv)&&all(uv>.003)&&all(uv<.997)&&tex2Dlod(RawDepthSampler,float4(uv,0,0)).r>.999995)
   result=max(0,tex2D(ReferenceASampler,uv).rgb);
 }
 return result;
}
bool DecodeHitUV(float4 encodedValue,out float2 uv){float4 bytes=round(encodedValue*255);float2 value=float2(bytes.r+256*bytes.g,bytes.b+256*bytes.a);uv=value/65534;return all(value<65535);}
float MapDepth(float2 uv){float2 b=round(tex2Dlod(MaskSampler,float4(uv,0,0)).ba*255);float v=b.x+256*b.y;return v<65535?exp2(v/65534*24)-1:0;}
float FootprintTap(float2 receiver,float2 hitUV,float2 offset,float depth,float3 normal){
 float2 sampleUV=receiver+offset/float2(1920,1080);float2 sampleHit=0;float result=1;
 float kind=round(tex2Dlod(WeightsSampler,float4(sampleUV,0,0)).b*255);
 float d=MapDepth(sampleUV);float3 n=normalize(tex2Dlod(MaterialNormalSampler,float4(sampleUV*MaterialUVScale,0,0)).rgb*2-1);
 if(kind==1&&d>0&&abs(d-depth)<max(.035,depth*.012)&&dot(normal,n)>.94&&DecodeHitUV(tex2Dlod(ReflectionUVSampler,float4(sampleUV,0,0)),sampleHit)){
  float footprint=length((sampleHit-hitUV)*float2(BUFFER_WIDTH,BUFFER_HEIGHT));
  // Occlusion/geometry boundaries do not describe a texture footprint.
  if(footprint>=1&&footprint<=32)result=footprint;
 }
 return result;
}
float3 FilterRadiance(float2 uv,float bias,float2 receiver,bool mirror){
 float lod=bias;
 if(mirror){float depth=MapDepth(receiver);float3 normal=normalize(tex2Dlod(MaterialNormalSampler,float4(receiver*MaterialUVScale,0,0)).rgb*2-1);
  float x=min(FootprintTap(receiver,uv,float2(1,0),depth,normal),FootprintTap(receiver,uv,float2(-1,0),depth,normal));
  float y=min(FootprintTap(receiver,uv,float2(0,1),depth,normal),FootprintTap(receiver,uv,float2(0,-1),depth,normal));
  lod=max(lod,log2(max(1,max(x,y))));
 }
 return max(0,tex2Dlod(ReferenceASampler,float4(uv,0,min(5,lod))).rgb);
}
void Gather(float2 uv,float weight,float expectedDepth,float radianceBias,bool includeReflection,inout float valid,inout float attenuation,inout float4 reflected,inout float4 indirect){
 float4 mask=tex2D(MaskSampler,uv);float2 encoded=round(mask.ba*255);float encodedValue=encoded.x+encoded.y*256;
 if(encodedValue>=65535)return;
 float actual=exp2(encodedValue/65534*24)-1;
 float tolerance=max(.035,expectedDepth*.012);
 float agreement=1-smoothstep(tolerance,tolerance*2,abs(actual-expectedDepth));
 float w=weight*agreement;valid+=w;attenuation+=mask.r*w;
 float4 weights=tex2D(WeightsSampler,uv);float2 hitUV;
 if(includeReflection){
 float reflectionKind=round(weights.b*255);float3 reflectionColor=0;float reflectionWeight=weights.r*w;
 if(DecodeHitUV(tex2D(ReflectionUVSampler,uv),hitUV)){
  if(reflectionKind==1){
   if(IsHUD(hitUV))reflectionWeight=0;
   reflectionColor=FilterRadiance(hitUV,radianceBias,uv,true);
   float edge=min(min(hitUV.x,hitUV.y),min(1-hitUV.x,1-hitUV.y));float fade=smoothstep(.003,.02,edge);
   reflectionColor=lerp(tex2D(SkyPaletteSampler,float2(.5,.5)).rgb*.2,reflectionColor,fade);
  }else if(reflectionKind==2)reflectionColor=SkyEnvironment(DecodeDirection(hitUV));
  else if(reflectionKind==3){reflectionColor=0;reflectionWeight=0;}
  else reflectionWeight=0;
  reflected+=float4(reflectionColor*reflectionWeight,reflectionWeight);
 }
 }
 if(DecodeHitUV(tex2D(IndirectUVSampler,uv),hitUV)&&!IsHUD(hitUV)){float strength=weights.g*w;indirect+=float4(FilterRadiance(hitUV,max(1,radianceBias),uv,false)*strength,strength);}
}
void SmoothReflectionTap(float2 source,float2 offset,float depth,float bias,float3 normal,float kind,inout float4 result,inout float total){
 float2 neighbor=source+offset/float2(1920,1080);if(IsHUD(neighbor)||any(neighbor<=0)||any(neighbor>=1))return;
 float neighborKind=round(tex2Dlod(WeightsSampler,float4(neighbor,0,0)).b*255);
 if(neighborKind!=kind)return;
 float3 other=normalize(tex2Dlod(MaterialNormalSampler,float4(neighbor*MaterialUVScale,0,0)).rgb*2-1);
 float d=MapDepth(neighbor),tolerance=max(.035,depth*.012);
 float w=pow(saturate(dot(normal,other)),32)*(1-smoothstep(tolerance,tolerance*2,abs(d-depth)));
 if(d<=0||w<.01)return;
 float valid=0,attenuation=0;float4 reflected=0,indirect=0;
 Gather(neighbor,1,depth,bias,true,valid,attenuation,reflected,indirect);
 if(reflected.a>0){result+=reflected*w;total+=w;}
}
float4 SmoothReflection(float2 source,float depth,float bias,float4 center){
 float4 result=center;
 float kind=round(tex2Dlod(WeightsSampler,float4(source,0,0)).b*255);
 if(ReflectionSpatialStrength>0&&MaterialReady&&center.a>.001&&(kind==1||kind==2)){
  float3 normal=normalize(tex2Dlod(MaterialNormalSampler,float4(source*MaterialUVScale,0,0)).rgb*2-1);
  float4 sum=center*2;float total=2;
  SmoothReflectionTap(source,float2(1,0),depth,bias,normal,kind,sum,total);
  SmoothReflectionTap(source,float2(-1,0),depth,bias,normal,kind,sum,total);
  SmoothReflectionTap(source,float2(0,1),depth,bias,normal,kind,sum,total);
  SmoothReflectionTap(source,float2(0,-1),depth,bias,normal,kind,sum,total);
  result=lerp(center,sum/total,ReflectionSpatialStrength);
 }
 return result;
}
float MaterialResponse(float2 uv){
 float response=0;
 if(MaterialReady){float roughness=tex2Dlod(MaterialNormalSampler,float4(uv*MaterialUVScale,0,0)).a;float3 specular=tex2Dlod(MaterialSpecularSampler,float4(uv*MaterialUVScale,0,0)).rgb;
  float f0=max(specular.r,max(specular.g,specular.b));float gloss=1-smoothstep(.35,.90,roughness);
  response=saturate(f0/.04)*gloss*gloss;
 }
 return response;
}
float4 Composite(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target{
 float4 color=tex2D(ReShade::BackBuffer,uv);if(!RTXReady||!CameraReady||!DepthReady||IsHUD(uv))return color;
 float2 source;float depth;bool identicalCamera;if(!SourceCoordinate(uv,source,depth,identicalCamera))return EffectsDebug==3?float4(1,0,0,1):color;
 if(EffectsDebug==5){float kind=round(tex2Dlod(WeightsSampler,float4(source,0,0)).b*255);float actual=MapDepth(source);float tolerance=max(.035,depth*.012);float3 status=float3(.8,.05,.05);if(actual>0&&abs(actual-depth)<2*tolerance){if(kind==1)status=float3(.05,.8,.1);else if(kind==2)status=float3(.05,.25,.85);else if(kind==3)status=float3(.8,.05,.8);}return float4(status,1);}
 float2 size=float2(1920,1080),pixel=source*size-.5,corner=floor(pixel),fraction=frac(pixel);
 float roughness=MaterialReady?tex2Dlod(MaterialNormalSampler,float4(uv*MaterialUVScale,0,0)).a:0;float radianceBias=min(2.5,roughness*roughness*3);
 float response=MaterialResponse(uv);bool includeReflection=response>0||EffectsDebug==1;
 float valid=0,attenuation=0;float4 reflected=0,indirect=0;
 // DXR maps and output share a pixel grid at native 1080p with equal cameras.
 // Keep the four-tap path for a changed camera or a different output size.
 if(identicalCamera&&BUFFER_WIDTH==1920&&BUFFER_HEIGHT==1080){
  source=p.xy/size;
  Gather(source,1,depth,radianceBias,includeReflection,valid,attenuation,reflected,indirect);
 }else{
 Gather((corner+.5)/size,(1-fraction.x)*(1-fraction.y),depth,radianceBias,includeReflection,valid,attenuation,reflected,indirect);
 Gather((corner+float2(1.5,.5))/size,fraction.x*(1-fraction.y),depth,radianceBias,includeReflection,valid,attenuation,reflected,indirect);
 Gather((corner+float2(.5,1.5))/size,(1-fraction.x)*fraction.y,depth,radianceBias,includeReflection,valid,attenuation,reflected,indirect);
 Gather((corner+1.5)/size,fraction.x*fraction.y,depth,radianceBias,includeReflection,valid,attenuation,reflected,indirect);
 }
 if(includeReflection)reflected=SmoothReflection(source,depth,radianceBias,reflected);
 if(EffectsDebug==3)return float4(0,valid,0,1);
 if(EffectsDebug==1)return float4(pow(max(0,reflected.rgb),1/2.2),1);
 if(EffectsDebug==2)return float4(pow(max(0,indirect.rgb),1/2.2),1);
 if(EffectsDebug==4)return MaterialReady?float4(response.xxx,1):float4(1,0,0,1);
 reflected*=response;
 float3 base=pow(max(0,color.rgb),2.2);
 float4 local=tex2D(LightingSampler,source);float3 localRadiance=local.rgb/max(.0039,1-local.rgb);
 base*=1-Strength*(valid-attenuation);
 base*=1-LocalLightStrength*local.a*valid;
 base+=base*min(localRadiance,4)*LocalLightStrength*.15*valid;
 float reflectionGain=min(1,ReflectionStrength*1.35);
 base=base*(1-reflectionGain*reflected.a)+reflected.rgb*reflectionGain;
 base+=indirect.rgb*base*IndirectStrength;
 return float4(pow(max(0,base),1/2.2),color.a);
}
technique NFSMW_RTX_Live <ui_label="NFSMW RTX v19: geometria valida sem refit";>{
 pass {VertexShader=PostProcessVS;PixelShader=AcceptReference;RenderTarget=ReferenceA;GenerateMipMaps=true;}
 pass {VertexShader=PostProcessVS;PixelShader=BuildSky;RenderTarget=SkyPalette;}
 pass {VertexShader=PostProcessVS;PixelShader=Composite;}
}



