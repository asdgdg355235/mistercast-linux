#pragma once

// Private adapter to the pinned AVIR revision. No AVIR types enter public APIs.
#include <lancir.h>

#include <algorithm>
#include <cstdint>

namespace mistercast::detail {

class LancIRRuntime final : public avir::CLancIR {
public:
    int resizeStrip(const std::uint8_t* source, int sourceWidth, int sourceHeight,
                    int sourceStride, std::uint8_t* destination, int outputWidth,
                    int fullOutputHeight, int channels, int firstRow, int endRow)
    {
        const double fullKy = double(sourceHeight) / fullOutputHeight;
        const double fullOy = (fullKy - 1.0) * 0.5;
        avir::CLancIRParams params;
        params.SrcSSize = sourceStride;
        params.NewSSize = outputWidth * channels;
        params.ky = -fullKy;
        params.oy = fullOy + fullKy * firstRow;

        // In real arithmetic stripOy + ky*i equals fullOy + ky*(firstRow+i).
        // In double precision it can cross getFilter()'s 1/1000 phase boundary
        // (e.g. source height 3 -> output height 40). Prepare the pinned
        // upstream position table using GLOBAL row arithmetic, then let the
        // unmodified resizeImage() reuse it. Filters, SIMD and pixel arithmetic
        // remain upstream's. This adapter depends on its protected cache API.
        if (rfv.update(params.la, fullKy, channels)) {
            rsv.reset();
            rsh.reset();
        }
        const int height = endRow - firstRow;
        rsv.update(sourceHeight, height, params.oy, rfv, spv);
        const int half = rfv.KernelLen / 2;
        rsv.padl = std::max(half - 1 - int(std::floor(params.oy)), 0);
        rsv.padr = std::max(int(std::floor(fullOy + fullKy * (endRow - 1))) +
                                half + 1 - sourceHeight, 0);
        for (int i = 0; i < height; ++i) {
            const double position = fullOy + fullKy * (firstRow + i);
            const int integer = int(std::floor(position));
            rsv.pos[i].flt = rfv.getFilter(position - integer);
            rsv.pos[i].so = rsv.padl - half + 1 + integer;
        }
        rsv.updateSPO(rfv, spv);
        return resizeImage(source, sourceWidth, sourceHeight, destination,
                           outputWidth, height, channels, &params);
    }
};
}
