#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>

// Dino Crisis Modern: an XInput filter between Classic REbirth and the
// system's XInput that adds camera-relative movement, run by tilt, modern
// triggers and face buttons, an authentic door skip and a modern camera.
// Settings: dc_modern.ini beside the DLL.
// REbirth's ddraw.dll imports XInputGetState from XINPUT9_1_0.dll. On PC this
// file is dropped into the game folder as xinput9_1_0.dll, which the loader
// finds before the system's; on the PS5 it is dc_modern.dll and ddraw.dll's
// import names it (install.py). Either way the real XInputGetState is loaded
// by full path from the system directory, never by name, so it cannot find
// itself.
static HMODULE self;
static SRWLOCK lock = SRWLOCK_INIT;
static char ini[MAX_PATH];
static bool configured, supported;
static DWORD configTime;
static int enabled=1, onPercent=75, offPercent=65;
static int modern=1, turnRate=4096*2, deadzone=8000, holdCamera=1;
static int triggers=1, triggerThreshold=77, faceButtons=1, doorSkip=1;
static int cameraMode=0, camDistance=1700, camHeight=1450, camSide=420,
           camLook=2600, camLookHeight=1250, camTurnRate=4096/2,
           camCollide=1, camRadius=200, camRiseWhenClose=500, camMinDistance=550;
static WORD runButton=XINPUT_GAMEPAD_X;
static LARGE_INTEGER frequency;
struct Pad {
    bool running=false, latched=false; DWORD packet=0; XINPUT_GAMEPAD last{};
    int camera=0, stickAngle=0; LARGE_INTEGER time{};
};
static Pad pads[4];
static bool camHeld, camOn;      // shoulder camera: holding the view / switched on

// Game state offsets, verified against the Dino Crisis.exe build checked in
// config(). Angles use 4096 units per turn; a yaw y faces (sin y, cos y) in
// world X/Z for both the camera and the actor.
constexpr uintptr_t ACTOR=0x1a48, CAMERA_EYE=0xd4, CAMERA_TARGET=0xe4;

static void config() {
    DWORD now=GetTickCount();
    if (configured && now-configTime<1000) return;
    if (!configured) {
        // dc_modern.ini beside the DLL, whatever the DLL is called.
        GetModuleFileNameA(self,ini,MAX_PATH);
        char* slash=strrchr(ini,'\\');
        if(slash && slash-ini+sizeof("\\dc_modern.ini")<=MAX_PATH) strcpy(slash,"\\dc_modern.ini");
        auto base=reinterpret_cast<unsigned char*>(GetModuleHandleA(nullptr));
        auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
        supported = base==reinterpret_cast<unsigned char*>(0x400000) &&
            nt->OptionalHeader.SizeOfImage>0x27b86c &&
            memcmp(base+0x1ee9d0,"\xa1\xc4\xbf\x67\x00\x53\x33\xdb",8)==0;
        QueryPerformanceFrequency(&frequency);
        configured=true;
    }
    enabled=GetPrivateProfileIntA("Stick","Enabled",1,ini);
    onPercent=GetPrivateProfileIntA("Stick","RunOnPercent",75,ini);
    offPercent=GetPrivateProfileIntA("Stick","RunOffPercent",65,ini);
    if(onPercent<35 || onPercent>100) onPercent=75;
    if(offPercent<25 || offPercent>=onPercent) offPercent=onPercent-10;
    runButton=static_cast<WORD>(GetPrivateProfileIntA("Stick","RunButton",0x4000,ini));
    modern=GetPrivateProfileIntA("Modern","Enabled",1,ini);
    int degrees=GetPrivateProfileIntA("Modern","TurnDegreesPerSecond",720,ini);
    if(degrees<90 || degrees>3600) degrees=720;
    turnRate=degrees*4096/360;
    int dead=GetPrivateProfileIntA("Modern","DeadzonePercent",25,ini);
    if(dead<5 || dead>60) dead=25;
    deadzone=32767*dead/100;
    holdCamera=GetPrivateProfileIntA("Modern","HoldCameraOnCut",1,ini);
    triggers=GetPrivateProfileIntA("Triggers","Enabled",1,ini);
    int pull=GetPrivateProfileIntA("Triggers","PullPercent",30,ini);
    if(pull<5 || pull>95) pull=30;
    triggerThreshold=255*pull/100;
    faceButtons=GetPrivateProfileIntA("Buttons","Enabled",1,ini);
    doorSkip=GetPrivateProfileIntA("Doors","Skip",1,ini);
    char mode[16];
    GetPrivateProfileStringA("Camera","Mode","Fixed",mode,sizeof(mode),ini);
    cameraMode=!lstrcmpiA(mode,"Modern");
    camDistance=GetPrivateProfileIntA("Camera","Distance",1700,ini);
    camHeight=GetPrivateProfileIntA("Camera","Height",1450,ini);
    camSide=GetPrivateProfileIntA("Camera","SideOffset",420,ini);
    camLook=GetPrivateProfileIntA("Camera","LookAhead",2600,ini);
    camLookHeight=GetPrivateProfileIntA("Camera","LookHeight",1250,ini);
    camTurnRate=GetPrivateProfileIntA("Camera","TurnDegreesPerSecond",180,ini)*4096/360;
    camCollide=GetPrivateProfileIntA("Camera","Collision",1,ini);
    camRadius=std::max(40,std::min(600,(int)GetPrivateProfileIntA("Camera","CollisionRadius",200,ini)));
    camRiseWhenClose=GetPrivateProfileIntA("Camera","RiseWhenClose",500,ini);
    camMinDistance=GetPrivateProfileIntA("Camera","MinDistance",550,ini);
    configTime=now;
}

static bool readable(uintptr_t p, size_t size) {
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m)) &&
        m.State==MEM_COMMIT && !(m.Protect&(PAGE_NOACCESS|PAGE_GUARD)) &&
        p+size>=p && p+size<=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
}
static uintptr_t gameState() {
    if(!supported) return 0;
    uintptr_t scratch=*reinterpret_cast<volatile uint32_t*>(0x67b868);
    if(!readable(scratch,4)) return 0;
    uintptr_t g=*reinterpret_cast<volatile uint32_t*>(scratch);
    return readable(g,0x1a90)?g:0;
}
// Returns the game state while Regina is in ordinary locomotion, else 0.
static uintptr_t gameplay() {
    uintptr_t g=gameState();
    if(!g) return 0;
    auto flags=*reinterpret_cast<volatile uint32_t*>(g+0x40);
    auto outer=*reinterpret_cast<volatile uint16_t*>(g+0x28);
    auto pauseFlags=*reinterpret_cast<volatile uint32_t*>(g+0x3c);
    auto mode=*reinterpret_cast<volatile uint8_t*>(g+ACTOR+0x3c);
    auto action=*reinterpret_cast<volatile uint8_t*>(g+ACTOR+0x3d);
    // Outer state 3 is gameplay; state 5 is inventory/UI. The pause
    // handler tests flag bank 2, bit 29 (g+0x3c), before updating actors.
    bool ok=outer==3 && !(pauseFlags&0x20000000) && !(flags&4) &&
        mode==1 && (action==0 || action==1 || action==5);
    return ok?g:0;
}

static int angle4096(double x, double y) {
    return static_cast<int>(std::lround(std::atan2(x,y)*(2048.0/3.14159265358979323846)))&0xfff;
}
static int delta4096(int to, int from) { return ((to-from+2048)&0xfff)-2048; }
static int cameraYaw(uintptr_t g) {
    auto eye=reinterpret_cast<volatile int16_t*>(g+CAMERA_EYE);
    auto target=reinterpret_cast<volatile int16_t*>(g+CAMERA_TARGET);
    return angle4096(target[0]-eye[0],target[2]-eye[2]);
}

// Modern triggers: L2 aims (the game's R1) and R2 fires while aiming (the
// game's Cross). Classic REbirth also reads L2 as Confirm/Fire, so L2 never
// reaches the game itself. R2 without aiming stays the game's Turn Around.
static void remapTriggers(XINPUT_GAMEPAD& pad) {
    if(!triggers) return;
    bool aimL2=pad.bLeftTrigger>triggerThreshold;
    bool aiming=aimL2 || (pad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER);
    pad.bLeftTrigger=0;
    if(aimL2) pad.wButtons|=XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if(aiming) {
        if(pad.bRightTrigger>triggerThreshold) pad.wButtons|=XINPUT_GAMEPAD_A;
        pad.bRightTrigger=0;
    }
}

// Door skip. Outer state 6 runs the room transition as a frame task whose
// context is at 0x678E48: phase, step, ... and at +0xF a frame counter.
// Phase 1 step 0 plays the door type's animation (table 0x657428), step 1
// fades out and step 2 waits for the next room, which the loader
// (0x4715C1) only starts 60 frames into the phase and flags itself when
// done. Skipping goes straight to the fade and starts the load at once.
constexpr uintptr_t OUTER_STATE=0x28, DOOR_TASK=0x678e48;
static void skipDoor(uintptr_t g) {
    if(!doorSkip || !g || *reinterpret_cast<volatile uint16_t*>(g+OUTER_STATE)!=6) return;
    auto task=reinterpret_cast<volatile int16_t*>(DOOR_TASK);
    if(task[0]!=1) return;
    if(task[1]==0) task[1]=1;
    // Step 1 fades the door scene out over 20 frames; finish that fade now
    // (gameState+0x6C mode 1 = to black, +0x6D running, +0x6E frames left).
    auto fadeMode=reinterpret_cast<volatile uint8_t*>(g+0x6c);
    auto fadeRunning=reinterpret_cast<volatile uint8_t*>(g+0x6d);
    auto fadeLeft=reinterpret_cast<volatile int8_t*>(g+0x6e);
    if(task[1]==1 || task[1]==2) {
        if(*fadeRunning && *fadeMode==1 && *fadeLeft>0) *fadeLeft=0;
    }
    auto frames=reinterpret_cast<volatile int8_t*>(DOOR_TASK+0xf);
    if(*frames>=0 && *frames<=60) *frames=61;
}

// Over-the-shoulder camera. UpdateSceneViewTransform (0x423313) runs the
// room's camera callback through the table at 0x645C1C (10 cdecl entries,
// indexed by context+0x70) and then builds the view from the angles at
// gameState+0xDC. The callback also switches the room's cameras, and with
// them the parts of the room that are drawn, so it must keep running: each
// entry is replaced by a hook that calls the original and then, while the
// shoulder camera is on, puts the eye (+0xD4) behind Regina's right shoulder
// and the target (+0xE4) ahead, with the distance and the angles from the
// game's own helpers. The right stick turns it; aiming swings it behind her.
// Movement stays relative to the camera, so it plays like RE4 Remake. The
// touchpad (on the PS5), R3 (on PC) or F12 toggles it.
constexpr uintptr_t CAM_EYE=0xd4, CAM_TARGET=0xe4, CAM_DISTANCE=0xf0, CAM_ANGLES=0xdc;
constexpr uintptr_t CAM_CALLBACKS=0x645c1c;
constexpr WORD XINPUT_GAMEPAD_GUIDE=0x0400;
constexpr int CAM_CALLBACK_COUNT=10;
typedef void (__cdecl *CameraCallback)(uintptr_t context);
typedef void (__cdecl *ViewAnglesFn)(volatile int16_t* target, volatile int16_t* eye, volatile int16_t* angles);
typedef int16_t (__cdecl *PointDistanceFn)(volatile int16_t* a, volatile int16_t* b);
static const auto computeViewAngles=reinterpret_cast<ViewAnglesFn>(0x5f0b90);
static const auto pointDistance=reinterpret_cast<PointDistanceFn>(0x421da7);
static CameraCallback cameraCallbacks[CAM_CALLBACK_COUNT];
static bool callbacksHooked;
static volatile bool camAiming;
static volatile int camYaw;
static LARGE_INTEGER camTime;
static WORD camPrevButtons;

// Camera collision, with the room's own wall rules. The protagonist's
// resolver (ResolveActorAgainstAreaBoundaryShapes, 0x468F60) walks the
// area's 16-byte shapes (general list and her band's list from the
// collision resource at gameState+0x154) and lets
// ResolveActorAgainstAreaCollisionShape (0x4699D2) correct a probe that only
// holds positions, a radius and shape scratch, nothing of the actor. The
// camera repeats that loop with its own probe, walking from Regina to the
// wanted eye in short steps as if she walked there, and stops where a wall
// holds it back.
struct CameraProbe {
    int16_t prevX, prevZ, x, z, dx, dz;
    int16_t quad[8];
    uint8_t scratch[0x2c-0x1c];
    uint16_t radius;                       // +0x2C
    uint8_t type, edges;                   // +0x2E, +0x2F
    uint8_t tail[0x20];
};
static_assert(offsetof(CameraProbe,radius)==0x2c,"probe layout");
typedef char (__cdecl *ShapeResolveFn)(CameraProbe* probe, const uint8_t* shape, char mode);
typedef char (__cdecl *ShapePathFn)(const int16_t* quad, CameraProbe* probe, uint8_t* out);
static const auto resolveShape=reinterpret_cast<ShapeResolveFn>(0x4699d2);
static const auto shapePath=reinterpret_cast<ShapePathFn>(0x4a278a);

// One step of the probe against every shape that applies to Regina.
static void probeStep(uintptr_t g, CameraProbe& p) {
    uintptr_t coll=*reinterpret_cast<volatile uint32_t*>(g+0x154);
    if(!coll || !readable(coll,0x110)) return;
    uintptr_t actor=g+ACTOR;
    uint8_t band=*reinterpret_cast<volatile uint8_t*>(actor+0x48);
    uint16_t actorFlags=*reinterpret_cast<volatile uint16_t*>(actor+0x140);
    uint8_t onLadder=*reinterpret_cast<volatile uint8_t*>(actor+0x27);
    auto shapeA=reinterpret_cast<const int8_t*>(0x6533b0);
    auto shapeB=reinterpret_cast<const int8_t*>(0x6533b7);
    auto shapeShift=reinterpret_cast<const uint8_t*>(0x6533be);
    unsigned header=band+1u;
    for(int list=0;list<2;list++) {
        unsigned h=list==0?0:header;
        if(list==1 && h>=0x10) break;
        unsigned count=*reinterpret_cast<const uint8_t*>(coll+1+h*0x10);
        auto rec=reinterpret_cast<const uint8_t*>(coll+0x110+*reinterpret_cast<const int32_t*>(coll+8+h*0x10));
        for(unsigned i=0;i<count;i++,rec+=16) {
            uint16_t flags=*reinterpret_cast<const uint16_t*>(rec+0xc);
            if(rec[0]>6 || (flags&0x8000) || (actorFlags&flags&1)) continue;
            if((flags&0x1000) && !onLadder) continue;
            if((flags&0x800) && onLadder) continue;
            if(rec[3] && (band<rec[2] || band>rec[3])) continue;
            int16_t x0=int16_t(*reinterpret_cast<const int16_t*>(rec+4)+*reinterpret_cast<const int16_t*>(rec+8)*shapeA[rec[0]]);
            int16_t z0=int16_t(*reinterpret_cast<const int16_t*>(rec+6)+*reinterpret_cast<const int16_t*>(rec+10)*shapeB[rec[0]]);
            uint16_t w=uint16_t(*reinterpret_cast<const int16_t*>(rec+8)<<shapeShift[rec[0]]);
            uint16_t d=uint16_t(*reinterpret_cast<const int16_t*>(rec+10)<<shapeShift[rec[0]]);
            int16_t quad[8]={x0,int16_t(z0+d),int16_t(x0+w),int16_t(z0+d),int16_t(x0+w),z0,x0,z0};
            uint32_t r=p.radius;
            bool inBounds=uint32_t(p.x+r-x0)<uint32_t(w)+r*2 && uint32_t(p.z+r-z0)<uint32_t(d)+r*2;
            uint8_t out[16];
            if(inBounds || shapePath(quad,&p,out)) resolveShape(&p,rec,0);
        }
    }
}
// Where the camera may stand on the way from (fromX, fromZ) to (toX, toZ).
static void collideCamera(uintptr_t g, double fromX, double fromZ, double& toX, double& toZ) {
    double lx=toX-fromX, lz=toZ-fromZ, len=std::sqrt(lx*lx+lz*lz);
    if(len<1) return;
    CameraProbe p{};
    p.radius=uint16_t(camRadius);
    double x=fromX, z=fromZ;
    int steps=int(len/120)+1;
    for(int i=1;i<=steps;i++) {
        double wantX=fromX+lx*i/steps, wantZ=fromZ+lz*i/steps;
        p.prevX=int16_t(x); p.prevZ=int16_t(z);
        p.x=int16_t(wantX); p.z=int16_t(wantZ);
        probeStep(g,p);
        double gotX=p.x, gotZ=p.z;
        // A wall beside the path only slides the probe along it; a wall
        // across it stops the probe from getting further from Regina.
        double stepX=gotX-x, stepZ=gotZ-z, along=(stepX*lx+stepZ*lz)/len;
        if(along<len/steps*0.3) break;
        x=gotX; z=gotZ;
    }
    toX=x; toZ=z;
}

static double camPull=1;                   // 0..1 of the wanted distance

static void placeShoulderCamera(uintptr_t g) {
    auto pos=reinterpret_cast<volatile int16_t*>(g+ACTOR+0x20);
    double a=camYaw*(3.14159265358979323846/2048.0), fx=std::sin(a), fz=std::cos(a);
    double rx=fz, rz=-fx;                  // screen right of a camera facing (fx, fz)
    bool aiming=camAiming;
    double dist=aiming?camDistance*0.55:camDistance, shoulder=aiming?camSide*1.2:camSide;
    auto eye=reinterpret_cast<volatile int16_t*>(g+CAM_EYE);
    auto target=reinterpret_cast<volatile int16_t*>(g+CAM_TARGET);
    auto clamp16=[](double v){ return int16_t(std::max(-32768.0,std::min(32767.0,v))); };
    // y grows downward: negative heights are above the floor
    double ex=pos[0]-fx*dist+rx*shoulder, ez=pos[2]-fz*dist+rz*shoulder;
    if(camCollide) {
        double cx=ex, cz=ez;
        collideCamera(g,pos[0],pos[2],cx,cz);
        double want=std::sqrt((ex-pos[0])*(ex-pos[0])+(ez-pos[2])*(ez-pos[2]));
        double got=std::sqrt((cx-pos[0])*(cx-pos[0])+(cz-pos[2])*(cz-pos[2]));
        // Never closer than MinDistance behind her, so her shoulder stays in view.
        double pull=want>1?std::max(got,std::min(want,double(camMinDistance)))/want:1;
        // In at once, so a wall is never seen from inside; out gently.
        camPull=pull<camPull?pull:camPull+(pull-camPull)*0.12;
        ex=pos[0]+(ex-pos[0])*camPull; ez=pos[2]+(ez-pos[2])*camPull;
    }
    // Pulled in by a wall, the camera also rises over her head, looking
    // ahead, so it never ends up inside her.
    double rise=(1-camPull)*camRiseWhenClose;
    eye[0]=clamp16(ex);
    eye[1]=clamp16(pos[1]-camHeight-rise);
    eye[2]=clamp16(ez);
    target[0]=clamp16(pos[0]+fx*camLook+rx*shoulder*0.5);
    target[1]=clamp16(pos[1]-camLookHeight);
    target[2]=clamp16(pos[2]+fz*camLook+rz*shoulder*0.5);
    *reinterpret_cast<volatile int16_t*>(g+CAM_DISTANCE)=pointDistance(target,eye);
    computeViewAngles(target,eye,reinterpret_cast<volatile int16_t*>(g+CAM_ANGLES));
}
// The room's meshes: 36 records of 0x88 bytes at gameState+0x7CE8. Three
// passes skip a mesh without flag bit 1 (visible) in byte +0x30: the view
// transform (ProcessSceneViewTransforms, which writes its translation +0x14),
// the renderer (RenderActiveRoomMeshesBySortMode, which also needs bit 0,
// active) and a second room pass (0x44D52F). Each fixed camera's scripts
// show and hide meshes (opcode 0x48) for its own view, so a free camera sees
// holes. While the shoulder camera is on, those tests are patched instead:
// the transform and the second pass test bit 0, and the renderer tests byte
// +0x2F, which a story event sets to hide a mesh. The game's own bits are
// never touched, so a camera cut while the shoulder camera is on cannot leave
// them wrong.
constexpr uintptr_t ROOM_MESHES=0x7ce8, ROOM_MESH_SIZE=0x88;
constexpr int ROOM_MESH_COUNT=36;
struct CodePatch { uintptr_t at; uint8_t size; uint8_t original[17], patched[17]; };
static const CodePatch roomPatches[]={
    // ProcessSceneViewTransforms, room-mesh loop: and ecx,2 -> and ecx,1.
    {0x44c3eb,3,{0x83,0xe1,0x02},{0x83,0xe1,0x01}},
    // RenderActiveRoomMeshesBySortMode: mov ecx,[ebp-10]; mov edx,[ecx+30];
    // and edx,2; test edx,edx; je 0x44D516 (next mesh) becomes mov ecx,[ebp-10];
    // cmp byte [ecx+2F],0; jne 0x44D516.
    {0x44cfc7,17,
     {0x8b,0x4d,0xf0,0x8b,0x51,0x30,0x83,0xe2,0x02,0x85,0xd2,0x0f,0x84,0x3e,0x05,0x00,0x00},
     {0x8b,0x4d,0xf0,0x80,0x79,0x2f,0x00,0x0f,0x85,0x42,0x05,0x00,0x00,0x90,0x90,0x90,0x90}},
    // The second room pass: and eax,2 -> and eax,1.
    {0x44d615,3,{0x83,0xe0,0x02},{0x83,0xe0,0x01}},
};
static bool wholeRoom;
static uint64_t meshesSorted;
static uintptr_t meshesState;
static uint8_t meshSort[ROOM_MESH_COUNT];

static void drawWholeRoom(bool on) {
    if(on==wholeRoom) return;
    for(const CodePatch& p : roomPatches)
        if(memcmp(reinterpret_cast<void*>(p.at),on?p.original:p.patched,p.size)) return;
    for(const CodePatch& p : roomPatches) {
        void* at=reinterpret_cast<void*>(p.at);
        DWORD old;
        if(!VirtualProtect(at,p.size,PAGE_EXECUTE_READWRITE,&old)) continue;
        memcpy(at,on?p.patched:p.original,p.size);
        VirtualProtect(at,p.size,old,&old);
        FlushInstructionCache(GetCurrentProcess(),at,p.size);
    }
    wholeRoom=on;
}

// Room meshes in sort mode 2 sit at one fixed depth each (+0x4A), layered by
// hand for the fixed cameras: from the shoulder a mesh behind a wall can sit
// in a nearer layer and paint over it. Modes 0 and 1 sort each polygon by its
// nearest or farthest vertex (the callbacks at 0x5F18B0 / 0x5F1840); the
// farthest keeps a floor polygon from painting over the feet standing on it.
static void showWholeRoom(uintptr_t g) {
    drawWholeRoom(true);
    if(meshesState!=g) { meshesSorted=0; meshesState=g; }
    for(int i=0;i<ROOM_MESH_COUNT;i++) {
        auto mesh=g+ROOM_MESHES+i*ROOM_MESH_SIZE;
        auto flags=*reinterpret_cast<volatile uint8_t*>(mesh+0x30);
        auto sort=reinterpret_cast<volatile uint8_t*>(mesh+0x48);
        if(*sort==2 && (flags&1)) {
            meshSort[i]=*sort; *sort=1; meshesSorted|=1ull<<i;
        }
    }
}
static void restoreRoom(uintptr_t g) {
    drawWholeRoom(false);
    if(meshesState!=g || !g) { meshesSorted=0; return; }
    for(int i=0;i<ROOM_MESH_COUNT;i++)
        if(meshesSorted&(1ull<<i))
            *reinterpret_cast<volatile uint8_t*>(g+ROOM_MESHES+i*ROOM_MESH_SIZE+0x48)=meshSort[i];
    meshesSorted=0;
}

static void __cdecl cameraCallbackHook(uintptr_t context) {
    uint8_t index=*reinterpret_cast<volatile uint8_t*>(context+0x70);
    if(index<CAM_CALLBACK_COUNT && cameraCallbacks[index]) cameraCallbacks[index](context);
    uintptr_t g=context-0xb4;
    if(camHeld) {
        showWholeRoom(g);
        placeShoulderCamera(g);
    } else restoreRoom(g);
}
static void hookCameraCallbacks() {
    if(callbacksHooked || !supported) return;
    callbacksHooked=true;
    auto table=reinterpret_cast<CameraCallback*>(CAM_CALLBACKS);
    for(int i=0;i<CAM_CALLBACK_COUNT;i++) {
        auto entry=reinterpret_cast<uintptr_t>(table[i]);
        if(entry<0x401000 || entry>=0x620000) return;   // not this build's table
    }
    DWORD old;
    if(!VirtualProtect(table,sizeof(CameraCallback)*CAM_CALLBACK_COUNT,PAGE_READWRITE,&old)) return;
    for(int i=0;i<CAM_CALLBACK_COUNT;i++) { cameraCallbacks[i]=table[i]; table[i]=cameraCallbackHook; }
    VirtualProtect(table,sizeof(CameraCallback)*CAM_CALLBACK_COUNT,old,&old);
}

// prospero-win's Wine answers the title menu's private process class
// (PW_TITLE_MENU_PROCESS_INFO, a 836-byte snapshot). There L3+R3 opens the
// title's menu, so R3 alone must not toggle the camera; Windows fails the
// query.
static bool titleMenuPresent() {
    typedef LONG (WINAPI *QueryFn)(HANDLE,ULONG,void*,ULONG,ULONG*);
    auto query=reinterpret_cast<QueryFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess")));
    static uint8_t snapshot[836];
    return query && query(GetCurrentProcess(),0x50574d01,snapshot,sizeof(snapshot),nullptr)>=0;
}
static bool toggleKeyPressed() {
    static bool down;
    bool now=(GetAsyncKeyState(VK_F12)&0x8000)!=0;
    if(now) {
        DWORD owner=0;
        GetWindowThreadProcessId(GetForegroundWindow(),&owner);
        now=owner==GetCurrentProcessId();
    }
    bool pressed=now && !down;
    down=now;
    return pressed;
}

static void shoulderCamera(uintptr_t g, XINPUT_GAMEPAD& pad) {
    hookCameraCallbacks();
    // prospero-win reports the DualSense touchpad as the guide button. On PC
    // the guide button belongs to Steam and the Game Bar, so R3 toggles there.
    static int onPs5=-1;
    if(onPs5<0) onPs5=titleMenuPresent();
    WORD pressed=pad.wButtons&~camPrevButtons;
    camPrevButtons=pad.wButtons;
    WORD toggles=onPs5?XINPUT_GAMEPAD_GUIDE:XINPUT_GAMEPAD_RIGHT_THUMB;
    if((pressed&toggles) || toggleKeyPressed()) {
        camOn=!camOn;
        WritePrivateProfileStringA("Camera","Mode",camOn?"Modern":"Fixed",ini);
        cameraMode=camOn;
    }
    camOn=cameraMode!=0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt=camTime.QuadPart?std::min(0.05,double(now.QuadPart-camTime.QuadPart)/frequency.QuadPart):0;
    camTime=now;
    // Controllable play: outer state 3, not paused, Regina's own mode 1
    // (any action, so a quick turn or a hit keeps the camera).
    bool play=false, aiming=false;
    if(g && supported) {
        auto action=*reinterpret_cast<volatile uint8_t*>(g+ACTOR+0x3d);
        auto mode=*reinterpret_cast<volatile uint8_t*>(g+ACTOR+0x3c);
        auto outer=*reinterpret_cast<volatile uint16_t*>(g+0x28);
        auto pauseFlags=*reinterpret_cast<volatile uint32_t*>(g+0x3c);
        auto flags=*reinterpret_cast<volatile uint32_t*>(g+0x40);
        play=outer==3 && mode==1 && !(pauseFlags&0x20000000) && !(flags&4);
        aiming=play && action==6;
    }
    if(!camOn || !play || !callbacksHooked) { camHeld=false; return; }

    int yaw=camYaw;
    auto yawNow=*reinterpret_cast<volatile uint16_t*>(g+ACTOR+0x2a)&0xfff;
    if(!camHeld) yaw=yawNow;               // start behind her
    if(aiming) {
        int d=delta4096(yawNow,yaw);
        int step=int(dt*4096*2);
        yaw=std::abs(d)<=step?yawNow:yaw+(d>0?step:-step);
    } else if(std::abs(pad.sThumbRX)>deadzone) {
        yaw+=int(dt*camTurnRate*pad.sThumbRX/32767.0);
    }
    camYaw=yaw&0xfff;
    camAiming=aiming;
    pad.sThumbRX=pad.sThumbRY=0;
    if(!camHeld) placeShoulderCamera(g);   // the steering below reads it now
    camHeld=true;
}

// PlayStation-style face buttons: Triangle opens the status screen
// (inventory and map, the game's Circle) and Circle cancels (the game's
// Square, Cancel/Run). Classic REbirth also read Triangle as Confirm/Fire.
static void remapFaceButtons(XINPUT_GAMEPAD& pad) {
    if(!faceButtons) return;
    WORD in=pad.wButtons;
    pad.wButtons&=~(XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_B);
    if(in&XINPUT_GAMEPAD_Y) pad.wButtons|=XINPUT_GAMEPAD_B;
    if(in&XINPUT_GAMEPAD_B) pad.wButtons|=XINPUT_GAMEPAD_X;
}

// Camera-relative steering: Regina turns toward the stick direction as seen
// on screen and the game receives a plain forward push of the same tilt.
// Returns the tilt in stick units, or 0 when the stick stays with the game.
static int64_t steer(Pad& p, uintptr_t g, XINPUT_GAMEPAD& pad) {
    int64_t x=pad.sThumbLX,y=pad.sThumbLY,tilt=std::lround(std::sqrt(double(x*x+y*y)));
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt=p.time.QuadPart?double(now.QuadPart-p.time.QuadPart)/frequency.QuadPart:0;
    p.time=now;
    if(!modern || !g || tilt<deadzone ||
       (pad.wButtons&(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|
                      XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT))) {
        p.latched=false; return 0;
    }
    int stick=angle4096(double(x),double(y));
    // Hold the camera the stick was pushed under until the stick is released
    // or swung away, so a camera cut does not reverse Regina mid-run.
    // The shoulder camera moves continuously, so it is never held.
    if(!holdCamera || camHeld || !p.latched || std::abs(delta4096(stick,p.stickAngle))>0x200) {
        p.camera=cameraYaw(g); p.stickAngle=stick; p.latched=true;
    }
    int target=(p.camera+stick)&0xfff;
    auto yaw=reinterpret_cast<volatile uint16_t*>(g+ACTOR+0x2a);
    int current=*yaw&0xfff, diff=delta4096(target,current);
    int step=static_cast<int>(std::min(dt,0.05)*turnRate);
    if(std::abs(diff)<=step) current=target;
    else current+=diff>0?step:-step;
    *yaw=static_cast<uint16_t>(current&0xfff);
    tilt=std::min<int64_t>(tilt,32767);
    pad.sThumbLX=0;
    // Turn in place first when the new direction points well behind her.
    pad.sThumbLY=std::abs(delta4096(target,current))<0x400?static_cast<SHORT>(tilt):0;
    return pad.sThumbLY;
}

// XInputGetState clears the guide button; XInputGetStateEx (ordinal 100 of
// xinput1_4 and xinput1_3) keeps it, and the filter clears it itself before
// REbirth sees the state.
typedef DWORD (WINAPI *GetStateFn)(DWORD,XINPUT_STATE*);
static GetStateFn getState;
static BOOL CALLBACK loadXInput(INIT_ONCE*, void*, void**) {
    char path[MAX_PATH];
    UINT n=GetSystemDirectoryA(path,MAX_PATH);
    auto find=[&](const char* name, LPCSTR export_) -> GetStateFn {
        if(!n || n+strlen(name)+2>MAX_PATH) return nullptr;
        snprintf(path+n,MAX_PATH-n,"\\%s",name);
        HMODULE module=LoadLibraryA(path);
        return module?reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(module,export_))):nullptr;
    };
    for(const char* name : {"xinput1_4.dll","xinput1_3.dll"})
        if(!getState) getState=find(name,MAKEINTRESOURCEA(100));
    for(const char* name : {"xinput9_1_0.dll","xinput1_4.dll","xinput1_3.dll"})
        if(!getState) getState=find(name,"XInputGetState");
    return TRUE;
}

extern "C" DWORD WINAPI DcGetState(DWORD user,XINPUT_STATE* state) {
    static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once,loadXInput,nullptr,nullptr);
    if(!getState) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD result=getState(user,state);
    if(user>=4) return result;
    AcquireSRWLockExclusive(&lock);
    config();
    Pad& p=pads[user];
    if(result!=ERROR_SUCCESS || !state) {
        p.running=false; p.latched=false; ReleaseSRWLockExclusive(&lock); return result;
    }
    uintptr_t gs=enabled?gameState():0;
    if(enabled) shoulderCamera(gs,state->Gamepad);
    state->Gamepad.wButtons&=~XINPUT_GAMEPAD_GUIDE;
    if(enabled) {
        remapFaceButtons(state->Gamepad); remapTriggers(state->Gamepad); skipDoor(gs);
    }
    uintptr_t g=enabled?gameplay():0;
    int64_t tilt=steer(p,g,state->Gamepad);
    int64_t x=state->Gamepad.sThumbLX,y=state->Gamepad.sThumbLY;
    int64_t threshold=32767LL*(p.running?offPercent:onPercent)/100;
    bool forward=y>6000;
    if(g && (tilt || forward)) {
        p.running=tilt?tilt>=threshold:x*x+y*y>=threshold*threshold;
    } else p.running=false;
    if(p.running) state->Gamepad.wButtons|=runButton;
    // Synthetic changes must also invalidate a consumer's packet cache.
    if(memcmp(&p.last,&state->Gamepad,sizeof(p.last))) {
        ++p.packet; p.last=state->Gamepad;
    }
    state->dwPacketNumber=p.packet;
    ReleaseSRWLockExclusive(&lock);
    return result;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { self=module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
