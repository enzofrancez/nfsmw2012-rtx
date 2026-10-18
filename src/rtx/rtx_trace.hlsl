#include "capture_limits.h"
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float3> positions : register(t1);
RWStructuredBuffer<uint> output : register(u0);
cbuffer Config : register(b0) {float px,py; uint width,height,vertexCount,clipWidth,compactGeometry;}
Texture2D<float4> clipPositions : register(t2);
Texture2D<float4> gameNormals : register(t3);
Texture2D<float> rasterDepth : register(t4);
RWStructuredBuffer<float3> convertedPositions : register(u1);
RWStructuredBuffer<uint> validCounter : register(u2);
float4 ReadClip(uint index) {return clipPositions.Load(int3(index%clipWidth,index/clipWidth,0));}
float2 ProjectionScale(){float4 x=ReadClip((RTX_CAPTURE_MAX_VERTICES+0)),y=ReadClip((RTX_CAPTURE_MAX_VERTICES+1));float2 scale=float2(length(x.xyz),length(y.xyz));return all(scale>.1)&&all(scale<20)?scale:float2(px,py);}
[numthreads(64,1,1)] void ConvertGeometry(uint3 id:SV_DispatchThreadID) {
 uint base=id.x*3;
 if(vertexCount==0){if(base==0){convertedPositions[0]=0;convertedPositions[1]=0;convertedPositions[2]=0;}return;}
 if(base+2>=vertexCount)return;
 float4 a=ReadClip(base),b=ReadClip(base+1),c=ReadClip(base+2);
 // Signed view positions retain geometry behind the camera for secondary rays.
 float2 scale=ProjectionScale();
 float3 x=float3(a.x/scale.x,a.y/scale.y,a.w),y=float3(b.x/scale.x,b.y/scale.y,b.w),z=float3(c.x/scale.x,c.y/scale.y,c.w);
 float3 n=cross(y-x,z-x);
 bool valid=all(isfinite(a))&&all(isfinite(b))&&all(isfinite(c))
   &&all(abs(x)<1e6)&&all(abs(y)<1e6)&&all(abs(z)<1e6)&&dot(n,n)>1e-14;
 uint waveCount=WaveActiveCountBits(valid),prefix=WavePrefixCountBits(valid),offset=0;
 if(WaveIsFirstLane())InterlockedAdd(validCounter[0],waveCount*3,offset);
 offset=WaveReadLaneFirst(offset);
 if(compactGeometry!=0){if(valid){uint outIndex=offset+prefix*3;convertedPositions[outIndex]=x;convertedPositions[outIndex+1]=y;convertedPositions[outIndex+2]=z;}}
 else{convertedPositions[base]=valid?x:0;convertedPositions[base+1]=valid?y:0;convertedPositions[base+2]=valid?z:0;}
}
bool blocked(float3 origin,float3 direction,float nearT,float farT) {
 RayDesc r; r.Origin=origin;r.Direction=direction;r.TMin=nearT;r.TMax=farT;
 RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_FORCE_OPAQUE> q;
 q.TraceRayInline(scene,0,255,r);while(q.Proceed()){}
 return q.CommittedStatus()==COMMITTED_TRIANGLE_HIT;
}
bool nearest(float3 origin,float3 direction,float nearT,float farT,out float3 hitPosition,out float distance) {
 RayDesc r;r.Origin=origin;r.Direction=direction;r.TMin=nearT;r.TMax=farT;
 RayQuery<RAY_FLAG_FORCE_OPAQUE> q;q.TraceRayInline(scene,0,255,r);while(q.Proceed()){}
 distance=q.CommittedStatus()==COMMITTED_TRIANGLE_HIT?q.CommittedRayT():0;
 hitPosition=origin+direction*distance;return distance>0;
}
uint visibleColorUV(float3 hitPosition) {
 // Radiance is only available for directly visible points in the captured image.
 // A geometric hit behind another surface must not borrow the occluder's color.
 if(hitPosition.z<=.001)return 0xffffffff;
 float2 uv=float2(hitPosition.x*ProjectionScale().x/hitPosition.z*.5+.5,.5-hitPosition.y*ProjectionScale().y/hitPosition.z*.5);
 if(any(uv<=0)||any(uv>=1))return 0xffffffff;
 if(ReadClip(RTX_CAPTURE_MAX_VERTICES+5).x>.5){uint2 pixel=min(uint2(uv*float2(width,height)),uint2(width-1,height-1));float z=rasterDepth.Load(int3(pixel,0));if(z<=0||abs(z-hitPosition.z)>max(.01,hitPosition.z*.002))return 0xffffffff;uint2 packed=(uint2)round(uv*65534);return packed.x|(packed.y<<16);}
 float lengthToPoint=length(hitPosition),t;float3 visible;
 if(!nearest(0,hitPosition/lengthToPoint,.0001,lengthToPoint+max(.01,lengthToPoint*.002),visible,t))return 0xffffffff;
 if(abs(t-lengthToPoint)>max(.01,lengthToPoint*.002))return 0xffffffff;
 uint2 packed=(uint2)round(uv*65534);return packed.x|(packed.y<<16);
}
uint EncodeDirection(float3 direction){
 float3 d=direction/(abs(direction.x)+abs(direction.y)+abs(direction.z));float2 oct=d.xy;
 if(d.z<0)oct=(1-abs(oct.yx))*float2(oct.x>=0?1:-1,oct.y>=0?1:-1);
 uint2 encoded=(uint2)round(saturate(oct*.5+.5)*65534);return encoded.x|(encoded.y<<16);
}
float3 ToViewDirection(float3 world){return float3(dot(normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+0)).xyz),world),dot(normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+1)).xyz),world),dot(normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+2)).xyz),world));}
float3 ToViewPosition(float3 world){return float3(dot(ReadClip((RTX_CAPTURE_MAX_VERTICES+0)),float4(world,1))/length(ReadClip((RTX_CAPTURE_MAX_VERTICES+0)).xyz),dot(ReadClip((RTX_CAPTURE_MAX_VERTICES+1)),float4(world,1))/length(ReadClip((RTX_CAPTURE_MAX_VERTICES+1)).xyz),dot(ReadClip((RTX_CAPTURE_MAX_VERTICES+2)),float4(world,1)));}
float luminance(float3 c){return dot(c,float3(.2126,.7152,.0722));}
float4 ReadLight(uint row){return ReadClip((RTX_CAPTURE_MAX_VERTICES+16)+row);}
float3 RasterPosition(int2 pixel,float z){float2 scale=ProjectionScale();return float3((2*(pixel.x+.5)/width-1)/scale.x*z,(1-2*(pixel.y+.5)/height)/scale.y*z,z);}
bool PrimarySurface(uint2 pixel,float3 ray,out float distance,out float3 normal){
 distance=0;normal=0;
 if(ReadClip(RTX_CAPTURE_MAX_VERTICES+5).x>.5&&ReadClip(RTX_CAPTURE_MAX_VERTICES+4).x>.5){
  float z=rasterDepth.Load(int3(pixel,0));if(z<=0)return false;
  int2 p=int2(pixel);int2 l=max(p-int2(1,0),0),r=min(p+int2(1,0),int2(width-1,height-1)),u=max(p-int2(0,1),0),d=min(p+int2(0,1),int2(width-1,height-1));
  float zl=rasterDepth.Load(int3(l,0)),zr=rasterDepth.Load(int3(r,0)),zu=rasterDepth.Load(int3(u,0)),zd=rasterDepth.Load(int3(d,0));
  float le=zl>0?abs(zl-z):1e8,re=zr>0?abs(zr-z):1e8,ue=zu>0?abs(zu-z):1e8,de=zd>0?abs(zd-z):1e8;
  int2 xp=le<re?l:r,yp=ue<de?u:d;float zx=le<re?zl:zr,zy=ue<de?zu:zd;
  if(min(le,re)<max(.02,z*.01)&&min(ue,de)<max(.02,z*.01)&&z<150){
   float3 center=RasterPosition(p,z),dx=RasterPosition(xp,zx)-center,dy=RasterPosition(yp,zy)-center;float3 n=cross(dx,dy);
   if(dot(n,n)>1e-15){normal=normalize(n);if(dot(normal,ray)>0)normal=-normal;distance=z/ray.z;return true;}
  }
 }
 RayDesc r;r.Origin=0;r.Direction=ray;r.TMin=.0001;r.TMax=1e8;RayQuery<RAY_FLAG_FORCE_OPAQUE>q;q.TraceRayInline(scene,0,255,r);while(q.Proceed()){}
 if(q.CommittedStatus()!=COMMITTED_TRIANGLE_HIT)return false;
 uint i=q.CommittedPrimitiveIndex()*3;normal=normalize(cross(positions[i+1]-positions[i],positions[i+2]-positions[i]));if(dot(normal,ray)>0)normal=-normal;distance=q.CommittedRayT();return true;
}
 groupshared uint tileReflective;
 [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint index:SV_GroupIndex) {
  if(index==0)tileReflective=0;
  GroupMemoryBarrierWithGroupSync();
  bool materialKnown=ReadClip(RTX_CAPTURE_MAX_VERTICES+4).x>.5;
  bool reflective=!materialKnown;
  if(materialKnown){
   for(uint sample=index;sample<144;sample+=64){
    int2 p=clamp(int2(group.xy*8)+int2(sample%12,sample/12)-2,int2(0,0),int2(width-1,height-1));
    if(gameNormals.Load(int3(p,0)).a<.90)reflective=true;
   }
  }
  if(reflective)InterlockedOr(tileReflective,1);
  GroupMemoryBarrierWithGroupSync();
 if(id.x>=width||id.y>=height)return;
 float2 scale=ProjectionScale();
 float3 ray=normalize(float3((2*(id.x+.5)/width-1)/scale.x,(1-2*(id.y+.5)/height)/scale.y,1));
 float primaryDistance=0;float3 primaryNormal=0;bool primaryHit=PrimarySurface(id.xy,ray,primaryDistance,primaryNormal);
 float factor=1,gray=.025,hit=0,reflectionWeight=0,indirectWeight=0,viewDepth=0;
 float3 localVisible=0;float localBlocked=0,localTotal=0;
 uint reflectionUV=0xffffffff,indirectUV=0xffffffff,reflectionKind=0;
 if(primaryHit){
  hit=1;float3 n=primaryNormal;
  float3 geometricNormal=n;
  if(ReadClip((RTX_CAPTURE_MAX_VERTICES+4)).x>.5){float3 worldNormal=gameNormals.Load(int3(id.xy,0)).xyz*2-1;
   if(dot(worldNormal,worldNormal)>.1){worldNormal=normalize(worldNormal);float3 smooth=normalize(float3(dot(normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+0)).xyz),worldNormal),dot(normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+1)).xyz),worldNormal),dot(normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+2)).xyz),worldNormal)));if(dot(smooth,ray)>0)smooth=-smooth;float confidence=smoothstep(.1,.35,dot(smooth,geometricNormal));n=normalize(lerp(geometricNormal,smooth,confidence));}
  }
  viewDepth=primaryDistance*ray.z;
  float t=primaryDistance,eps=max(.0001,t*1e-5);float3 origin=ray*t+geometricNormal*eps;
  float4 lightHeader=ReadLight(0);float3 sun=lightHeader.x>.5?-ReadLight(2).xyz:float3(0,1,0);float3 right=normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+0)).xyz),up=normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+1)).xyz),forward=normalize(ReadClip((RTX_CAPTURE_MAX_VERTICES+2)).xyz);float3 light=normalize(ToViewDirection(sun));
  float sunStrength=lightHeader.x>.5?saturate(luminance(max(0,ReadLight(1).rgb))/.15):0;
  bool shadow=sunStrength>0&&dot(n,light)>0&&blocked(origin,light,eps,10000);
  float3 surface=ray*t;
  if(lightHeader.y>.5){float3 position=ToViewPosition(ReadLight(3).xyz),delta=position-surface;float distance=length(delta);float3 toLight=delta/max(distance,.001);float4 cone=ReadLight(5);float coneWeight=0;
   // Cone coefficients are world-space, and the ray direction points from surface to lamp.
   float3 worldToLight=right*toLight.x+up*toLight.y+forward*toLight.z;coneWeight=exp2(clamp(dot(cone.xyz,worldToLight)+cone.w,-80,0));
   float3 irradiance=max(0,ReadLight(4).rgb)*coneWeight*pow(max(distance,.01),-1.25)*saturate(dot(n,toLight));float energy=luminance(irradiance);
   if(energy>.0001&&distance>eps*2){bool occluded=blocked(origin,toLight,eps,max(eps,distance-eps));localTotal+=energy;if(occluded)localBlocked+=energy;else localVisible+=irradiance;}
  }
  // Deferred point positions are relative to the active camera in world axes.
  [loop]for(uint li=0;li<min(16,(uint)lightHeader.z);++li){float4 colour=ReadLight(16+li*2);float3 position=ToViewDirection(ReadLight(17+li*2).xyz);float3 delta=position-surface;float distance=length(delta);float3 toLight=delta/max(distance,.001);
   float attenuation=saturate(1/(distance*distance+.001)-colour.w);float3 irradiance=max(0,colour.rgb)*attenuation*saturate(dot(n,toLight));float energy=luminance(irradiance);
   if(energy>.0001&&distance>eps*2){bool occluded=blocked(origin,toLight,eps,max(eps,distance-eps));localTotal+=energy;if(occluded)localBlocked+=energy;else localVisible+=irradiance;}
  }
  float3 tangent=normalize(cross(abs(n.y)<.9?float3(0,1,0):float3(1,0,0),n));
  float3 bitangent=cross(n,tangent);
  // The first AO direction and diffuse bounce share one nearest-hit query.
  float3 bounce=normalize(n+tangent*.65+bitangent*.35),bounceHitPosition;float bounceDistance;
  bool bounceHit=nearest(origin,bounce,eps,20,bounceHitPosition,bounceDistance);
  float ao=1-.5*((bounceHit&&bounceDistance<=5?1:0)+blocked(origin,normalize(n-tangent*.65-bitangent*.35),eps,5));
  factor=.7+.3*(.5*ao+.5*(shadow?1-.8*sunStrength:1));gray=.62*(.2*ao+.8*max(0,dot(n,light))*(shadow?.08:1));
  float3 hitPosition;float secondaryDistance;
  if(tileReflective!=0){
  float3 mirror=reflect(ray,n);float hemisphere=dot(mirror,geometricNormal);if(hemisphere<.001)mirror=normalize(mirror+geometricNormal*(.001-hemisphere));
  float3 mirrorWorld=normalize(right*mirror.x+up*mirror.y+forward*mirror.z);
  float fresnel=.04+.96*pow(1-saturate(dot(n,-ray)),5);reflectionWeight=fresnel;
  if(nearest(origin,mirror,eps,2000,hitPosition,secondaryDistance)){
   reflectionUV=visibleColorUV(hitPosition);
   if(reflectionUV!=0xffffffff)reflectionKind=1;
   else{reflectionKind=3;reflectionUV=EncodeDirection(mirrorWorld);}
  }else{reflectionKind=2;reflectionUV=EncodeDirection(mirrorWorld);}
  }
  // One deterministic diffuse bounce, not a path-traced full GI solution.
  if(bounceHit){
   indirectUV=visibleColorUV(bounceHitPosition);
   indirectWeight=indirectUV!=0xffffffff?saturate(dot(n,bounce))/(1+bounceDistance*.05):0;
  }
 }
 float3 mappedLocal=localVisible/(1+localVisible);uint3 lightBytes=(uint3)round(saturate(mappedLocal)*255);uint shadowByte=(uint)round(saturate(localBlocked/(.2+localTotal))*255);
 output[4*width*height+id.y*width+id.x]=lightBytes.x|(lightBytes.y<<8)|(lightBytes.z<<16)|(shadowByte<<24);
 uint a=(uint)round(saturate(factor)*255),b=(uint)round(pow(saturate(gray),1/2.2)*255);
 uint encodedDepth=hit?(uint)round(saturate(log2(1+viewDepth)/24)*65534):65535;
 output[id.y*width+id.x]=a|(b<<8)|(encodedDepth<<16);
 uint pixel=id.y*width+id.x,pixels=width*height;
 output[pixels+pixel]=reflectionUV;output[2*pixels+pixel]=indirectUV;
 output[3*pixels+pixel]=(uint)round(saturate(reflectionWeight)*255)
   |((uint)round(saturate(indirectWeight)*255)<<8)|(reflectionKind<<16)|0xff000000u;
}



