#include "input/VirtualGamepad.h"
#include "input/ControllerReceiver.h"
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <cstdarg>
#include <algorithm>
#include <bit>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

using namespace mistercast;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
struct Device {
    uinput_setup setup{};
    std::map<int, input_absinfo> axes;
    std::vector<int> keys, types;
    std::vector<input_event> events;
    bool created{}, destroyed{};
};
std::mutex mutex;
std::condition_variable changed;
std::map<int, Device> devices;
int nextFd = 100000;
bool failOpen{}, failWrite{};
std::vector<std::string> messages;
bool hasEvent(const std::vector<input_event>& events, int type, int code, int value)
{
    for (auto& e : events) if (e.type == type && e.code == code && e.value == value) return true;
    return false;
}
std::vector<std::uint8_t> wire(std::uint32_t frame, std::uint8_t order, std::uint16_t b0,
                             std::uint16_t b1, bool analog = false)
{
    std::vector<std::uint8_t> b(analog ? 17 : 9);
    for (unsigned i=0; i<4; ++i) b[i] = frame >> (8*i);
    b[4]=order; b[5]=b0; b[6]=b0>>8; b[7]=b1; b[8]=b1>>8;
    if (analog) { b[9]=128; b[10]=127; b[11]=255; b[12]=1; b[13]=42; b[14]=214; }
    return b;
}
void unit()
{
    auto b=wire(10,1,0x3ff0,0x401,true);
    auto p=decodeControllerPacket(b);
    check(p && p->analog && p->axes[0]==std::array{-128,127,-1,1} && p->axes[1]==std::array{42,-42,0,0}, "signed analog decoding and independent slots");
    for (unsigned n=0;n<100;++n) if(n!=9 && n!=17)
        check(!decodeControllerPacket(std::vector<std::uint8_t>(n)), "malformed length");
    auto s=gamepadState(*p,0);
    auto events=gamepadEvents({},s,true);
    check(hasEvent(events,EV_KEY,BTN_SOUTH,1) && hasEvent(events,EV_KEY,BTN_TR2,1), "all digital buttons incl triggers");
    check(hasEvent(events,EV_ABS,ABS_X,-128) && hasEvent(events,EV_ABS,ABS_Y,127), "analog signs");
    check(events.back().type==EV_SYN && events.back().code==SYN_REPORT, "coherent SYN report");
    check(gamepadEvents(s,s,true).empty(), "unchanged holds must emit no duplicate events");
    auto release=gamepadEvents(s,{},true);
    check(hasEvent(release,EV_KEY,BTN_SOUTH,0) && hasEvent(release,EV_ABS,ABS_X,0), "release resets axes and keys");
    p=decodeControllerPacket(wire(11,1,0xf,0));
    check(!p->analog && gamepadState(*p,0).axes==std::array{0,0,0,0}, "opposing directions neutral");
    p->buttons[0]=2|8;
    s=gamepadState(*p,0);
    events=gamepadEvents({},s,false);
    check(hasEvent(events,EV_ABS,ABS_HAT0X,-1) && hasEvent(events,EV_ABS,ABS_X,-1), "digital hat and detection axes");
    p->buttons[0]=1|4;
    events=gamepadEvents(s,gamepadState(*p,0),false);
    check(hasEvent(events,EV_ABS,ABS_HAT0Y,1), "rapid directional reversal");
    const auto t=GamepadInput::Clock::now();
    GamepadInput input(controllerTimeout(30));
    p->frame=0xffffffff; p->order=255;
    check(input.update(*p,t),"first snapshot");
    check(!input.update(*p,t+1s),"duplicate");
    p->frame=0;p->order=0;
    check(input.update(*p,t+2s),"rollover");
    check(!input.expire(t+92s),"holds survive 2700 blits at 30Hz");
    p->order=1;
    check(input.update(*p,t+92s),"idle keepalive");
    check(!input.expire(t+196s) && input.expire(t+197s),"cadence watchdog");
    p->frame=1;
    check(input.update(*p,t+198s),"sender reset recovery");
    p->frame=0;
    check(!input.update(*p,t+199s),"reordered old frame");
    check(controllerTimeout(60)==60s && controllerTimeout(50)==69s && controllerTimeout(30)==105s,"watchdog derivation");
}
void kernelApi()
{
    std::string error;
    VirtualGamepad pad;
    check(pad.create(0,true,"test",error),"create");
    int fd=nextFd-1;
    {
        auto& d=devices.at(fd);
        check(d.created && d.setup.id.bustype==BUS_VIRTUAL && !d.setup.id.vendor && !d.setup.id.product,"virtual identity");
        check(std::string(d.setup.name)=="MiSTerCast Gamepad 1","name");
        check(d.axes.at(ABS_RX).minimum==-128 && d.axes.at(ABS_RX).maximum==127 && d.axes.at(ABS_RX).flat==0,"axis precision/deadzone");
        check(d.axes.size()==6,"analog capability count");
        check(std::find(d.types.begin(),d.types.end(),EV_SYN)!=d.types.end() &&
              std::find(d.types.begin(),d.types.end(),EV_FF)==d.types.end(),"SYN enabled, no unsupported rumble");
    }
    GamepadState s{0x30,{-128,127,-1,1}};
    check(pad.update(s,error) && pad.update(s,error),"write held");
    pad.close();
    check(devices.at(fd).destroyed && hasEvent(devices.at(fd).events,EV_KEY,BTN_SOUTH,0),"shutdown releases before destroy");
    check(pad.create(1,false,"test",error),"digital create");
    fd=nextFd-1;
    check(devices.at(fd).axes.size()==4 && devices.at(fd).axes.at(ABS_X).maximum==1,"digital no fabricated analog");
    failWrite=true;
    check(!pad.update(s,error) && !pad.exists() && devices.at(fd).destroyed,"write failure destroys stale device");
    failWrite=false; failOpen=true;
    check(!pad.create(0,true,"test",error) && error.find("/dev/uinput")!=std::string::npos,"permissions diagnostic");
    failOpen=false;
}
// Exercise wire decoding, slot isolation, capabilities and delivery together.
void allButtonBindings()
{
    constexpr std::array expected{BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
        BTN_SELECT, BTN_START, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
        BTN_THUMBL, BTN_THUMBR};
    for (bool analog : {false, true}) {
        std::array<VirtualGamepad, 2> pads;
        std::array<int, 2> fds{};
        std::string error;
        for (unsigned slot=0; slot<2; ++slot) {
            check(pads[slot].create(slot,analog,"bindings",error),"create binding test pad");
            fds[slot]=nextFd-1;
            const auto& keys=devices.at(fds[slot]).keys;
            check(keys==std::vector<int>(expected.begin(),expected.end()),"all twelve advertised codes in original order; no trigger-happy aliases");
        }
        auto deliver=[&](std::uint16_t b0,std::uint16_t b1) {
            auto packet=decodeControllerPacket(wire(20,1,b0,b1,analog));
            check(packet.has_value(),"decode binding fixture");
            for(unsigned slot=0;slot<2;++slot)
                check(pads[slot].update(gamepadState(*packet,slot),error),"deliver binding state");
        };
        for(unsigned slot=0;slot<2;++slot) {
            auto& events=devices.at(fds[slot]).events;
            auto& other=devices.at(fds[1-slot]).events;
            for(unsigned i=0;i<expected.size();++i) {
                events.clear();other.clear();
                const auto mask=static_cast<std::uint16_t>(1u<<(i+4));
                deliver(slot==0?mask:0,slot==1?mask:0);
                check(hasEvent(events,EV_KEY,expected[i],1),"wire mask press binding");
                check(std::count_if(events.begin(),events.end(),[](auto e){return e.type==EV_KEY;})==1,"one wire bit produces one button press");
                check(!std::any_of(other.begin(),other.end(),[](auto e){return e.type==EV_KEY;}),"no crossover between controller slots");
                const auto count=events.size();
                deliver(slot==0?mask:0,slot==1?mask:0);
                check(events.size()==count,"held mask produces no duplicate events");
                deliver(0,0);
                check(hasEvent(events,EV_KEY,expected[i],0),"wire mask release binding");
            }
            for (auto masks : {std::array<std::uint16_t,2>{0x4000,0x8000},
                               std::array<std::uint16_t,2>{0x8000,0x4000},
                               std::array<std::uint16_t,2>{0x0020,0x0040}}) {
                deliver(slot==0?(masks[0]|masks[1]):0,slot==1?(masks[0]|masks[1]):0);
                events.clear();
                deliver(slot==0?masks[1]:0,slot==1?masks[1]:0);
                const int released=expected[std::countr_zero(masks[0])-4];
                const int held=expected[std::countr_zero(masks[1])-4];
                check(hasEvent(events,EV_KEY,released,0) &&
                    !hasEvent(events,EV_KEY,held,0),"partial release preserves other stick click or Button 2/3");
                deliver(0,0);
            }
        }
        deliver(0xc000,0xc000);
        for(unsigned slot=0;slot<2;++slot) {
            auto& d=devices.at(fds[slot]);d.events.clear();
            pads[slot].close();
            check(d.destroyed && hasEvent(d.events,EV_KEY,BTN_THUMBL,0) &&
                hasEvent(d.events,EV_KEY,BTN_THUMBR,0),"shutdown releases both stick clicks and destroys each pad");
        }
    }
}
void receiver()
{
    const int server=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    check(server>=0,"UDP socket");
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(::bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"UDP bind");
    socklen_t size=sizeof(address);
    ::getsockname(server,reinterpret_cast<sockaddr*>(&address),&size);
    auto report=[](const std::string& s){std::lock_guard lock(mutex);messages.push_back(s);changed.notify_all();};
    for(unsigned cycle=0;cycle<3;++cycle) {
        ControllerReceiver receiver, competing;
        std::string error;
        int base;
        { std::lock_guard lock(mutex); base=nextFd; messages.clear(); }
        if (!receiver.start("127.0.0.1","",report,error,ntohs(address.sin_port),true)) throw std::runtime_error(error);
        pollfd poller{server,POLLIN,0}; check(::poll(&poller,1,1000)>0,"registration received");
        sockaddr_in client{};size=sizeof(client);char registration[2];
        check(::recvfrom(server,registration,2,0,reinterpret_cast<sockaddr*>(&client),&size)==1,"one-byte registration");
        check(!competing.start("127.0.0.1","",report,error,ntohs(address.sin_port),true),"competing receiver must fail before registration");
        auto send=[&](auto b){check(::sendto(server,b.data(),b.size(),0,reinterpret_cast<sockaddr*>(&client),size)==static_cast<ssize_t>(b.size()),"send fixture");};
        receiver.activate();
        send(wire(10,1,0x30,0x800,true));
        auto waitFor=[&](auto predicate){std::unique_lock lock(mutex);check(changed.wait_for(lock,2s,predicate),"receiver state timeout");};
        waitFor([&]{return devices.count(base+1) && hasEvent(devices.at(base+1).events,EV_KEY,BTN_TR,1);});
        {std::lock_guard lock(mutex);
            check(hasEvent(devices.at(base).events,EV_KEY,BTN_SOUTH,1),"slot0 combination");
            check(!hasEvent(devices.at(base+1).events,EV_KEY,BTN_SOUTH,1),"slot isolation");}
        send(wire(9,200,0,0,true)); // stale must not release
        send(std::vector<std::uint8_t>(18));
        send(wire(11,1,0x30,0x800,true)); // unchanged keepalive
        // Barrier: a valid slot1 transition proves preceding stale/hold packets drained.
        send(wire(11,2,0x30,0x820,true));
        waitFor([&]{return hasEvent(devices.at(base+1).events,EV_KEY,BTN_EAST,1);});
        {std::lock_guard lock(mutex);
            const auto& e=devices.at(base).events;
            check(!hasEvent(e,EV_KEY,BTN_SOUTH,0),"stale input must not release held buttons");
            check(std::count_if(e.begin(),e.end(),[](const auto& v){return v.type==EV_KEY && v.code==BTN_SOUTH && v.value==1;})==1,"hold must not repeat press");}
        const int wrong=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
        const auto spoof=wire(9999,1,0xffff,0xffff,true);
        ::sendto(wrong,spoof.data(),spoof.size(),0,reinterpret_cast<sockaddr*>(&client),size);
        ::close(wrong);
        waitFor([&]{return std::any_of(messages.begin(),messages.end(),[](const auto& m){return m.find("unexpected endpoint")!=std::string::npos;});});
        send(wire(12,1,0,0,true));
        waitFor([&]{return hasEvent(devices.at(base).events,EV_KEY,BTN_SOUTH,0);});
        send(wire(13,1,0x10,0x20,true));
        send(wire(14,1,0x10,0x20)); // analog-to-digital mode change
        waitFor([&]{return devices.count(base+3) && hasEvent(devices.at(base+3).events,EV_KEY,BTN_EAST,1);});
        {std::lock_guard lock(mutex);
            check(devices.at(base).destroyed && devices.at(base+1).destroyed,"mode recreation releases old pads");
            check(devices.at(base+2).axes.size()==4,"digital receiver capabilities");}
        receiver.stop();receiver.stop();
        {std::lock_guard lock(mutex);check(devices.at(base+2).destroyed && devices.at(base+3).destroyed,"both slots destroyed on stop");}
    }
    ::close(server);
}
}
extern "C" int __real_open(const char*,int,...);
extern "C" int __real_close(int);
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __real_ioctl(int,unsigned long,...);
extern "C" int __wrap_open(const char* path,int flags,...)
{
    if(std::strcmp(path,"/dev/uinput")) {
        va_list args;va_start(args,flags);
        const int mode=(flags & O_CREAT) ? va_arg(args,int) : 0;
        va_end(args);return __real_open(path,flags,mode);
    }
    std::lock_guard lock(mutex);
    if(failOpen){errno=EACCES;return -1;}
    const int fd=nextFd++;devices[fd]={};return fd;
}
extern "C" int __wrap_close(int fd)
{
    if(fd<100000) return __real_close(fd);
    return 0;
}
extern "C" int __wrap_ioctl(int fd,unsigned long request,...)
{
    va_list args;va_start(args,request);
    if(fd<100000){void* value=va_arg(args,void*);va_end(args);return __real_ioctl(fd,request,value);}
    std::lock_guard lock(mutex);auto& d=devices.at(fd);
    if(request==UI_SET_EVBIT)d.types.push_back(va_arg(args,int));
    else if(request==UI_SET_KEYBIT)d.keys.push_back(va_arg(args,int));
    else if(request==UI_DEV_SETUP)d.setup=*va_arg(args,uinput_setup*);
    else if(request==UI_ABS_SETUP){auto* a=va_arg(args,uinput_abs_setup*);d.axes[a->code]=a->absinfo;}
    else if(request==UI_DEV_CREATE)d.created=true;
    else if(request==UI_DEV_DESTROY)d.destroyed=true;
    else if(_IOC_NR(request)==_IOC_NR(UI_GET_SYSNAME(128))) std::strcpy(va_arg(args,char*),"input-test");
    va_end(args);changed.notify_all();return 0;
}
extern "C" ssize_t __wrap_write(int fd,const void* bytes,size_t size)
{
    if(fd<100000)return __real_write(fd,bytes,size);
    std::lock_guard lock(mutex);
    if(failWrite){errno=EIO;return -1;}
    auto* e=static_cast<const input_event*>(bytes);
    devices.at(fd).events.insert(devices.at(fd).events.end(),e,e+size/sizeof(input_event));
    changed.notify_all();return size;
}
int main()
{
    try {unit();kernelApi();allButtonBindings();receiver();std::cout<<"PASS packet/state, uinput syscall harness, UDP receiver and repeated sessions\n";}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
