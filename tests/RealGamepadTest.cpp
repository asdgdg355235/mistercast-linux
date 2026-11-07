// Optional host integration check. Never substitutes for a physical MiSTer test.
#include "input/VirtualGamepad.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <cstring>
#include <array>
using namespace mistercast;
using namespace std::chrono_literals;
int main()
{
    if (::access("/dev/uinput", W_OK) != 0) {
        std::cout << "SKIP kernel gamepad test: /dev/uinput: " << std::strerror(errno) << '\n';
        return 77;
    }
    VirtualGamepad pad;
    std::string error;
    if (!pad.create(0,true,"host-test",error)) { std::cerr << error << '\n'; return 1; }
    int fd=-1;
    const auto deadline=ControllerInput::Clock::now()+2s;
    std::filesystem::path node;
    while (fd<0 && ControllerInput::Clock::now()<deadline) {
        std::error_code ec;
        const auto dir=std::filesystem::path("/sys/devices/virtual/input")/pad.sysname();
        for (const auto& entry:std::filesystem::directory_iterator(dir,ec)) {
            if (entry.path().filename().string().starts_with("event")) {
                node=std::filesystem::path("/dev/input")/entry.path().filename();
                fd=::open(node.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
                break;
            }
        }
        if(fd<0)std::this_thread::sleep_for(10ms);
    }
    if(fd<0){std::cerr<<"Gamepad created, but evdev node inaccessible: "<<node<<'\n';return 1;}
    char name[128]{};
    input_id id{};
    input_absinfo axis{};
    if (::ioctl(fd,EVIOCGNAME(sizeof(name)),name)<0 ||
        ::ioctl(fd,EVIOCGID,&id)<0 || ::ioctl(fd,EVIOCGABS(ABS_X),&axis)<0 ||
        id.bustype!=BUS_VIRTUAL || axis.minimum!=-128 || axis.maximum!=127) return 1;
    std::cout<<"Actual evdev device: "<<node<<" name="<<name<<" bus="<<id.bustype<<" vendor="<<id.vendor<<" product="<<id.product<<'\n';
    std::array<unsigned char, (KEY_MAX+8)/8> keys{};
    if (::ioctl(fd,EVIOCGBIT(EV_KEY,keys.size()),keys.data())<0) return 1;
    auto advertised=[&](unsigned code){return keys[code/8] & (1u<<(code%8));};
    if(!advertised(BTN_THUMBL)||!advertised(BTN_THUMBR)||
        advertised(BTN_TRIGGER_HAPPY1)||advertised(BTN_TRIGGER_HAPPY2))return 1;
    if(!pad.update({0xc000,{}},error)||!pad.update({0x8000,{}},error)||
        !pad.update({},error))return 1;
    std::vector<std::pair<int,int>> clicks;
    const auto clickDeadline=ControllerInput::Clock::now()+2s;
    while(clicks.size()<4 && ControllerInput::Clock::now()<clickDeadline) {
        input_event e[32];const auto n=::read(fd,e,sizeof(e));
        for(ssize_t i=0;i<n/static_cast<ssize_t>(sizeof(input_event));++i)
            if(e[i].type==EV_KEY && (e[i].code==BTN_THUMBL||e[i].code==BTN_THUMBR))
                clicks.emplace_back(e[i].code,e[i].value);
        if(n<0)std::this_thread::sleep_for(10ms);
    }
    if(clicks!=std::vector<std::pair<int,int>>{{BTN_THUMBL,1},{BTN_THUMBR,1},
        {BTN_THUMBL,0},{BTN_THUMBR,0}})return 1;
    const auto eventDeadline=ControllerInput::Clock::now()+2s;
    if(!pad.update({0x30,{-128,127,-1,1}},error) || !pad.update({},error))return 1;
    bool press=false,release=false,negative=false,syn=false;
    while(ControllerInput::Clock::now()<eventDeadline && !(press&&release&&negative&&syn)){
        input_event events[64];
        const auto count=::read(fd,events,sizeof(events));
        for(ssize_t i=0;i<count/static_cast<ssize_t>(sizeof(input_event));++i){
            const auto& e=events[i];
            if(e.type==EV_KEY && e.code==BTN_SOUTH){press|=e.value==1;release|=e.value==0;}
            negative|=e.type==EV_ABS && e.code==ABS_X && e.value==-128;
            syn|=e.type==EV_SYN && e.code==SYN_REPORT;
        }
        if(count<0)std::this_thread::sleep_for(10ms);
    }
    ::close(fd);
    if(!(press&&release&&negative&&syn)){std::cerr<<"Missing real evdev transitions\n";return 1;}
    void* sdl=::dlopen("libSDL2-2.0.so.0",RTLD_NOW);
    if(sdl){
        auto init=reinterpret_cast<int(*)(unsigned)>(::dlsym(sdl,"SDL_Init"));
        auto quit=reinterpret_cast<void(*)()>(::dlsym(sdl,"SDL_Quit"));
        auto count=reinterpret_cast<int(*)()>(::dlsym(sdl,"SDL_NumJoysticks"));
        auto nameFor=reinterpret_cast<const char*(*)(int)>(::dlsym(sdl,"SDL_JoystickNameForIndex"));
        auto isController=reinterpret_cast<int(*)(int)>(::dlsym(sdl,"SDL_IsGameController"));
        if(!init||!quit||!count||!nameFor||!isController||init(0x200)<0)return 1;
        auto open=reinterpret_cast<void*(*)(int)>(::dlsym(sdl,"SDL_JoystickOpen"));
        auto close=reinterpret_cast<void(*)(void*)>(::dlsym(sdl,"SDL_JoystickClose"));
        auto update=reinterpret_cast<void(*)()>(::dlsym(sdl,"SDL_JoystickUpdate"));
        auto getAxis=reinterpret_cast<short(*)(void*,int)>(::dlsym(sdl,"SDL_JoystickGetAxis"));
        auto getButton=reinterpret_cast<unsigned char(*)(void*,int)>(::dlsym(sdl,"SDL_JoystickGetButton"));
        if(!open||!close||!update||!getAxis||!getButton)return 1;
        bool found=false;
        for(int i=0;i<count();++i)if(nameFor(i) && std::string(nameFor(i))==name){
            std::cout<<"SDL2 joystick detected; gamecontroller="<<isController(i)<<'\n';
            void* joystick=open(i);
            if(!joystick || !pad.update({0x10,{-128,127,0,0}},error))return 1;
            const auto until=ControllerInput::Clock::now()+2s;
            while(ControllerInput::Clock::now()<until){
                update();
                if(getAxis(joystick,0)<0 && getAxis(joystick,1)>0 && getButton(joystick,0)) {found=true;break;}
                std::this_thread::sleep_for(10ms);
            }
            if(!pad.update({},error))return 1;
            bool released=false;
            const auto releaseUntil=ControllerInput::Clock::now()+2s;
            while(ControllerInput::Clock::now()<releaseUntil){
                update();
                if(getAxis(joystick,0)==0 && !getButton(joystick,0)) {released=true;break;}
                std::this_thread::sleep_for(10ms);
            }
            close(joystick);found=found&&released;
        }
        quit();::dlclose(sdl);
        if(!found){std::cerr<<"evdev works, SDL2 cannot detect device\n";return 1;}
    }else std::cout<<"SDL2 detection unverified: runtime not installed\n";
    std::cout<<"PASS kernel evdev create/capabilities/press/release/axes\n";
}
