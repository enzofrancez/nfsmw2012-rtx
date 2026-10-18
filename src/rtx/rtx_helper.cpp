#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <string>
#include "gpu_protocol.hpp"
namespace rtxlive=rtxgpu;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
static void check(HRESULT h,const char*s){if(FAILED(h)){char b[200];sprintf_s(b,"%s HRESULT=%08X",s,(unsigned)h);throw std::runtime_error(b);}}
struct GPU {
 ComPtr<ID3D12Device5> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList4> list;
 ComPtr<ID3D12Fence> conversionFence;ComPtr<ID3D12Fence> fence;HANDLE event=nullptr;UINT64 serial=0,frequency=0;
 ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline,convert;
 ComPtr<ID3D12Resource> validCount,validRead,zeroUpload;ComPtr<ID3D12Resource> upload,vertices,blas,tlas,scratch,instanceUpload,mask,readback,timestamps;
 ComPtr<ID3D12QueryHeap> timer;
 ComPtr<ID3D12Resource> gameNormals,gameDepth,clip,outputs[2][rtxlive::Planes];
 ComPtr<ID3D12Fence> inputFence,readFence;
 ComPtr<ID3D12DescriptorHeap> inputHeap;
 std::vector<HANDLE> sharedHandles;
 UINT64 blasBytes=0,scratchBytes=0;bool first=true;UINT previousSource=0,previousValid=0;
 static constexpr UINT64 vertexBytes=UINT64(rtxlive::MaxVertices)*12,maskBytes=rtxlive::Pixels*4*rtxlive::Planes;
 ~GPU(){if(event)CloseHandle(event);for(auto h:sharedHandles)CloseHandle(h);}
 ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"CreateBuffer");return r;
 }
 void barrier(ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&x);}
 void uav(ID3D12Resource*r){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=r;list->ResourceBarrier(1,&b);}
 template<class T> void openShared(UINT pid,const wchar_t*suffix,ComPtr<T>&target){HANDLE h;check(device->OpenSharedHandleByName(rtxgpu::name(pid,suffix).c_str(),GENERIC_ALL,&h),"Open GPU handle by name");HRESULT hr=device->OpenSharedHandle(h,IID_PPV_ARGS(&target));CloseHandle(h);check(hr,"Open GPU object");}
 D3D12_RAYTRACING_GEOMETRY_DESC geometry(UINT count){D3D12_RAYTRACING_GEOMETRY_DESC g{};g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;g.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;g.Triangles.VertexBuffer={vertices->GetGPUVirtualAddress(),12};g.Triangles.VertexCount=count;g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;g.Triangles.IndexFormat=DXGI_FORMAT_UNKNOWN;return g;}
 D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom(D3D12_RAYTRACING_GEOMETRY_DESC*g){D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};in.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;in.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;in.NumDescs=1;in.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;in.pGeometryDescs=g;return in;}
 void init(rtxlive::Shared&s,const fs::path&shader){
  ComPtr<IDXGIFactory6> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");
  for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE||desc.AdapterLuid.LowPart!=s.adapterLow||desc.AdapterLuid.HighPart!=s.adapterHigh)continue;ComPtr<ID3D12Device5>d;if(FAILED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d))))continue;D3D12_FEATURE_DATA_D3D12_OPTIONS5 o{};if(SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&o,sizeof(o)))&&o.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1){device=d;WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,s.adapter,sizeof(s.adapter),nullptr,nullptr);break;}}
  if(!device)throw std::runtime_error("No hardware DXR 1.1 adapter. Software fallback is disabled.");
  D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");check(queue->GetTimestampFrequency(&frequency),"TimestampFrequency");
  check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"List");check(list->Close(),"Close");check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"Fence");check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&conversionFence)),"Conversion fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("CreateEvent failed");
  D3D12_DESCRIPTOR_RANGE inputRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,2,0,0};
  D3D12_ROOT_PARAMETER p[7]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[0].Descriptor.ShaderRegister=0;p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1].Descriptor.ShaderRegister=1;p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[3].Constants={0,0,7};p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[4].DescriptorTable={1,&inputRange};p[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[5].Descriptor.ShaderRegister=1;p[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[6].Descriptor.ShaderRegister=2;
  D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=7;rd.pParameters=p;ComPtr<ID3DBlob> blob,errors;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"SerializeRoot");check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Root");
  std::ifstream f(shader,std::ios::binary);std::vector<char> code((std::istreambuf_iterator<char>(f)),{});if(code.empty())throw std::runtime_error("Missing rtx_trace.cso");D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code.data(),code.size()};check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"DXR ray query pipeline");
  std::ifstream cf(shader.parent_path()/L"rtx_convert.cso",std::ios::binary);std::vector<char> cc((std::istreambuf_iterator<char>(cf)),{});if(cc.empty())throw std::runtime_error("Missing GPU geometry shader");pd.CS={cc.data(),cc.size()};check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&convert)),"Convert pipeline");
  vertices=buffer(vertexBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  validCount=buffer(4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);validRead=buffer(4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  zeroUpload=buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void*zeroData;D3D12_RANGE noRead{0,0};check(zeroUpload->Map(0,&noRead,&zeroData),"Zero upload map");memset(zeroData,0,256);zeroUpload->Unmap(0,nullptr);

  openShared(s.parentPID,L"clip",clip);openShared(s.parentPID,L"inputFence",inputFence);openShared(s.parentPID,L"readFence",readFence);
  D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;dh.NumDescriptors=3;dh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;check(device->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&inputHeap)),"Input heap");D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Texture2D.MipLevels=1;device->CreateShaderResourceView(clip.Get(),&sd,inputHeap->GetCPUDescriptorHandleForHeapStart());
  openShared(s.parentPID,L"normals",gameNormals);sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;auto normalHandle=inputHeap->GetCPUDescriptorHandleForHeapStart();normalHandle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateShaderResourceView(gameNormals.Get(),&sd,normalHandle);
  openShared(s.parentPID,L"depth",gameDepth);sd.Format=DXGI_FORMAT_R32_FLOAT;normalHandle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateShaderResourceView(gameDepth.Get(),&sd,normalHandle);
  D3D12_RESOURCE_DESC od{};od.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;od.Width=rtxlive::Width;od.Height=rtxlive::Height;od.MipLevels=od.DepthOrArraySize=1;od.Format=DXGI_FORMAT_R8G8B8A8_UNORM;od.SampleDesc.Count=1;od.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
  for(UINT slot=0;slot<2;++slot)for(UINT plane=0;plane<rtxlive::Planes;++plane){check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_SHARED,&od,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&outputs[slot][plane])),"Shared result texture");HANDLE h;check(device->CreateSharedHandle(outputs[slot][plane].Get(),nullptr,GENERIC_ALL,rtxgpu::outputName(s.parentPID,slot,plane).c_str(),&h),"Share result");sharedHandles.push_back(h);}
  HANDLE fenceHandle;check(device->CreateSharedHandle(fence.Get(),nullptr,GENERIC_ALL,rtxgpu::name(s.parentPID,L"outputFence").c_str(),&fenceHandle),"Share output fence");sharedHandles.push_back(fenceHandle);
  auto g=geometry(rtxlive::MaxVertices);auto in=bottom(&g);D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bi{};device->GetRaytracingAccelerationStructurePrebuildInfo(&in,&bi);blasBytes=bi.ResultDataMaxSizeInBytes;scratchBytes=bi.ScratchDataSizeInBytes;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;top.NumDescs=1;top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ti{};device->GetRaytracingAccelerationStructurePrebuildInfo(&top,&ti);
  blas=buffer(blasBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);tlas=buffer(ti.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);scratch=buffer(std::max(scratchBytes,ti.ScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  instanceUpload=buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);D3D12_RAYTRACING_INSTANCE_DESC instance{};instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1;instance.InstanceMask=255;instance.AccelerationStructure=blas->GetGPUVirtualAddress();void*m=nullptr;D3D12_RANGE empty{0,0};check(instanceUpload->Map(0,&empty,&m),"MapInstance");memcpy(m,&instance,sizeof(instance));instanceUpload->Unmap(0,nullptr);
  mask=buffer(maskBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);timestamps=buffer(32,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_QUERY_HEAP_DESC th{};th.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;th.Count=4;check(device->CreateQueryHeap(&th,IID_PPV_ARGS(&timer)),"Timer");
 }
 void render(rtxlive::Shared&s){
  auto begin=std::chrono::steady_clock::now();UINT count=std::min(s.vertexCount,rtxlive::MaxVertices)/3*3;s.triangles=count/3;
  UINT slot=(UINT(s.inputSerial)-1)&1;
  check(queue->Wait(inputFence.Get(),UINT64(s.inputSerial)),"Wait for GPU geometry");
  // Input N is signalled after the game queue consumed N-1. This also protects the reused output slot.

  check(allocator->Reset(),"Reset allocator");check(list->Reset(allocator.Get(),convert.Get()),"Reset list");list->EndQuery(timer.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
  bool compact=count>=65535&&previousSource>0&&UINT64(previousValid)*100<UINT64(previousSource)*65;
  if(!first)barrier(validCount.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(validCount.Get(),0,zeroUpload.Get(),0,4);barrier(validCount.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if(compact){barrier(vertices.Get(),first?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(vertices.Get(),0,zeroUpload.Get(),0,36);barrier(vertices.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
  else if(!first)barrier(vertices.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  barrier(clip.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);barrier(gameNormals.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);barrier(gameDepth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap*heaps[]={inputHeap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetComputeRootDescriptorTable(4,inputHeap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootUnorderedAccessView(5,vertices->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(6,validCount->GetGPUVirtualAddress());
  struct{float px,py;UINT width,height,vertices,clipWidth,compact;}cfg{1.73205080757f/s.aspect,1.73205080757f,rtxlive::Width,rtxlive::Height,count,rtxlive::ClipWidth,compact?1u:0u};list->SetComputeRoot32BitConstants(3,7,&cfg,0);list->Dispatch(std::max(1u,(s.triangles+63)/64),1,1);uav(vertices.Get());barrier(vertices.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  uav(validCount.Get());barrier(validCount.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(validRead.Get(),0,validCount.Get(),0,4);
  UINT compactCount=count;
  if(compact){
   check(list->Close(),"Close conversion");ID3D12CommandList*conversionLists[]={list.Get()};queue->ExecuteCommandLists(1,conversionLists);check(queue->Signal(conversionFence.Get(),UINT64(s.inputSerial)),"Signal conversion");check(conversionFence->SetEventOnCompletion(UINT64(s.inputSerial),event),"Conversion event");if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("Geometry count conversion timeout");
   void*countData;D3D12_RANGE countRange{0,4},emptyRange{0,0};check(validRead->Map(0,&countRange,&countData),"Map valid vertex count");compactCount=*(UINT*)countData;validRead->Unmap(0,&emptyRange);if(compactCount>count||compactCount%3)throw std::runtime_error("Invalid compact vertex count");
   check(allocator->Reset(),"Reset after conversion");check(list->Reset(allocator.Get(),convert.Get()),"Reset build list");list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetComputeRootDescriptorTable(4,inputHeap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRoot32BitConstants(3,7,&cfg,0);
  }
  list->EndQuery(timer.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
  auto g=geometry(std::max(compactCount,3u));D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=bottom(&g);build.DestAccelerationStructureData=blas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();list->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(blas.Get());uav(scratch.Get());
  build={};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.NumDescs=1;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;build.Inputs.InstanceDescs=instanceUpload->GetGPUVirtualAddress();build.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();list->BuildRaytracingAccelerationStructure(&build,0,nullptr);uav(tlas.Get());
  list->EndQuery(timer.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);
  if(!first)barrier(mask.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetPipelineState(pipeline.Get());list->SetComputeRootShaderResourceView(0,tlas->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(1,vertices->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(2,mask->GetGPUVirtualAddress());list->Dispatch((rtxlive::Width+7)/8,(rtxlive::Height+7)/8,1);barrier(mask.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  for(UINT plane=0;plane<rtxlive::Planes;++plane){barrier(outputs[slot][plane].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=mask.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint.Offset=UINT64(plane)*rtxlive::Pixels*4;src.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,rtxlive::Width,rtxlive::Height,1,rtxlive::Width*4};D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=outputs[slot][plane].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(outputs[slot][plane].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);}
  barrier(clip.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);barrier(gameNormals.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);barrier(gameDepth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
  list->EndQuery(timer.Get(),D3D12_QUERY_TYPE_TIMESTAMP,3);list->ResolveQueryData(timer.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,4,timestamps.Get(),0);check(list->Close(),"Close render");ID3D12CommandList*lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++serial),"Signal result");check(fence->SetEventOnCompletion(serial,event),"Fence event");if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU transport timeout");check(device->GetDeviceRemovedReason(),"GPU device");
  void*countData;D3D12_RANGE countRange{0,4},emptyCount{0,0};check(validRead->Map(0,&countRange,&countData),"Map geometry statistics");previousValid=*(UINT*)countData;validRead->Unmap(0,&emptyCount);previousSource=count;s.sourceVertices=count;s.validVertices=previousValid;s.compactMode=compact?1:0;s.triangles=(compact?previousValid:count)/3;
  void*m;D3D12_RANGE range{0,32},empty{0,0};check(timestamps->Map(0,&range,&m),"Map timestamps");auto*ts=(UINT64*)m;s.convertMs=float(double(ts[1]-ts[0])*1000/frequency);s.buildMs=float(double(ts[2]-ts[1])*1000/frequency);s.traceMs=float(double(ts[3]-ts[2])*1000/frequency);s.gpuMs=float(double(ts[3]-ts[0])*1000/frequency);timestamps->Unmap(0,&empty);s.totalMs=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-begin).count();s.readySlot=slot;s.readySerial=LONG(serial);++s.frames;first=false;
 }

};
int wmain(int argc,wchar_t**argv){
 if(argc!=2)return 2;UINT pid=wcstoul(argv[1],nullptr,10);HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,rtxgpu::name(pid,L"ipc").c_str());if(!mapping)return 3;auto*s=(rtxlive::Shared*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(rtxlive::Shared));if(!s){CloseHandle(mapping);return 4;}HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,pid);
 HANDLE wake=OpenEventW(SYNCHRONIZE,FALSE,rtxgpu::name(pid,L"wake").c_str()),ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,rtxgpu::name(pid,L"ready").c_str()); try{if(!wake||!ready)throw std::runtime_error("GPU events missing");if(s->magic!=rtxlive::Magic||s->version!=10)throw std::runtime_error("GPU IPC version mismatch");wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);GPU gpu;gpu.init(*s,fs::path(exe).parent_path()/L"rtx_trace.cso");InterlockedExchange(&s->state,1);SetEvent(ready);LONG completed=0;
 while(parent&&WaitForSingleObject(parent,0)==WAIT_TIMEOUT){
  LONG state=InterlockedCompareExchange(&s->state,0,0);if(state<0)break;
  LONG submitted=InterlockedCompareExchange(&s->inputSerial,0,0);
  if(submitted<=completed){WaitForSingleObject(wake,100);continue;}
  // Every published serial must be processed; a newer submission cannot replace
  // an earlier queued fence dependency. Geometry/camera are protected by the
  // game GPU queue: stage N+1 executes only after result N and its consumers.
  auto&input=s->jobs[UINT(completed+1)%rtxlive::JobSlots];if(InterlockedCompareExchange(&input.serial,0,0)!=completed+1)throw std::runtime_error("GPU geometry job serial mismatch");rtxlive::Shared job{};job.inputSerial=completed+1;job.vertexCount=input.vertices;job.aspect=input.aspect;job.draws=input.draws;
  gpu.render(job);++completed;
  s->sourceVertices=job.sourceVertices;s->validVertices=job.validVertices;s->compactMode=job.compactMode;s->convertMs=job.convertMs;s->buildMs=job.buildMs;s->traceMs=job.traceMs;s->triangles=job.triangles;s->gpuMs=job.gpuMs;s->totalMs=job.totalMs;s->frames=completed;s->readySlot=job.readySlot;InterlockedExchange(&s->readySerial,job.readySerial);
  if(InterlockedCompareExchange(&s->state,0,0)>=0)InterlockedExchange(&s->state,3);SetEvent(ready);
 }}

 catch(const std::exception&e){strncpy_s(s->error,e.what(),_TRUNCATE);InterlockedExchange(&s->state,-1);if(ready)SetEvent(ready);Sleep(1000);}if(wake)CloseHandle(wake);if(ready)CloseHandle(ready);if(parent)CloseHandle(parent);UnmapViewOfFile(s);CloseHandle(mapping);return 0;
}

