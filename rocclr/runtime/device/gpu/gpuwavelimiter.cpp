//
// Copyright (c) 2008 Advanced Micro Devices, Inc. All rights reserved.
//

#include "device/gpu/gpukernel.hpp"
#include "device/gpu/gpuwavelimiter.hpp"
#include "os/os.hpp"
#include "utils/flags.hpp"

#include <cstdlib>
using namespace std;

namespace gpu {

uint WaveLimiter::MaxWave;
uint WaveLimiter::WarmUpCount;
uint WaveLimiter::AdaptCount;
uint WaveLimiter::RunCount;
uint WaveLimiter::AbandonThresh;
uint WaveLimiter::DscThresh;

void WaveLimiter::clearData() {
    waves_ = MaxWave;
    countAll_ = 0;
    clear(measure_);
    clear(reference_);
    clear(trial_);
    clear(ratio_);
    discontinuous_ = false;
    waveSet_ = false;
    dataCount_ = 0;
}

void WaveLimiter::enable() {
    if (waves_ > 0) {
        return;
    }
    auto gpuDev = reinterpret_cast<const Device*>(&owner_->dev());
    auto hwInfo = gpuDev->hwInfo();
    // Enable it only for SI+, unless GPU_WAVE_LIMIT_ENABLE is set to 1
    setIfNotDefault(enable_, GPU_WAVE_LIMIT_ENABLE,
         owner_->workGroupInfo()->limitWave_ && gpuDev->settings().siPlus_);
    if (!enable_) {
        return;
    }
    waves_ = MaxWave;
}

WaveLimiter::WaveLimiter(Kernel *owner) :
        owner_(owner), dumper_(owner_->name()) {
    auto gpuDev = reinterpret_cast<const Device*>(&owner_->dev());
    auto attrib = gpuDev->getAttribs();
    auto hwInfo = gpuDev->hwInfo();
    setIfNotDefault(SIMDPerSH_, GPU_WAVE_LIMIT_CU_PER_SH,
            attrib.numberOfCUsperShaderArray * hwInfo->simdPerCU_);

    MaxWave = GPU_WAVE_LIMIT_MAX_WAVE;
    WarmUpCount = GPU_WAVE_LIMIT_WARMUP;
    AdaptCount = 2 * MaxWave + 1;
    RunCount = GPU_WAVE_LIMIT_RUN * MaxWave;
    AbandonThresh = GPU_WAVE_LIMIT_ABANDON;
    DscThresh = GPU_WAVE_LIMIT_DSC_THRESH;

    state_ = WARMUP;
    dynRunCount_ = RunCount;
    measure_.resize(MaxWave + 1);
    reference_.resize(MaxWave + 1);
    trial_.resize(MaxWave + 1);
    ratio_.resize(MaxWave + 1);
    clearData();
    if (!flagIsDefault(GPU_WAVE_LIMIT_TRACE)) {
        traceStream_.open(std::string(GPU_WAVE_LIMIT_TRACE) + owner_->name() +
            ".txt");
    }

    waves_ = GPU_WAVES_PER_SIMD;
    bestWave_ = MaxWave;
    enable_ = false;
    waveSet_ = false;
}

WaveLimiter::~WaveLimiter() {
    if (traceStream_.is_open()) {
        traceStream_.close();
    }
}

uint WaveLimiter::getWavesPerSH() const {
    waveSet_ = true;
    return waves_ * SIMDPerSH_;
}

void WaveLimiter::updateData(ulong time) {
    auto count = dataCount_ - 1;
    assert(count < 2 * MaxWave + 1);
    assert(time > 0);
    if (count % 2 == 0) {
        assert(waves_ == MaxWave);
        auto pos = count / 2;
        measure_[pos] = time;
        if (pos > 0) {
            auto wave = MaxWave + 1 - pos;
            if (abs(static_cast<long>(measure_[pos - 1]) -
                    static_cast<long>(measure_[pos])) * 100 / measure_[pos] >
                    DscThresh) {
                discontinuous_ = true;
            }
            reference_[wave] = (time + measure_[pos - 1]) / 2;
            ratio_[wave] = trial_[wave] * 100 / reference_[wave];
            if (ratio_[bestWave_] > ratio_[wave] && !discontinuous_) {
                bestWave_ = wave;
            }
        }
    } else {
        assert(waves_ == MaxWave - count / 2);
        trial_[waves_] = time;
    }
    outputTrace();
}

void WaveLimiter::outputTrace() {
    if (!traceStream_.is_open()) {
        return;
    }

    traceStream_ << "[WaveLimiter] " << owner_->name() << " state=" << state_
            << " waves=" << waves_ << " bestWave=" << bestWave_ << '\n';
    output(traceStream_, "\n measure = ", measure_);
    output(traceStream_, "\n reference = ", reference_);
    output(traceStream_, "\n ratio = ", ratio_);
    traceStream_ << "\n\n";
}

void WaveLimiter::callback(ulong duration) {
    dumper_.addData(duration, waves_, static_cast<char>(state_));

    if (!enable_) {
        return;
    }

    countAll_++;

    switch (state_) {
    case WARMUP:
        if (countAll_ < WarmUpCount) {
            return;
        }
        state_ = ADAPT;
        bestWave_ = MaxWave;
        clearData();
        return;
    case ADAPT:
        assert(duration > 0);
        if (waveSet_) {
            dataCount_++;
            updateData(duration);
            waves_ = MaxWave + 1 - dataCount_ / 2;
            if (dataCount_ == 1 || (dataCount_ < AdaptCount &&
                !discontinuous_ && (dataCount_ % 2 == 0 ||
                ratio_[waves_] < AbandonThresh))) {
                if (dataCount_ % 2 == 1) {
                    --waves_;
                } else {
                    waves_ = MaxWave;
                }
                waveSet_ = false;
                return;
            }
        }
        waves_ = bestWave_;
        waveSet_ = false;
        if (dataCount_ >= AdaptCount) {
            dynRunCount_ = RunCount;
        } else {
            dynRunCount_ = AdaptCount;
        }
        countAll_ = rand() % MaxWave;
        state_ = RUN;
        return;
    case RUN:
        if (countAll_ < dynRunCount_) {
            return;
        }
        state_ = ADAPT;
        bestWave_ = MaxWave;
        clearData();
        return;
    }
}

WaveLimiter::DataDumper::DataDumper(const std::string &kernelName) {
    enable_ = !flagIsDefault(GPU_WAVE_LIMIT_DUMP);
    if (enable_) {
        fileName_ = std::string(GPU_WAVE_LIMIT_DUMP) + kernelName + ".csv";
    }
}

WaveLimiter::DataDumper::~DataDumper() {
    if (!enable_) {
        return;
    }

    std::ofstream OFS(fileName_);
    for (size_t i = 0, e = time_.size(); i != e; ++i) {
        OFS << i << ',' << time_[i] << ',' << wavePerSIMD_[i] << ','
            << static_cast<uint>(state_[i]) << '\n';
    }
    OFS.close();
}

void WaveLimiter::DataDumper::addData(ulong time, uint wave, char state) {
    if (!enable_) {
        return;
    }

    time_.push_back(time);
    wavePerSIMD_.push_back(wave);
    state_.push_back(state);
}

amd::ProfilingCallback* WaveLimiter::getProfilingCallback() const {
    if (enable_ || dumper_.enabled()) {
        return const_cast<WaveLimiter*>(this);
    }
    return NULL;
}
}

