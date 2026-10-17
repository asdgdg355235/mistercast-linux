#include "core/FrameProcessor.h"
#include "core/ScalingExecutor.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mistercast;
int main(int argc, char** argv)
{
    try {
        const auto workers = argc > 1 ? std::stoul(argv[1]) : ScalingExecutor::defaultWorkerCount();
        const auto iterations = argc > 2 ? std::stoul(argv[2]) : 30;
        if (!workers || !iterations) throw std::invalid_argument("arguments must be positive");
        std::vector<std::uint8_t> source(1920 * 1440 * 3), output(720 * 576 * 3);
        for (std::size_t i = 0; i < source.size(); ++i) source[i] = (i*71+i/37)%256;
        std::cout << "1920x1440 -> 720x576 BGR; hardware_concurrency="
                  << std::thread::hardware_concurrency() << "; iterations=" << iterations << '\n';
        std::cout << "algorithm,workers,mean_ms\n";
        for (const auto& descriptor : availableScalers()) {
            for (const auto count : {std::size_t{1},std::size_t{workers}}) {
                ScalingExecutor executor(count);
                const auto resize = [&] {
                    if (!FrameProcessor::resizeBgr(executor, source, 1920,1440,output,720,576,descriptor.algorithm))
                        throw std::runtime_error("scale failed");
                };
                for(int warm=0;warm<3;++warm) resize();
                const auto start=std::chrono::steady_clock::now();
                for(std::size_t i=0;i<iterations;++i) resize();
                const auto milliseconds=std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-start).count()/iterations;
                std::cout << descriptor.key << ',' << count << ',' << std::fixed
                          << std::setprecision(3) << milliseconds << '\n';
            }
        }
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
