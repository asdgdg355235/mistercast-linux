#include "core/FrameProcessor.h"
#include "core/ScalingExecutor.h"
#include "core/LancIRRuntime.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int lancirReferenceResize(avir::CLancIR&, const std::uint8_t*, int, int,
                         std::uint8_t*, int, int, int, const avir::CLancIRParams&);

using namespace mistercast;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

namespace {
std::vector<std::uint8_t> pixels(int width, int height, int channels = 3, unsigned seed = 123)
{
    std::vector<std::uint8_t> result(width * height * channels);
    for (auto& v : result) {
        seed = seed * 1664525u + 1013904223u;
        v = seed >> 24;
    }
    return result;
}
std::vector<std::uint8_t> reference(const std::uint8_t* input, int sw, int sh,
                                  int ow, int oh, int channels = 3, int stride = 0)
{
    avir::CLancIR resizer;
    avir::CLancIRParams params;
    params.SrcSSize = stride;
    std::vector<std::uint8_t> output(ow * oh * channels);
    CHECK(lancirReferenceResize(resizer,input,sw,sh,output.data(),ow,oh,channels,params) == oh);
    return output;
}

void fullReferenceAndReuse()
{
    const std::array<std::array<int,4>,11> dimensions{{
        {37,29,13,17}, {37,29,13,17}, {7,5,23,19}, {37,29,13,17},
        {31,3,19,40}, {1,1,7,9},
        {13,17,13,17}, {43,31,17,11}, {37,29,13,17}, {1920,1440,720,576}, {7,5,3,1}}};
    for (const auto n : {1,2,7}) {
        ScalingExecutor executor(n);
        CHECK(executor.workerCount() == std::size_t(n));
        CHECK(executor.threadsStarted() == std::size_t(n));
        unsigned seed = 9;
        for (int repeat = 0; repeat < 2; ++repeat) for (const auto& d : dimensions) {
            const auto source = pixels(d[0],d[1],3,seed++);
            const auto expected = reference(source.data(),d[0],d[1],d[2],d[3]);
            std::vector<std::uint8_t> output(expected.size());
            CHECK(FrameProcessor::resizeBgr(executor,source,d[0],d[1],output,d[2],d[3],ScalingAlgorithm::LancIR));
            CHECK(output == expected);
            CHECK(executor.threadsStarted() == std::size_t(n));
        }
    }
}

void stripEquivalence()
{
    // Calls the production strip adapter on persistent independent CLancIRs.
    // Includes the 3 -> 40 phase-rounding regression and odd/uneven partitions.
    std::array<detail::LancIRRuntime,7> strips;
    avir::CLancIR full;
    for (const auto channels : {3,4}) for (int sh = 1; sh <= 41; ++sh) {
        const auto source = pixels(31,sh,channels);
        for (int oh = 1; oh <= 47; ++oh) {
            std::vector<std::uint8_t> expected(19*oh*channels), output(expected.size());
            CHECK(full.resizeImage(source.data(),31,sh,expected.data(),19,oh,channels) == oh);
            for (const auto n : {1,2,7}) {
                const auto active = std::min(n,oh);
                for (int i = 0; i < active; ++i) {
                    const auto begin = oh*i/active, end = oh*(i+1)/active;
                    CHECK(strips[i].resizeStrip(source.data(),31,sh,31*channels,
                        output.data()+begin*19*channels,19,oh,channels,begin,end) == end-begin);
                }
                CHECK(output == expected);
            }
        }
    }
}

struct Encoded {
    std::vector<std::uint8_t> storage;
    int width, height, stride;
    PixelFormat format;
    bool negative;
    SourceFrame frame() const {
        return {storage.data()+(negative ? (height-1)*stride : 0),
                static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),
                negative ? -stride : stride,format};
    }
};
Encoded encode(const std::vector<std::uint8_t>& bgr, int w, int h, PixelFormat format,
               bool negative, int alphaSeed)
{
    const int channels = format == PixelFormat::Bgr || format == PixelFormat::Rgb ? 3 : 4;
    const bool rgb = format == PixelFormat::Rgb || format == PixelFormat::Rgbx || format == PixelFormat::Rgba;
    const int stride = w*channels+11;
    Encoded result{std::vector<std::uint8_t>(stride*h,0xfd),w,h,stride,format,negative};
    for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) {
        auto* p = result.storage.data()+(negative ? h-1-y : y)*stride+x*channels;
        const auto* src = bgr.data()+(y*w+x)*3;
        p[rgb?2:0]=src[0]; p[1]=src[1]; p[rgb?0:2]=src[2];
        if (channels==4) p[3]=(x*79+y*37+alphaSeed)%256;
    }
    return result;
}

void formatsStridesAndThreadCounts()
{
    ScalingExecutor single(1), two(2), seven(7);
    const auto bgr = pixels(31,23);
    for (const auto& descriptor : availableScalers()) {
        for (const auto rotation : {Rotation::None,Rotation::Clockwise90,Rotation::CounterClockwise90,Rotation::Flip180}) {
            for (const auto width : {13,45}) {
                FramingSettings settings;
                settings.scaling=descriptor.algorithm;
                settings.rotation=rotation;
                settings.offsetX=2; settings.offsetY=-1;
                std::vector<std::uint8_t> expected(width*17*3);
                CHECK(FrameProcessor::convertToBgr(single,{bgr.data(),31,23,0,PixelFormat::Bgr},expected,width,17,settings));
                for (const auto format : {PixelFormat::Bgr,PixelFormat::Bgrx,PixelFormat::Bgra,
                                          PixelFormat::Rgb,PixelFormat::Rgbx,PixelFormat::Rgba}) {
                    for (const bool negative : {false,true}) for (const int alpha : {0,101}) {
                        const auto image=encode(bgr,31,23,format,negative,alpha);
                        for (auto* executor : {&single,&two,&seven}) {
                            std::vector<std::uint8_t> output(expected.size());
                            CHECK(FrameProcessor::convertToBgr(*executor,image.frame(),output,width,17,settings));
                            CHECK(output==expected);
                        }
                    }
                }
            }
        }
    }
}

void cropAndRotationReference()
{
    struct Case {
        int width,height,x,y,cw,ch;
        HorizontalAlignment horizontal;
        VerticalAlignment vertical;
        int ox,oy;
        Rotation rotation;
    };
    using H=HorizontalAlignment; using V=VerticalAlignment; using R=Rotation;
    const std::array cases{
        Case{20,8,5,0,10,8,H::Center,V::Center,0,0,R::None},
        Case{20,8,0,0,10,8,H::Left,V::Center,0,0,R::None},
        Case{20,8,10,0,10,8,H::Right,V::Center,0,0,R::None},
        Case{20,8,3,0,10,8,H::Left,V::Center,3,0,R::Flip180},
        Case{8,20,0,7,8,6,H::Center,V::Center,0,0,R::None},
        Case{8,20,0,0,8,6,H::Center,V::Top,0,0,R::None},
        Case{8,20,0,14,8,6,H::Center,V::Bottom,0,0,R::None},
        Case{8,20,0,12,8,6,H::Center,V::Bottom,0,-2,R::Flip180},
        Case{17,20,1,0,15,20,H::Center,V::Center,0,0,R::Clockwise90},
        Case{17,20,2,0,15,20,H::Left,V::Center,2,0,R::CounterClockwise90}};
    ScalingExecutor one(1), many(7);
    for (const auto& c : cases) {
        const auto source=pixels(c.width,c.height);
        std::vector<std::uint8_t> crop(c.cw*c.ch*3);
        for (int y=0;y<c.ch;++y) std::copy_n(source.data()+((y+c.y)*c.width+c.x)*3,
                                            c.cw*3,crop.data()+y*c.cw*3);
        constexpr int ow=13,oh=9;
        const bool quarter=c.rotation==R::Clockwise90||c.rotation==R::CounterClockwise90;
        const int pw=quarter?oh:ow,ph=quarter?ow:oh;
        const auto resized=reference(crop.data(),c.cw,c.ch,pw,ph);
        std::vector<std::uint8_t> expected(ow*oh*3);
        // Independent forward rotation of each pre-rotation source pixel.
        for (int y=0;y<ph;++y) for(int x=0;x<pw;++x) {
            int dx=x,dy=y;
            if(c.rotation==R::Clockwise90){dx=ph-1-y;dy=x;}
            if(c.rotation==R::CounterClockwise90){dx=y;dy=pw-1-x;}
            if(c.rotation==R::Flip180){dx=pw-1-x;dy=ph-1-y;}
            std::copy_n(resized.data()+(y*pw+x)*3,3,expected.data()+(dy*ow+dx)*3);
        }
        FramingSettings settings{c.horizontal,c.vertical,c.ox,c.oy,c.rotation,ScalingAlgorithm::LancIR};
        for(auto* executor:{&one,&many}) {
            std::vector<std::uint8_t> output(expected.size());
            CHECK(FrameProcessor::convertToBgr(*executor,{source.data(),static_cast<unsigned>(c.width),
                static_cast<unsigned>(c.height),0,PixelFormat::Bgr},output,ow,oh,settings));
            CHECK(output==expected);
        }
    }
}

void lifecycleAndErrors()
{
    CHECK(ScalingExecutor::defaultWorkerCount()==std::max(1u,std::thread::hardware_concurrency()));
    ScalingExecutor executor(3);
    const auto source=pixels(37,29);
    std::vector<std::uint8_t> output(13*17*3);
    for (const auto& descriptor : availableScalers()) {
        CHECK(!FrameProcessor::resizeBgr(executor,std::span(source).first(3),37,29,output,13,17,descriptor.algorithm));
        CHECK(FrameProcessor::resizeBgr(executor,source,37,29,output,13,17,descriptor.algorithm));
    }
    CHECK(!FrameProcessor::resizeBgr(executor,source,37,29,output,13,17,static_cast<ScalingAlgorithm>(99)));
    CHECK(executor.threadsStarted()==3);
    CHECK(!executor.isStopped());
    executor.shutdown();
    CHECK(executor.isStopped()); // Set only AFTER all joins, never on stop request.
    CHECK(!FrameProcessor::resizeBgr(executor,source,37,29,output,13,17,ScalingAlgorithm::LancIR));
    executor.shutdown();
    { ScalingExecutor idle(2); } // Idle workers must wake and join on destruction.
    { ScalingExecutor fallback(0); CHECK(fallback.workerCount()==1); }

    // Race dispatch with shutdown under an explicit barrier, without sleeps.
    // Either admission order is valid; an admitted frame must finish in full.
    for (int repeat=0;repeat<12;++repeat) {
        ScalingExecutor concurrent(2);
        std::barrier start(2);
        bool success=false;
        std::jthread submitter([&]{
            start.arrive_and_wait();
            success=FrameProcessor::resizeBgr(concurrent,source,37,29,output,13,17,ScalingAlgorithm::LancIR);
        });
        start.arrive_and_wait();
        concurrent.shutdown();
        submitter.join();
        CHECK(concurrent.isStopped());
        if(success) CHECK(output==reference(source.data(),37,29,13,17));
    }
}
}
int main()
{
    try {
        stripEquivalence();
        fullReferenceAndReuse();
        formatsStridesAndThreadCounts();
        cropAndRotationReference();
        lifecycleAndErrors();
        std::cout << "Scaling executor reference, format, partition, reuse and shutdown tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
