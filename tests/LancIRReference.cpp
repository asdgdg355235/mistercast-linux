#include <lancir.h>
#include <cstdint>

// Separate translation unit keeps the direct upstream call independent of the
// production adapter and avoids GCC's inlining false positives about caches
// whose uninitialized fields are guarded by their initialized validity fields.
int lancirReferenceResize(avir::CLancIR& resizer, const std::uint8_t* input,
                         int sw, int sh, std::uint8_t* output, int ow, int oh,
                         int channels, const avir::CLancIRParams& params)
{
    return resizer.resizeImage(input,sw,sh,output,ow,oh,channels,&params);
}
