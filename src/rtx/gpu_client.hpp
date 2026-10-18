#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "gpu_protocol.hpp"
namespace rtxgpu {
using Microsoft::WRL::ComPtr;
inline void check(HRESULT h,const char*s){if(FAILED(h)){char b[200];sprintf_s(b,"%s HRESULT=%08X",s,(unsigned)h);throw std::runtime_error(b);}}
class Client {
 ComPtr<ID3D11Device> device;ComPtr<ID3D11Device1>d1;ComPtr<ID3D11Device5>d5;
 ComPtr<ID3D11DeviceContext> ctx;ComPtr<ID3D11DeviceContext1>c1;ComPtr<ID3D11DeviceContext4>c4;
 ComPtr<ID3D12Device> opener;
 ComPtr<ID3D11Buffer> clearStream;ComPtr<ID3D11Buffer> stream;ComPtr<ID3D11ShaderResourceView>streamSRV;
 UINT lastGeometryBound=MaxVertices,stagedVertices=MaxVertices;ComPtr<ID3D11Texture2D> metadataZero;
 ComPtr<ID3D11Texture2D> depthInput;ComPtr<ID3D11RenderTargetView>depthRTV;ComPtr<ID3D11ShaderResourceView>depthSource;ComPtr<ID3D11PixelShader>depthPS;bool depthUsed=false;
 ComPtr<ID3D11Texture2D> normalInput,normalSource,normalFlag;bool normalsUsed=false;
 ComPtr<ID3D11Texture2D> lighting;ComPtr<ID3D11RenderTargetView>lightingRTV;bool sunValid=false,headValid=false;UINT pointCount=0;
 ComPtr<ID3D11Texture2D> clip;ComPtr<ID3D11RenderTargetView>clipRTV;
 ComPtr<ID3DDeviceContextState> copyState;
 ComPtr<ID3D11VertexShader> copyVS;ComPtr<ID3D11PixelShader>copyPS,cameraPS;
 ComPtr<ID3D11Texture2D> cameraLive,cameraPending,cameraActive;ComPtr<ID3D11RenderTargetView>cameraRTV;
 ComPtr<ID3D11ShaderResourceView>cameraLiveSRV,cameraActiveSRV;ComPtr<ID3D11Buffer>cameraSelection;
 ComPtr<ID3D12Fence> recoveryFence;
 std::atomic_bool stopping{false};std::atomic<LONG> awaited{0};std::atomic<ULONGLONG> queuedAt{0};std::thread watchdog;std::mutex watchMutex;std::condition_variable watchWake;
 bool cameraValid=false,pendingCameraValid=false,activeCameraValid=false;
 ComPtr<ID3D11Fence> inputFence,readFence,outputFence;
 ComPtr<ID3D11Texture2D> outputs[2][Planes];ComPtr<ID3D11ShaderResourceView>views[2][Planes];
 std::vector<HANDLE> handles;HANDLE mapping=nullptr,process=nullptr,wakeEvent=nullptr,readyEvent=nullptr;
 UINT pid=0;LONG inputSerial=0,readSerial=0;bool opened=false,staged=false;
 void fence(const wchar_t*suffix,ComPtr<ID3D11Fence>&f){check(d5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&f)),"CreateFence");HANDLE h;check(f->CreateSharedHandle(nullptr,GENERIC_ALL,name(pid,suffix).c_str(),&h),"ShareFence");handles.push_back(h);}
public:
 Shared*shared=nullptr;int active=-1;
 ~Client(){stopping=true;watchWake.notify_all();if(watchdog.joinable())watchdog.join();if(recoveryFence&&awaited>0&&outputFence->GetCompletedValue()<UINT64(awaited.load()))recoveryFence->Signal(UINT64(awaited.load()));if(shared){InterlockedExchange(&shared->state,-2);if(wakeEvent)SetEvent(wakeEvent);UnmapViewOfFile(shared);}if(mapping)CloseHandle(mapping);if(process)CloseHandle(process);if(wakeEvent)CloseHandle(wakeEvent);if(readyEvent)CloseHandle(readyEvent);for(auto h:handles)CloseHandle(h);}
 HANDLE readySignal()const{return readyEvent;}
 ID3D11Buffer*streamBuffer()const{return stream.Get();}
 ID3D11Texture2D*texture(UINT plane)const{return active>=0?outputs[active][plane].Get():nullptr;}
 ID3D11ShaderResourceView*view(UINT plane)const{return active>=0?views[active][plane].Get():nullptr;}
 ID3D11ShaderResourceView*liveCameraView()const{return cameraLiveSRV.Get();}
 ID3D11ShaderResourceView*activeCameraView()const{return cameraActiveSRV.Get();}
void setNormals(ID3D11ShaderResourceView*srv){normalSource.Reset();if(srv){ComPtr<ID3D11Resource>r;srv->GetResource(&r);r.As(&normalSource);}}
 void setDepth(ID3D11ShaderResourceView*srv){depthSource=srv;}
 bool usedDepth()const{return depthUsed;}
 bool usedNormals()const{return normalsUsed;}
 bool hasCamera()const{return cameraValid;}
 bool hasActiveCamera()const{return activeCameraValid;}
 bool canQueue()const{return shared&&inputSerial-InterlockedCompareExchange(&shared->readySerial,0,0)<LONG(JobSlots-1);}
 LONG state()const{return shared?InterlockedCompareExchange(&shared->state,0,0):-1;}
 void init(ID3D11Device*native,ID3D11DeviceContext*context,const std::filesystem::path&helper){
  device=native;ctx=context;pid=GetCurrentProcessId();check(device.As(&d1),"D3D11.1");check(device.As(&d5),"D3D11.5");check(ctx.As(&c1),"Context1");check(ctx.As(&c4),"Context4");
  ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;check(device.As(&dx),"DXGI device");check(dx->GetAdapter(&adapter),"adapter");DXGI_ADAPTER_DESC ad{};adapter->GetDesc(&ad);
  // x86 uses D3D12 only to open named shared handles. DXR stays in the x64 helper.
  check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&opener)),"handle opener");
  D3D11_BUFFER_DESC bd{};bd.ByteWidth=MaxVertices*16;bd.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_SHADER_RESOURCE;check(device->CreateBuffer(&bd,nullptr,&stream),"SO buffer");
  std::vector<unsigned char> zeros(MaxVertices*16);D3D11_BUFFER_DESC clearDesc=bd;clearDesc.BindFlags=0;D3D11_SUBRESOURCE_DATA clearData{zeros.data(),0,0};check(device->CreateBuffer(&clearDesc,&clearData,&clearStream),"Zero geometry buffer");
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sv.Buffer.NumElements=MaxVertices;check(device->CreateShaderResourceView(stream.Get(),&sv,&streamSRV),"SO SRV");
  D3D11_TEXTURE2D_DESC td{};td.Width=ClipWidth;td.Height=ClipHeight;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  check(device->CreateTexture2D(&td,nullptr,&clip),"Shared clip texture");check(device->CreateRenderTargetView(clip.Get(),nullptr,&clipRTV),"Clip RTV");ComPtr<IDXGIResource1> resource;check(clip.As(&resource),"Clip DXGI");HANDLE h;check(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,name(pid,L"clip").c_str(),&h),"Share clip");handles.push_back(h);
  {D3D11_TEXTURE2D_DESC nd=td;nd.Width=Width;nd.Height=Height;nd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
   check(device->CreateTexture2D(&nd,nullptr,&normalInput),"Shared game normals");ComPtr<IDXGIResource1> nr;check(normalInput.As(&nr),"Normals DXGI");HANDLE nh;check(nr->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,name(pid,L"normals").c_str(),&nh),"Share normals");handles.push_back(nh);
   nd.Width=nd.Height=1;nd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;nd.MiscFlags=0;nd.BindFlags=0;float flag[4]{1,0,0,0};D3D11_SUBRESOURCE_DATA fd{flag,16,16};check(device->CreateTexture2D(&nd,&fd,&normalFlag),"Normal availability flag");}
  {D3D11_TEXTURE2D_DESC dd=td;dd.Width=Width;dd.Height=Height;dd.Format=DXGI_FORMAT_R32_FLOAT;check(device->CreateTexture2D(&dd,nullptr,&depthInput),"Shared raster depth");check(device->CreateRenderTargetView(depthInput.Get(),nullptr,&depthRTV),"Depth RTV");ComPtr<IDXGIResource1>dr;check(depthInput.As(&dr),"Depth DXGI");HANDLE dh;check(dr->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,name(pid,L"depth").c_str(),&dh),"Share depth");handles.push_back(dh);}
  {D3D11_TEXTURE2D_DESC md=td;md.Width=80;md.Height=1;md.MiscFlags=md.BindFlags=0;float metadataZeros[80*4]{};D3D11_SUBRESOURCE_DATA init{metadataZeros,sizeof(metadataZeros),0};check(device->CreateTexture2D(&md,&init,&metadataZero),"Metadata zeros");}
  fence(L"inputFence",inputFence);fence(L"readFence",readFence);
  auto level=device->GetFeatureLevel();check(d1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&copyState),"Copy state");
  const char*vs="float4 main(uint i:SV_VertexID):SV_Position{float2 uv=float2((i<<1)&2,i&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}";
  const char*ps="Buffer<float4> source:register(t0);float4 main(float4 p:SV_Position):SV_Target{uint i=(uint)p.y*1024+(uint)p.x;return i<" RTX_STRINGIFY(RTX_CAPTURE_MAX_VERTICES) "?source[i]:0;}";
  ComPtr<ID3DBlob>b,e;check(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e),"Copy VS compile");check(device->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&copyVS),"Copy VS");b.Reset();e.Reset();check(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e),"Copy PS compile");check(device->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&copyPS),"Copy PS");
  const char*cameraSource="cbuffer Game:register(b0){float4 rows[4096];};cbuffer Select:register(b1){uint row;};float4 main(float4 p:SV_Position):SV_Target{return rows[row+(uint)p.x];}";
  b.Reset();e.Reset();check(D3DCompile(cameraSource,strlen(cameraSource),nullptr,nullptr,nullptr,"main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e),"Camera PS compile");check(device->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&cameraPS),"Camera PS");
  const char*depthCode="Texture2D<float> source:register(t0);Texture2D<float4> camera:register(t1);float main(float4 p:SV_Position):SV_Target{float d=source.Load(int3((uint2)p.xy,0));float4 terms=camera.Load(int3(3,0,0));float div=d*terms.z-terms.x;float z=abs(div)>1e-8?(terms.y-d*terms.w)/div:0;return d<.9999999&&z>0&&z<1e6?z:0;}";
  b.Reset();e.Reset();check(D3DCompile(depthCode,strlen(depthCode),nullptr,nullptr,nullptr,"main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e),"Raster depth PS compile");check(device->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&depthPS),"Raster depth PS");
  td.Width=4;td.Height=1;td.MiscFlags=0;check(device->CreateTexture2D(&td,nullptr,&cameraLive),"Live camera");check(device->CreateTexture2D(&td,nullptr,&cameraPending),"Pending camera");check(device->CreateTexture2D(&td,nullptr,&cameraActive),"Active camera");
  check(device->CreateRenderTargetView(cameraLive.Get(),nullptr,&cameraRTV),"Camera RTV");check(device->CreateShaderResourceView(cameraLive.Get(),nullptr,&cameraLiveSRV),"Live camera SRV");check(device->CreateShaderResourceView(cameraActive.Get(),nullptr,&cameraActiveSRV),"Active camera SRV");
  td.Width=64;check(device->CreateTexture2D(&td,nullptr,&lighting),"Lighting metadata");check(device->CreateRenderTargetView(lighting.Get(),nullptr,&lightingRTV),"Lighting RTV");
  bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(device->CreateBuffer(&bd,nullptr,&cameraSelection),"Camera selector");
  mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name(pid,L"ipc").c_str());if(!mapping)throw std::runtime_error("Create GPU mapping failed");shared=(Shared*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared));if(!shared)throw std::runtime_error("Map GPU IPC failed");memset(shared,0,sizeof(*shared));shared->magic=Magic;shared->version=10;shared->parentPID=pid;shared->adapterLow=ad.AdapterLuid.LowPart;shared->adapterHigh=ad.AdapterLuid.HighPart;shared->activeSlot=-1;
  wakeEvent=CreateEventW(nullptr,FALSE,FALSE,name(pid,L"wake").c_str());readyEvent=CreateEventW(nullptr,FALSE,FALSE,name(pid,L"ready").c_str());if(!wakeEvent||!readyEvent)throw std::runtime_error("GPU events failed");
  std::wstring command=L"\""+helper.wstring()+L"\" "+std::to_wstring(pid);STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
  if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,helper.parent_path().c_str(),&si,&pi))throw std::runtime_error("GPU helper launch failed");process=pi.hProcess;CloseHandle(pi.hThread);
 }
 bool openOutputs(){
  if(opened)return true;if(state()<1)return false;
  for(UINT slot=0;slot<2;++slot)for(UINT plane=0;plane<Planes;++plane){check(d1->OpenSharedResourceByName(outputName(pid,slot,plane).c_str(),DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,IID_PPV_ARGS(&outputs[slot][plane])),"Open output texture");check(device->CreateShaderResourceView(outputs[slot][plane].Get(),nullptr,&views[slot][plane]),"Output SRV");}
  HANDLE h;check(opener->OpenSharedHandleByName(name(pid,L"outputFence").c_str(),GENERIC_ALL,&h),"Open output fence handle");HRESULT hr=d5->OpenSharedFence(h,IID_PPV_ARGS(&outputFence));check(opener->OpenSharedHandle(h,IID_PPV_ARGS(&recoveryFence)),"Recovery fence");CloseHandle(h);check(hr,"Open output fence");opened=true;
  watchdog=std::thread([this]{std::unique_lock<std::mutex>lock(watchMutex);while(!stopping){watchWake.wait_for(lock,std::chrono::milliseconds(20));ULONGLONG issuedAt=queuedAt.load();LONG target=awaited.load();if(!target||outputFence->GetCompletedValue()>=UINT64(target))continue;
   ULONGLONG now=GetTickCount64();ULONGLONG elapsed=now>=issuedAt?now-issuedAt:0;
   bool failedWorker=WaitForSingleObject(process,0)==WAIT_OBJECT_0||state()==-1;bool timeout=issuedAt&&elapsed>2000;
   if(failedWorker||timeout){
    char detail[256]{};DWORD exitCode=STILL_ACTIVE;GetExitCodeProcess(process,&exitCode);
    const char*original=shared&&shared->error[0]?shared->error:"no worker error reported";
    _snprintf_s(detail,sizeof(detail),_TRUNCATE,"DXR %s: %.110s; target=%ld done=%llu input=%llu ready=%ld elapsed=%llums exit=%lu. RTX disabled.",failedWorker?"worker failure":"timeout",original,target,outputFence->GetCompletedValue(),inputFence->GetCompletedValue(),shared?shared->readySerial:0,elapsed,exitCode);
    recoveryFence->Signal(UINT64(target));if(shared){strncpy_s(shared->error,detail,_TRUNCATE);InterlockedExchange(&shared->state,-1);}awaited=0;
   }}});
  return true;
 }
 void clearGeometry(){D3D11_BOX box{0,0,0,std::max(3u,lastGeometryBound)*16,1,1};ctx->CopySubresourceRegion(stream.Get(),0,0,0,0,clearStream.Get(),0,&box);}
 void updateCamera(ID3D11Buffer*buffer,UINT firstRow){
  UINT selection[4]{firstRow,0,0,0};ctx->UpdateSubresource(cameraSelection.Get(),0,nullptr,selection,0,0);
  ComPtr<ID3DDeviceContextState> saved;c1->SwapDeviceContextState(copyState.Get(),&saved);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(copyVS.Get(),nullptr,0);ctx->PSSetShader(cameraPS.Get(),nullptr,0);
  ID3D11Buffer*buffers[]{buffer,cameraSelection.Get()};ctx->PSSetConstantBuffers(0,2,buffers);auto*rtv=cameraRTV.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT vp{0,0,4,1,0,1};ctx->RSSetViewports(1,&vp);ctx->Draw(3,0);c1->SwapDeviceContextState(saved.Get(),nullptr);cameraValid=true;
 }
 void clearLights(){sunValid=headValid=false;pointCount=0;}
 bool hasSun()const{return sunValid;}bool hasHeadlight()const{return headValid;}UINT points()const{return pointCount;}
 void copyLightRows(ID3D11Buffer*buffer,UINT first,UINT rows,UINT destination){
  UINT selection[4]{first-destination,0,0,0};ctx->UpdateSubresource(cameraSelection.Get(),0,nullptr,selection,0,0);
  ComPtr<ID3DDeviceContextState>saved;c1->SwapDeviceContextState(copyState.Get(),&saved);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(copyVS.Get(),nullptr,0);ctx->PSSetShader(cameraPS.Get(),nullptr,0);
  ID3D11Buffer*buffers[]{buffer,cameraSelection.Get()};ctx->PSSetConstantBuffers(0,2,buffers);auto*rtv=lightingRTV.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);
  D3D11_VIEWPORT vp{float(destination),0,float(rows),1,0,1};ctx->RSSetViewports(1,&vp);ctx->Draw(3,0);c1->SwapDeviceContextState(saved.Get(),nullptr);
 }
 void updateSun(ID3D11Buffer*b,UINT colour,UINT direction){if(sunValid)return;copyLightRows(b,colour,1,1);copyLightRows(b,direction,1,2);sunValid=true;}
 void updateHeadlight(ID3D11Buffer*b,UINT row){if(headValid)return;copyLightRows(b,row,3,3);headValid=true;}
 void updatePoints(ID3D11Buffer*b,UINT colours,UINT positions,UINT count){
  for(UINT i=0;i<count&&pointCount<16;++i,++pointCount){copyLightRows(b,colours+i,1,16+pointCount*2);copyLightRows(b,positions+i,1,17+pointCount*2);}
 }
 void stage(UINT vertices=MaxVertices){
  if(vertices>MaxVertices||!canQueue())throw std::runtime_error("Geometry job bounds/queue unavailable");stagedVertices=vertices;lastGeometryBound=vertices;
  ComPtr<ID3DDeviceContextState> saved;c1->SwapDeviceContextState(copyState.Get(),&saved);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(copyVS.Get(),nullptr,0);ctx->PSSetShader(copyPS.Get(),nullptr,0);auto*srv=streamSRV.Get();ctx->PSSetShaderResources(0,1,&srv);auto*rtv=clipRTV.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT vp{0,0,float(ClipWidth),float(std::max(1u,(vertices+ClipWidth-1)/ClipWidth)),0,1};ctx->RSSetViewports(1,&vp);ctx->Draw(3,0);c1->SwapDeviceContextState(saved.Get(),nullptr);
  {D3D11_BOX box{0,0,0,80,1,1};ctx->CopySubresourceRegion(clip.Get(),0,MaxVertices%ClipWidth,MaxVertices/ClipWidth,0,metadataZero.Get(),0,&box);}
  if(cameraValid){ctx->CopyResource(cameraPending.Get(),cameraLive.Get());D3D11_BOX box{0,0,0,4,1,1};ctx->CopySubresourceRegion(clip.Get(),0,MaxVertices%ClipWidth,MaxVertices/ClipWidth,0,cameraPending.Get(),0,&box);}pendingCameraValid=cameraValid;
  normalsUsed=false;if(normalSource){D3D11_TEXTURE2D_DESC nd{};normalSource->GetDesc(&nd);if(nd.Width>=Width&&nd.Height>=Height&&nd.Format==DXGI_FORMAT_R8G8B8A8_UNORM&&nd.SampleDesc.Count==1){D3D11_BOX sourceBox{0,0,0,Width,Height,1};ctx->CopySubresourceRegion(normalInput.Get(),0,0,0,0,normalSource.Get(),0,&sourceBox);D3D11_BOX flagBox{0,0,0,1,1,1};ctx->CopySubresourceRegion(clip.Get(),0,(MaxVertices+4)%ClipWidth,(MaxVertices+4)/ClipWidth,0,normalFlag.Get(),0,&flagBox);normalsUsed=true;}}
  depthUsed=false;
  if(depthSource&&cameraValid){D3D11_SHADER_RESOURCE_VIEW_DESC vd{};depthSource->GetDesc(&vd);ComPtr<ID3D11Resource>r;ComPtr<ID3D11Texture2D>t;depthSource->GetResource(&r);r.As(&t);D3D11_TEXTURE2D_DESC dd{};if(t)t->GetDesc(&dd);
   if(t&&vd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&dd.Width>=Width&&dd.Height>=Height&&dd.SampleDesc.Count==1){
    ComPtr<ID3DDeviceContextState>state;c1->SwapDeviceContextState(copyState.Get(),&state);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(copyVS.Get(),nullptr,0);ctx->PSSetShader(depthPS.Get(),nullptr,0);
    ID3D11ShaderResourceView*sources[]{depthSource.Get(),cameraLiveSRV.Get()};ctx->PSSetShaderResources(0,2,sources);auto*target=depthRTV.Get();ctx->OMSetRenderTargets(1,&target,nullptr);D3D11_VIEWPORT depthViewport{0,0,float(Width),float(Height),0,1};ctx->RSSetViewports(1,&depthViewport);ctx->Draw(3,0);c1->SwapDeviceContextState(state.Get(),nullptr);
    D3D11_BOX flag{0,0,0,1,1,1};ctx->CopySubresourceRegion(clip.Get(),0,(MaxVertices+5)%ClipWidth,(MaxVertices+5)/ClipWidth,0,normalFlag.Get(),0,&flag);depthUsed=true;
   }
  }
  float lightHeader[4]{sunValid?1.f:0.f,headValid?1.f:0.f,float(pointCount),0};D3D11_BOX headerBox{0,0,0,1,1,1};ctx->UpdateSubresource(lighting.Get(),0,&headerBox,lightHeader,16,16);
  D3D11_BOX lightBox{0,0,0,64,1,1};ctx->CopySubresourceRegion(clip.Get(),0,(MaxVertices+16)%ClipWidth,(MaxVertices+16)/ClipWidth,0,lighting.Get(),0,&lightBox);
  check(c4->Signal(inputFence.Get(),++inputSerial),"Signal input");ctx->Flush();staged=true;
 }
 bool submit(UINT vertices,float aspect,UINT draws){
  if(state()<1||vertices>MaxVertices||(!staged&&!canQueue()))return false;if(!staged)stage(vertices);if(stagedVertices!=vertices)throw std::runtime_error("Staged geometry bound mismatch");
  auto&job=shared->jobs[UINT(inputSerial)%JobSlots];job.vertices=vertices;job.aspect=aspect;job.draws=draws;InterlockedExchange(&job.serial,inputSerial);shared->vertexCount=vertices;shared->aspect=aspect;shared->draws=draws;InterlockedExchange(&shared->inputSerial,inputSerial);staged=false;InterlockedExchange(&shared->state,2);SetEvent(wakeEvent);return true;
 }
 bool presentCurrentResult(){
  if(!opened||!pendingCameraValid)return false;
  queuedAt=GetTickCount64();awaited=inputSerial;
  // GPU queue dependency only: this call does not wait on the CPU.
  check(c4->Wait(outputFence.Get(),UINT64(inputSerial)),"Queue current-frame DXR dependency");
  ctx->CopyResource(cameraActive.Get(),cameraPending.Get());activeCameraValid=true;
  active=(inputSerial-1)&1;InterlockedExchange(&shared->activeSlot,active);return true;
 }
 bool receive(){
  if(!openOutputs()||state()!=3)return false;
  if(outputFence->GetCompletedValue()<UINT64(shared->readySerial))return false;
  if(pendingCameraValid)ctx->CopyResource(cameraActive.Get(),cameraPending.Get());activeCameraValid=pendingCameraValid;
  active=shared->readySlot;InterlockedExchange(&shared->activeSlot,active);InterlockedExchange(&shared->state,1);return true;
 }
 void finishReads(){if(active<0)return;check(c4->Signal(readFence.Get(),++readSerial),"Signal texture reads");InterlockedExchange(&shared->consumed[active],readSerial);}
};
}

