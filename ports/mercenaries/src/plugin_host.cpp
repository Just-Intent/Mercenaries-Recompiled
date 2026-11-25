#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plugin_host.h"
#include "plugin_manifest.h"
#include <memory>
#include <deque>
#include <map>
#include <iterator>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstring>

namespace {
using namespace mercplugins;
struct Loaded {
    Manifest manifest;
    HMODULE module=nullptr;
    MercPluginHost host{};
    MercPlugin callbacks{};
    std::string directory,settings;
    bool entered=false,active=false,tool_open=false;
};
std::vector<std::unique_ptr<Loaded>> loaded;
DWORD owner=0;
void *guest=nullptr;
size_t guest_size=0;
Loaded *calling=nullptr;
bool dispatching=false,initialized=false;
ULONGLONG previous=0;
uint64_t sequence=0;
std::string failure;
std::ofstream log_file;

std::wstring environment(const wchar_t *key) {
    DWORD n=GetEnvironmentVariableW(key,nullptr,0);
    if(!n)return {};
    std::wstring s(n,0);
    if(GetEnvironmentVariableW(key,s.data(),n)>=n)throw std::runtime_error("Plugin environment changed during startup.");
    s.resize(n-1);return s;
}
void record(const std::string &s) {
    if(log_file){log_file<<s<<"\n";log_file.flush();}
    fprintf(stderr,"[plugins] %s\n",s.c_str());
}
void MERC_PLUGIN_CALL plugin_log(void *ctx,const char *message) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||!message)return;
    record(calling->manifest.id+": "+std::string(message));
}
/* Regions already found accessible. Plugins call read/write_guest from hooks
 * on hot functions, where a VirtualQuery per call is too slow. */
struct Region {uintptr_t base,end;bool writable;};
Region region_cache[8];
unsigned region_next=0;
bool guest_ok=false; /* guest RAM is one read-write mapping; checked once */
bool accessible(void *address,size_t bytes,bool write) {
    auto p=reinterpret_cast<uintptr_t>(address),end=p+bytes;
    if(end<p)return false;
    for(const auto &r:region_cache)
        if(r.end&&p>=r.base&&end<=r.end&&(r.writable||!write))return true;
    while(p<end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))||mbi.State!=MEM_COMMIT||
           (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        DWORD protection=mbi.Protect&0xff;
        bool readable=protection==PAGE_READONLY||protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||
                      protection==PAGE_EXECUTE_READ||protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
        bool writable=protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||
                      protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
        if(!readable||(write&&!writable))return false;
        auto next=reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
        region_cache[region_next++%8]={reinterpret_cast<uintptr_t>(mbi.BaseAddress),next,writable};
        if(next<=p)return false;p=next;
    }return true;
}
bool span_raw(uint32_t address,size_t bytes) {
    return guest&&bytes&&address>=0x10000u&&address<guest_size&&bytes<=guest_size-address&&
        (guest_ok||accessible(static_cast<unsigned char*>(guest)+address,bytes,true));
}
bool span(void *ctx,uint32_t address,size_t bytes,bool write) {
    return GetCurrentThreadId()==owner&&calling&&ctx==calling&&guest&&bytes&&
        address>=0x10000u&&address<guest_size&&bytes<=guest_size-address&&
        (guest_ok||accessible(static_cast<unsigned char*>(guest)+address,bytes,write));
}
int MERC_PLUGIN_CALL read_guest(void *ctx,uint32_t address,void *out,size_t bytes) {
    if(!out||!span(ctx,address,bytes,false)||!accessible(out,bytes,true))return 0;
    memmove(out,static_cast<unsigned char*>(guest)+address,bytes);return 1;
}
/* The write guard. A plugin that keeps a pointer into game memory and writes
 * through it later can hit memory the game freed meanwhile and gave to
 * something else: Foundry Console's HUD art put back as a level loaded wrote
 * a texture hash into a new level's actor-pool link, and the next unload
 * hung (Gaza's level-load stops, 0.310). So every address a plugin writes is
 * remembered; the game's frees mark the ones inside a freed block stale; a
 * later write touching a stale one is refused and logged (the plugin's
 * write_guest returns 0). Cleared once each level has loaded. */
struct Touch {Loaded *plugin; uint32_t bytes; bool stale;};
std::map<uint32_t,Touch> touched;
unsigned refusals=0;
bool touched_stale(uint32_t address,size_t bytes) {
    if(touched.empty())return false;
    const uint64_t end=uint64_t(address)+bytes;
    auto it=touched.lower_bound(address);
    if(it!=touched.begin()) {
        auto p=std::prev(it);
        if(p->second.stale&&uint64_t(p->first)+p->second.bytes>address)return true;
    }
    for(;it!=touched.end()&&it->first<end;++it)
        if(it->second.stale)return true;
    return false;
}
int MERC_PLUGIN_CALL write_guest(void *ctx,uint32_t address,const void *data,size_t bytes) {
    if(!data||!span(ctx,address,bytes,true)||!accessible(const_cast<void*>(data),bytes,false))return 0;
    if(touched_stale(address,bytes)) {
        if(refusals++<32) {
            char line[240];
            snprintf(line,sizeof(line),"write guard: %s's write of %u byte(s) at %08X refused: the game freed the "
                     "memory it wrote there before (an object of the last level, or one taken away), so it may "
                     "belong to something else now; look the object up again",
                     calling->manifest.id.c_str(),unsigned(bytes),address);
            record(line);
        }
        return 0;
    }
    if(touched.size()>200000)touched.clear();
    touched[address]=Touch{calling,uint32_t(bytes),false};
    memmove(static_cast<unsigned char*>(guest)+address,data,bytes);return 1;
}
bool updating=false;
int hooking=0;
int in_lua=0;
struct Hook {Loaded *plugin;MercHookFn before,after;void *userdata;};
std::map<unsigned,std::vector<Hook>> hooks;
void call_error(MercGuestCall *call,const char *message) {
    snprintf(call->error,sizeof(call->error),"%s",message);
}
int MERC_PLUGIN_CALL call_guest(void *ctx,MercGuestCall *call) {
    if(!call||!accessible(call,sizeof(MercGuestCall),true))return 0;
    if(call->struct_size<sizeof(MercGuestCall)){call_error(call,"MercGuestCall.struct_size is too small.");return 0;}
    call->error[0]=0;call->eax=call->edx=0;call->st0=0;
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||!(updating||hooking||in_lua)) {
        call_error(call,"Guest calls are only allowed inside on_update, a hook or a script command.");return 0;
    }
    if(call->arg_count>MERC_GUEST_CALL_MAX_ARGS){call_error(call,"Too many arguments.");return 0;}
    return recomp_plugin_call_guest(call->address,call->this_ptr,call->args,call->arg_count,
        &call->eax,&call->edx,&call->st0,call->error,sizeof(call->error));
}
int MERC_PLUGIN_CALL guest_scratch(void *ctx,uint32_t *address,uint32_t *bytes) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||!(updating||hooking||in_lua)||!address||!bytes)return 0;
    return recomp_plugin_guest_scratch(address,bytes);
}
int MERC_PLUGIN_CALL in_gameplay(void *ctx) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling)return 0;
    return recomp_plugin_in_gameplay();
}
int MERC_PLUGIN_CALL hook_guest(void *ctx,uint32_t address,MercHookFn before,MercHookFn after,void *userdata) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||hooking||(!before&&!after))return 0;
    int index=recomp_foundry_hook_index(address);
    if(index<0)return 0;
    hooks[unsigned(index)].push_back({calling,before,after,userdata});
    recomp_foundry_hook_enable(unsigned(index));
    char text[96];snprintf(text,sizeof(text),"hooked 0x%08X",address);
    record(calling->manifest.id+": "+text);
    return 1;
}
void run_hooks(unsigned index,MercHookContext *call,bool after) {
    auto it=hooks.find(index);
    if(it==hooks.end())return;
    Loaded *previous=calling;
    for(auto &h:it->second) {
        MercHookFn fn=after?h.after:h.before;
        if(!fn)continue;
        if(!h.plugin) {fn(h.userdata,call);continue;} /* host-owned hook */
        if(!h.plugin->active)continue;
        calling=h.plugin;++hooking;
        try {fn(h.userdata,call);}
        catch(...) {h.plugin->active=false;record(h.plugin->manifest.name+": a hook threw; the plugin is stopped.");}
        --hooking;
    }
    calling=previous;
}
/* ----- Plugin script commands ----- */
struct LuaCommand {Loaded *plugin;std::string name;MercLuaFn fn;void *userdata;};
std::vector<LuaCommand> lua_commands;
bool lua_hooked=false;
uint32_t guest_call(uint32_t address,unsigned count,const uint32_t *args) {
    uint32_t eax=0,edx=0;double st0=0;char error[96];
    if(!recomp_plugin_call_guest(address,0,args,count,&eax,&edx,&st0,error,sizeof(error)))
        record(std::string("script command registration: ")+error);
    return eax;
}
/* After the game registers its own commands on a script's Lua state, add ours. */
void MERC_PLUGIN_CALL register_lua_commands(void *,MercHookContext *call) {
    uint32_t L=0,scratch=0,size=0;
    if(!span_raw(call->ecx+0x1Cu,4)||lua_commands.empty())return;
    memcpy(&L,static_cast<unsigned char*>(guest)+call->ecx+0x1Cu,4);
    if(!L||!recomp_plugin_guest_scratch(&scratch,&size)||size<64)return;
    for(unsigned i=0;i<lua_commands.size();i++) {
        const auto &c=lua_commands[i];
        memcpy(static_cast<unsigned char*>(guest)+scratch,c.name.c_str(),c.name.size()+1);
        uint32_t name_args[]={L,scratch},fn_args[]={L,FOUNDRY_LUA_BASE+4u*i,0},set_args[]={L,FOUNDRY_LUA_GLOBALSINDEX};
        guest_call(FOUNDRY_LUA_PUSHSTRING,2,name_args);
        guest_call(FOUNDRY_LUA_PUSHCCLOSURE,3,fn_args);
        guest_call(FOUNDRY_LUA_SETTABLE,2,set_args);
    }
}
bool command_name(const char *s) {
    if(!s||!((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')||*s=='_'))return false;
    size_t n=0;
    for(;s[n];n++)if(!((s[n]>='A'&&s[n]<='Z')||(s[n]>='a'&&s[n]<='z')||(s[n]>='0'&&s[n]<='9')||s[n]=='_'))return false;
    return n<64;
}
int MERC_PLUGIN_CALL add_lua_command(void *ctx,const char *name,MercLuaFn fn,void *userdata) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||hooking||in_lua||!fn||!command_name(name))return 0;
    if(lua_commands.size()>=FOUNDRY_LUA_MAX)return 0;
    for(const auto &c:lua_commands)if(c.name==name)return 0;
    if(!lua_hooked) {
        int index=recomp_foundry_hook_index(FOUNDRY_LUASCRIPT_REGISTER_COMMANDS);
        if(index<0)return 0; /* build without hook wrappers */
        hooks[unsigned(index)].push_back({nullptr,nullptr,register_lua_commands,nullptr});
        recomp_foundry_hook_enable(unsigned(index));
        lua_hooked=true;
    }
    lua_commands.push_back({calling,name,fn,userdata});
    record(calling->manifest.id+": added script command "+name);
    return 1;
}
/* Plugin tool windows (counted; the host hears when the first opens and the
 * last closes). */
void (*tool_display)(int)=nullptr;
unsigned tools_open=0;
void set_tool(Loaded &p,bool open) {
    if(p.tool_open==open)return;
    p.tool_open=open;
    unsigned before=tools_open;
    tools_open+=open?1:-1;
    if(tool_display&&(before==0)!=(tools_open==0))tool_display(tools_open?1:0);
}
int MERC_PLUGIN_CALL set_tool_window(void *ctx,int open) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling)return 0;
    set_tool(*calling,open!=0);
    return 1;
}
/* Plugin overlays: one premultiplied BGRA image per plugin, copied in by any
 * thread, composited by the render thread at present. */
struct Overlay {std::vector<uint32_t> pixels;unsigned width=0,height=0,x=0,y=0;};
std::mutex overlay_lock;
std::map<const Loaded*,Overlay> overlays;
int MERC_PLUGIN_CALL set_overlay(void *ctx,const void *pixels,uint32_t width,uint32_t height,uint32_t x,uint32_t y) {
    bool known=false;
    for(auto &entry:loaded)if(entry.get()==ctx&&entry->active)known=true;
    if(!known)return 0;
    std::lock_guard<std::mutex> guard(overlay_lock);
    if(!pixels||!width||!height){overlays.erase(static_cast<Loaded*>(ctx));return 1;}
    if(width>960||height>540||x>=960||y>=540)return 0;
    auto &o=overlays[static_cast<Loaded*>(ctx)];
    o.pixels.assign(static_cast<const uint32_t*>(pixels),static_cast<const uint32_t*>(pixels)+size_t(width)*height);
    o.width=width;o.height=height;o.x=x;o.y=y;
    return 1;
}
/* Keyboard text for a plugin's in-game console: the window procedure hands
 * keys over while a plugin holds text input (main.c). */
void (*text_capture)(int)=nullptr;
Loaded *text_owner=nullptr;
std::deque<uint32_t> text_keys;
void release_text(Loaded *p) {
    std::lock_guard<std::mutex> guard(overlay_lock);
    if(text_owner!=p)return;
    text_owner=nullptr;text_keys.clear();
    if(text_capture)text_capture(0);
}
int MERC_PLUGIN_CALL set_text_input(void *ctx,int on) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling)return 0;
    if(!on){release_text(calling);return 1;}
    std::lock_guard<std::mutex> guard(overlay_lock);
    if(text_owner&&text_owner!=calling)return 0;
    if(!text_owner){text_owner=calling;text_keys.clear();if(text_capture)text_capture(1);}
    return 1;
}
int MERC_PLUGIN_CALL read_text_input(void *ctx,uint32_t *keys,uint32_t max) {
    std::lock_guard<std::mutex> guard(overlay_lock);
    if(!keys||ctx!=text_owner)return 0;
    uint32_t n=0;
    while(n<max&&!text_keys.empty()){keys[n++]=text_keys.front();text_keys.pop_front();}
    return int(n);
}
void shutdown_all() {
    for(auto &entry:loaded)release_text(entry.get());
    {std::lock_guard<std::mutex> guard(overlay_lock);overlays.clear();}
    for(auto &entry:loaded)set_tool(*entry,false);
    for(auto it=loaded.rbegin();it!=loaded.rend();++it) {
        auto &p=**it;
        if(p.entered&&p.callbacks.on_shutdown) {
            calling=&p;
            try {p.callbacks.on_shutdown(p.callbacks.userdata);}
            catch(...) {record(p.manifest.id+": shutdown threw an exception.");}
            calling=nullptr;
        }
        if(p.module)FreeLibrary(p.module);
    }
    loaded.clear();guest=nullptr;guest_size=0;guest_ok=false;initialized=false;dispatching=false;
    if(log_file)log_file.close();
}
}
extern "C" void merc_plugins_guest_freed(uint32_t base,uint32_t bytes) {
    if(touched.empty()||!bytes)return;
    const uint64_t end=uint64_t(base)+bytes;
    auto it=touched.lower_bound(base);
    if(it!=touched.begin()) {
        auto p=std::prev(it);
        if(uint64_t(p->first)+p->second.bytes>base)p->second.stale=true;
    }
    for(;it!=touched.end()&&it->first<end;++it)it->second.stale=true;
}
extern "C" void merc_plugins_level_loaded(void) {touched.clear();}
extern "C" const char *merc_plugins_error(void) {return failure.c_str();}
extern "C" int merc_plugins_initialize(void *base,size_t bytes) {
    if(initialized){failure="Plugin host was initialized twice.";return 0;}
    owner=GetCurrentThreadId();guest=base;guest_size=std::min<size_t>(bytes,0x4000000u);
    sequence=0;failure.clear();
    try {
        auto path=environment(L"MERCENARIES_PLUGIN_LIST");
        if(path.empty()){initialized=true;return 1;}
        auto settings=environment(L"MERCENARIES_PLUGIN_SETTINGS");
        if(settings.empty()||!fs::path(settings).is_absolute())throw std::runtime_error("Missing absolute plugin settings path.");
        fs::path list_path(path);
        if(!list_path.is_absolute()||fs::file_size(list_path)>65536)throw std::runtime_error("Invalid plugin selection file.");
        fs::create_directories(settings);
        log_file.open(fs::path(settings)/L"plugins.log",std::ios::trunc);
        if(!log_file)throw std::runtime_error("Cannot open plugin-settings/plugins.log.");
        std::ifstream input(list_path,std::ios::binary);std::string line;
        std::getline(input,line);
        if(trim(line)!="MERCENARIES_PLUGINS_1")throw std::runtime_error("Unsupported plugin selection file.");
        std::vector<Manifest> manifests;std::set<std::string> seen;
        while(std::getline(input,line)) {
            line=trim(line);
            if(!identifier(line)||!seen.insert(line).second||seen.size()>128)throw std::runtime_error("Invalid or duplicate plugin selection.");
            auto m=read_manifest(list_path.parent_path()/wide(line));
            if(!m||m->id!=line)throw std::runtime_error("Staged plugin manifest does not match "+line);
            manifests.push_back(*m);
        }
        validate(manifests);
        if(!base||guest_size<=0x10000)throw std::runtime_error("Guest RAM is not ready for plugins.");
        guest_ok=accessible(static_cast<unsigned char*>(guest)+0x10000,guest_size-0x10000,true);
        for(auto &m:manifests) {
            auto p=std::make_unique<Loaded>();p->manifest=m;
            p->directory=utf8(m.root.wstring());p->settings=utf8((fs::path(settings)/wide(m.id)).wstring());
            fs::create_directories(fs::path(wide(p->settings)));
            p->host={sizeof(MercPluginHost),MERCENARIES_PLUGIN_API_VERSION,p.get(),
                p->manifest.id.c_str(),p->directory.c_str(),p->settings.c_str(),plugin_log,read_guest,write_guest,
                call_guest,guest_scratch,in_gameplay,hook_guest,add_lua_command,set_tool_window,set_overlay,
                set_text_input,read_text_input};
            p->callbacks.struct_size=sizeof(MercPlugin);p->callbacks.api_version=MERCENARIES_PLUGIN_API_VERSION;
            auto library=m.root/wide(m.library);
            // Own directory for dependencies; no working-directory or PATH search.
            p->module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!p->module)throw std::runtime_error(m.name+": could not load "+m.library+" (Windows error "+std::to_string(GetLastError())+"). Check x64 architecture and dependency DLLs.");
            auto entry=reinterpret_cast<MercPluginEntry>(GetProcAddress(p->module,"MercenariesPlugin_Init"));
            loaded.push_back(std::move(p));
            auto &plugin=*loaded.back();
            if(!entry)throw std::runtime_error(m.name+": DLL does not export MercenariesPlugin_Init.");
            char error[1024]={};plugin.entered=true;calling=&plugin;
            int ok=0;
            try {ok=entry(&plugin.host,&plugin.callbacks,error,sizeof(error));}
            catch(...) {calling=nullptr;throw std::runtime_error(m.name+": initialization threw an exception.");}
            calling=nullptr;error[sizeof(error)-1]=0;
            if(plugin.callbacks.struct_size!=sizeof(MercPlugin)||plugin.callbacks.api_version!=MERCENARIES_PLUGIN_API_VERSION) {
                plugin.entered=false;
                throw std::runtime_error(m.name+": invalid plugin callback ABI.");
            }
            if(!ok)throw std::runtime_error(m.name+": initialization failed. "+error);
            plugin.active=true;record(m.name+" ["+m.id+"]: initialized");
        }
        previous=GetTickCount64();initialized=true;return 1;
    }catch(const std::exception &e) {
        calling=nullptr;failure=e.what();record(failure);shutdown_all();return 0;
    }
}
extern "C" int merc_plugins_update(void) {
    if(!initialized||GetCurrentThreadId()!=owner||dispatching||loaded.empty())return 1;
    dispatching=true;
    ULONGLONG now=GetTickCount64();
    MercPluginUpdate update{sizeof(MercPluginUpdate),MERCENARIES_PLUGIN_API_VERSION,++sequence,double(now-previous)/1000.0};
    previous=now;std::set<std::string> stopped;failure.clear();
    for(auto &entry:loaded) {
        auto &p=*entry;
        if(!p.active)continue;
        bool dependency_failed=false;
        for(const auto &id:p.manifest.requires)if(stopped.count(id))dependency_failed=true;
        if(dependency_failed) {
            p.active=false;stopped.insert(p.manifest.id);record(p.manifest.name+": updates stopped because a required plugin failed.");continue;
        }
        if(!p.callbacks.on_update)continue;
        calling=&p;int ok=0;updating=true;
        try {ok=p.callbacks.on_update(p.callbacks.userdata,&update);}catch(...) {ok=0;}
        updating=false;calling=nullptr;
        if(!ok) {
            p.active=false;stopped.insert(p.manifest.id);set_tool(p,false);release_text(&p);
            if(!failure.empty())failure+="\n";
            failure+=p.manifest.name+": update failed; this plugin and its dependents will receive no further updates. Restart the game after correcting the mod.";
            record(failure);
        }
    }
    dispatching=false;return failure.empty()?1:0;
}
extern "C" int merc_plugins_hook_ready(void) {
    return initialized&&GetCurrentThreadId()==owner;
}
extern "C" void merc_plugins_hook_before(unsigned index,MercHookContext *call) {run_hooks(index,call,false);}
extern "C" void merc_plugins_hook_after(unsigned index,MercHookContext *call) {run_hooks(index,call,true);}
extern "C" void merc_plugins_set_tool_display(void (*callback)(int)) {tool_display=callback;}
extern "C" void merc_plugins_set_text_capture(void (*callback)(int)) {text_capture=callback;}
extern "C" int merc_plugins_text_key(unsigned message,unsigned key) {
    std::lock_guard<std::mutex> guard(overlay_lock);
    if(!text_owner)return 0;
    uint32_t value=0;
    if(message==WM_CHAR) {
        if(key>=32&&key!=127)value=key;          /* printable; control characters come as keys */
    } else if(message==WM_KEYDOWN) {
        switch(key) {
        case VK_RETURN:case VK_ESCAPE:case VK_BACK:case VK_TAB:case VK_DELETE:
        case VK_HOME:case VK_END:case VK_LEFT:case VK_RIGHT:case VK_UP:case VK_DOWN:
            value=MERC_KEY_FLAG|key;break;
        default:break;
        }
    } else return 0;
    if(value&&text_keys.size()<256)text_keys.push_back(value);
    return 1;                                    /* the game doesn't see typing */
}
extern "C" void merc_plugins_each_overlay(void (*fn)(const void*,unsigned,unsigned,unsigned,unsigned)) {
    std::lock_guard<std::mutex> guard(overlay_lock);
    for(auto &entry:overlays) {
        const Overlay &o=entry.second;
        if(entry.first->active&&!o.pixels.empty())fn(o.pixels.data(),o.width,o.height,o.x,o.y);
    }
}
extern "C" unsigned merc_plugins_lua_count(void) {return unsigned(lua_commands.size());}
extern "C" int merc_plugins_lua_command(unsigned index,uint32_t L) {
    if(!initialized||GetCurrentThreadId()!=owner||index>=lua_commands.size())return 0;
    auto &c=lua_commands[index];
    if(!c.plugin->active)return 0;
    Loaded *previous=calling;
    calling=c.plugin;++in_lua;int results=0;
    try {results=c.fn(c.userdata,L);}
    catch(...) {c.plugin->active=false;record(c.plugin->manifest.name+": script command "+c.name+" threw; the plugin is stopped.");}
    --in_lua;calling=previous;
    return results;
}
extern "C" void merc_plugins_shutdown(void) {
    if(GetCurrentThreadId()!=owner||dispatching)return;
    hooks.clear(); /* before any plugin DLL is unloaded */
    lua_commands.clear();
    dispatching=true;shutdown_all();
}


extern "C" void merc_plugins_show_error(void) {
    int n=MultiByteToWideChar(CP_UTF8,0,failure.c_str(),-1,nullptr,0);
    std::wstring text(n ? n : 1,0);
    if(n)MultiByteToWideChar(CP_UTF8,0,failure.c_str(),-1,text.data(),n);
    MessageBoxW(nullptr,text.c_str(),L"Plugin error",MB_OK|MB_ICONERROR);
}
