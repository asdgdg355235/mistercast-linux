#include "core/CrtGeometry.h"
#include "core/Gamma24Tables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

using mistercast::blendGamma24;

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    std::ifstream file(argv[1], std::ios::binary);
    constexpr std::size_t count = 256 * 256 * 257;
    std::vector<std::uint8_t> canonical(count);
    if (!file.read(reinterpret_cast<char*>(canonical.data()), canonical.size()) ||
        file.peek() != EOF) return 1;

    constexpr long double q = (1 << 24) - 1;
    std::array<long double, 256> decoded{};
    for (unsigned i = 0; i < decoded.size(); ++i) {
        decoded[i] = std::pow(i / 255.0L, 12.0L / 5.0L);
    }
    long double maxLinearError = 0, maxOutputLightError = 0, maxCanonicalLightError = 0;
    std::uint64_t absoluteError = 0, referenceMismatch = 0;
    std::int64_t signedError = 0;
    int maxError = 0;
    std::size_t i = 0;
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned b = 0; b < 256; ++b) {
            int previous = a;
            for (unsigned w = 0; w <= 256; ++w, ++i) {
                const long double linear = ((256-w)*decoded[a] + w*decoded[b]) / 256;
                const int reference = std::clamp(static_cast<int>(
                    std::floor(255 * std::pow(linear, 5.0L / 12.0L) + 0.5L)), 0, 255);
                referenceMismatch += reference != canonical[i];
                const int actual = blendGamma24(a, b, w);
                const int error = actual - canonical[i];
                maxError = std::max(maxError, std::abs(error));
                absoluteError += std::abs(error);
                signedError += error;
                if ((w == 0 && actual != static_cast<int>(a)) ||
                    (w == 256 && actual != static_cast<int>(b)) ||
                    (a == b && actual != static_cast<int>(a)) ||
                    (a < b && actual < previous) || (a > b && actual > previous) ||
                    actual != blendGamma24(b, a, 256-w)) return 1;
                previous = actual;

                const auto numerator = std::uint64_t{256-w} * mistercast::gamma24::decode[a] +
                    std::uint64_t{w} * mistercast::gamma24::decode[b];
                maxLinearError = std::max(maxLinearError, std::abs(numerator/(256*q) - linear));
                maxOutputLightError = std::max(maxOutputLightError, std::abs(decoded[actual] - linear));
                maxCanonicalLightError = std::max(maxCanonicalLightError,
                    std::abs(decoded[actual] - decoded[canonical[i]]));
            }
        }
    }
    const auto mae = static_cast<double>(absoluteError) / count;
    std::cout << "comparisons=" << count << " reference_mismatches=" << referenceMismatch
              << " max_code_error=" << maxError << " mean_absolute_code_error=" << mae
              << " mean_signed_code_error=" << static_cast<double>(signedError) / count
              << " max_pre_encode_light_error=" << maxLinearError
              << " max_analytic_output_light_error=" << maxOutputLightError
              << " max_canonical_output_light_error=" << maxCanonicalLightError << '\n';
    return referenceMismatch || maxError > 1 || mae > 0.001L || maxLinearError > 0.5L / q;
}
