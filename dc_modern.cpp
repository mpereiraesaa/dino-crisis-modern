#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <d3d9.h>
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
// Classic REbirth replaces the game's polygon builders, vertex projection and
// winding test with its own and draws through Direct3D 9: anything that
// decides which polygons reach the screen must be patched in REbirth's code,
// not the game's (docs/classic-rebirth.md).
static HMODULE self;
static SRWLOCK lock = SRWLOCK_INIT;
static char ini[MAX_PATH];
static bool configured, supported;
static DWORD configTime;
static int interpolate=0;
static int enabled=1, onPercent=75, offPercent=65;
static int modern=1, turnRate=4096*2, deadzone=8000, holdCamera=1;
static int triggers=1, triggerThreshold=77, faceButtons=1, doorSkip=1;
static int cameraMode=0, camDistance=1700, camHeight=1850, camSide=520,
           camLook=2800, camLookHeight=1550, camTurnRate=4096/2,
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
    camHeight=GetPrivateProfileIntA("Camera","Height",1850,ini);
    camSide=GetPrivateProfileIntA("Camera","SideOffset",520,ini);
    camLook=GetPrivateProfileIntA("Camera","LookAhead",2800,ini);
    camLookHeight=GetPrivateProfileIntA("Camera","LookHeight",1550,ini);
    camTurnRate=GetPrivateProfileIntA("Camera","TurnDegreesPerSecond",180,ini)*4096/360;
    camCollide=GetPrivateProfileIntA("Camera","Collision",1,ini);
    camRadius=std::max(40,std::min(600,(int)GetPrivateProfileIntA("Camera","CollisionRadius",200,ini)));
    camRiseWhenClose=GetPrivateProfileIntA("Camera","RiseWhenClose",500,ini);
    camMinDistance=GetPrivateProfileIntA("Camera","MinDistance",550,ini);
    interpolate=GetPrivateProfileIntA("Debug","Interpolate",0,ini);
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

constexpr int AIM_HEIGHT=1250;           // the gun's height while aiming
static volatile bool crosshairShown;
static volatile double aimPitch;         // radians, positive up
static volatile bool freeAim;            // aiming with the shoulder camera: our hit test

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
    // Aiming, the target sits on the gun's line at chest height, so the
    // crosshair in the middle of the screen marks where she shoots.
    double side=aiming?0:shoulder*0.5;
    target[0]=clamp16(pos[0]+fx*camLook+rx*side);
    target[1]=clamp16(pos[1]-(aiming?AIM_HEIGHT+std::tan(aimPitch)*camLook:camLookHeight));
    target[2]=clamp16(pos[2]+fz*camLook+rz*side);
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

// Classic REbirth replaces the room's polygon builders with its own (the
// game's 0x5F18F0 and 0x5F1AD0 start with a jump into ddraw.dll), and those
// drop a polygon facing away from the camera. Walls and fences exist only on
// the side a fixed camera sees, so from the shoulder a fence seen from behind
// vanished while its collision stayed. While the shoulder camera is on: the
// quad builder's `jl` after its winding test (ddraw.dll 0x100549B4, preferred
// base 0x10000000) becomes NOPs, and the triangle builder's unsigned `ja`
// (0x100546D3), which drops both back faces and huge triangles, becomes a
// signed `jg`, which still drops the huge ones. Any other REbirth build is
// left alone.
struct ModulePatch { uintptr_t rva; uint8_t size; uint8_t original[6], patched[6]; };
static const ModulePatch rebirthPatches[]={
    {0x549b4,6,{0x0f,0x8c,0x59,0x02,0x00,0x00},{0x90,0x90,0x90,0x90,0x90,0x90}},
    {0x546d3,6,{0x0f,0x87,0xde,0x01,0x00,0x00},{0x0f,0x8f,0xde,0x01,0x00,0x00}},
};

static bool applyPatch(void* at, const uint8_t* bytes, size_t size) {
    DWORD old;
    if(!VirtualProtect(at,size,PAGE_EXECUTE_READWRITE,&old)) return false;
    memcpy(at,bytes,size);
    VirtualProtect(at,size,old,&old);
    FlushInstructionCache(GetCurrentProcess(),at,size);
    return true;
}

static void drawWholeRoom(bool on) {
    if(on==wholeRoom) return;
    for(const CodePatch& p : roomPatches)
        if(memcmp(reinterpret_cast<void*>(p.at),on?p.original:p.patched,p.size)) return;
    for(const CodePatch& p : roomPatches)
        applyPatch(reinterpret_cast<void*>(p.at),on?p.patched:p.original,p.size);
    if(auto rebirth=reinterpret_cast<uint8_t*>(GetModuleHandleA("ddraw.dll"))) {
        auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(rebirth);
        auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(rebirth+dos->e_lfanew);
        bool known=nt->OptionalHeader.SizeOfImage>0x549ba;
        for(const ModulePatch& p : rebirthPatches)
            known=known && !memcmp(rebirth+p.rva,on?p.original:p.patched,p.size);
        if(known)
            for(const ModulePatch& p : rebirthPatches)
                applyPatch(rebirth+p.rva,on?p.patched:p.original,p.size);
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
        if(camAiming) *reinterpret_cast<volatile uint16_t*>(g+ACTOR+0x2a)=static_cast<uint16_t>(camYaw);
        placeShoulderCamera(g);
    } else restoreRoom(g);
}
// Free aim's hit test. Every weapon asks FUN_004620C1(player, zone) which
// enemies a shot reaches: the 10 actors at gameState+0x288 (0x260 bytes each)
// that are active (+0x30 bit 0), hittable (+0x11E bit 6 clear, +0x175 bit 7
// clear), in sight (FUN_00469E84 against the room's collision) and inside the
// weapon's zone, a trapezoid on the floor 200..20200 ahead and up to 8000
// wide: a wide cone, the game's auto-aim, blind to height. It stores up to 3,
// nearest first, in the player's +0x24C list, which the weapon then hits.
// While she aims with the shoulder camera the hook answers instead: a ray
// from the camera through the crosshair must cross a cylinder around the
// actor (its height included) in front of Regina, within the same 20200.
// Hits, damage and reactions stay the game's.
constexpr uintptr_t HIT_TEST=0x4620c1, LINE_OF_SIGHT=0x469e84;
constexpr double HIT_RANGE=20200, HIT_RADIUS=500, HIT_HEIGHT=1600;
typedef void (__cdecl *HitTestFn)(uintptr_t player, uintptr_t zone);
typedef char (__cdecl *LineOfSightFn)(volatile int16_t* from, volatile int16_t* to, uint32_t mode);
static HitTestFn originalHitTest;

static void __cdecl freeAimHitTest(uintptr_t player, uintptr_t zone) {
    uintptr_t g=gameState();
    if(!freeAim || !g || player!=g+ACTOR) { originalHitTest(player,zone); return; }
    auto eye=reinterpret_cast<volatile int16_t*>(g+CAM_EYE);
    auto at=reinterpret_cast<volatile int16_t*>(g+CAM_TARGET);
    auto me=reinterpret_cast<volatile int16_t*>(player+0x20);
    double ox=eye[0], oy=eye[1], oz=eye[2];
    double dx=at[0]-ox, dy=at[1]-oy, dz=at[2]-oz;
    double len=std::sqrt(dx*dx+dy*dy+dz*dz);
    if(len<1) { originalHitTest(player,zone); return; }
    dx/=len; dy/=len; dz/=len;
    double flat=dx*dx+dz*dz;                // the ray's horizontal part, squared
    // Regina's own distance along the ray: hits must lie beyond her.
    double self=(me[0]-ox)*dx+(me[1]-oy)*dy+(me[2]-oz)*dz;
    struct Hit { double t; uint8_t index; } hits[10];
    int count=0;
    auto lineOfSight=reinterpret_cast<LineOfSightFn>(LINE_OF_SIGHT);
    for(int i=0;i<10;i++) {
        uintptr_t a=g+0x288+i*0x260;
        auto flags=*reinterpret_cast<volatile uint32_t*>(a+0x30);
        if(!(flags&1) || (*reinterpret_cast<volatile uint8_t*>(a+0x11e)&0x40) ||
           (*reinterpret_cast<volatile uint8_t*>(a+0x175)&0x80)) continue;
        auto pos=reinterpret_cast<volatile int16_t*>(a+0x20);
        double ax=pos[0]-ox, az=pos[2]-oz;
        if(std::hypot(pos[0]-me[0],pos[2]-me[2])>HIT_RANGE || flat<1e-9) continue;
        // Where the ray's horizontal part enters and leaves the cylinder.
        double b=ax*dx+az*dz, c=ax*ax+az*az-HIT_RADIUS*HIT_RADIUS, disc=b*b-flat*c;
        if(disc<0) continue;
        double t0=(b-std::sqrt(disc))/flat, t1=(b+std::sqrt(disc))/flat;
        if(t1<=self) continue;
        t0=std::max(t0,self);
        // y grows downward: the cylinder runs from the feet up HIT_HEIGHT.
        double y0=oy+dy*t0, y1=oy+dy*t1, feet=pos[1], top=pos[1]-HIT_HEIGHT;
        if(std::max(y0,y1)<top || std::min(y0,y1)>feet) continue;
        if(lineOfSight(me,pos,5)) continue;    // a wall in between
        hits[count++]={t0,static_cast<uint8_t>(i)};
    }
    for(int i=1;i<count;i++)                // nearest first; at most 10
        for(int j=i;j>0 && hits[j].t<hits[j-1].t;j--) std::swap(hits[j],hits[j-1]);
    int n=std::min(count,3);
    auto list=reinterpret_cast<volatile uint8_t*>(player+0x24c);
    for(int i=0;i<n;i++) list[n-i]=hits[i].index;   // nearest last: popped first
    list[0]=static_cast<uint8_t>(n);
}

// A jmp at the hit test's entry; the original stays callable through a copy
// of its first 6 bytes (push ebp; mov ebp,esp; sub esp,0x5C) and a jump back.
static void hookHitTest() {
    static const uint8_t prologue[6]={0x55,0x8b,0xec,0x83,0xec,0x5c};
    auto entry=reinterpret_cast<uint8_t*>(HIT_TEST);
    if(originalHitTest || memcmp(entry,prologue,6)) return;
    auto thunk=static_cast<uint8_t*>(VirtualAlloc(nullptr,16,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!thunk) return;
    memcpy(thunk,prologue,6);
    thunk[6]=0xe9;
    int32_t back=int32_t(HIT_TEST+6-(reinterpret_cast<uintptr_t>(thunk)+11));
    memcpy(thunk+7,&back,4);
    FlushInstructionCache(GetCurrentProcess(),thunk,16);
    originalHitTest=reinterpret_cast<HitTestFn>(thunk);
    uint8_t jump[6]={0xe9,0,0,0,0,0x90};
    int32_t rel=int32_t(reinterpret_cast<uintptr_t>(freeAimHitTest)-(HIT_TEST+5));
    memcpy(jump+1,&rel,4);
    applyPatch(entry,jump,6);
}

static void hookDispatch();
static void hookCameraCallbacks() {
    if(callbacksHooked || !supported) return;
    callbacksHooked=true;
    hookHitTest();
    hookDispatch();
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
    if(!camOn || !play || !callbacksHooked) { camHeld=false; crosshairShown=false; freeAim=false; return; }

    int yaw=camYaw;
    auto yawNow=*reinterpret_cast<volatile uint16_t*>(g+ACTOR+0x2a)&0xfff;
    if(!camHeld) yaw=yawNow;               // start behind her
    if(aiming) {
        // Free aim, as in RE4: the right stick moves the aim, left and right
        // and up and down, and she turns to face it (cameraCallbackHook). The
        // left stick still walks her forward and back; it no longer turns her.
        if(!camAiming) aimPitch=0;
        if(std::abs(pad.sThumbRX)>deadzone) yaw+=int(dt*camTurnRate*0.6*pad.sThumbRX/32767.0);
        if(std::abs(pad.sThumbRY)>deadzone)
            aimPitch=std::max(-0.6,std::min(0.6,aimPitch+dt*1.2*pad.sThumbRY/32767.0));
        pad.sThumbLX=0;
    } else if(std::abs(pad.sThumbRX)>deadzone) {
        yaw+=int(dt*camTurnRate*pad.sThumbRX/32767.0);
    }
    camYaw=yaw&0xfff;
    camAiming=aiming;
    crosshairShown=aiming;
    freeAim=aiming;
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

// The crosshair. REbirth draws through Direct3D 9 (d3d9!Direct3DCreate9 in
// ddraw.dll's imports): its import is redirected here at load, the device's
// Present is hooked when REbirth creates it (REbirth draws the game into a
// texture and scales it onto the back buffer, so EndScene is too early), and
// while she aims with the shoulder camera a crosshair is drawn on the back
// buffer just before it is shown, REbirth's state and render target saved
// and restored around it.
typedef IDirect3D9* (WINAPI *Create9Fn)(UINT);
typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFn)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
static Create9Fn realCreate9;
static CreateDeviceFn realCreateDevice;
static PresentFn realPresent;

static void hookVtable(void* object, int slot, void* hook, void** original) {
    void** vtable=*reinterpret_cast<void***>(object);
    if(vtable[slot]==hook) return;
    DWORD old;
    if(!VirtualProtect(&vtable[slot],sizeof(void*),PAGE_EXECUTE_READWRITE,&old)) return;
    *original=vtable[slot];
    vtable[slot]=hook;
    VirtualProtect(&vtable[slot],sizeof(void*),old,&old);
}

struct CrosshairVertex { float x,y,z,rhw; DWORD color; };
static void addBar(CrosshairVertex*& v, float x0, float y0, float x1, float y1, DWORD color) {
    const CrosshairVertex c[6]={{x0,y0,0,1,color},{x1,y0,0,1,color},{x0,y1,0,1,color},
                                {x1,y0,0,1,color},{x1,y1,0,1,color},{x0,y1,0,1,color}};
    memcpy(v,c,sizeof(c)); v+=6;
}

static void drawCrosshair(IDirect3DDevice9* device) {
    IDirect3DSurface9 *target=nullptr, *back=nullptr;
    if(FAILED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back))) return;
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    IDirect3DStateBlock9* saved=nullptr;
    if(!desc.Width || !desc.Height || FAILED(device->CreateStateBlock(D3DSBT_ALL,&saved))) { back->Release(); return; }
    device->GetRenderTarget(0,&target);
    device->SetRenderTarget(0,back);
    device->BeginScene();
    D3DVIEWPORT9 viewport={0,0,desc.Width,desc.Height,0,1};
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    device->SetTexture(0,nullptr);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    device->SetRenderState(D3DRS_ZENABLE,FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING,FALSE);
    device->SetRenderState(D3DRS_FOGENABLE,FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,0xf);
    // Four short bars around a gap, white over a thin dark outline, scaled to
    // the screen's height.
    float cx=desc.Width*0.5f, cy=desc.Height*0.5f, h=float(desc.Height);
    float gap=h*0.009f, len=h*0.016f, half=std::max(1.0f,h*0.0016f), edge=std::max(1.0f,h*0.0012f);
    CrosshairVertex vertices[48], *v=vertices;
    for(int pass=0;pass<2;pass++) {
        float e=pass?0:edge, w=half+e;
        DWORD color=pass?0xf0ffffff:0x90000000;
        addBar(v,cx-w,cy-gap-len-e,cx+w,cy-gap+e,color);
        addBar(v,cx-w,cy+gap-e,cx+w,cy+gap+len+e,color);
        addBar(v,cx-gap-len-e,cy-w,cx-gap+e,cy+w,color);
        addBar(v,cx+gap-e,cy-w,cx+gap+len+e,cy+w,color);
    }
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,16,vertices,sizeof(CrosshairVertex));
    device->EndScene();
    saved->Apply();
    saved->Release();
    if(target) { device->SetRenderTarget(0,target); target->Release(); }
    back->Release();
}

static HRESULT STDMETHODCALLTYPE hookPresent(IDirect3DDevice9* device, const RECT* source,
        const RECT* dest, HWND window, const RGNDATA* dirty) {
    if(crosshairShown) drawCrosshair(device);
    return realPresent(device,source,dest,window,dirty);
}
static HRESULT STDMETHODCALLTYPE hookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type,
        HWND window, DWORD flags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) {
    HRESULT result=realCreateDevice(d3d,adapter,type,window,flags,params,device);
    if(SUCCEEDED(result) && device && *device)
        hookVtable(*device,17,reinterpret_cast<void*>(hookPresent),reinterpret_cast<void**>(&realPresent));
    return result;
}
static IDirect3D9* WINAPI hookCreate9(UINT version) {
    IDirect3D9* d3d=realCreate9(version);
    if(d3d) hookVtable(d3d,16,reinterpret_cast<void*>(hookCreateDevice),reinterpret_cast<void**>(&realCreateDevice));
    return d3d;
}
// At load, before REbirth creates its device: its ddraw.dll import table is
// already bound, so the d3d9!Direct3DCreate9 slot is swapped for the hook.
static void hookDirect3D() {
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleA("ddraw.dll"));
    HMODULE d3d9=GetModuleHandleA("d3d9.dll");
    if(!base || !d3d9) return;
    auto real=reinterpret_cast<void*>(GetProcAddress(d3d9,"Direct3DCreate9"));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    auto& dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!real || !dir.VirtualAddress) return;
    for(auto imp=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);imp->Name;imp++) {
        if(lstrcmpiA(reinterpret_cast<char*>(base+imp->Name),"d3d9.dll")) continue;
        for(auto slot=reinterpret_cast<void**>(base+imp->FirstThunk);*slot;slot++) {
            if(*slot!=real) continue;
            DWORD old;
            if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)) return;
            realCreate9=reinterpret_cast<Create9Fn>(real);
            *slot=reinterpret_cast<void*>(hookCreate9);
            VirtualProtect(slot,sizeof(void*),old,&old);
            return;
        }
    }
}

// 60 fps (work in progress). The game ticks at 30: Classic REbirth's frame
// loop (ddraw.dll 0x1004DD10) calls DispatchFrameOutputCallback (0x5F30E0)
// with 2, so every image is presented for two 60 Hz vsyncs. The second one
// shows an extra frame instead: the scene is built again, with the camera
// halfway between the last two ticks, into the idle half of the double
// buffer, drawn and presented; then everything is put back, so the next tick
// runs exactly as before. Only the call from REbirth's loop is redirected
// (its jump thunk reads the target from ddraw.dll 0x100014A2).
typedef int (__cdecl *DispatchFn)(int);
typedef void (__cdecl *VoidFn)();
typedef void (__cdecl *ClearOTFn)(uintptr_t ot, int count);
typedef void (__cdecl *PtrFn)(uintptr_t);
constexpr uintptr_t DISPATCH=0x5f30e0, PRESENT_CALLBACK=0x67c018, RENDER_SCENE=0x44cbc2,
    CLEAR_OT=0x5f2a30, DRAW_OT=0x5f2c90, PUT_DRAW_ENV=0x5f1ee0, PUT_DISP_ENV=0x5f1fb0,
    BUFFER_INDEX=0x6d3e24, BUFFER_DESC=0x6de994, DESCS=0x6d3ce0,
    GAME_BLOCK=0x67b868;
struct CameraState { int16_t matrix[9], eye[3], angles[3], target[3], view[3]; };
static CameraState camPrev, camCur;

// Characters: the 11 actors at gameState+0x288 (0x260 bytes each, Regina is
// the 11th). An actor's root has its world position (+0x20) and angles
// (+0x28); its bones (first at +0x4C, count at +0x50, next at bone+0x34)
// carry world positions and angles too, which the renderer turns into
// matrices. UpdateActorNodeWorldTransforms writes the bones at the end of
// a tick's scene, for the next tick's: bones lag the root and the camera by
// one tick, so they blend one tick further back.
constexpr int ACTORS=11, MAX_BONES=32;
struct ActorPose {
    bool valid; uintptr_t first; int bones;
    int16_t pos[3], ang[3], bonePos[MAX_BONES][3], boneAng[MAX_BONES][3];
};
static ActorPose poses[3][ACTORS];      // [0] this tick, [1] the one before, [2] before that
static bool posesValid[3];

static void readPoses(uintptr_t g, ActorPose* out) {
    for(int i=0;i<ACTORS;i++) {
        uintptr_t a=g+0x288+i*0x260;
        ActorPose& p=out[i];
        p.valid=(*reinterpret_cast<volatile uint32_t*>(a+0x30)&1)!=0;
        if(!p.valid) continue;
        p.first=*reinterpret_cast<volatile uint32_t*>(a+0x4c);
        p.bones=int(*reinterpret_cast<volatile uint32_t*>(a+0x50));
        if(p.bones<0 || p.bones>MAX_BONES || (p.bones && !readable(p.first,0xac))) { p.valid=false; continue; }
        for(int k=0;k<3;k++) {
            p.pos[k]=*reinterpret_cast<volatile int16_t*>(a+0x20+2*k);
            p.ang[k]=*reinterpret_cast<volatile int16_t*>(a+0x28+2*k);
        }
        uintptr_t b=p.first;
        for(int n=0;n<p.bones;n++) {
            if(!readable(b,0x38)) { p.valid=false; break; }
            for(int k=0;k<3;k++) {
                p.bonePos[n][k]=*reinterpret_cast<volatile int16_t*>(b+0x20+2*k);
                p.boneAng[n][k]=*reinterpret_cast<volatile int16_t*>(b+0x28+2*k);
            }
            b=*reinterpret_cast<volatile uint32_t*>(b+0x34);
        }
    }
}

static bool closeEnough(const int16_t* a, const int16_t* b, const int16_t* ra, const int16_t* rb) {
    for(int k=0;k<3;k++)
        if(std::abs(a[k]-b[k])>1500 || std::abs(delta4096(ra[k],rb[k]))>1024) return false;
    return true;
}
static int16_t lerpShort(int a, int b, float t) { return int16_t(std::lround(a+(b-a)*t)); }
static int16_t lerpAngle(int a, int b, float t) { return int16_t(a+std::lround(delta4096(b,a)*t)); }

// Effects and moving room pieces: the 60 objects at gameState+0x5FD8 (0x7C
// bytes; muzzle flashes, blood, casings and lamps come from here) and the 36
// room meshes at +0x7CE8 (0x88 bytes; doors, fans). Each has a world
// position (+0x20) and angles (+0x28) that the scene reads after the tick's
// logic, as it does an actor's root. A slot blends only while it holds the
// same thing (+0x3C) and does not jump; one just spawned or gone is drawn
// as it is.
struct PieceTable { uintptr_t offset, stride; int count; };
static const PieceTable pieceTables[2]={{0x5fd8,0x7c,60},{0x7ce8,0x88,36}};
struct PiecePose { bool valid; uint32_t kind; int16_t pos[3], ang[3]; };
static PiecePose pieces[2][2][60];        // [0] this tick, [1] the one before; [table][slot]

static void readPieces(uintptr_t g, PiecePose out[2][60]) {
    for(int t=0;t<2;t++)
        for(int i=0;i<pieceTables[t].count;i++) {
            uintptr_t a=g+pieceTables[t].offset+i*pieceTables[t].stride;
            PiecePose& p=out[t][i];
            p.valid=(*reinterpret_cast<volatile uint32_t*>(a+0x30)&1)!=0;
            p.kind=*reinterpret_cast<volatile uint32_t*>(a+0x3c);
            for(int k=0;k<3;k++) {
                p.pos[k]=*reinterpret_cast<volatile int16_t*>(a+0x20+2*k);
                p.ang[k]=*reinterpret_cast<volatile int16_t*>(a+0x28+2*k);
            }
        }
}
static void blendPieces(uintptr_t g, float t, PiecePose saved[2][60]) {
    readPieces(g,saved);
    for(int tb=0;tb<2;tb++)
        for(int i=0;i<pieceTables[tb].count;i++) {
            const PiecePose &a0=pieces[1][tb][i], &a1=pieces[0][tb][i], &now=saved[tb][i];
            if(!now.valid || !a0.valid || !a1.valid || a0.kind!=a1.kind || a1.kind!=now.kind ||
               !closeEnough(a0.pos,a1.pos,a0.ang,a1.ang)) continue;
            uintptr_t a=g+pieceTables[tb].offset+i*pieceTables[tb].stride;
            for(int k=0;k<3;k++) {
                *reinterpret_cast<volatile int16_t*>(a+0x20+2*k)=lerpShort(a0.pos[k],a1.pos[k],t);
                *reinterpret_cast<volatile int16_t*>(a+0x28+2*k)=lerpAngle(a0.ang[k],a1.ang[k],t);
            }
        }
}
static void restorePieces(uintptr_t g, const PiecePose saved[2][60]) {
    for(int tb=0;tb<2;tb++)
        for(int i=0;i<pieceTables[tb].count;i++) {
            uintptr_t a=g+pieceTables[tb].offset+i*pieceTables[tb].stride;
            for(int k=0;k<3;k++) {
                *reinterpret_cast<volatile int16_t*>(a+0x20+2*k)=saved[tb][i].pos[k];
                *reinterpret_cast<volatile int16_t*>(a+0x28+2*k)=saved[tb][i].ang[k];
            }
        }
}

// Writes poses blended at t, keeping what it overwrote in `saved`.
static void blendPoses(uintptr_t g, float t, ActorPose* saved) {
    readPoses(g,saved);
    for(int i=0;i<ACTORS;i++) {
        const ActorPose &r0=poses[1][i], &r1=poses[0][i], &b0=poses[2][i], &b1=poses[1][i];
        uintptr_t a=g+0x288+i*0x260;
        if(!saved[i].valid) continue;
        if(posesValid[1] && r0.valid && r1.valid && closeEnough(r0.pos,r1.pos,r0.ang,r1.ang))
            for(int k=0;k<3;k++) {
                *reinterpret_cast<volatile int16_t*>(a+0x20+2*k)=lerpShort(r0.pos[k],r1.pos[k],t);
                *reinterpret_cast<volatile int16_t*>(a+0x28+2*k)=lerpAngle(r0.ang[k],r1.ang[k],t);
            }
        if(!(posesValid[2] && b0.valid && b1.valid && b0.first==b1.first && b0.bones==b1.bones &&
             b1.first==saved[i].first && b1.bones==saved[i].bones)) continue;
        uintptr_t b=b0.first;
        for(int n=0;n<b0.bones;n++) {
            if(closeEnough(b0.bonePos[n],b1.bonePos[n],b0.boneAng[n],b1.boneAng[n]))
                for(int k=0;k<3;k++) {
                    *reinterpret_cast<volatile int16_t*>(b+0x20+2*k)=lerpShort(b0.bonePos[n][k],b1.bonePos[n][k],t);
                    *reinterpret_cast<volatile int16_t*>(b+0x28+2*k)=lerpAngle(b0.boneAng[n][k],b1.boneAng[n][k],t);
                }
            b=*reinterpret_cast<volatile uint32_t*>(b+0x34);
        }
    }
}
static void restorePoses(uintptr_t g, const ActorPose* saved) {
    for(int i=0;i<ACTORS;i++) {
        const ActorPose& p=saved[i];
        if(!p.valid) continue;
        uintptr_t a=g+0x288+i*0x260;
        for(int k=0;k<3;k++) {
            *reinterpret_cast<volatile int16_t*>(a+0x20+2*k)=p.pos[k];
            *reinterpret_cast<volatile int16_t*>(a+0x28+2*k)=p.ang[k];
        }
        uintptr_t b=p.first;
        for(int n=0;n<p.bones;n++) {
            for(int k=0;k<3;k++) {
                *reinterpret_cast<volatile int16_t*>(b+0x20+2*k)=p.bonePos[n][k];
                *reinterpret_cast<volatile int16_t*>(b+0x28+2*k)=p.boneAng[n][k];
            }
            b=*reinterpret_cast<volatile uint32_t*>(b+0x34);
        }
    }
}
static bool camPrevValid;
static uint8_t camZonePrev;

static CameraState readCamera(uintptr_t g) {
    CameraState c;
    for(int i=0;i<9;i++) c.matrix[i]=*reinterpret_cast<volatile int16_t*>(g+0xb4+2*i);
    for(int i=0;i<3;i++) {
        c.eye[i]=*reinterpret_cast<volatile int16_t*>(g+0xd4+2*i);
        c.angles[i]=*reinterpret_cast<volatile int16_t*>(g+0xdc+2*i);
        c.target[i]=*reinterpret_cast<volatile int16_t*>(g+0xe4+2*i);
        c.view[i]=*reinterpret_cast<volatile int16_t*>(g+0xec+2*i);
    }
    return c;
}
static void writeCamera(uintptr_t g, const CameraState& c) {
    for(int i=0;i<3;i++) {
        *reinterpret_cast<volatile int16_t*>(g+0xd4+2*i)=c.eye[i];
        *reinterpret_cast<volatile int16_t*>(g+0xdc+2*i)=c.angles[i];
        *reinterpret_cast<volatile int16_t*>(g+0xe4+2*i)=c.target[i];
        *reinterpret_cast<volatile int16_t*>(g+0xec+2*i)=c.view[i];
    }
    for(int i=0;i<9;i++) *reinterpret_cast<volatile int16_t*>(g+0xb4+2*i)=c.matrix[i];
}

// The camera's rotation (Q12, rows) blended between two ticks: each row
// interpolated, then made orthonormal again (Gram-Schmidt).
static void blendRotation(const int16_t* a, const int16_t* b, float t, int16_t* out) {
    double r[3][3];
    for(int i=0;i<3;i++) for(int j=0;j<3;j++) r[i][j]=a[i*3+j]+(b[i*3+j]-a[i*3+j])*t;
    auto norm=[](double* v){ double l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l>1e-9) for(int k=0;k<3;k++) v[k]/=l; };
    auto dot=[](const double* x,const double* y){ return x[0]*y[0]+x[1]*y[1]+x[2]*y[2]; };
    norm(r[0]);
    double d=dot(r[1],r[0]); for(int k=0;k<3;k++) r[1][k]-=d*r[0][k];
    norm(r[1]);
    double d0=dot(r[2],r[0]), d1=dot(r[2],r[1]); for(int k=0;k<3;k++) r[2][k]-=d0*r[0][k]+d1*r[1][k];
    norm(r[2]);
    for(int i=0;i<3;i++) for(int j=0;j<3;j++) out[i*3+j]=int16_t(std::lround(r[i][j]*4096));
}

static void renderExtraFrame(uintptr_t g, float t) {
    CameraState mid;
    for(int i=0;i<3;i++) {
        auto lerp=[t](int a,int b){ return int16_t(std::lround(a+(b-a)*t)); };
        mid.eye[i]=lerp(camPrev.eye[i],camCur.eye[i]);
        mid.target[i]=lerp(camPrev.target[i],camCur.target[i]);
        mid.view[i]=lerp(camPrev.view[i],camCur.view[i]);
        mid.angles[i]=int16_t(camPrev.angles[i]+std::lround(delta4096(camCur.angles[i],camPrev.angles[i])*t));
    }
    blendRotation(camPrev.matrix,camCur.matrix,t,mid.matrix);
    uint8_t saved[0x50];
    memcpy(saved,reinterpret_cast<void*>(g+0xb4),sizeof(saved));
    auto block=*reinterpret_cast<uintptr_t**>(GAME_BLOCK);
    uint8_t index=*reinterpret_cast<volatile uint8_t*>(BUFFER_INDEX);
    uintptr_t desc=*reinterpret_cast<volatile uintptr_t*>(BUFFER_DESC), packets=block[1];
    // The idle half: its list was drawn last tick. Packets go to a buffer of
    // our own: the game's second packet area is too small once the shoulder
    // camera draws whole rooms.
    static uint8_t* extraPackets=static_cast<uint8_t*>(VirtualAlloc(nullptr,1<<20,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!extraPackets) return;
    uint8_t idle=index^1;
    uintptr_t idleDesc=DESCS+idle*0x84;
    *reinterpret_cast<volatile uint8_t*>(BUFFER_INDEX)=idle;
    *reinterpret_cast<volatile uintptr_t*>(BUFFER_DESC)=idleDesc;
    block[1]=reinterpret_cast<uintptr_t>(extraPackets);
    auto clear=reinterpret_cast<ClearOTFn>(CLEAR_OT);
    clear(idleDesc+0x70,5);
    clear(0x6d1c40+idle*0x1000,0x400);
    clear(0x6d3e00+idle*0xc,3);
    clear(0x6de9a0+idle*0x10,4);
    writeCamera(g,mid);
    static ActorPose savedPoses[ACTORS];
    static PiecePose savedPieces[2][60];
    blendPoses(g,t,savedPoses);
    if(posesValid[1]) blendPieces(g,t,savedPieces); else readPieces(g,savedPieces);
    reinterpret_cast<VoidFn>(RENDER_SCENE)();
    restorePieces(g,savedPieces);
    restorePoses(g,savedPoses);
    reinterpret_cast<PtrFn>(PUT_DRAW_ENV)(idleDesc);
    reinterpret_cast<PtrFn>(PUT_DISP_ENV)(idleDesc+0x14);
    reinterpret_cast<PtrFn>(DRAW_OT)(idleDesc+0x80);
    memcpy(reinterpret_cast<void*>(g+0xb4),saved,sizeof(saved));
    block[1]=packets;
    *reinterpret_cast<volatile uintptr_t*>(BUFFER_DESC)=desc;
    *reinterpret_cast<volatile uint8_t*>(BUFFER_INDEX)=index;
}

static int __cdecl dispatchHook(int count) {
    auto dispatch=reinterpret_cast<DispatchFn>(DISPATCH);
    uintptr_t g=gameState();
    bool ok=false;
    if(g && count==2 && interpolate) {
        auto outer=*reinterpret_cast<volatile uint16_t*>(g+0x28);
        auto sub=*reinterpret_cast<volatile uint16_t*>(g+0x2a);
        auto pauseFlags=*reinterpret_cast<volatile uint32_t*>(g+0x3c);
        bool scene=outer==3 && sub==0 && !(pauseFlags&0x20000000) &&
            !*reinterpret_cast<volatile uint8_t*>(g+0x54) &&
            !(*reinterpret_cast<volatile uint8_t*>(g+0x38)&0x80) &&
            !*reinterpret_cast<volatile uint8_t*>(g+0x6d);
        CameraState now=readCamera(g);
        uint8_t zone=*reinterpret_cast<volatile uint8_t*>(g+0x110);
        // A cut or a jump: nothing to blend.
        bool cut=!camPrevValid || zone!=camZonePrev;
        for(int i=0;i<3 && !cut;i++)
            cut=std::abs(now.target[i]-camCur.target[i])>2500 || std::abs(delta4096(now.angles[i],camCur.angles[i]))>500;
        camPrev=camCur; camCur=now; camZonePrev=zone;
        camPrevValid=scene;
        memmove(poses[1],poses[0],sizeof(poses[0])*2);
        posesValid[2]=posesValid[1]; posesValid[1]=posesValid[0];
        readPoses(g,poses[0]); posesValid[0]=scene && !cut;
        memcpy(pieces[1],pieces[0],sizeof(pieces[0]));
        readPieces(g,pieces[0]);
        ok=scene && !cut;
    } else { camPrevValid=false; posesValid[0]=posesValid[1]=posesValid[2]=false; }
    if(!ok) return dispatch(count);
    auto present=*reinterpret_cast<VoidFn*>(PRESENT_CALLBACK);
    present();
    renderExtraFrame(g,0.5f);
    present();
    return 0;
}

static void hookDispatch() {
    static bool done;
    if(done) return;
    done=true;
    auto rebirth=reinterpret_cast<uint8_t*>(GetModuleHandleA("ddraw.dll"));
    if(!rebirth) return;
    auto slot=reinterpret_cast<uintptr_t*>(rebirth+0x14a2);
    auto thunk=rebirth+0x149c;
    if(thunk[0]!=0xff || thunk[1]!=0x25 || *slot!=DISPATCH) return;
    uintptr_t hook=reinterpret_cast<uintptr_t>(dispatchHook);
    applyPatch(slot,reinterpret_cast<uint8_t*>(&hook),sizeof(hook));
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
    if(reason==DLL_PROCESS_ATTACH) { self=module; DisableThreadLibraryCalls(module); hookDirect3D(); }
    return TRUE;
}
