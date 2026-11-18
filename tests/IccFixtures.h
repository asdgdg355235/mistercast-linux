#pragma once

#include <lcms2.h>

#include <array>
#include <filesystem>
#include <memory>

namespace icc_test {

using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
using Curve = std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)>;

inline cmsInt32Number sample(const cmsUInt16Number in[], cmsUInt16Number out[], void*)
{
    out[0] = in[1] / 2 + 3000;
    out[1] = in[2] / 2 + 6000;
    out[2] = in[0] / 2 + 9000;
    return 1;
}

inline bool writeLut(cmsHPROFILE profile, cmsTagSignature tag, cmsToneCurve* identity)
{
    std::unique_ptr<cmsPipeline, decltype(&cmsPipelineFree)> p(
        cmsPipelineAlloc(nullptr, 3, 3), cmsPipelineFree);
    cmsToneCurve* curves[] = {identity, identity, identity};
    auto* clut = cmsStageAllocCLut16bit(nullptr, 3, 3, 3, nullptr);
    return cmsPipelineInsertStage(p.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, curves)) &&
           cmsPipelineInsertStage(p.get(), cmsAT_END, clut) &&
           cmsStageSampleCLut16bit(clut, sample, nullptr, 0) &&
           cmsPipelineInsertStage(p.get(), cmsAT_END, cmsStageAllocToneCurves(nullptr, 3, curves)) &&
           cmsWriteTag(profile, tag, p.get());
}

inline bool writeDisplay(const std::filesystem::path& path, bool lut, bool calibration)
{
    Curve identity(cmsBuildGamma(nullptr, 1.0), cmsFreeToneCurve);
    Curve green(cmsBuildGamma(nullptr, 1.9), cmsFreeToneCurve);
    Curve blue(cmsBuildGamma(nullptr, 3.0), cmsFreeToneCurve);
    cmsToneCurve* trc[] = {identity.get(), green.get(), blue.get()};
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE primaries{{0.64, 0.33, 1.0}, {0.30, 0.60, 1.0}, {0.15, 0.06, 1.0}};
    Profile p(cmsCreateRGBProfile(&white, &primaries, trc), cmsCloseProfile);
    if (!p) return false;
    cmsSetProfileVersion(p.get(), 2.2);
    if (lut && (!writeLut(p.get(), cmsSigBToA1Tag, identity.get()) ||
                !writeLut(p.get(), cmsSigAToB1Tag, identity.get()))) return false;
    if (calibration && !cmsWriteTag(p.get(), cmsSigVcgtTag, trc)) return false;
    return cmsSaveProfileToFile(p.get(), path.c_str());
}

} // namespace icc_test
