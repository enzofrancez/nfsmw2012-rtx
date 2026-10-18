// Continuous post-VS geometry capture with an x64 hardware DXR 1.1 companion.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <reshade.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include "gpu_client.hpp"
#include <memory>
#include <unordered_set>
#include <regex>
#include "material_buffers.hpp"
using Microsoft::WRL::ComPtr;
namespace api = reshade::api;
namespace fs = std::filesystem;
static fs::path root;
static MaterialBuffers materialBuffers;
static std::unordered_set<uint64_t>vehicleMaterialShaders;
static std::mutex shaderMutex, imageMutex, logMutex;
static std::unordered_map<uint64_t,ComPtr<ID3D11GeometryShader>> shaders;
static ComPtr<ID3D11Buffer> streamBuffer;
static ComPtr<ID3D11Query> statisticsQuery;
static std::unique_ptr<rtxgpu::Client> transport;
static api::device*gameDevice=nullptr;
struct CameraBinding{UINT slot,row;};
static std::unordered_map<uint64_t,CameraBinding>cameraBindings;
static bool cameraThisFrame=false,cameraReported=false;
struct LightBinding{UINT slot=0,colour=0,direction=0,head=0,positions=0,count=0;bool sun=false,headlight=false,point=false;};
static std::unordered_map<uint64_t,LightBinding> lightBindings;
static bool live=false,failed=false;
static ULONGLONG captureTime=0,lastResult=0,resultCaptureTime=0;
static uint32_t updates=0;


static bool firstDraw=true;
static uint32_t screenWidth=1920,screenHeight=1080,captureDraws=0;
static uint64_t requestedVertices=0,rejectedVertices=0;
static uint32_t nonMaterialDraws=0,budgetRejectedDraws=0;
static const UINT maxVertices=rtxgpu::MaxVertices;
static D3D11_QUERY_DATA_SO_STATISTICS capturedStats{};
enum class Phase {idle,capture,query,readback,processing};
static Phase phase=Phase::idle;
static thread_local bool internalCall=false;
static std::atomic_uint shaderSuccess{0},shaderFailure{0};
static void log(const std::string &s) {
 std::lock_guard<std::mutex> lock(logMutex);
 std::ofstream(root/L"NFSMW_RTX.log",std::ios::app)<<s<<"\n";
}
static void initPipeline(api::device *device,api::pipeline_layout,uint32_t count,const api::pipeline_subobject *objects,api::pipeline pipeline) {
 if(!internalCall&&device->get_api()==api::device_api::d3d11&&count==1&&objects[0].type==api::pipeline_subobject_type::pixel_shader){
  auto&desc=*static_cast<const api::shader_desc*>(objects[0].data);ComPtr<ID3D11ShaderReflection>r;
  if(desc.code&&SUCCEEDED(D3DReflect(desc.code,desc.code_size,IID_PPV_ARGS(&r)))){
   D3D11_SHADER_DESC shaderDesc{};r->GetDesc(&shaderDesc);
   for(UINT k=0;k<shaderDesc.ConstantBuffers;++k){auto*cb=r->GetConstantBufferByIndex(k);D3D11_SHADER_BUFFER_DESC cbd{};D3D11_SHADER_INPUT_BIND_DESC bind{};if(FAILED(cb->GetDesc(&cbd))||FAILED(r->GetResourceBindingDescByName(cbd.Name,&bind)))continue;
    LightBinding light{};light.slot=bind.BindPoint;D3D11_SHADER_VARIABLE_DESC a{},b{};
    if(SUCCEEDED(cb->GetVariableByName("KeyLightDirection")->GetDesc(&a))&&SUCCEEDED(cb->GetVariableByName("KeyLightColour")->GetDesc(&b))&&a.StartOffset%16==0&&b.StartOffset%16==0){light.sun=true;light.direction=a.StartOffset/16;light.colour=b.StartOffset/16;}
    if(SUCCEEDED(cb->GetVariableByName("gPlayerHeadlight")->GetDesc(&a))&&a.Size==48&&a.StartOffset%16==0){light.headlight=true;light.head=a.StartOffset/16;}
    if(SUCCEEDED(cb->GetVariableByName("deferred_lightColours")->GetDesc(&a))&&SUCCEEDED(cb->GetVariableByName("deferred_lightPositions")->GetDesc(&b))&&a.Size==64&&b.Size==60){
     ComPtr<ID3DBlob>assembly;if(SUCCEEDED(D3DDisassemble(desc.code,desc.code_size,0,nullptr,&assembly))){std::string text((char*)assembly->GetBufferPointer(),assembly->GetBufferSize());std::smatch match;
      // Known deferred shader loops have a literal packet count. Never trace unused CB entries.
      if(std::regex_search(text,match,std::regex("ige r[0-9]+\\.[xyzw], r[0-9]+\\.[xyzw], l\\(([1-4])\\)"))){light.point=true;light.count=std::stoul(match[1]);light.colour=a.StartOffset/16;light.positions=b.StartOffset/16;}}
    }
    if(light.sun||light.headlight||light.point){std::lock_guard<std::mutex>lock(shaderMutex);lightBindings[pipeline.handle]=light;}
   }
   D3D11_SHADER_DESC sd{};r->GetDesc(&sd);for(UINT i=0;i<sd.ConstantBuffers;++i){auto*cb=r->GetConstantBufferByIndex(i);D3D11_SHADER_VARIABLE_DESC vd{};if(SUCCEEDED(cb->GetVariableByName("PbrPaintDiffuseColour")->GetDesc(&vd))){std::lock_guard<std::mutex>lock(shaderMutex);vehicleMaterialShaders.insert(pipeline.handle);break;}}}return;
 }
 if(internalCall||device->get_api()!=api::device_api::d3d11||count!=1||(objects[0].type!=api::pipeline_subobject_type::vertex_shader&&objects[0].type!=api::pipeline_subobject_type::geometry_shader))return;
 auto &desc=*static_cast<const api::shader_desc*>(objects[0].data);
 if(!desc.code||!desc.code_size)return;
 ID3D11Device *native=(ID3D11Device*)(uintptr_t)device->get_native();
 ComPtr<ID3D11ShaderReflection> reflection;
 if(FAILED(D3DReflect(desc.code,desc.code_size,IID_PPV_ARGS(&reflection))))return;
 D3D11_SHADER_DESC sd{};reflection->GetDesc(&sd);
 if(objects[0].type==api::pipeline_subobject_type::vertex_shader){
  for(UINT b=0;b<sd.ConstantBuffers;++b){auto*cb=reflection->GetConstantBufferByIndex(b);D3D11_SHADER_BUFFER_DESC bd{};if(FAILED(cb->GetDesc(&bd)))continue;
   auto*variable=cb->GetVariableByName("ViewProjectionModified");D3D11_SHADER_VARIABLE_DESC vd{};D3D11_SHADER_TYPE_DESC td{};D3D11_SHADER_INPUT_BIND_DESC binding{};
   if(SUCCEEDED(variable->GetDesc(&vd))&&SUCCEEDED(variable->GetType()->GetDesc(&td))&&td.Rows==4&&td.Columns==4&&td.Class==D3D_SVC_MATRIX_ROWS&&vd.StartOffset%16==0&&SUCCEEDED(reflection->GetResourceBindingDescByName(bd.Name,&binding))){std::lock_guard<std::mutex>lock(shaderMutex);cameraBindings[pipeline.handle]={binding.BindPoint,vd.StartOffset/16};}
  }
 }
 // Screen quads have no camera/object constant buffer and are not scene meshes.
 if(objects[0].type==api::pipeline_subobject_type::vertex_shader&&sd.ConstantBuffers==0)return;
 for(UINT i=0;i<sd.OutputParameters;++i){
  D3D11_SIGNATURE_PARAMETER_DESC output{};reflection->GetOutputParameterDesc(i,&output);
  if(output.SystemValueType!=D3D_NAME_POSITION||output.Mask!=15)continue;
  D3D11_SO_DECLARATION_ENTRY entry{0,output.SemanticName,output.SemanticIndex,0,4,0};UINT stride=16;
  ComPtr<ID3D11GeometryShader> captureShader;
  internalCall=true;
  // Stream 0 keeps the original rasterization while collecting clip positions.
  HRESULT hr=native->CreateGeometryShaderWithStreamOutput(desc.code,desc.code_size,&entry,1,&stride,1,0,nullptr,&captureShader);
  internalCall=false;
  if(SUCCEEDED(hr)){std::lock_guard<std::mutex> lock(shaderMutex);shaders[pipeline.handle]=captureShader;++shaderSuccess;}
  else {++shaderFailure;if(shaderFailure<5)log("SO shader rejected HRESULT="+std::to_string((uint32_t)hr));}
  break;
 }
}
static void destroyPipeline(api::device*,api::pipeline pipeline) {
 if(internalCall)return;
 ComPtr<ID3D11GeometryShader> removed;
 {std::lock_guard<std::mutex> lock(shaderMutex);lightBindings.erase(pipeline.handle);vehicleMaterialShaders.erase(pipeline.handle);cameraBindings.erase(pipeline.handle);auto it=shaders.find(pipeline.handle);if(it!=shaders.end()){removed=std::move(it->second);shaders.erase(it);}}
 internalCall=true;removed.Reset();internalCall=false;
}
static bool makeBuffers(ID3D11Device *device) {
 if(statisticsQuery)return true;if(!transport)return false;
 streamBuffer=transport->streamBuffer();D3D11_QUERY_DESC q{D3D11_QUERY_SO_STATISTICS,0};
 return SUCCEEDED(device->CreateQuery(&q,&statisticsQuery));
}

static void observeCamera(api::command_list*list,UINT count){
 if(internalCall||cameraThisFrame||!transport||!materialBuffers.inMaterialPass()||count<300||list->get_device()!=gameDevice)return;
 auto*ctx=(ID3D11DeviceContext*)(uintptr_t)list->get_native();if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
 UINT n=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&n,&vp);if(n!=1||vp.Width<screenWidth*.95f||vp.Width>screenWidth*1.05f||vp.Height<screenHeight*.95f||vp.Height>screenHeight*1.05f)return;
 ComPtr<ID3D11DepthStencilView>dsv;ctx->OMGetRenderTargets(0,nullptr,&dsv);if(!dsv)return;
 ComPtr<ID3D11VertexShader>vs;ctx->VSGetShader(&vs,nullptr,nullptr);CameraBinding binding{};
 {std::lock_guard<std::mutex>lock(shaderMutex);auto it=cameraBindings.find((uint64_t)(uintptr_t)vs.Get());if(it==cameraBindings.end())return;binding=it->second;}
 ComPtr<ID3D11Buffer>buffer;UINT first=0,length=0;ComPtr<ID3D11DeviceContext1>context1;
 if(SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&context1))))context1->VSGetConstantBuffers1(binding.slot,1,&buffer,&first,&length);else ctx->VSGetConstantBuffers(binding.slot,1,&buffer);
 if(!buffer)return;D3D11_BUFFER_DESC bd{};buffer->GetDesc(&bd);if((binding.row+first+4)*16>bd.ByteWidth)return;
 internalCall=true;try{transport->updateCamera(buffer.Get(),binding.row+first);cameraThisFrame=true;if(!cameraReported){cameraReported=true;log("Actual ViewProjectionModified captured on GPU, slot="+std::to_string(binding.slot)+" row="+std::to_string(binding.row));}}catch(const std::exception&e){log(std::string("Camera GPU copy failed: ")+e.what());failed=true;live=false;}internalCall=false;
}
static void observeLights(api::command_list*list){
 if(internalCall||!transport||list->get_device()!=gameDevice)return;auto*ctx=(ID3D11DeviceContext*)(uintptr_t)list->get_native();if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
 UINT n=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&n,&vp);if(n!=1||vp.TopLeftX!=0||vp.TopLeftY!=0||vp.Width<screenWidth*.95f||vp.Height<screenHeight*.95f||vp.Width>screenWidth*1.05f||vp.Height>screenHeight*1.05f)return;
 ComPtr<ID3D11PixelShader>ps;ctx->PSGetShader(&ps,nullptr,nullptr);LightBinding binding{};{std::lock_guard<std::mutex>lock(shaderMutex);auto it=lightBindings.find((uint64_t)(uintptr_t)ps.Get());if(it==lightBindings.end())return;binding=it->second;}
 ComPtr<ID3D11Buffer>b;ComPtr<ID3D11DeviceContext1>c1;UINT first=0,length=0;if(SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&c1))))c1->PSGetConstantBuffers1(binding.slot,1,&b,&first,&length);else ctx->PSGetConstantBuffers(binding.slot,1,&b);if(!b)return;
 D3D11_BUFFER_DESC bd{};b->GetDesc(&bd);UINT end=0;if(binding.sun)end=std::max(binding.colour,binding.direction)+1;if(binding.headlight)end=std::max(end,binding.head+3);if(binding.point)end=std::max(end,std::max(binding.colour,binding.positions)+binding.count);if((first+end)*16>bd.ByteWidth)return;
 internalCall=true;try{if(binding.sun)transport->updateSun(b.Get(),first+binding.colour,first+binding.direction);if(binding.headlight)transport->updateHeadlight(b.Get(),first+binding.head);if(binding.point)transport->updatePoints(b.Get(),first+binding.colour,first+binding.positions,binding.count);}catch(const std::exception&e){log(std::string("Light GPU copy failed: ")+e.what());failed=true;live=false;}internalCall=false;
}
static void observeMaterials(api::command_list*list){
 if(internalCall||list->get_device()!=gameDevice)return;auto*ctx=(ID3D11DeviceContext*)(uintptr_t)list->get_native();if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
 ComPtr<ID3D11PixelShader>ps;ctx->PSGetShader(&ps,nullptr,nullptr);bool recognized=false;{std::lock_guard<std::mutex>lock(shaderMutex);recognized=vehicleMaterialShaders.count((uint64_t)(uintptr_t)ps.Get())!=0;}
 if(recognized){static bool first=true;if(first){first=false;log("Recognized PBR vehicle pixel shader; checking actual MRT descriptions.");}}
 internalCall=true;materialBuffers.observe(ctx,recognized,screenWidth,screenHeight);internalCall=false;
 {static std::string previous;if(materialBuffers.info()!=previous){previous=materialBuffers.info();if(!previous.empty())log("Material resource: "+previous);}}
}
static void renderTargetTransition(api::command_list*list,uint32_t,const api::resource_view*,api::resource_view){
 if(internalCall||list->get_device()!=gameDevice)return;auto*ctx=(ID3D11DeviceContext*)(uintptr_t)list->get_native();if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
 internalCall=true;materialBuffers.transition(ctx);internalCall=false;
}
static double drawHookMs=0;static UINT drawHookCalls=0;
struct DrawHookTimer {std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();~DrawHookTimer(){drawHookMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();++drawHookCalls;}};
template<class Draw> static bool captureDraw(api::command_list *list,uint32_t count,uint32_t instances,Draw execute) {
 if(internalCall)return false;
 DrawHookTimer hookTimer;
 observeLights(list);
 observeMaterials(list);
 observeCamera(list,count);
 if(internalCall||phase!=Phase::capture||count<3||!instances||list->get_device()->get_api()!=api::device_api::d3d11)return false;
 if(!materialBuffers.inMaterialPass()){++nonMaterialDraws;return false;}
 auto *ctx=(ID3D11DeviceContext*)(uintptr_t)list->get_native();
 if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
 D3D11_PRIMITIVE_TOPOLOGY topology;ctx->IAGetPrimitiveTopology(&topology);
 if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)return false;
 D3D11_VIEWPORT viewport{};UINT n=1;ctx->RSGetViewports(&n,&viewport);
 if(n!=1||viewport.Width<screenWidth*.95f||viewport.Height<screenHeight*.95f||viewport.Width>screenWidth*1.05f||viewport.Height>screenHeight*1.05f)return false;
 ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(1,&rtv,&dsv);
 if(!dsv)return false;
 ComPtr<ID3D11DepthStencilState> depth;ctx->OMGetDepthStencilState(&depth,nullptr);
 if(depth){D3D11_DEPTH_STENCIL_DESC d{};depth->GetDesc(&d);if(!d.DepthEnable||d.DepthFunc==D3D11_COMPARISON_ALWAYS||d.DepthFunc==D3D11_COMPARISON_NEVER)return false;}
 ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
 UINT classCount=0;ctx->GSGetShader(&gs,nullptr,&classCount);if(classCount)return false;ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
 if(hs||ds)return false;
 ID3D11Buffer *oldTargets[4]{};ctx->SOGetTargets(4,oldTargets);bool hasSO=false;
 for(auto *t:oldTargets)if(t){hasSO=true;t->Release();}if(hasSO)return false;
 ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);if(!vs)return false;
 ComPtr<ID3D11GeometryShader> captureShader;
 {std::lock_guard<std::mutex> lock(shaderMutex);auto it=shaders.find((uint64_t)(uintptr_t)(gs?static_cast<IUnknown*>(gs.Get()):static_cast<IUnknown*>(vs.Get())));if(it==shaders.end())return false;captureShader=it->second;}
 uint64_t vertices=uint64_t(count)*instances*(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP?3:1);
 if(requestedVertices+vertices>maxVertices){++budgetRejectedDraws;rejectedVertices+=vertices;return false;}
 requestedVertices+=vertices;
 UINT offset=firstDraw?0:UINT(-1);ID3D11Buffer *target=streamBuffer.Get();
 // Execute the original draw once with rasterization and SO both enabled.
 // Return true from the outer ReShade callback to suppress a second draw.
 internalCall=true;ctx->GSSetShader(captureShader.Get(),nullptr,0);ctx->SOSetTargets(1,&target,&offset);
 execute(ctx);
 ctx->SOSetTargets(0,nullptr,nullptr);ctx->GSSetShader(gs.Get(),nullptr,0);internalCall=false;
 firstDraw=false;++captureDraws;return true;
}
static bool draw(api::command_list *list,uint32_t count,uint32_t instances,uint32_t first,uint32_t firstInstance) {
 return captureDraw(list,count,instances,[=](ID3D11DeviceContext *ctx){ctx->DrawInstanced(count,instances,first,firstInstance);});
}
static bool drawIndexed(api::command_list *list,uint32_t count,uint32_t instances,uint32_t first,int32_t base,uint32_t firstInstance) {
 return captureDraw(list,count,instances,[=](ID3D11DeviceContext *ctx){ctx->DrawIndexedInstanced(count,instances,first,base,firstInstance);});
}
static void setBool(api::effect_runtime*r,const char*name,bool v){auto u=r->find_uniform_variable("NFSMW_RTX.fx",name);if(u.handle)r->set_uniform_value_bool(u,v);}
static void setFloat(api::effect_runtime*r,const char*name,float v){auto u=r->find_uniform_variable("NFSMW_RTX.fx",name);if(u.handle)r->set_uniform_value_float(u,v);}
static void setInt(api::effect_runtime*r,const char*name,int v){auto u=r->find_uniform_variable("NFSMW_RTX.fx",name);if(u.handle)r->set_uniform_value_int(u,v);}
static void beginEffects(api::effect_runtime *runtime,api::command_list *list,api::resource_view,api::resource_view) {
 if(list->get_device()->get_api()!=api::device_api::d3d11)return;
 auto*ctx=(ID3D11DeviceContext*)(uintptr_t)list->get_native();if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
 runtime->get_screenshot_width_and_height(&screenWidth,&screenHeight);
 double frameDrawHookMs=drawHookMs;UINT frameDrawHookCalls=drawHookCalls;drawHookMs=0;drawHookCalls=0;
 bool captureReference=false,newResult=false;
 auto technique=runtime->find_technique("NFSMW_RTX.fx","NFSMW_RTX_Live");
 bool techniqueEnabled=technique.handle&&runtime->get_technique_state(technique);
 {static auto sampleStart=std::chrono::steady_clock::now();static UINT samples=0;
  if(live&&techniqueEnabled){if(samples++==0)sampleStart=std::chrono::steady_clock::now();else if(samples>=121){float ms=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-sampleStart).count()/120;log("Frame interval average ms="+std::to_string(ms)+" render_callbacks_per_second="+std::to_string(ms>0?1000/ms:0));samples=0;}}
  else samples=0;
 }
 try{
  if(!transport&&!failed){ComPtr<ID3D11Device>device;ctx->GetDevice(&device);gameDevice=list->get_device();transport=std::make_unique<rtxgpu::Client>();internalCall=true;transport->init(device.Get(),ctx,root/L"NFSMW_RTX_GPU_Helper.exe");internalCall=false;log("GPU bridge initialized; direct shared textures, no CPU geometry/image transport.");}
  if(transport){
   transport->setNormals(materialBuffers.available()?materialBuffers.view(0):nullptr);transport->setDepth(materialBuffers.hasDepth()?materialBuffers.depth():nullptr);
   auto state=transport->state();
   if(state==-1)throw std::runtime_error(transport->shared->error);
   static LONG reportedSerial=0;
   if(transport->shared->readySerial>reportedSerial){
    reportedSerial=transport->shared->readySerial;
    lastResult=GetTickCount64();if(updates<=3||updates%30==0){auto*s=transport->shared;log("Completed GPU job: GPU_ms="+std::to_string(s->gpuMs)+" convert_ms="+std::to_string(s->convertMs)+" build_ms="+std::to_string(s->buildMs)+" trace_ms="+std::to_string(s->traceMs)+" valid_vertices="+std::to_string(s->validVertices)+" source_vertices="+std::to_string(s->sourceVertices)+" compact="+std::to_string(s->compactMode)+" helper_ms="+std::to_string(s->totalMs));}
   }
   transport->openOutputs();
   if(phase==Phase::capture){
    if(live&&techniqueEnabled&&!failed&&cameraThisFrame&&transport->state()>=1&&transport->canQueue()){
     internalCall=true;transport->stage(UINT(requestedVertices));{static int previous=-1;int used=transport->usedDepth()?1:0;if(used!=previous){previous=used;log(std::string("DXR primary depth: ")+(used?"current material-pass snapshot":"ray-query fallback"));}}{static int previous=-1;int used=transport->usedNormals()?1:0;if(used!=previous){previous=used;log(std::string("DXR normals: ")+(used?"current game material normals":"geometric fallback"));}}
     if(transport->submit(UINT(requestedVertices),float(screenWidth)/std::max(1u,screenHeight),captureDraws)&&transport->presentCurrentResult()){
      captureReference=newResult=true;++updates;
      if(updates<=3||updates%30==0)log("Current-frame DXR capture_mode=single-pass queued="+std::to_string(updates)+" draw_hook_cpu_ms="+std::to_string(frameDrawHookMs)+" draw_hook_calls="+std::to_string(frameDrawHookCalls)+" captured_draws="+std::to_string(captureDraws)+" vertices="+std::to_string(requestedVertices)+" budget_skipped_draws="+std::to_string(budgetRejectedDraws)+" budget_skipped_vertices="+std::to_string(rejectedVertices)+" other_pass_draws="+std::to_string(nonMaterialDraws)+" slot="+std::to_string(transport->active));
     }internalCall=false;
    }
    phase=Phase::idle;
   }
   if(transport->active>=0){const char*semantics[]={"RTX_GPU_MASK","RTX_GPU_REFLECTION","RTX_GPU_INDIRECT","RTX_GPU_WEIGHTS","RTX_GPU_LIGHTING"};for(UINT i=0;i<rtxgpu::Planes;++i){api::resource_view view{(uint64_t)(uintptr_t)transport->view(i)};runtime->update_texture_bindings(semantics[i],view,view);}}
   {api::resource_view current{(uint64_t)(uintptr_t)transport->liveCameraView()},captured{(uint64_t)(uintptr_t)transport->activeCameraView()};runtime->update_texture_bindings("RTX_CAMERA_CURRENT",current,current);runtime->update_texture_bindings("RTX_CAMERA_CAPTURED",captured,captured);}

  }
 }catch(const std::exception&e){internalCall=false;if(!failed)log(std::string("GPU bridge failed: ")+e.what());failed=true;live=false;}
 static ULONGLONG lastPoll=0;static std::string lastRequest;
 if(GetTickCount64()-lastPoll>150){lastPoll=GetTickCount64();std::ifstream request(root/L"NFSMW_RTX.request");std::string value;std::getline(request,value);if(!value.empty()&&value!=lastRequest){lastRequest=value;
  if(value.rfind("reload:",0)==0){runtime->reload_effect_next_frame("NFSMW_RTX.fx");log("Effect reload requested");}
  else if(value.rfind("motion-off:",0)==0){setBool(runtime,"MotionReprojection",false);log("Motion reprojection disabled");}
  else if(value.rfind("ray-debug:",0)==0){setInt(runtime,"EffectsDebug",5);live=true;log("Reflection radiance availability diagnostic");}
  else if(value.rfind("material-debug:",0)==0){setInt(runtime,"EffectsDebug",4);live=true;log("Material mask diagnostic");}
  else if(value.rfind("shadow-only:",0)==0){setFloat(runtime,"Strength",.65f);setFloat(runtime,"ReflectionStrength",0);setFloat(runtime,"IndirectStrength",0);live=true;log("Diagnostic: shadows only");}
  else if(value.rfind("reflections-only:",0)==0){setFloat(runtime,"Strength",0);setFloat(runtime,"ReflectionStrength",.45f);setFloat(runtime,"IndirectStrength",0);live=true;log("Diagnostic: reflections only");}
  else if(value.rfind("indirect-only:",0)==0){setFloat(runtime,"Strength",0);setFloat(runtime,"ReflectionStrength",0);setFloat(runtime,"IndirectStrength",.35f);live=true;log("Diagnostic: indirect only");}
  else{live=value.rfind("off:",0)!=0;if(value.rfind("all:",0)==0){setInt(runtime,"EffectsDebug",0);setFloat(runtime,"Strength",.65f);setFloat(runtime,"ReflectionStrength",.45f);setFloat(runtime,"IndirectStrength",.35f);setBool(runtime,"MotionReprojection",true);}log(std::string("GPU RTX ")+(live?"enabled":"disabled"));}
 }}
 bool materialReady=materialBuffers.available();setBool(runtime,"MaterialReady",materialReady);
 {auto uniform=runtime->find_uniform_variable("NFSMW_RTX.fx","MaterialUVScale");if(uniform.handle){float scale[2]{materialBuffers.scaleX(),materialBuffers.scaleY()};runtime->set_uniform_value_float(uniform,scale,2);}}
 if(materialReady){api::resource_view normal{(uint64_t)(uintptr_t)materialBuffers.view(0)},specular{(uint64_t)(uintptr_t)materialBuffers.view(1)};runtime->update_texture_bindings("RTX_MATERIAL_NORMAL",normal,normal);runtime->update_texture_bindings("RTX_MATERIAL_SPECULAR",specular,specular);}
 {static int previous=-1;if(int(materialReady)!=previous){previous=int(materialReady);log(std::string("Material buffers ")+(materialReady?"captured: roughness RT0.a, specular RT2.rgb":"unavailable: additional reflections suppressed"));}}
 {static std::string previous;std::string summary="RT light sources: sun="+std::to_string(transport&&transport->hasSun())+" headlights="+std::to_string(transport&&transport->hasHeadlight())+" points="+std::to_string(transport?transport->points():0);if(summary!=previous){previous=summary;log(summary);}}
 if(transport)transport->clearLights();
 materialBuffers.endFrame();
 bool depthReady=false;auto depthVariable=runtime->find_texture_variable("NFSMW_RTX.fx","CurrentDepth");
 if(depthVariable.handle){api::resource_view view{};runtime->get_texture_binding(depthVariable,&view,nullptr);if(view.handle){auto*srv=(ID3D11ShaderResourceView*)(uintptr_t)view.handle;ComPtr<ID3D11Resource>resource;srv->GetResource(&resource);ComPtr<ID3D11Texture2D>texture;if(SUCCEEDED(resource.As(&texture))){D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);depthReady=desc.Width>=screenWidth*.95f&&desc.Height>=screenHeight*.95f;}}}
 {static bool previous=false;if(depthReady!=previous){previous=depthReady;log(std::string("Current frame depth ")+(depthReady?"available":"unavailable"));}}
 setBool(runtime,"DepthReady",depthReady);
 setBool(runtime,"CameraReady",transport&&transport->hasActiveCamera()&&cameraThisFrame);cameraThisFrame=false;
 setBool(runtime,"CaptureReference",captureReference);setBool(runtime,"NewResult",newResult);
 setBool(runtime,"RTXReady",live&&techniqueEnabled&&!failed&&newResult);
 if(live&&techniqueEnabled&&!failed&&transport&&transport->state()>=1&&phase==Phase::idle){ComPtr<ID3D11Device>device;ctx->GetDevice(&device);if(makeBuffers(device.Get())){captureDraws=nonMaterialDraws=budgetRejectedDraws=0;requestedVertices=rejectedVertices=0;firstDraw=true;captureTime=GetTickCount64();internalCall=true;transport->clearGeometry();internalCall=false;phase=Phase::capture;}}

}
static void finishEffects(api::effect_runtime*,api::command_list*list,api::resource_view,api::resource_view){
 if(list->get_device()!=gameDevice||!transport||failed)return;
 try{transport->finishReads();}catch(const std::exception&e){log(std::string("GPU read fence failed: ")+e.what());failed=true;live=false;}
}
static void destroyDevice(api::device*device){
 if(device!=gameDevice)return;live=false;phase=Phase::idle;internalCall=true;transport.reset();materialBuffers.reset();streamBuffer.Reset();statisticsQuery.Reset();{std::lock_guard<std::mutex> lock(shaderMutex);shaders.clear();cameraBindings.clear();lightBindings.clear();vehicleMaterialShaders.clear();}internalCall=false;gameDevice=nullptr;
}
extern "C" __declspec(dllexport) const char *NAME="NFSMW continuous hardware DXR";
extern "C" __declspec(dllexport) const char *DESCRIPTION="GPU shared geometry and double buffered DXR results. Experimental camera/materials. File commands support safe per-effect diagnostics.";
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
 if(reason==DLL_PROCESS_ATTACH){
  wchar_t host[MAX_PATH]{};GetModuleFileNameW(nullptr,host,MAX_PATH);
  if(_wcsicmp(fs::path(host).filename().c_str(),L"NFS13.exe"))return FALSE;
  wchar_t path[MAX_PATH]{};GetModuleFileNameW(module,path,MAX_PATH);root=fs::path(path).parent_path();
  if(!reshade::register_addon(module))return FALSE;
  reshade::register_event<reshade::addon_event::init_pipeline>(initPipeline);
  reshade::register_event<reshade::addon_event::destroy_pipeline>(destroyPipeline);
  reshade::register_event<reshade::addon_event::draw>(draw);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(renderTargetTransition);
  reshade::register_event<reshade::addon_event::draw_indexed>(drawIndexed);
  reshade::register_event<reshade::addon_event::reshade_begin_effects>(beginEffects);
  reshade::register_event<reshade::addon_event::destroy_device>(destroyDevice);
  reshade::register_event<reshade::addon_event::reshade_finish_effects>(finishEffects);
  log("Loaded RTX v19 valid-triangle compaction, full geometry and no BLAS refit. Shared GPU geometry/results, double buffered and fence synchronized. Disabled until requested.");
 }else if(reason==DLL_PROCESS_DETACH){reshade::unregister_addon(module);}
 return TRUE;
}


