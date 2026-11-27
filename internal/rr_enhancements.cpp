/* Runtime-managed enhancements. No fixed game method RVAs are used here.
 * All controls default off; only metadata-validated owners are modified.
 * Never unload this module after its optional recoil detour is installed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include "game/rr_enhancements.h"
#include "game/rr_threads.h"
#include "game/rr_weapon_patch.h"
#include "game/rr_reveal_patch.h"
#include "memory/rr_mem.h"
#include "game/rr_unity.h"
#include "ui/menu_bridge.h"

struct API {
    void* (*domain)();
    void* (*attach)(void*);
    void* (*current)();
    const void** (*assemblies)(void*, size_t*);
    void* (*image)(const void*);
    void* (*klass)(void*, const char*, const char*);
    void* (*parent)(void*);
    void* (*fields)(void*, void**);
    const char* (*field_name)(void*);
    size_t (*field_offset)(void*);
    int (*field_flags)(void*);
    void* (*field_type)(void*);
    void* (*from_type)(void*);
    const char* (*class_name)(void*);
    bool (*value_type)(void*);
    int32_t (*value_size)(void*, uint32_t*);
    void* (*element)(void*);
    int (*type_kind)(void*);
    void* (*method)(void*, const char*, int);
    void* (*return_type)(void*);
};
static API api;
static SRWLOCK init_lock = SRWLOCK_INIT, fields_lock = SRWLOCK_INIT;
static SRWLOCK feed_lock = SRWLOCK_INIT, log_lock = SRWLOCK_INIT;
static int bound;
static volatile LONG runtime_ready, started, silent_on, recoil_flag;
static volatile LONG status_codes[5];
static volatile LONG silent_input_ready, silent_angles_at, silent_hook_state;
static volatile LONG64 silent_angles;
static volatile LONG silent_packet_count;
static DWORD feed_at;
static int feed_live;
static RR_V3 eye, target;

static void log_line(const char* fmt, ...) {
    AcquireSRWLockExclusive(&log_lock);
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
    if (n && n < MAX_PATH &&
        !strcat_s(path, sizeof path, "\\RobRunInternalUpdated")) {
        CreateDirectoryA(path, NULL);
        if (!strcat_s(path, sizeof path, "\\runtime-offsets.log")) {
            FILE* f = fopen(path, "a+");
            if (f) {
                fseek(f, 0, SEEK_END);
                if (ftell(f) < 4*1024*1024) {
                    fprintf(f, "%lu ", GetTickCount());
                    va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
                    fputc('\n', f);
                }
                fclose(f);
            }
        }
    }
    ReleaseSRWLockExclusive(&log_lock);
}
static int ready() {
    AcquireSRWLockExclusive(&init_lock);
    if (!bound) {
        HMODULE m = GetModuleHandleA("GameAssembly.dll");
        if (!m) { ReleaseSRWLockExclusive(&init_lock); return 0; }
#define LOAD(member, name) api.member = reinterpret_cast<decltype(api.member)>(GetProcAddress(m, "il2cpp_" name))
        LOAD(domain,"domain_get"); LOAD(attach,"thread_attach"); LOAD(current,"thread_current");
        LOAD(assemblies,"domain_get_assemblies"); LOAD(image,"assembly_get_image");
        LOAD(klass,"class_from_name"); LOAD(parent,"class_get_parent");
        LOAD(fields,"class_get_fields"); LOAD(field_name,"field_get_name");
        LOAD(field_offset,"field_get_offset"); LOAD(field_flags,"field_get_flags");
        LOAD(field_type,"field_get_type"); LOAD(from_type,"class_from_type");
        LOAD(class_name,"class_get_name"); LOAD(value_type,"class_is_valuetype");
        LOAD(value_size,"class_value_size"); LOAD(element,"class_get_element_class");
        LOAD(type_kind,"type_get_type"); LOAD(method,"class_get_method_from_name");
        LOAD(return_type,"method_get_return_type");
#undef LOAD
        bound = api.domain && api.attach && api.current && api.assemblies && api.image &&
            api.klass && api.parent && api.fields && api.field_name && api.field_offset &&
            api.field_flags && api.field_type && api.from_type && api.class_name &&
            api.value_type && api.value_size && api.element && api.type_kind &&
            api.method && api.return_type ? 1 : -1;
        log_line("IL2CPP export binding %s", bound == 1 ? "ready" : "FAILED - features disabled");
    }
    int ok = bound == 1;
    ReleaseSRWLockExclusive(&init_lock);
    if (!ok) return 0;
    void* d = api.domain();
    if (!d || (!api.current() && !api.attach(d))) return 0;
    InterlockedExchange(&runtime_ready, 1);
    return 1;
}
static void normalize(const char* in, char out[80]) {
    int j = 0;
    if (!in) { out[0] = 0; return; }
    if (*in == '<') ++in;
    while (*in && *in != '>' && j < 79) {
        char c = *in++;
        if (c == '_') continue;
        if (c >= 'A' && c <= 'Z') c += 'a'-'A';
        out[j++] = c;
    }
    out[j] = 0;
}
struct FIELD { void* klass; void* info; int offset; char name[80]; };
static FIELD field_cache[512];
static int field_count;
static FIELD field_for(void* klass, const char* wanted) {
    FIELD result{}; result.offset = -1;
    if (!klass) return result;
    char key[80]; normalize(wanted, key);
    AcquireSRWLockShared(&fields_lock);
    for (int i=0;i<field_count;++i) if (field_cache[i].klass == klass &&
        !strcmp(field_cache[i].name,key)) {
        result = field_cache[i]; ReleaseSRWLockShared(&fields_lock); return result;
    }
    ReleaseSRWLockShared(&fields_lock);
    for (void* k=klass; k; k=api.parent(k)) {
        void* iterator = NULL;
        while (void* f=api.fields(k,&iterator)) {
            char name[80]; normalize(api.field_name(f),name);
            if (strcmp(name,key) || (api.field_flags(f)&0x10)) continue;
            size_t off=api.field_offset(f);
            if (off < 0x10 || off > 0x10000) return result;
            result.klass=klass; result.info=f; result.offset=(int)off;
            strcpy_s(result.name,key);
            AcquireSRWLockExclusive(&fields_lock);
            if (field_count < 512) field_cache[field_count++] = result;
            ReleaseSRWLockExclusive(&fields_lock);
            log_line("field %s.%s +0x%X", api.class_name(klass),wanted,result.offset);
            return result;
        }
    }
    return result;
}
static void* object_class(uint64_t o) {
    uint64_t k=0;
    return PL(o) && rr_read(o,&k,8) && PL(k) ? (void*)k : NULL;
}
static FIELD field(uint64_t o,const char* name) { return field_for(object_class(o),name); }
static uint64_t pointer(uint64_t o,const char* name,const char* type=NULL) {
    FIELD f=field(o,name);
    uint64_t p=f.offset>=0 ? rr_u64(o+f.offset) : 0;
    return PL(p) && (!type || u_is(p,type)) ? p : 0;
}
static uint64_t primitive_data(uint64_t o) { return pointer(o,"data"); }
static int value_field(uint64_t o,int kind) {
    FIELD f=field(o,"Value");
    return f.offset>=0 && api.type_kind(api.field_type(f.info))==kind ? f.offset : -1;
}
static void* find_class(const char* ns,const char* name) {
    size_t n=0; const void** assemblies=api.assemblies(api.domain(),&n);
    if (!assemblies || n>2048) return NULL;
    for (size_t i=0;i<n;++i) {
        void* k=api.klass(api.image(assemblies[i]),ns,name);
        if (k) return k;
    }
    return NULL;
}
static void* entry(void* method) {
    void* p=NULL; MEMORY_BASIC_INFORMATION mb{};
    if (!method || !rr_read((uint64_t)method,&p,sizeof p) ||
        !VirtualQuery(p,&mb,sizeof mb) || mb.State != MEM_COMMIT ||
        !(mb.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
        return NULL;
    return p;
}

/* Exact-build shot-input call site. Never install against an unchecked build:
 * the old 8 ms model writer can miss the tick in which the packet is built. */
static constexpr uintptr_t kInputRva = 0x0455D820u;
static constexpr uintptr_t kCallRva = 0x00E80038u;
static const unsigned char kCallBytes[8] =
    { 0xE8, 0xE3, 0xD7, 0x6D, 0x03, 0x84, 0xC0, 0x75 };
using TryGetInput = unsigned char (*)(uint64_t, uint64_t, uint64_t, uint64_t);
static TryGetInput original_input;

static unsigned char hooked_input(uint64_t self, uint64_t ts, uint64_t data, uint64_t method) {
    const unsigned char ok = original_input(self, ts, data, method);
    if (!ok || !data || !InterlockedCompareExchange(&silent_on, 0, 0) ||
        !InterlockedCompareExchange(&silent_input_ready, 0, 0) ||
        GetTickCount() - (DWORD)InterlockedCompareExchange(&silent_angles_at, 0, 0) > 100)
        return ok;
    /* BattleInputData is an unboxed value type here: verified boxed field
     * offsets are 0x24, 0x28, 0x2c, 0x3c and 0x48. Only redirect actual shots. */
    if (*(const unsigned char*)(data + 0x2c) != 1) return ok;
    const uint64_t bits = (uint64_t)InterlockedCompareExchange64(&silent_angles, 0, 0);
    float yaw, pitch;
    const uint32_t lo = (uint32_t)bits, hi = (uint32_t)(bits >> 32);
    memcpy(&yaw, &lo, sizeof yaw);
    memcpy(&pitch, &hi, sizeof pitch);
    *(unsigned char*)(data + 0x14) = 1;
    memcpy((void*)(data + 0x18), &yaw, sizeof yaw);
    memcpy((void*)(data + 0x1c), &pitch, sizeof pitch);
    *(int32_t*)(data + 0x38) = 0;
    InterlockedIncrement(&silent_packet_count);
    return ok;
}

static int install_silent_input_hook() {
    LONG state = InterlockedCompareExchange(&silent_hook_state, 0, 0);
    if (state) return state == 1;
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) return 0;
    const uintptr_t base = (uintptr_t)ga;
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        dos->e_lfanew > 0x1000) return 0;
    const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage != 0x0904F000u ||
        nt->FileHeader.TimeDateStamp != 0x6B00114Du) {
        InterlockedExchange(&silent_hook_state, -1);
        log_line("silent input hook disabled: unsupported GameAssembly build");
        return 0;
    }
    void* klass = find_class("Models.Prediction.BattleInput", "BattleInputModel");
    void* data_klass = find_class("Models.Prediction", "BattleInputData");
    if (!klass || !data_klass || !api.value_type(data_klass) ||
        entry(api.method(klass, "TryGetInputData", 2)) != (void*)(base + kInputRva)) {
        InterlockedExchange(&silent_hook_state, -1);
        log_line("silent input hook disabled: method identity mismatch");
        return 0;
    }
    struct Check { const char* name; int offset, kind; };
    const Check checks[] = {
        {"IsLookDirectionInput", 0x24, 2}, {"Lx", 0x28, 12},
        {"Ly", 0x2c, 12}, {"IsShotInput", 0x3c, 2},
        {"ShotLookOffset", 0x48, 8}
    };
    for (const auto& check : checks) {
        FIELD f = field_for(data_klass, check.name);
        if (f.offset != check.offset ||
            api.type_kind(api.field_type(f.info)) != check.kind) {
            InterlockedExchange(&silent_hook_state, -1);
            log_line("silent input hook disabled: %s layout mismatch", check.name);
            return 0;
        }
    }
    const uintptr_t site = base + kCallRva;
    if (site & 7 || memcmp((void*)site, kCallBytes, sizeof kCallBytes)) {
        InterlockedExchange(&silent_hook_state, -1);
        log_line("silent input hook disabled: call bytes mismatch");
        return 0;
    }
    int32_t old_disp;
    memcpy(&old_disp, (const void*)(site + 1), sizeof old_disp);
    if (site + 5 + old_disp != base + kInputRva) {
        InterlockedExchange(&silent_hook_state, -1);
        log_line("silent input hook disabled: call target mismatch");
        return 0;
    }
    original_input = (TryGetInput)(base + kInputRva);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity;
    const uintptr_t aligned = site - site % step;
    unsigned char* relay = nullptr;
    for (uintptr_t d = step; d < 0x40000000 && !relay; d += step) {
        MEMORY_BASIC_INFORMATION mb{};
        if (VirtualQuery((void*)(aligned + d), &mb, sizeof mb) && mb.State == MEM_FREE)
            relay = (unsigned char*)VirtualAlloc((void*)(aligned + d), 4096,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!relay) return 0;
    const unsigned char stub[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(relay, stub, sizeof stub);
    const uintptr_t callback = (uintptr_t)&hooked_input;
    memcpy(relay + 6, &callback, sizeof callback);
    DWORD old_protect = 0;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old_protect)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, 14);
    const int64_t delta = (int64_t)(uintptr_t)relay - (int64_t)(site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return 0;
    }
    unsigned char replacement[8];
    memcpy(replacement, kCallBytes, sizeof replacement);
    const int32_t disp = (int32_t)delta;
    memcpy(replacement + 1, &disp, sizeof disp);
    uint64_t expected, desired;
    memcpy(&expected, kCallBytes, 8);
    memcpy(&desired, replacement, 8);
    if (!VirtualProtect((void*)site, 8, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return 0;
    }
    const uint64_t actual = (uint64_t)InterlockedCompareExchange64(
        (volatile LONG64*)site, (LONG64)desired, (LONG64)expected);
    FlushInstructionCache(GetCurrentProcess(), (const void*)site, 8);
    DWORD ignored;
    VirtualProtect((void*)site, 8, old_protect, &ignored);
    if (actual != expected) {
        VirtualFree(relay, 0, MEM_RELEASE);
        InterlockedExchange(&silent_hook_state, -1);
        log_line("silent input hook disabled: call site changed");
        return 0;
    }
    InterlockedExchange(&silent_hook_state, 1);
    log_line("silent input call armed (shot packets still unverified)");
    return 1;
}
static int64_t unix_ms() {
    FILETIME t; ULARGE_INTEGER n;
    GetSystemTimeAsFileTime(&t); n.LowPart=t.dwLowDateTime; n.HighPart=t.dwHighDateTime;
    return n.QuadPart>=116444736000000000ULL ?
        (int64_t)((n.QuadPart-116444736000000000ULL)/10000ULL) : 0;
}
extern "C" void rr_features_target_state(RR_PLAYER* p) {
    if (!p || !p->addr || !ready()) return;
    uint64_t state=pointer(p->addr,"StateModel","BattleCharacterStateModel");
    uint64_t dead=primitive_data(pointer(state,"isDeadModel"));
    unsigned char flag;
    int off=value_field(dead,2); /* IL2CPP_TYPE_BOOLEAN */
    if (off>=0 && rr_read(dead+off,&flag,1) && flag<=1) p->dead=flag;
    uint64_t knock=pointer(state,"knockoutModel","KnockoutModel");
    uint64_t ts=pointer(knock,"endTs","LongData");
    int64_t end=0, now=unix_ms(); off=value_field(ts,10); /* IL2CPP_TYPE_I8 */
    if (off>=0 && rr_read(ts+off,&end,8) && end>=0 && now>0)
        p->down = end>now && end-now>1000;
}

/* An optional entry detour resolved by exact managed type/method, never by an
 * old RVA. Only the supplied known five-byte prologue is relocatable here.
 * The relay remains allocated until process exit, avoiding use-after-free. */
/* No detour until a verified current-build implementation with a correct
 * trampoline exists. Clearing model floats alone is only diagnostic. */
static int recoil_hook() { return 0; }
static const RR_WEAPON_IO weapon_io={rr_read,rr_write};
static RR_WEAPON_FIELD weapon_fields[9];
static RR_REVEAL_BOOK reveal_book;
static int reveal_valid(const RR_REVEAL_PATCH* p) {
    return PL(p->owner) && PL(p->root) && rr_u64(p->owner)==p->owner_klass &&
        rr_u64(p->root)==p->root_klass && p->address==p->owner+p->owner_offset;
}
static const RR_REVEAL_IO reveal_io={rr_read,rr_write,reveal_valid};
static int reveal_write(uint64_t root,uint64_t owner,int off,float value) {
    if (!root || !owner || off<0) return 0;
    RR_REVEAL_PATCH p{}; p.owner=owner; p.owner_klass=rr_u64(owner);
    p.root=root; p.root_klass=rr_u64(root);
    p.owner_offset=off; p.address=owner+off;
    return rr_reveal_apply(&reveal_book,&p,value,&reveal_io);
}
static int setting_duration(uint64_t root,uint64_t desc,const char* setting) {
    FIELD f=field(desc,setting);
    if (f.offset<0) return 0;
    void* k=api.from_type(api.field_type(f.info));
    FIELD duration=field_for(k,"Duration");
    if (duration.offset<0 || api.type_kind(api.field_type(duration.info))!=12) return 0;
    if (api.value_type(k)) {
        /* IL2CPP field offsets are boxed; the enclosing value is unboxed. */
        return reveal_write(root,desc,f.offset+duration.offset-0x10,0.0f);
    }
    uint64_t settings=rr_u64(desc+f.offset);
    return object_class(settings)==k ? reveal_write(root,settings,duration.offset,0.0f) : 0;
}
static int fast_reveal(uint64_t updater) {
    uint64_t content=pointer(updater,"battleScreenContent","BattleScreenContent");
    uint64_t root=pointer(content,"RevealViewDescriptions","InventoryRevealViewDescriptions");
    if (!root) return 0;
    uint64_t list=pointer(root,"list"), items=pointer(list,"items");
    FIELD size=field(list,"size"); int count=0;
    uint64_t capacity=items ? rr_u64(items+0x18) : 0;
    if (size.offset<0 || !rr_read(list+size.offset,&count,4) ||
        count<0 || count>64 || capacity<(uint64_t)count || capacity>4096) return 0;
    void* element=api.element(object_class(items));
    if (!element) return 0;
    FIELD description=field_for(element,"Description");
    if (description.offset<0) return 0;
    int values=api.value_type(element), stride=8;
    if (values) {
        uint32_t align=0; stride=api.value_size(element,&align);
        if (stride<8 || stride>256 || description.offset<0x10 ||
            description.offset-0x10+8>stride) return 0;
    }
    FIELD time=field(root,"TimeToOneCellBorder");
    if (time.offset<0 || api.type_kind(api.field_type(time.info))!=12 ||
        !reveal_write(root,root,time.offset,0.0001f)) return -1;
    int written=1;
    for (int i=0;i<count;++i) {
        uint64_t cell=items+0x20+(uint64_t)i*stride;
        uint64_t pair=values ? cell : rr_u64(cell);
        if (!pair) continue;
        if (!values && object_class(pair)!=element) continue;
        uint64_t desc=rr_u64(pair+description.offset-(values ? 0x10 : 0));
        if (!u_is(desc,"RevealViewDescription")) continue;
        written+=setting_duration(root,desc,"ShineSettings");
        written+=setting_duration(root,desc,"IconRevealSettings");
    }
    static DWORD last_log;
    if (GetTickCount()-last_log >= 5000) {
        last_log=GetTickCount();
        log_line("reveal root=verified pairs=%d fields=%d saved=%d skipped=%u (readback checked; gameplay unverified)",
            count,written,rr_reveal_pending(&reveal_book),reveal_book.skipped);
    }
    return written;
}
static int anti_flash(uint64_t priv) {
    uint64_t blinded=pointer(priv,"BlindedState","BlindedStateModel");
    if (!blinded) return 0;
    void* k=object_class(blinded);
    void* get=api.method(k,"get_IsBlinded",0), *clear=api.method(k,"ClearBlindedState",0);
    auto get_fn=(bool(*)(uint64_t,void*))entry(get);
    auto clear_fn=(void(*)(uint64_t,void*))entry(clear);
    if (!get_fn || !clear_fn) return 0;
    if (get_fn(blinded,get)) clear_fn(blinded,clear);
    return 1;
}
static int vector_write(uint64_t owner,const char* name,const RR_V3& dir) {
    FIELD f=field(owner,name);
    if (f.offset<0) return 0;
    void* k=api.from_type(api.field_type(f.info)); uint32_t align=0;
    if (!k || !api.value_type(k) || api.value_size(k,&align)!=12) return 0;
    return rr_write(owner+f.offset,&dir,12) ? 1 : -1;
}
static void silent_tick(uint64_t local,uint64_t updater,const RR_V3& origin,const RR_V3& point) {
    RR_V3 delta={point.x-origin.x,point.y-origin.y,point.z-origin.z};
    float len=sqrtf(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    if (!(len>0.001f) || !isfinite(len)) {
        InterlockedExchange(&silent_input_ready, 0);
        InterlockedExchange(&status_codes[4],3);
        return;
    }
    RR_V3 dir={delta.x/len,delta.y/len,delta.z/len};
    int ok=0; BR_Config* b=br_get();
    /* Publish both angles as one atomic word so the game-thread callback
     * cannot observe yaw from one target and pitch from another. */
    const float yaw=atan2f(delta.x,delta.z);
    const float pitch=-atan2f(delta.y,hypotf(delta.x,delta.z));
    uint32_t yaw_bits, pitch_bits;
    memcpy(&yaw_bits,&yaw,4); memcpy(&pitch_bits,&pitch,4);
    const uint64_t angles=(uint64_t)yaw_bits | ((uint64_t)pitch_bits << 32);
    InterlockedExchange64(&silent_angles,(LONG64)angles);
    InterlockedExchange(&silent_angles_at,(LONG)GetTickCount());
    const int input_hook=install_silent_input_hook();
    InterlockedExchange(&silent_input_ready,input_hook ? 1 : 0);
    if (b->silent_path!=1) {
        ok+=vector_write(pointer(local,"LookDirection","LookDirectionModel"),"value",dir)>0;
        ok+=vector_write(pointer(local,"ShotInterpolationModel","ShotInterpolationModel"),"lookDirection",dir)>0;
        ok+=vector_write(pointer(local,"LastLookDirection","LastLookDirectionModel"),"Direction",dir)>0;
    }
    if (b->silent_path!=0) {
        uint64_t model=pointer(updater,"battleModel","BattleModel");
        uint64_t input=pointer(model,"BattleInputModel");
        if (!input) input=pointer(model,"InputModel");
        if (u_is(input,"BattleInputModel")) {
            FIELD look=field(input,"lookDirection"), changed=field(input,"IsLookDirectionChanged"),
                  spread=field(input,"ShotLookOffset");
            float h=hypotf(delta.x,delta.z), limit=1.553343034f;
            float pair[2]={atan2f(delta.x,delta.z),fmaxf(-limit,fminf(limit,-atan2f(delta.y,h)))};
            unsigned char yes=1; int zero=0;
            if (look.offset>=0 && changed.offset>=0 && spread.offset>=0 && h>=0.001f) {
                int wrote=rr_write(input+spread.offset,&zero,4) &&
                    rr_write(input+look.offset,pair,8) && rr_write(input+changed.offset,&yes,1);
                ok+=wrote;
            }
        }
    }
    InterlockedExchange(&status_codes[4],input_hook ? 2 : ok ? 6 : 3);
}
extern "C" void rr_features_feed(int live,const RR_CAMERA* camera) {
    AcquireSRWLockExclusive(&feed_lock);
    feed_live=!!live; feed_at=GetTickCount();
    if (camera) eye=camera->pos;
    ReleaseSRWLockExclusive(&feed_lock);
}
extern "C" void rr_features_silent(int on,RR_V3 point) {
    AcquireSRWLockExclusive(&feed_lock); target=point;
    InterlockedExchange(&silent_on,!!on);
    if (!on) InterlockedExchange(&silent_input_ready,0);
    ReleaseSRWLockExclusive(&feed_lock);
}
static DWORD WINAPI feature_worker(void*) {
    DWORD loot_at=0, printed=0;
    for (;;) {
        if (!ready()) { for (auto& s:status_codes) InterlockedExchange(&s,7); Sleep(500); continue; }
        RR_V3 origin, point; int live;
        AcquireSRWLockShared(&feed_lock);
        origin=eye; point=target; live=feed_live && GetTickCount()-feed_at<=100;
        ReleaseSRWLockShared(&feed_lock);
        BR_Config* b=br_get();
        int recoil=live && b->no_recoil, spread=live && b->no_spread;
        int flash=live && b->anti_flash, loot=live && b->instant_inventory_reveal;
        uint64_t battle=0,updater=0; game_scene_debug(&battle,&updater,NULL);
        uint64_t local=live ? rr_local_player() : 0;
        uint64_t priv=pointer(local,"PrivateModel","BattleCharacterPrivateModel");
        uint64_t player=pointer(battle,"Player","PlayerEntityModel");
        uint64_t client=pointer(player,"ClientModel","PlayerClientModel");
        uint64_t r=recoil ? pointer(client,"RecoilModel","RecoilModel") : 0;
        uint64_t f=spread ? pointer(client,"FakeSpreadModel","PlayerFakeSpreadModel") : 0;
        uint64_t args=spread ? pointer(priv,"WeaponSpreadArgsModel","WeaponSpreadArgsModel") : 0;
        uint64_t data=spread ? primitive_data(pointer(priv,"WeaponSpread")) : 0;
        const uint64_t owners[9]={r,r,f,f,f,data,args,args,args};
        const char* names[9]={"RecoilOffset","RecoilOffset","Spread","Air","Move","Value",
                             "Spread","ShotSpreadOffsetX","ShotSpreadOffsetY"};
        int failed_r=0,failed_s=0, resolved_r=0,resolved_s=0;
        for (int i=0;i<9;++i) {
            FIELD sf=field(owners[i],names[i]); int offset=sf.offset;
            if (i<2) {
                void* type=sf.info ? api.from_type(api.field_type(sf.info)) : NULL;
                uint32_t align=0;
                if (!type || !api.value_type(type) || api.value_size(type,&align)!=8) offset=-1;
                else offset+=i*4;
            } else if (offset>=0 && api.type_kind(api.field_type(sf.info))!=12) offset=-1;
            uint64_t owner=offset>=0 ? owners[i] : 0;
            int success=rr_weapon_zero_field(&weapon_fields[i],owner,(unsigned)(offset>=0 ? offset : 0),&weapon_io);
            if (i<2) { failed_r|=!success; resolved_r+=!!owner; }
            else { failed_s|=!success; if(i>=5) resolved_s+=!!owner; }
        }
        int hooked=recoil && resolved_r==2 && recoil_hook();
        InterlockedExchange(&recoil_flag,hooked && !failed_r);
        InterlockedExchange(&status_codes[0],!b->no_recoil ? 0 : !live ? 5 :
            failed_r ? 4 : resolved_r!=2 ? 3 : hooked ? 2 : 6);
        InterlockedExchange(&status_codes[1],!b->no_spread ? 0 : !live ? 5 :
            failed_s ? 4 : resolved_s==4 ? 2 : 3);
        InterlockedExchange(&status_codes[2],!b->anti_flash ? 0 : !live ? 5 : 1);
        if (flash) {
            int good=0;
            __try { good=anti_flash(priv); } __except(EXCEPTION_EXECUTE_HANDLER) { good=0; }
            InterlockedExchange(&status_codes[2],good ? 2 : 3);
        }
        if (!loot) {
            rr_reveal_restore(&reveal_book,&reveal_io);
            InterlockedExchange(&status_codes[3],!b->instant_inventory_reveal ? 0 : 5);
        } else if (GetTickCount()-loot_at>=500) {
            loot_at=GetTickCount(); rr_reveal_prune(&reveal_book,&reveal_io);
            int count=fast_reveal(updater);
            InterlockedExchange(&status_codes[3],count>0 ? 2 : count<0 ? 4 : 3);
        }
        if (live && InterlockedCompareExchange(&silent_on,0,0)) silent_tick(local,updater,origin,point);
        else {
            InterlockedExchange(&silent_input_ready,0);
            InterlockedExchange(&status_codes[4],0);
        }
        if (GetTickCount()-printed>=5000) {
            printed=GetTickCount();
            log_line("features live=%d recoil=%ld spread=%ld flash=%ld reveal=%ld silent=%ld shot_packets=%ld",
                live,status_codes[0],status_codes[1],status_codes[2],status_codes[3],status_codes[4],
                InterlockedCompareExchange(&silent_packet_count,0,0));
        }
        Sleep(live && (recoil || spread || flash || loot ||
              InterlockedCompareExchange(&silent_on,0,0)) ? 8 : 20);
    }
    return 0;
}
extern "C" int rr_features_start(void) {
    if (InterlockedCompareExchange(&started,1,0)) return 1;
    HANDLE h=rr_thread_start(feature_worker,NULL);
    if (!h) { InterlockedExchange(&started,0); return 0; }
    CloseHandle(h); return 1;
}
extern "C" const char* rr_features_status(int feature) {
    static const char* names[]={"off","waiting","active - effect unverified",
        "model/field unavailable","read/write failed","paused (menu/focus/scene)",
        "data only - hook not verified","IL2CPP API unavailable"};
    if (feature<0 || feature>=5) return "invalid feature";
    LONG s=InterlockedCompareExchange(&status_codes[feature],0,0);
    if (feature == 4 && s == 2) return "shot hook armed - effect unverified";
    return s>=0 && s<8 ? names[s] : "unknown";
}
extern "C" const char* rr_features_runtime_status(void) {
    return InterlockedCompareExchange(&runtime_ready,0,0) ?
        "runtime metadata ready - build effects unverified" : "waiting for IL2CPP metadata";
}
