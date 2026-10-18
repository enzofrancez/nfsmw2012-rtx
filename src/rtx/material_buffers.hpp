#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
// Capture the deferred buffers when the game leaves its material pass, before
// lighting/postprocessing can reuse them. No CPU image readback or draw replay.
class MaterialBuffers {
 using Texture=Microsoft::WRL::ComPtr<ID3D11Texture2D>;
 Texture depthSource,depthCopy;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>depthView;bool depthReady=false;
 Texture sources[2],copies[2];Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>views[2];
 bool dirty=false,ready=false,currentMaterialPass=false;UINT captures=0;
 UINT sourceWidth=1,sourceHeight=1,frameWidth=1,frameHeight=1;bool multisampled[2]{};std::string description;
 static Texture texture(ID3D11RenderTargetView*view){Texture result;if(view){Microsoft::WRL::ComPtr<ID3D11Resource>resource;view->GetResource(&resource);resource.As(&result);}return result;}
public:
 void reset(){depthSource.Reset();depthCopy.Reset();depthView.Reset();depthReady=false;for(auto&s:sources)s.Reset();for(auto&c:copies)c.Reset();for(auto&v:views)v.Reset();dirty=ready=currentMaterialPass=false;captures=0;}
 bool available()const{return ready;}
 bool hasDepth()const{return ready&&depthReady&&depthView;}
 ID3D11ShaderResourceView*depth()const{return depthView.Get();}
 bool inMaterialPass()const{return currentMaterialPass;}
 UINT count()const{return captures;}
 const std::string&info()const{return description;}
 float scaleX()const{return float(frameWidth)/sourceWidth;}
 float scaleY()const{return float(frameHeight)/sourceHeight;}
 ID3D11ShaderResourceView*view(UINT i)const{return views[i].Get();}
 void endFrame(){ready=currentMaterialPass=depthReady=false;}
 void observe(ID3D11DeviceContext*ctx,bool recognizedVehicle,UINT width,UINT height){
  currentMaterialPass=false;
  if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
  D3D11_VIEWPORT vp{};UINT n=1;ctx->RSGetViewports(&n,&vp);if(n!=1||vp.Width<width*.95f||vp.Width>width*1.05f||vp.Height<height*.95f||vp.Height>height*1.05f)return;
  ID3D11RenderTargetView*raw[4]{};Microsoft::WRL::ComPtr<ID3D11DepthStencilView>dsv;ctx->OMGetRenderTargets(4,raw,&dsv);Microsoft::WRL::ComPtr<ID3D11RenderTargetView>rt[4];for(UINT i=0;i<4;++i)rt[i].Attach(raw[i]);
  if(!rt[0]||!rt[1]||!rt[2]||!rt[3])return;auto normal=texture(rt[0].Get()),specular=texture(rt[2].Get());if(!normal||!specular)return;
  if(sources[0].Get()!=normal.Get()||sources[1].Get()!=specular.Get()){
   if(!recognizedVehicle)return;ready=false;
   Texture next[2]{normal,specular};Microsoft::WRL::ComPtr<ID3D11Device>device;ctx->GetDevice(&device);
   Texture newCopies[2];Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>newViews[2];
   for(UINT i=0;i<2;++i){D3D11_TEXTURE2D_DESC desc{};next[i]->GetDesc(&desc);
    description="RT"+std::to_string(i==0?0:2)+" format="+std::to_string(desc.Format)+" size="+std::to_string(desc.Width)+"x"+std::to_string(desc.Height)+" samples="+std::to_string(desc.SampleDesc.Count)+" mips="+std::to_string(desc.MipLevels);
    bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS||desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if(!rgba||desc.Width<width||desc.Height<height||desc.Width>width*2||desc.Height>height*2||desc.ArraySize!=1){description="Rejected "+description;return;}
    if(i==0){sourceWidth=desc.Width;sourceHeight=desc.Height;}else if(desc.Width!=sourceWidth||desc.Height!=sourceHeight){description="Rejected unequal material buffer dimensions";return;}
    multisampled[i]=desc.SampleDesc.Count>1;desc.SampleDesc.Count=1;desc.SampleDesc.Quality=0;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.MipLevels=1;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.CPUAccessFlags=desc.MiscFlags=0;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&newCopies[i]))||FAILED(device->CreateShaderResourceView(newCopies[i].Get(),nullptr,&newViews[i]))){description="Failed to create material snapshot";return;}
   }
   for(UINT i=0;i<2;++i){sources[i]=next[i];copies[i]=newCopies[i];views[i]=newViews[i];}
   frameWidth=width;frameHeight=height;description="Accepted "+description;
  }
  Texture nextDepth;if(dsv){Microsoft::WRL::ComPtr<ID3D11Resource>resource;dsv->GetResource(&resource);resource.As(&nextDepth);}
  if(nextDepth.Get()!=depthSource.Get()){
   depthSource=nextDepth;depthCopy.Reset();depthView.Reset();depthReady=false;
   if(nextDepth){D3D11_TEXTURE2D_DESC dd{};nextDepth->GetDesc(&dd);DXGI_FORMAT viewFormat=DXGI_FORMAT_UNKNOWN,copyFormat=DXGI_FORMAT_UNKNOWN;
    if(dd.Format==DXGI_FORMAT_R24G8_TYPELESS||dd.Format==DXGI_FORMAT_D24_UNORM_S8_UINT){copyFormat=DXGI_FORMAT_R24G8_TYPELESS;viewFormat=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;}
    if(dd.Format==DXGI_FORMAT_R32_TYPELESS||dd.Format==DXGI_FORMAT_D32_FLOAT){copyFormat=DXGI_FORMAT_R32_TYPELESS;viewFormat=DXGI_FORMAT_R32_FLOAT;}
    if(dd.Format==DXGI_FORMAT_R16_TYPELESS||dd.Format==DXGI_FORMAT_D16_UNORM){copyFormat=DXGI_FORMAT_R16_TYPELESS;viewFormat=DXGI_FORMAT_R16_UNORM;}
    if(dd.SampleDesc.Count==1&&dd.ArraySize==1&&dd.Width==sourceWidth&&dd.Height==sourceHeight&&copyFormat!=DXGI_FORMAT_UNKNOWN){
     dd.Format=copyFormat;dd.MipLevels=1;dd.Usage=D3D11_USAGE_DEFAULT;dd.BindFlags=D3D11_BIND_SHADER_RESOURCE;dd.CPUAccessFlags=dd.MiscFlags=0;
     Microsoft::WRL::ComPtr<ID3D11Device>device;ctx->GetDevice(&device);D3D11_SHADER_RESOURCE_VIEW_DESC vd{};vd.Format=viewFormat;vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;vd.Texture2D.MipLevels=1;
     if(FAILED(device->CreateTexture2D(&dd,nullptr,&depthCopy))||FAILED(device->CreateShaderResourceView(depthCopy.Get(),&vd,&depthView))){depthCopy.Reset();depthView.Reset();}
    }
   }
  }
  dirty=true;currentMaterialPass=true;
 }
 void transition(ID3D11DeviceContext*ctx){
  if(!dirty||!sources[0])return;
  ID3D11RenderTargetView*raw[3]{};ctx->OMGetRenderTargets(3,raw,nullptr);Microsoft::WRL::ComPtr<ID3D11RenderTargetView>rt[3];for(UINT i=0;i<3;++i)rt[i].Attach(raw[i]);
  if(texture(rt[0].Get()).Get()==sources[0].Get()&&texture(rt[2].Get()).Get()==sources[1].Get())return;
  for(UINT i=0;i<2;++i){if(multisampled[i])ctx->ResolveSubresource(copies[i].Get(),0,sources[i].Get(),0,DXGI_FORMAT_R8G8B8A8_UNORM);else ctx->CopySubresourceRegion(copies[i].Get(),0,0,0,0,sources[i].Get(),0,nullptr);}if(depthCopy&&depthSource){ctx->CopySubresourceRegion(depthCopy.Get(),0,0,0,0,depthSource.Get(),0,nullptr);depthReady=true;}dirty=false;ready=true;++captures;
 }
};
