/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Benchmark.h"
#include "BufferMediaResource.h"
#include "MediaData.h"
#include "PDMFactory.h"
#include "WebMDemuxer.h"
#include "WebMSample.h"
#include "mozilla/Preferences.h"
#include "mozilla/Telemetry.h"

namespace mozilla {

const char* VP9Benchmark::sBenchmarkFpsPref = "media.benchmark.vp9.fps";
bool VP9Benchmark::sHasRunTest = false;

bool
VP9Benchmark::IsVP9DecodeFast()
{
  MOZ_ASSERT(NS_IsMainThread());

  bool hasPref = Preferences::HasUserValue(sBenchmarkFpsPref);

  if (!sHasRunTest && !hasPref) {
    sHasRunTest = true;

    RefPtr<WebMDemuxer> demuxer =
      new WebMDemuxer(new BufferMediaResource(sWebMSample, sizeof(sWebMSample), nullptr,
                                              NS_LITERAL_CSTRING("video/webm")));
    RefPtr<Benchmark> estimiser =
      new Benchmark(demuxer,
                    {
                      Preferences::GetInt("media.benchmark.frames", 300), // frames to measure
                      1, // start benchmarking after decoding this frame.
                      8, // loop after decoding that many frames.
                      TimeDuration::FromMilliseconds(
                        Preferences::GetUint("media.benchmark.timeout", 1000))
                    });
    estimiser->Run()->Then(
      AbstractThread::MainThread(), __func__,
      [](uint32_t aDecodeFps) {
        Preferences::SetUint(sBenchmarkFpsPref, aDecodeFps);
        Telemetry::Accumulate(Telemetry::ID::VIDEO_VP9_BENCHMARK_FPS, aDecodeFps);
      },
      []() { });
  }

  if (!hasPref) {
    return false;
  }

  uint32_t decodeFps = Preferences::GetUint(sBenchmarkFpsPref);
  uint32_t threshold =
    Preferences::GetUint("media.benchmark.vp9.threshold", 150);

  return decodeFps >= threshold;
}

Benchmark::Benchmark(MediaDataDemuxer* aDemuxer, const Parameters& aParameters)
  : QueueObject(AbstractThread::MainThread())
  , mParameters(aParameters)
  , mKeepAliveUntilComplete(this)
  , mPlaybackState(this, aDemuxer)
{
  MOZ_COUNT_CTOR(Benchmark);
  MOZ_ASSERT(NS_IsMainThread());
}

Benchmark::~Benchmark()
{
  MOZ_COUNT_DTOR(Benchmark);
}

RefPtr<Benchmark::BenchmarkPromise>
Benchmark::Run()
{
  RefPtr<BenchmarkPromise> p = mPromise.Ensure(__func__);
  RefPtr<Benchmark> self = this;
  mPlaybackState.Dispatch(
    NS_NewRunnableFunction([self]() { self->mPlaybackState.DemuxSamples(); }));
  return p;
}

void
Benchmark::ReturnResult(uint32_t aDecodeFps)
{
  MOZ_ASSERT(NS_IsMainThread());

  mPromise.ResolveIfExists(aDecodeFps, __func__);
}

void
Benchmark::Dispose()
{
  MOZ_ASSERT(NS_IsMainThread());

  mKeepAliveUntilComplete = nullptr;
  mPromise.RejectIfExists(false, __func__);
}

BenchmarkPlayback::BenchmarkPlayback(Benchmark* aMainThreadState,
                                     MediaDataDemuxer* aDemuxer)
  : QueueObject(new TaskQueue(GetMediaThreadPool(MediaThreadType::PLAYBACK)))
  , mMainThreadState(aMainThreadState)
  , mDecoderTaskQueue(new FlushableTaskQueue(GetMediaThreadPool(
                        MediaThreadType::PLATFORM_DECODER)))
  , mDemuxer(aDemuxer)
  , mSampleIndex(0)
  , mFrameCount(0)
  , mFinished(false)
{
  MOZ_ASSERT(NS_IsMainThread());
  PDMFactory::Init();
}

void
BenchmarkPlayback::DemuxSamples()
{
  MOZ_ASSERT(OnThread());

  RefPtr<Benchmark> ref(mMainThreadState);
  mDemuxer->Init()->Then(
    Thread(), __func__,
    [this, ref](nsresult aResult) {
      MOZ_ASSERT(OnThread());
      mTrackDemuxer =
        mDemuxer->GetTrackDemuxer(TrackInfo::kVideoTrack, 0);
      if (!mTrackDemuxer) {
        MainThreadShutdown();
      }
      DemuxNextSample();
    },
    [this, ref](DemuxerFailureReason aReason) { MainThreadShutdown(); });
}

void
BenchmarkPlayback::DemuxNextSample()
{
  MOZ_ASSERT(OnThread());

  RefPtr<Benchmark> ref(mMainThreadState);
  RefPtr<MediaTrackDemuxer::SamplesPromise> promise = mTrackDemuxer->GetSamples();
  promise->Then(
    Thread(), __func__,
    [this, ref](RefPtr<MediaTrackDemuxer::SamplesHolder> aHolder) {
      mSamples.AppendElements(Move(aHolder->mSamples));
      if (ref->mParameters.mStopAtFrame &&
          mSamples.Length() == (size_t)ref->mParameters.mStopAtFrame.ref()) {
        InitDecoder(Move(*mTrackDemuxer->GetInfo()));
      } else {
        Dispatch(NS_NewRunnableFunction([this, ref]() { DemuxNextSample(); }));
      }
    },
    [this, ref](DemuxerFailureReason aReason) {
      switch (aReason) {
        case DemuxerFailureReason::END_OF_STREAM:
          InitDecoder(Move(*mTrackDemuxer->GetInfo()));
          break;
        default:
          MainThreadShutdown();
      }
    });
}

void
BenchmarkPlayback::InitDecoder(TrackInfo&& aInfo)
{
  MOZ_ASSERT(OnThread());

  RefPtr<PDMFactory> platform = new PDMFactory();
  mDecoder = platform->CreateDecoder(aInfo, mDecoderTaskQueue, this);
  if (!mDecoder) {
    MainThreadShutdown();
    return;
  }
  RefPtr<Benchmark> ref(mMainThreadState);
  mDecoder->Init()->Then(
    ref->Thread(), __func__,
    [this, ref](TrackInfo::TrackType aTrackType) {
      Dispatch(NS_NewRunnableFunction([this, ref]() { InputExhausted(); }));
    },
    [this, ref](MediaDataDecoder::DecoderFailureReason aReason) {
      MainThreadShutdown();
    });
}

void
BenchmarkPlayback::MainThreadShutdown()
{
  MOZ_ASSERT(OnThread());

  if (mDecoder) {
    mDecoder->Flush();
    mDecoder->Shutdown();
    mDecoder = nullptr;
  }

  mDecoderTaskQueue->BeginShutdown();
  mDecoderTaskQueue->AwaitShutdownAndIdle();
  mDecoderTaskQueue = nullptr;

  if (mTrackDemuxer) {
    mTrackDemuxer->Reset();
    mTrackDemuxer->BreakCycles();
    mTrackDemuxer = nullptr;
  }

  RefPtr<Benchmark> ref(mMainThreadState);
  Thread()->AsTaskQueue()->BeginShutdown()->Then(
    ref->Thread(), __func__,
    [ref]() {  ref->Dispose(); },
    []() { MOZ_CRASH("not reached"); });
}

void
BenchmarkPlayback::Output(MediaData* aData)
{
  RefPtr<Benchmark> ref(mMainThreadState);
  Dispatch(NS_NewRunnableFunction([this, ref]() {
    mFrameCount++;
    if (mFrameCount == ref->mParameters.mStartupFrame) {
      mDecodeStartTime = TimeStamp::Now();
    }
    int32_t frames = mFrameCount - ref->mParameters.mStartupFrame;
    TimeDuration elapsedTime = TimeStamp::Now() - mDecodeStartTime;
    if (!mFinished &&
        (frames == ref->mParameters.mFramesToMeasure ||
         elapsedTime >= ref->mParameters.mTimeout)) {
      uint32_t decodeFps = frames / elapsedTime.ToSeconds();
      mFinished = true;
      MainThreadShutdown();
      ref->Dispatch(NS_NewRunnableFunction([ref, decodeFps]() {
        ref->ReturnResult(decodeFps);
      }));
    }
  }));
}

void
BenchmarkPlayback::Error()
{
  RefPtr<Benchmark> ref(mMainThreadState);
  Dispatch(NS_NewRunnableFunction([this, ref]() {  MainThreadShutdown(); }));
}

void
BenchmarkPlayback::InputExhausted()
{
  RefPtr<Benchmark> ref(mMainThreadState);
  Dispatch(NS_NewRunnableFunction([this, ref]() {
    MOZ_ASSERT(OnThread());
    if (mFinished || mSampleIndex >= mSamples.Length()) {
      return;
    }
    mDecoder->Input(mSamples[mSampleIndex]);
    mSampleIndex++;
    if (mSampleIndex == mSamples.Length()) {
      if (ref->mParameters.mStopAtFrame) {
        mSampleIndex = 0;
      } else {
        mDecoder->Drain();
      }
    }
  }));
}

void
BenchmarkPlayback::DrainComplete()
{
  RefPtr<Benchmark> ref(mMainThreadState);
  Dispatch(NS_NewRunnableFunction([this, ref]() {
    int32_t frames = mFrameCount - ref->mParameters.mStartupFrame;
    TimeDuration elapsedTime = TimeStamp::Now() - mDecodeStartTime;
    uint32_t decodeFps = frames / elapsedTime.ToSeconds();
    mFinished = true;
    MainThreadShutdown();
    ref->Dispatch(NS_NewRunnableFunction([ref, decodeFps]() {
      ref->ReturnResult(decodeFps);
    }));
  }));
}

bool
BenchmarkPlayback::OnReaderTaskQueue()
{
  return OnThread();
}

}
