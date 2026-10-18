#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include "capture_limits.h"
namespace rtxgpu {
constexpr UINT Magic=0x41585452,Width=1920,Height=1080,Pixels=Width*Height,Planes=5,MaxVertices=RTX_CAPTURE_MAX_VERTICES,ClipWidth=1024,ClipHeight=(MaxVertices+80+ClipWidth-1)/ClipWidth;
static_assert(ClipHeight<=8192,"FL10 texture limit");
static_assert(MaxVertices%ClipWidth+80<=ClipWidth,"Metadata must fit one row");
constexpr UINT JobSlots=64;
struct CaptureJob{volatile LONG serial;UINT vertices,draws;float aspect;};
static_assert(sizeof(CaptureJob)==16,"Job ABI");
struct Shared {
 UINT magic,version,parentPID,adapterLow;LONG adapterHigh;
 volatile LONG state,activeSlot,readySlot,inputSerial,readySerial,consumed[2];
 UINT vertexCount,draws,frames,triangles;float aspect,gpuMs,totalMs;
 char adapter[128],error[256];
 float convertMs,buildMs,traceMs;UINT sourceVertices,validVertices,compactMode;
 CaptureJob jobs[JobSlots];
};
static_assert(sizeof(Shared)==1508,"x86/x64 GPU IPC ABI");
inline std::wstring name(UINT pid,const std::wstring&suffix){return L"Local\\NFSMW_RTX_GPU_"+std::to_wstring(pid)+L"_"+suffix;}
inline std::wstring outputName(UINT pid,UINT slot,UINT plane){return name(pid,L"out"+std::to_wstring(slot)+L"_"+std::to_wstring(plane));}
}

