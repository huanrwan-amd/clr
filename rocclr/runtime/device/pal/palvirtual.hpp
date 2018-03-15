//
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
//
#pragma once

#include <queue>
#include "device/pal/paldefs.hpp"
#include "device/pal/palconstbuf.hpp"
#include "device/pal/palprintf.hpp"
#include "device/pal/paltimestamp.hpp"
#include "device/pal/palsched.hpp"
#include "device/pal/paldebugger.hpp"
#include "platform/commandqueue.hpp"
#include "device/blit.hpp"
#include "palUtil.h"
#include "palCmdBuffer.h"
#include "palCmdAllocator.h"
#include "palQueue.h"
#include "palFence.h"
#include "palLinearAllocator.h"

/*! \addtogroup PAL PAL Resource Implementation
 *  @{
 */

//! PAL Device Implementation
namespace pal {

class Device;
class Kernel;
class Memory;
class CalCounterReference;
class VirtualGPU;
class Program;
class BlitManager;
class ThreadTrace;
class HSAILKernel;

//! Virtual GPU
class VirtualGPU : public device::VirtualDevice {
 public:
  class Queue : public amd::HeapObject {
   public:
    // Note: More command buffers may cause a HW hang with HWSC on VI family in OCLPerfKernelArguments
    static const uint MaxCmdBuffers = 8;
    static const uint MaxCommands = 256;
    static const uint StartCmdBufIdx = 1;
    static const uint FirstMemoryReference = 0x80000000;
    static const uint64_t WaitTimeoutInNsec = 6000000000;
    static const uint64_t PollIntervalInNsec = 200000;

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    static Queue* Create(Pal::IDevice* palDev,                 //!< PAL device object
                         Pal::QueueType queueType,             //!< PAL queue type
                         uint engineIdx,                       //!< Select particular engine index
                         Pal::ICmdAllocator* cmdAlloc,         //!< PAL CMD buffer allocator
                         uint rtCU,                            //!< The number of reserved CUs
                         amd::CommandQueue::Priority priority, //!< Queue priority
                         uint64_t residency_limit              //!< Enables residency limit
                         );

    Queue(Pal::IDevice* palDev, uint64_t residency_limit)
        : iQueue_(nullptr),
          last_kernel_(nullptr),
          iDev_(palDev),
          cmdBufIdSlot_(StartCmdBufIdx),
          cmdBufIdCurrent_(StartCmdBufIdx),
          cmbBufIdRetired_(0),
          cmdCnt_(0),
          vlAlloc_(64 * Ki),
          residency_size_(0),
          residency_limit_(residency_limit)
    {
      for (uint i = 0; i < MaxCmdBuffers; ++i) {
        iCmdBuffs_[i] = nullptr;
        iCmdFences_[i] = nullptr;
      }
      vlAlloc_.Init();
    }

    ~Queue();

    void addCmdMemRef(GpuMemoryReference* mem);
    void removeCmdMemRef(GpuMemoryReference* mem);

    void addCmdDoppRef(Pal::IGpuMemory* iMem, bool lastDoppCmd, bool pfpaDoppCmd);

    void addMemRef(Pal::IGpuMemory* iMem) const {
      Pal::GpuMemoryRef memRef = {};
      memRef.pGpuMemory = iMem;
      iDev_->AddGpuMemoryReferences(1, &memRef, nullptr, Pal::GpuMemoryRefCantTrim);
    }
    void removeMemRef(Pal::IGpuMemory* iMem) const {
      iDev_->RemoveGpuMemoryReferences(1, &iMem, nullptr);
    }

    // ibReuse forces event wait without polling, to make sure event occured
    template <bool ibReuse>
    bool waifForFence(uint cbId) const {
      Pal::Result result = Pal::Result::Success;
      uint64_t start;
      uint64_t end;
      if (!ibReuse) {
        start = amd::Os::timeNanos();
      }
      while ((Pal::Result::Success != (result = iCmdFences_[cbId]->GetStatus())) || ibReuse) {
        if (result == Pal::Result::ErrorFenceNeverSubmitted) {
          result = Pal::Result::Success;
          break;
        }
        if (!ibReuse) {
          end = amd::Os::timeNanos();
        }
        if (!ibReuse && ((end - start) < PollIntervalInNsec)) {
          amd::Os::yield();
          continue;
        }
        result = iDev_->WaitForFences(1, &iCmdFences_[cbId], true, WaitTimeoutInNsec);
        if (Pal::Result::Success == result) {
          break;
        } else if ((Pal::Result::NotReady == result) || (Pal::Result::Timeout == result)) {
          LogWarning("PAL fence isn't ready!");
          if (GPU_ANALYZE_HANG) {
            DumpMemoryReferences();
          }
        } else {
          LogError("PAL wait for a fence failed!");
          break;
        }
      }
      return (result == Pal::Result::Success) ? true : false;
    }

    //! Flushes the current command buffer to HW
    //! Returns ID associated with the submission
    template <bool avoidBarrierSubmit = false>
    uint submit(bool forceFlush);

    bool flush();

    bool waitForEvent(uint id);

    bool isDone(uint id);

    Pal::ICmdBuffer* iCmd() const { return iCmdBuffs_[cmdBufIdSlot_]; }

    uint cmdBufId() const { return cmdBufIdCurrent_; }

    Pal::IQueue* iQueue_;                        //!< PAL queue object
    Pal::ICmdBuffer* iCmdBuffs_[MaxCmdBuffers];  //!< PAL command buffers
    Pal::IFence* iCmdFences_[MaxCmdBuffers];     //!< PAL fences, associated with CMD
    const amd::Kernel* last_kernel_;             //!< Last submitted kernel

  private:
    void DumpMemoryReferences() const;
    Pal::IDevice* iDev_;    //!< PAL device
    uint cmdBufIdSlot_;     //!< Command buffer ID slot for submissions
    uint cmdBufIdCurrent_;  //!< Current global command buffer ID
    uint cmbBufIdRetired_;  //!< The last retired command buffer ID
    uint cmdCnt_;           //!< Counter of commands
    std::map<GpuMemoryReference*, uint> memReferences_;
    Util::VirtualLinearAllocator vlAlloc_;
    std::vector<Pal::GpuMemoryRef> palMemRefs_;
    std::vector<Pal::IGpuMemory*> palMems_;
    std::vector<Pal::DoppRef> palDoppRefs_;
    std::set<Pal::IGpuMemory*>      sdiReferences_;
    std::vector<const Pal::IGpuMemory*>   palSdiRefs_;
    uint64_t  residency_size_;  //!< Resource residency size
    uint64_t  residency_limit_; //!< Enables residency limit
  };

  struct CommandBatch : public amd::HeapObject {
    amd::Command* head_;           //!< Command batch head
    GpuEvent events_[AllEngines];  //!< Last known GPU events
    TimeStamp* lastTS_;            //!< TS associated with command batch

    //! Constructor
    CommandBatch(amd::Command* head,      //!< Command batch head
                 const GpuEvent* events,  //!< HW events on all engines
                 TimeStamp* lastTS        //!< Last TS in command batch
                 ) {
      init(head, events, lastTS);
    }

    void init(amd::Command* head,      //!< Command batch head
              const GpuEvent* events,  //!< HW events on all engines
              TimeStamp* lastTS        //!< Last TS in command batch
              ) {
      head_ = head;
      lastTS_ = lastTS;
      memcpy(&events_, events, AllEngines * sizeof(GpuEvent));
    }
  };

  //! The virtual GPU states
  union State {
    struct {
      uint profiling_          : 1;     //!< Profiling is enabled
      uint forceWait_          : 1;     //!< Forces wait in flush()
      uint profileEnabled_     : 1;     //!< Profiling is enabled for WaveLimiter
      uint perfCounterEnabled_ : 1;     //!< PerfCounter is enabled
      uint rgpCaptureEnabled_  : 1;     //!< RGP capture is enabled in the runtime
    };
    uint value_;
    State() : value_(0) {}
  };

  typedef std::vector<ConstBuffer*> constbufs_t;

  class MemoryDependency : public amd::EmbeddedObject {
   public:
    //! Default constructor
    MemoryDependency()
        : memObjectsInQueue_(nullptr), numMemObjectsInQueue_(0), maxMemObjectsInQueue_(0) {}

    ~MemoryDependency() { delete[] memObjectsInQueue_; }

    //! Creates memory dependecy structure
    bool create(size_t numMemObj);

    //! Notify the tracker about new kernel
    void newKernel() { endMemObjectsInQueue_ = numMemObjectsInQueue_; }

    //! Validates memory object on dependency
    void validate(VirtualGPU& gpu, const Memory* memory, bool readOnly);

    //! Clear memory dependency
    void clear(bool all = true);

   private:
    struct MemoryState {
      uint64_t start_;  //! Busy memory start address
      uint64_t end_;    //! Busy memory end address
      bool readOnly_;   //! Current GPU state in the queue
    };

    MemoryState* memObjectsInQueue_;  //!< Memory object state in the queue
    size_t endMemObjectsInQueue_;     //!< End of mem objects in the queue
    size_t numMemObjectsInQueue_;     //!< Number of mem objects in the queue
    size_t maxMemObjectsInQueue_;     //!< Maximum number of mem objects in the queue
  };


  class DmaFlushMgmt : public amd::EmbeddedObject {
   public:
    DmaFlushMgmt(const Device& dev);

    // Resets DMA command buffer workload
    void resetCbWorkload(const Device& dev);

    // Finds split size for the current dispatch
    void findSplitSize(const Device& dev,  //!< GPU device object
                       uint64_t threads,   //!< Total number of execution threads
                       uint instructions   //!< Number of ALU instructions
                       );

    // Returns TRUE if DMA command buffer is ready for a flush
    bool isCbReady(VirtualGPU& gpu,   //!< Virtual GPU object
                   uint64_t threads,  //!< Total number of execution threads
                   uint instructions  //!< Number of ALU instructions
                   );

    // Returns dispatch split size
    uint dispatchSplitSize() const { return dispatchSplitSize_; }

   private:
    uint64_t maxDispatchWorkload_;  //!< Maximum number of operations for a single dispatch
    uint64_t maxCbWorkload_;        //!< Maximum number of operations for DMA command buffer
    uint64_t cbWorkload_;           //!< Current number of operations in DMA command buffer
    uint aluCnt_;                   //!< All ALUs on the chip
    uint dispatchSplitSize_;        //!< Dispath split size in elements
  };

 public:
  VirtualGPU(Device& device);
  //! Creates virtual gpu object
  bool create(bool profiling,            //!< Enables profilng on the queue
              uint deviceQueueSize = 0,  //!< Device queue size, 0 if host queue
              uint rtCUs = amd::CommandQueue::RealTimeDisabled,
              amd::CommandQueue::Priority priority = amd::CommandQueue::Priority::Normal);
  ~VirtualGPU();

  void submitReadMemory(amd::ReadMemoryCommand& vcmd);
  void submitWriteMemory(amd::WriteMemoryCommand& vcmd);
  void submitCopyMemory(amd::CopyMemoryCommand& vcmd);
  void submitCopyMemoryP2P(amd::CopyMemoryP2PCommand& vcmd) {}
  void submitMapMemory(amd::MapMemoryCommand& vcmd);
  void submitUnmapMemory(amd::UnmapMemoryCommand& vcmd);
  void submitKernel(amd::NDRangeKernelCommand& vcmd);
  bool submitKernelInternal(
      const amd::NDRangeContainer& sizes,  //!< Workload sizes
      const amd::Kernel& kernel,           //!< Kernel for execution
      const_address parameters,            //!< Parameters for the kernel
      bool nativeMem = true,               //!< Native memory objects
      amd::Event* enqueueEvent = nullptr   //!< Event provided in the enqueue kernel command
      );
  void submitNativeFn(amd::NativeFnCommand& vcmd);
  void submitFillMemory(amd::FillMemoryCommand& vcmd);
  void submitMigrateMemObjects(amd::MigrateMemObjectsCommand& cmd);
  void submitMarker(amd::Marker& vcmd);
  void submitAcquireExtObjects(amd::AcquireExtObjectsCommand& vcmd);
  void submitReleaseExtObjects(amd::ReleaseExtObjectsCommand& vcmd);
  void submitPerfCounter(amd::PerfCounterCommand& vcmd);
  void submitThreadTraceMemObjects(amd::ThreadTraceMemObjectsCommand& cmd);
  void submitThreadTrace(amd::ThreadTraceCommand& vcmd);
  void submitSignal(amd::SignalCommand& vcmd);
  void submitMakeBuffersResident(amd::MakeBuffersResidentCommand& vcmd);
  virtual void submitSvmFreeMemory(amd::SvmFreeMemoryCommand& cmd);
  virtual void submitSvmCopyMemory(amd::SvmCopyMemoryCommand& cmd);
  virtual void submitSvmFillMemory(amd::SvmFillMemoryCommand& cmd);
  virtual void submitSvmMapMemory(amd::SvmMapMemoryCommand& cmd);
  virtual void submitSvmUnmapMemory(amd::SvmUnmapMemoryCommand& cmd);
  virtual void submitTransferBufferFromFile(amd::TransferBufferFileCommand& cmd);

  void releaseMemory(GpuMemoryReference* mem);

  void flush(amd::Command* list = nullptr, bool wait = false);
  bool terminate() { return true; }

  //! Returns GPU device object associated with this kernel
  const Device& dev() const { return gpuDevice_; }

  //! Set the last known GPU event
  void setGpuEvent(GpuEvent gpuEvent,  //!< GPU event for tracking
                   bool flush = false  //!< TRUE if flush is required
                   );

  //! Flush DMA buffer on the specified engine
  void flushDMA(uint engineID  //!< Engine ID for DMA flush
                );

  //! Wait for all engines on this Virtual GPU
  //! Returns TRUE if CPU didn't wait for GPU
  bool waitAllEngines(CommandBatch* cb = nullptr  //!< Command batch
                      );

  //! Waits for the latest GPU event with a lock to prevent multiple entries
  void waitEventLock(CommandBatch* cb  //!< Command batch
                     );

  //! Returns a resource associated with the constant buffer
  const ConstBuffer* cb(uint idx) const { return constBufs_[idx]; }

  //! Adds CAL objects into the constant buffer vector
  void addConstBuffer(ConstBuffer* cb) { constBufs_.push_back(cb); }

  constbufs_t constBufs_;  //!< constant buffers

  //! Start the command profiling
  void profilingBegin(amd::Command& command,     //!< Command queue object
                      bool drmProfiling = false  //!< Measure DRM time
                      );

  //! End the command profiling
  void profilingEnd(amd::Command& command);

  //! Collect the profiling results
  bool profilingCollectResults(CommandBatch* cb,               //!< Command batch
                               const amd::Event* waitingEvent  //!< Waiting event
                               );

  //! Adds a memory handle into the GSL memory array for Virtual Heap
  inline void addVmMemory(const Memory* memory  //!< GPU memory object
                          );

  //! Adds the last submitted kernel to the queue for tracking a possible hang
  inline void AddKernel(const amd::Kernel& kernel  //!< AMD kernel object
                        ) const;

  //! Adds a dopp desktop texture reference
  void addDoppRef(const Memory* memory,  //!< GPU memory object
                  bool lastDoopCmd,      //!< is the last submission for the pre-present primary
                  bool pfpaDoppCmd       //!< is a submission for the pre-present primary
                  );

  //! Adds a stage write buffer into a list
  void addXferWrite(Memory& memory);

  //! Adds a pinned memory object into a map
  void addPinnedMem(amd::Memory* mem);

  //! Release pinned memory objects
  void releasePinnedMem();

  //! Finds if pinned memory is cached
  amd::Memory* findPinnedMem(void* addr, size_t size);

  //! Returns the monitor object for execution access by VirtualGPU
  amd::Monitor& execution() { return execution_; }

  //! Returns the virtual gpu unique index
  uint index() const { return index_; }

  //! Get the PrintfDbgHSA object
  PrintfDbgHSA& printfDbgHSA() const { return *printfDbgHSA_; }

  //! Enables synchronized transfers
  void enableSyncedBlit() const;

  //! Checks if profiling is enabled
  bool profiling() const { return state_.profiling_; }

  //! Returns memory dependency class
  MemoryDependency& memoryDependency() { return memoryDependency_; }

  //! Returns hsaQueueMem_
  const Memory* hsaQueueMem() const { return hsaQueueMem_; }

  //! Returns DMA flush management structure
  const DmaFlushMgmt& dmaFlushMgmt() const { return dmaFlushMgmt_; }

  //! Returns the HW ring used on this virtual device
  uint hwRing() const { return hwRing_; }

  //! Returns virtual queue object for device enqueuing
  Memory* vQueue() const { return virtualQueue_; }

  //! Update virtual queue header
  void writeVQueueHeader(VirtualGPU& hostQ, uint64_t kernelTable);

  //! Returns TRUE if virtual queue was successfully allocatted
  bool createVirtualQueue(uint deviceQueueSize  //!< Device queue size
                          );

  EngineType engineID_;  //!< Engine ID for this VirtualGPU

  //! Returns PAL command buffer interface
  Pal::ICmdBuffer* iCmd() const {
    Queue* queue = queues_[engineID_];
    return queue->iCmd();
  }

  //! Returns queue, associated with VirtualGPU
  Queue& queue(EngineType id) const { return *queues_[id]; }

  void addBarrier(bool flushL2 = false) const {
    Pal::BarrierInfo barrier = {};
    barrier.pipePointWaitCount = 1;
    Pal::HwPipePoint point = Pal::HwPipePostCs;
    barrier.pPipePoints = &point;
    barrier.transitionCount = 1;
    uint32_t cacheMask = (flushL2) ? Pal::CoherCopy : Pal::CoherShader;
    Pal::BarrierTransition trans = {cacheMask,
                                    cacheMask,
                                    {nullptr,
                                     {{Pal::ImageAspect::Color, 0, 0}, 0, 0},
                                     Pal::LayoutShaderRead,
                                     Pal::LayoutShaderRead}};
    barrier.pTransitions = &trans;
    barrier.waitPoint = Pal::HwPipePreCs;
    iCmd()->CmdBarrier(barrier);
    queues_[engineID_]->submit<true>(false);
  }

  void eventBegin(EngineType engId) const {
    const static bool Begin = true;
    profileEvent(engId, Begin);
  }

  void eventEnd(EngineType engId, GpuEvent& event, bool forceExec = false) const {
    constexpr bool End = false;
    if (forceExec) {
      constexpr bool ForceFlush = true;
      event.id = queues_[engId]->submit(ForceFlush);
      profileEvent(engId, End);
    } else {
      profileEvent(engId, End);
      event.id = queues_[engId]->submit(GPU_FLUSH_ON_EXECUTION);
    }
    event.engineId_ = engId;
  }

  void waitForEvent(GpuEvent* event) const {
    if (event->isValid()) {
      assert(event->engineId_ < AllEngines);
      queues_[event->engineId_]->waitForEvent(event->id);
      event->invalidate();
    }
  }

  bool isDone(GpuEvent* event) {
    if (event->isValid()) {
      assert(event->engineId_ < AllEngines);
      if (queues_[event->engineId_]->isDone(event->id)) {
        event->invalidate();
        return true;
      }
      return false;
    }
    return true;
  }

  //! Returns TRUE if SDMA requires overlap synchronizaiton
  bool validateSdmaOverlap(const Resource& src,  //!< Source resource for SDMA transfer
                           const Resource& dst   //!< Destination resource for SDMA transfer
                           );

  //! Checks if RGP capture is enabled
  bool rgpCaptureEna() const { return state_.rgpCaptureEnabled_; }

 protected:
  void profileEvent(EngineType engine, bool type) const;

  //! Creates buffer object from image
  amd::Memory* createBufferFromImage(
      amd::Memory& amdImage  //! The parent image object(untiled images only)
      ) const;

 private:
  struct MemoryRange {
    uint64_t start_;  //!< Memory range start address
    uint64_t end_;    //!< Memory range end address
    MemoryRange() : start_(0), end_(0) {}
  };

  //! Allocates constant buffers
  bool allocConstantBuffers();

  //! Releases stage write buffers
  void releaseXferWrite();

  //! Allocate hsaQueueMem_
  bool allocHsaQueueMem();

  //! Awaits a command batch with a waiting event
  bool awaitCompletion(CommandBatch* cb,                         //!< Command batch for to wait
                       const amd::Event* waitingEvent = nullptr  //!< A waiting event
                       );

  //! Detects memory dependency for HSAIL kernels and flushes caches
  bool processMemObjectsHSA(const amd::Kernel& kernel,  //!< AMD kernel object for execution
                            const_address params,       //!< Pointer to the param's store
                            bool nativeMem              //!< Native memory objects
                            );

  //! Common function for fill memory used by both svm Fill and non-svm fill
  bool fillMemory(cl_command_type type,        //!< the command type
                  amd::Memory* amdMemory,      //!< memory object to fill
                  const void* pattern,         //!< pattern to fill the memory
                  size_t patternSize,          //!< pattern size
                  const amd::Coord3D& origin,  //!< memory origin
                  const amd::Coord3D& size     //!< memory size for filling
                  );

  bool copyMemory(cl_command_type type,            //!< the command type
                  amd::Memory& srcMem,             //!< source memory object
                  amd::Memory& dstMem,             //!< destination memory object
                  bool entire,                     //!< flag of entire memory copy
                  const amd::Coord3D& srcOrigin,   //!< source memory origin
                  const amd::Coord3D& dstOrigin,   //!< destination memory object
                  const amd::Coord3D& size,        //!< copy size
                  const amd::BufferRect& srcRect,  //!< region of source for copy
                  const amd::BufferRect& dstRect   //!< region of destination for copy
                  );

  void buildKernelInfo(const HSAILKernel& hsaKernel,          //!< hsa kernel
                       hsa_kernel_dispatch_packet_t* aqlPkt,  //!< aql packet for dispatch
                       HwDbgKernelInfo& kernelInfo,           //!< kernel info for the dispatch
                       amd::Event* enqueueEvent  //!< Event provided in the enqueue kernel command
                       );

  void assignDebugTrapHandler(const DebugToolInfo& dbgSetting,  //!< debug settings
                              HwDbgKernelInfo& kernelInfo       //!< kernel info for the dispatch
                              );

  void PrintChildren(const HSAILKernel& hsaKernel,  //!< The parent HSAIL kernel
                     VirtualGPU* gpuDefQueue        //!< Device queue for children execution
                     );

  bool PreDeviceEnqueue(const amd::Kernel& kernel,    //!< Parent amd kernel object
                        const HSAILKernel& hsaKernel, //!< Parent HSAIL object
                        VirtualGPU** gpuDefQueue,     //!< [Return] GPU default queue
                        uint64_t* vmDefQueue          //!< [Return] VM handle to the virtual queue
                        );

  void PostDeviceEnqueue(const amd::Kernel& kernel,    //!< Parent amd kernel object
                         const HSAILKernel& hsaKernel, //!< Parent HSAIL object
                         VirtualGPU* gpuDefQueue,      //!< GPU default queue
                         uint64_t vmDefQueue,          //!< VM handle to the virtual queue
                         uint64_t vmParentWrap,        //!< VM handle to the wrapped AQL packet location
                         GpuEvent* gpuEvent            //!< [Return] GPU event associated with the device enqueue
                         );

  Device& gpuDevice_;       //!< physical GPU device
  amd::Monitor execution_;  //!< Lock to serialise access to all device objects
  uint index_;              //!< The virtual device unique index

  PrintfDbgHSA* printfDbgHSA_;  //!< HSAIL printf implemenation

  TimeStampCache* tsCache_;            //!< TimeStamp cache
  MemoryDependency memoryDependency_;  //!< Memory dependency class

  DmaFlushMgmt dmaFlushMgmt_;  //!< DMA flush management

  std::vector<Memory*> xferWriteBuffers_;  //!< Stage write buffers
  std::vector<amd::Memory*> pinnedMems_;   //!< Pinned memory list

  typedef std::queue<CommandBatch*> CommandBatchQueue;
  CommandBatchQueue cbQueue_;      //!< Queue of command batches
  CommandBatchQueue freeCbQueue_;  //!< Queue of free command batches

  uint hwRing_;  //!< HW ring used on this virtual device

  State state_;          //!< virtual GPU current state
  GpuEvent events_[AllEngines];  //!< Last known GPU events

  uint64_t readjustTimeGPU_;   //!< Readjust time between GPU and CPU timestamps
  TimeStamp* lastTS_;          //!< Last timestamp executed on Virtual GPU
  TimeStamp* profileTs_;       //!< current profiling timestamp for command

  AmdVQueueHeader* vqHeader_;  //!< Sysmem copy for virtual queue header
  Memory* virtualQueue_;       //!< Virtual device queue
  Memory* schedParams_;        //!< The scheduler parameters
  uint schedParamIdx_;         //!< Index in the scheduler parameters buffer
  uint deviceQueueSize_;       //!< Device queue size
  uint maskGroups_;  //!< The number of mask groups processed in the scheduler by one thread

  Memory* hsaQueueMem_;               //!< Memory for the amd_queue_t object
  Pal::ICmdAllocator* cmdAllocator_;  //!< Command buffer allocator
  Queue* queues_[AllEngines];         //!< HW queues for all engines
  MemoryRange sdmaRange_;             //!< SDMA memory range for write access
};

inline void VirtualGPU::addVmMemory(const Memory* memory) {
  GpuEvent event(queues_[MainEngine]->cmdBufId());
  queues_[MainEngine]->addCmdMemRef(memory->memRef());
  memory->setBusy(*this, event);
}

inline void VirtualGPU::AddKernel(const amd::Kernel& kernel) const {
  queues_[MainEngine]->last_kernel_ = &kernel;
}

template <bool avoidBarrierSubmit>
uint VirtualGPU::Queue::submit(bool forceFlush) {
  cmdCnt_++;
  uint id = cmdBufIdCurrent_;
  bool flushCmd = ((cmdCnt_ > MaxCommands) || forceFlush) && !avoidBarrierSubmit;
  if (flushCmd) {
    if (!flush()) {
      return GpuEvent::InvalidID;
    }
  }
  return id;
}

/*@}*/} // namespace pal
