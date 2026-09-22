#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <string.h>
#define SH_BUILD 1
#include "scripthook.h"
#define FRAME_CB_MAX 16
/* TU25 Ai::SpawningManagerUpdate; no hook is installed until registration. */
#define FRAME_RVA 0x0BF42B60u
#define STOLEN 6
#define MAX_PEERS 2048
static ShFrameFn_t volatile g_frameCb[FRAME_CB_MAX];
static void *g_frameCbUser[FRAME_CB_MAX];
static DWORD frameTls = TLS_OUT_OF_INDEXES;
static volatile LONG installing;
static unsigned char *frameStub;
static HANDLE peers[MAX_PEERS];
static int peerCount;
extern void *ShAllocNear(uint64_t target);
static const unsigned char expected[] = {
  0x40,0x57,0x48,0x83,0xec,0x50,0x48,0x89,0xcf,0x48,0x8b,0x49,0x08,0x48,0x85,0xc9,
  0x0f,0x84,0x19,0x01,0x00,0x00,0x48,0x89,0x5c,0x24,0x60,0xe8,0x80,0x79,0x06,0xf5
};
static void RunFrameCallbacks(void) {
    int i;
    for (i = 0; i < FRAME_CB_MAX; i++) {
        ShFrameFn_t fn = g_frameCb[i];
        if (fn) fn(g_frameCbUser[i]);
    }
}
static void FrameDispatch(void) {
    DWORD error = GetLastError();
    if (!TlsGetValue(frameTls) && TlsSetValue(frameTls,(void *)1)) {
        RunFrameCallbacks();
        TlsSetValue(frameTls,NULL);
    }
    SetLastError(error);
}
static int ReadLocal(uintptr_t address,void *out,SIZE_T size) {
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),(void *)address,out,size,&got) && got==size;
}
static int ValidSite(uintptr_t *site) {
    uintptr_t base=(uintptr_t)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    unsigned char bytes[sizeof expected];
    if(!base || !ReadLocal(base,&dos,sizeof dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
       dos.e_lfanew<=0 || dos.e_lfanew>0x100000 || !ReadLocal(base+dos.e_lfanew,&nt,sizeof nt) ||
       nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
       nt.FileHeader.TimeDateStamp!=0x6A99768Au || nt.OptionalHeader.SizeOfImage!=0x185BA000u) return 0;
    *site=base+FRAME_RVA;
    return ReadLocal(*site,bytes,sizeof bytes) && !memcmp(bytes,expected,sizeof bytes);
}
static void ClosePeers(void) {
    int i;
    for(i=0;i<peerCount;i++) { if(peers[i]) CloseHandle(peers[i]); peers[i]=NULL; }
    peerCount=0;
}
static int CollectPeers(void) {
    THREADENTRY32 te;
    HANDLE snapshot;
    DWORD pid=GetCurrentProcessId(),self=GetCurrentThreadId();
    int ok=1;
    snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE)return 0;
    te.dwSize=sizeof te;
    if(!Thread32First(snapshot,&te)) { CloseHandle(snapshot);return 0; }
    do {
        HANDLE h;
        if(te.th32OwnerProcessID!=pid || te.th32ThreadID==self)continue;
        if(peerCount==MAX_PEERS) { ok=0;break; }
        h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|SYNCHRONIZE,FALSE,te.th32ThreadID);
        if(!h) { if(GetLastError()==ERROR_INVALID_PARAMETER)continue; ok=0;break; }
        peers[peerCount++]=h;
    } while(Thread32Next(snapshot,&te));
    if(ok && GetLastError()!=ERROR_NO_MORE_FILES)ok=0;
    CloseHandle(snapshot);
    if(!ok)ClosePeers();
    return ok;
}
static int PatchSite(uintptr_t site,unsigned char *stub) {
    int i,suspended=0,ok=1,patched=0;
    DWORD old=0,ignored;
    CONTEXT ctx;
    unsigned char patch[STOLEN]={0xE9,0,0,0,0,0x90};
    int64_t rel=(int64_t)(uintptr_t)stub-(int64_t)(site+5);
    if(rel<INT32_MIN || rel>INT32_MAX || !CollectPeers())return 0;
    { int32_t r=(int32_t)rel; memcpy(patch+1,&r,4); }
    for(i=0;i<peerCount;i++) {
        if(SuspendThread(peers[i])==(DWORD)-1) { ok=0;break; }
        suspended++;
        memset(&ctx,0,sizeof ctx);ctx.ContextFlags=CONTEXT_CONTROL;
        if(!GetThreadContext(peers[i],&ctx) || (ctx.Rip>site && ctx.Rip<site+STOLEN)) { ok=0;break; }
    }
    if(ok && memcmp((void *)site,expected,sizeof expected))ok=0;
    if(ok && VirtualProtect((void *)site,STOLEN,PAGE_EXECUTE_READWRITE,&old)) {
        memcpy((void *)site,patch,STOLEN);
        FlushInstructionCache(GetCurrentProcess(),(void *)site,STOLEN);
        patched=1;
        VirtualProtect((void *)site,STOLEN,old,&ignored);
    }
    /* Resume exactly the one suspend owned here before closing any handles. */
    for(i=0;i<suspended;i++) {
        while(ResumeThread(peers[i])==(DWORD)-1 && WaitForSingleObject(peers[i],0)!=WAIT_OBJECT_0) Sleep(1);
    }
    ClosePeers();
    return patched;
}
static const unsigned char SAVE[] = {0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x48,0x89,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x28,0x4c,0x89,0x44,0x24,0x30,0x4c,0x89,0x4c,0x24,0x38,0x4c,0x89,0x54,0x24,0x40,0x4c,0x89,0x5c,0x24,0x48,0x48,0x89,0x44,0x24,0x50,0x0f,0x11,0x44,0x24,0x60,0x0f,0x11,0x4c,0x24,0x70,0x0f,0x11,0x94,0x24,0x80,0x00,0x00,0x00,0x0f,0x11,0x9c,0x24,0x90,0x00,0x00,0x00,0x0f,0x11,0xa4,0x24,0xa0,0x00,0x00,0x00,0x0f,0x11,0xac,0x24,0xb0,0x00,0x00,0x00,0x48,0x8b,0x4c,0x24,0x20};
static const unsigned char REST[] = {0x0f,0x10,0xac,0x24,0xb0,0x00,0x00,0x00,0x0f,0x10,0xa4,0x24,0xa0,0x00,0x00,0x00,0x0f,0x10,0x9c,0x24,0x90,0x00,0x00,0x00,0x0f,0x10,0x94,0x24,0x80,0x00,0x00,0x00,0x0f,0x10,0x4c,0x24,0x70,0x0f,0x10,0x44,0x24,0x60,0x48,0x8b,0x44,0x24,0x50,0x4c,0x8b,0x5c,0x24,0x48,0x4c,0x8b,0x54,0x24,0x40,0x4c,0x8b,0x4c,0x24,0x38,0x4c,0x8b,0x44,0x24,0x30,0x48,0x8b,0x54,0x24,0x28,0x48,0x8b,0x4c,0x24,0x20,0x48,0x81,0xc4,0xd0,0x00,0x00,0x00};

static int EnsureFrameHook(void) {
    uintptr_t site=0;
    unsigned char *stub;
    size_t n=0;
    uint64_t address;
    int ok=0;
    if(InterlockedCompareExchange(&installing,1,0))return 0;
    if(frameStub) { ok=1;goto done; }
    if(!ValidSite(&site))goto done;
    if(frameTls==TLS_OUT_OF_INDEXES) frameTls=TlsAlloc();
    if(frameTls==TLS_OUT_OF_INDEXES)goto done;
    stub=ShAllocNear(site);
    if(!stub)goto done;
    stub[n++]=0x9C;
    memcpy(stub+n,SAVE,sizeof SAVE); n+=sizeof SAVE;
    stub[n++]=0x48;stub[n++]=0xB8;
    address=(uint64_t)(uintptr_t)FrameDispatch;memcpy(stub+n,&address,8);n+=8;
    stub[n++]=0xFF;stub[n++]=0xD0;
    memcpy(stub+n,REST,sizeof REST);n+=sizeof REST;
    stub[n++]=0x9D;
    memcpy(stub+n,expected,STOLEN);n+=STOLEN;
    stub[n++]=0xFF;stub[n++]=0x25;
    memset(stub+n,0,4);n+=4;
    address=site+STOLEN;memcpy(stub+n,&address,8);n+=8;
    if(!FlushInstructionCache(GetCurrentProcess(),stub,n) || !PatchSite(site,stub)) {
        VirtualFree(stub,0,MEM_RELEASE);goto done;
    }
    frameStub=stub;ok=1;
done:
    InterlockedExchange(&installing,0);
    return ok;
}
SH_API int ShRegisterFrameCallback(ShFrameFn_t fn, void *user) {
    int i;
    if (!fn) return 0;
    if (!EnsureFrameHook()) return 0;
    for (i = 0; i < FRAME_CB_MAX; i++) {
        if (g_frameCb[i]) continue;
        /* Publish user data first; unregister retains the historical non-fenced semantics. */
        g_frameCbUser[i] = user;
        g_frameCb[i] = fn;
        return 1;
    }
    return 0;
}
SH_API void ShUnregisterFrameCallback(ShFrameFn_t fn) {
    int i;
    for (i = 0; i < FRAME_CB_MAX; i++)
        if (g_frameCb[i] == fn) g_frameCb[i] = NULL;
}
