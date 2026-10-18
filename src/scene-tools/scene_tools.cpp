#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <array>
#include <algorithm>
#include <cstdint>
#ifndef SCENE_TOOLS_TEST
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#endif
namespace scene {
struct alignas(16) Vector {float x,y,z,w;};
static_assert(sizeof(Vector)==16,"Native camera input alignment");
static uintptr_t gameBase=0;
static std::filesystem::path folder;
static volatile LONG timeFreeze=0,timePending=0;
static float requestedTime=43200,originalTime=0;
static volatile uintptr_t clockObject=0;
static bool haveOriginalTime=false;
static bool cameraRequested=false,cameraInitialized=false,cameraAvailable=false;
static bool timeInstalled=false,cameraInstalled=false;
static float speed=12,yaw=0,pitch=0,hour=12;
static Vector position{};
static float mouseSensitivity=.0025f;
static bool mouseLook=true,overlayOpen=false,mouseCaptured=false;
static POINT mouseCenter{},savedCursor{};
static HWND mouseWindow=nullptr;
static void releaseMouse(){
 if(!mouseCaptured)return;mouseCaptured=false;ShowCursor(TRUE);if(mouseWindow&&GetForegroundWindow()==mouseWindow)SetCursorPos(savedCursor.x,savedCursor.y);mouseWindow=nullptr;
}
static void updateLook(float horizontal,float vertical){yaw-=horizontal;pitch=std::clamp(pitch+vertical,-1.5f,1.5f);}
static void moveCamera(float forward,float side,float vertical,float step){
 position.x+=(sinf(yaw)*cosf(pitch)*forward-cosf(yaw)*side)*step;
 position.y+=(sinf(pitch)*forward+vertical)*step;
 position.z+=(cosf(yaw)*cosf(pitch)*forward+sinf(yaw)*side)*step;
}
static void updateMouse(HWND window){
 bool active=cameraRequested&&cameraInitialized&&mouseLook&&!overlayOpen&&window&&GetForegroundWindow()==window&&!IsIconic(window);
 if(!active){releaseMouse();return;}
 RECT client{};if(!GetClientRect(window,&client)||client.right<=client.left||client.bottom<=client.top){releaseMouse();return;}
 POINT center{(client.left+client.right)/2,(client.top+client.bottom)/2};if(!ClientToScreen(window,&center)){releaseMouse();return;}
 POINT cursor{};if(!GetCursorPos(&cursor))return;
 if(!mouseCaptured){savedCursor=cursor;mouseWindow=window;mouseCaptured=true;ShowCursor(FALSE);}
 else if(center.x==mouseCenter.x&&center.y==mouseCenter.y){
  float dx=float(cursor.x-mouseCenter.x),dy=float(cursor.y-mouseCenter.y);
  updateLook(dx*mouseSensitivity,-dy*mouseSensitivity);
 }
 mouseCenter=center;SetCursorPos(center.x,center.y);
}

static std::array<unsigned char,0x190> savedCamera{},ownedCamera{};
static bool haveCameraBackup=false,haveOwnedCamera=false;
static uintptr_t currentCamera=0;
static ULONGLONG lastFrame=0;
static unsigned cameraReports=0;
static unsigned char originalTimeCode[8]{},originalCameraCall[5]{};
using CameraSetter=void(__thiscall*)(void*,const Vector*,const Vector*,const Vector*);
using FrustumGetter=void(__thiscall*)(void*,void*);
static CameraSetter setCamera=nullptr;
static FrustumGetter getFrustum=nullptr;
static void log(const char*message){if(!folder.empty())std::ofstream(folder/L"NFSMW_SceneTools.log",std::ios::app)<<message<<"\n";}
static uintptr_t address(uintptr_t preferred){return gameBase+preferred-0x400000;}
static bool readable(const void*p,size_t bytes){MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(p,&info,sizeof(info)))return false;return info.State==MEM_COMMIT&&!(info.Protect&(PAGE_NOACCESS|PAGE_GUARD))&&uintptr_t(p)+bytes<=uintptr_t(info.BaseAddress)+info.RegionSize;}
static bool finite(Vector p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&fabsf(p.x)<1e6f&&fabsf(p.y)<1e6f&&fabsf(p.z)<1e6f;}
static float wrapTime(float seconds){return seconds-floorf(seconds/86400.f)*86400.f;}
static void requestTime(float seconds,bool freeze){requestedTime=wrapTime(seconds);InterlockedExchange(&timeFreeze,freeze?1:0);InterlockedExchange(&timePending,1);char text[120];sprintf_s(text,"Clock request %.3f seconds, freeze=%d",requestedTime,freeze?1:0);log(text);}
static bool readTime(float&value){auto*p=reinterpret_cast<const float*>(clockObject+0x2a84);if(!clockObject||!readable(p,4))return false;value=*p;return std::isfinite(value)&&value>=0&&value<=86401;}
// Hook replaces exactly the native movss [esi+2A84],xmm0. The next JE uses
// flags from before this instruction: preserve them, along with all GPRs.
static __declspec(naked) void clockHook(){__asm{
 pushfd
 mov dword ptr [clockObject],esi
 cmp dword ptr [timeFreeze],0
 jne overrideTime
 cmp dword ptr [timePending],0
 je keepTime
 overrideTime:
 movss xmm0,dword ptr [requestedTime]
 mov dword ptr [timePending],0
 keepTime:
 movss dword ptr [esi+0x2a84],xmm0
 popfd
 ret
}}
static void applyCamera(void*object){
 if(!readable(object,0x190))return;
 auto*bytes=static_cast<unsigned char*>(object);
 if(!cameraRequested){
  if(cameraInitialized&&haveCameraBackup&&currentCamera==uintptr_t(object)){memcpy(bytes,savedCamera.data(),savedCamera.size());log("Freecam restored native camera");}
  cameraInitialized=false;haveCameraBackup=haveOwnedCamera=false;return;
 }
 if(!setCamera)return;
 if(!cameraInitialized){
  Vector initial=*reinterpret_cast<Vector*>(bytes+0x130);
  Vector back=*reinterpret_cast<Vector*>(bytes+0x100);
  float length=sqrtf(back.x*back.x+back.y*back.y+back.z*back.z);
  if(!finite(initial)||!finite(back)||length<.5f||length>1.5f){log("Freecam rejected invalid native camera vectors");cameraRequested=false;return;}
  position=initial;yaw=atan2f(-back.x,-back.z);pitch=asinf(std::clamp(-back.y/length,-1.f,1.f));
  memcpy(savedCamera.data(),bytes,savedCamera.size());haveCameraBackup=true;haveOwnedCamera=false;currentCamera=uintptr_t(object);cameraInitialized=true;log("Freecam initialized from native camera");
 }
 if(currentCamera!=uintptr_t(object)){cameraRequested=false;cameraInitialized=false;log("Freecam disabled: camera instance changed");return;}
 if(!haveOwnedCamera||memcmp(bytes,ownedCamera.data(),ownedCamera.size())!=0){memcpy(savedCamera.data(),bytes,savedCamera.size());haveCameraBackup=true;}
 Vector target{position.x+sinf(yaw)*cosf(pitch),position.y+sinf(pitch),position.z+cosf(yaw)*cosf(pitch),1};
 Vector up{0,1,0,0};position.w=1;
 // Native LookAt takes (position, up, target), and updates view, inverse and VP.
 setCamera(object,&position,&up,&target);
 memcpy(ownedCamera.data(),bytes,ownedCamera.size());haveOwnedCamera=true;
}
static void __fastcall cameraHook(void*object,void*,void*out){
 if(!cameraAvailable){cameraAvailable=true;log("Native main camera reached; freecam available");}
 applyCamera(object);getFrustum(object,out);
 if(cameraRequested&&cameraReports++%300==0){char text[160];sprintf_s(text,"Freecam position %.3f %.3f %.3f yaw %.3f pitch %.3f",position.x,position.y,position.z,yaw,pitch);log(text);}
}
static bool writeBranch(void*site,void*target,unsigned char opcode,size_t bytes){
 DWORD old=0;if(!VirtualProtect(site,bytes,PAGE_EXECUTE_READWRITE,&old))return false;
 unsigned char patch[8]{};memset(patch,0x90,bytes);patch[0]=opcode;int32_t displacement=int32_t(uintptr_t(target)-uintptr_t(site)-5);memcpy(patch+1,&displacement,4);memcpy(site,patch,bytes);
 FlushInstructionCache(GetCurrentProcess(),site,bytes);DWORD unused;VirtualProtect(site,bytes,old,&unused);return true;
}
static bool initialize(){
 gameBase=uintptr_t(GetModuleHandleW(nullptr));
 auto*dos=reinterpret_cast<IMAGE_DOS_HEADER*>(gameBase);auto*nt=reinterpret_cast<IMAGE_NT_HEADERS*>(gameBase+dos->e_lfanew);
 if(nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386){log("Unsupported architecture");return false;}
 const unsigned char timeExpected[]{0xf3,0x0f,0x11,0x86,0x84,0x2a,0,0};
 auto*timeSite=reinterpret_cast<void*>(address(0x8b4c28));
 if(readable(timeSite,8)&&memcmp(timeSite,timeExpected,8)==0){memcpy(originalTimeCode,timeSite,8);timeInstalled=writeBranch(timeSite,reinterpret_cast<void*>(&clockHook),0xe8,8);}
 else log("Clock hook signature mismatch; clock control disabled");
 auto*cameraSite=reinterpret_cast<unsigned char*>(address(0x8c818e));
 const unsigned char setterExpected[]{0x55,0x8b,0xec,0x83,0xe4,0xf0};
 if(readable(cameraSite,5)&&cameraSite[0]==0xe8&&uintptr_t(cameraSite)+5+*reinterpret_cast<int32_t*>(cameraSite+1)==address(0xa63ed0)&&readable(reinterpret_cast<void*>(address(0xa639c0)),6)&&memcmp(reinterpret_cast<void*>(address(0xa639c0)),setterExpected,6)==0){
  setCamera=reinterpret_cast<CameraSetter>(address(0xa639c0));getFrustum=reinterpret_cast<FrustumGetter>(address(0xa63ed0));memcpy(originalCameraCall,cameraSite,5);cameraInstalled=writeBranch(cameraSite,reinterpret_cast<void*>(&cameraHook),0xe8,5);
 }else log("Camera hook signature mismatch; freecam disabled");
 log(timeInstalled?"Clock hook installed":"Clock unavailable");log(cameraInstalled?"Camera hook installed":"Camera unavailable");return timeInstalled||cameraInstalled;
}
static void restoreBranch(uintptr_t site,void*target,const unsigned char*original,size_t bytes){
 auto*p=reinterpret_cast<unsigned char*>(site);if(!readable(p,bytes)||p[0]!=0xe8||uintptr_t(p)+5+*reinterpret_cast<int32_t*>(p+1)!=uintptr_t(target))return;
 DWORD old;if(VirtualProtect(p,bytes,PAGE_EXECUTE_READWRITE,&old)){memcpy(p,original,bytes);FlushInstructionCache(GetCurrentProcess(),p,bytes);DWORD unused;VirtualProtect(p,bytes,old,&unused);}
}
static void shutdown(){
 releaseMouse();
 InterlockedExchange(&timeFreeze,0);InterlockedExchange(&timePending,0);cameraRequested=false;
 if(haveCameraBackup&&haveOwnedCamera&&currentCamera&&readable(reinterpret_cast<void*>(currentCamera),0x190)&&memcmp(reinterpret_cast<void*>(currentCamera),ownedCamera.data(),0x190)==0)memcpy(reinterpret_cast<void*>(currentCamera),savedCamera.data(),0x190);
 if(timeInstalled)restoreBranch(address(0x8b4c28),reinterpret_cast<void*>(&clockHook),originalTimeCode,8);
 if(cameraInstalled)restoreBranch(address(0x8c818e),reinterpret_cast<void*>(&cameraHook),originalCameraCall,5);
}
#ifndef SCENE_TOOLS_TEST
static void tick(reshade::api::effect_runtime*r,reshade::api::command_list*,reshade::api::resource_view,reshade::api::resource_view){
 float current=0;if(readTime(current)&&!haveOriginalTime){originalTime=current;haveOriginalTime=true;char text[120];sprintf_s(text,"Clock available: %.3f seconds = %.3f hours",current,current/3600);log(text);}
 if(cameraInstalled&&r->is_key_pressed(VK_F6)){cameraRequested=!cameraRequested;log(cameraRequested?"Freecam requested":"Freecam exit requested");}
 ULONGLONG now=GetTickCount64();float dt=lastFrame?std::clamp(float(now-lastFrame)/1000,0.f,.1f):0;lastFrame=now;
 updateMouse(static_cast<HWND>(r->get_hwnd()));
 if(cameraRequested&&!overlayOpen&&GetForegroundWindow()==r->get_hwnd()){
  r->block_input_next_frame();
  float step=speed*dt*(r->is_key_down(VK_SHIFT)?4.f:1.f)*(r->is_key_down(VK_CONTROL)?.15f:1.f);
  updateLook((r->is_key_down(VK_RIGHT)-r->is_key_down(VK_LEFT))*dt*1.2f,(r->is_key_down(VK_UP)-r->is_key_down(VK_DOWN))*dt*1.2f);
  float forward=float(r->is_key_down('W'))-float(r->is_key_down('S')),side=float(r->is_key_down('D'))-float(r->is_key_down('A'));
  moveCamera(forward,side,float(r->is_key_down('E'))-float(r->is_key_down('Q')),step);
 }
}
static bool overlayChanged(reshade::api::effect_runtime*,bool open,reshade::api::input_source){overlayOpen=open;if(open)releaseMouse();return false;}
static void panel(reshade::api::effect_runtime*){
 ImGui::TextUnformatted("Ferramentas de cena - Most Wanted 2012 (offline)");
 if(timeInstalled){
  float current;if(readTime(current))ImGui::Text("Horario atual: %02d:%02d",int(current/3600)%24,int(current/60)%60);else ImGui::TextUnformatted("Aguardando atualizacao do relogio na rua...");
  ImGui::SliderFloat("Horario desejado",&hour,0.f,23.99f,"%.2f h");
  bool freeze=timeFreeze!=0;if(ImGui::Checkbox("Congelar horario",&freeze)){if(freeze){if(readTime(current))requestTime(current,true);}else InterlockedExchange(&timeFreeze,0);}
  if(ImGui::Button("Aplicar horario"))requestTime(hour*3600,timeFreeze!=0);
  ImGui::SameLine();if(ImGui::Button("Meio-dia")){hour=12;requestTime(43200,true);}
  ImGui::SameLine();if(ImGui::Button("Noite")){hour=0;requestTime(0,true);}
  if(ImGui::Button("Ciclo normal")){InterlockedExchange(&timeFreeze,0);InterlockedExchange(&timePending,0);}
  if(haveOriginalTime){ImGui::SameLine();if(ImGui::Button("Restaurar horario inicial")){hour=originalTime/3600;requestTime(originalTime,false);}}
 }else ImGui::TextUnformatted("Horario indisponivel: assinatura do jogo nao reconhecida.");
 ImGui::Separator();
 if(cameraInstalled){ImGui::Checkbox("Camera livre (F6)",&cameraRequested);ImGui::SliderFloat("Velocidade da camera",&speed,.5f,80.f,"%.1f m/s");ImGui::Checkbox("Olhar com o mouse",&mouseLook);ImGui::SliderFloat("Sensibilidade do mouse",&mouseSensitivity,.0003f,.008f,"%.4f rad/pixel");ImGui::TextUnformatted("WASD: mover | Q/E: descer/subir | Mouse ou setas: olhar");ImGui::TextUnformatted("Shift: rapido | Ctrl: preciso | F6: voltar ao carro");if(!cameraAvailable)ImGui::TextUnformatted("Aguardando camera principal do modo de corrida...");}
 else ImGui::TextUnformatted("Freecam indisponivel: assinatura do jogo nao reconhecida.");
 ImGui::TextUnformatted("Feche o painel com Home para usar os controles.");
}
#endif
} // namespace scene
#ifndef SCENE_TOOLS_TEST
extern "C" __declspec(dllexport) const char*NAME="NFSMW Scene Tools";
extern "C" __declspec(dllexport) const char*DESCRIPTION="Offline native time of day and free camera controls; Home panel, F6 camera toggle.";
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
 if(reason==DLL_PROCESS_ATTACH){wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);if(_wcsicmp(std::filesystem::path(executable).filename().c_str(),L"NFS13.exe"))return FALSE;
  wchar_t path[MAX_PATH]{};GetModuleFileNameW(module,path,MAX_PATH);scene::folder=std::filesystem::path(path).parent_path();if(!reshade::register_addon(module))return FALSE;
  scene::log("Scene Tools v2 loaded: corrected lateral/yaw directions and mouse look");scene::initialize();reshade::register_overlay("Ferramentas da cena",scene::panel);reshade::register_event<reshade::addon_event::reshade_begin_effects>(scene::tick);reshade::register_event<reshade::addon_event::reshade_open_overlay>(scene::overlayChanged);
 }else if(reason==DLL_PROCESS_DETACH){scene::shutdown();reshade::unregister_addon(module);}return TRUE;
}
#endif

