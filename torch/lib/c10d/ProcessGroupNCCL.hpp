#pragma once

#include <mutex>
#include <thread>
#include <unordered_map>

#include <c10d/NCCLUtils.hpp>
#include <c10d/ProcessGroup.hpp>
#include <c10d/Store.hpp>

#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDAEvent.h>
#include <c10/core/StreamGuard.h>

namespace c10d {

// Environment variable which controls whether or not wait() is blocking or
// non-blocking.
constexpr const char* NCCL_BLOCKING_WAIT = "NCCL_BLOCKING_WAIT";

// ProcessGroupNCCL implements NCCL bindings for c10d.
//
// All functions of the class are expected to be called in the same order
// across all processes in the process group.  This is the only way that we
// can guarantee to match up the same calls among all processes.
//
// All NCCL functions provided by this class are asynchronous functions. More
// specifically, each NCCL call is scheduled on a separate CUDA stream that is
// different from the current CUDA stream. This is for the purpose of
// achieving potentially concurrency and better performance. As a result,
// it is the callers' responsibility to make sure that the CUDA stream their
// code works on needs to wait for the NCCL operation from
// this class.
//
// This can be done by calling:
//
// either WorkNCCL::wait() or WorkNCCL::synchronize(), both achieves the same
// functionality and are synonyms.
//
// Also note that WorkNCCL::finishedGPUExecution() is a helper function only
// provided by ProcessGroupNCCL to check if the NCCL operation of WorkNCCL has
// finished execution on the GPU (not just scheduled).
//
// Example on using the NCCL process group
//
//   ProcessGroupNCCL pg(store, rank, size);
//   std::shared_ptr<WorkNCCL> work = pg.allreduce(tensors);
//
//   // At this point, NCCL kernel has already by queued successfully
//   // Now, let current stream wait for the NCCL to finish, this function is
//   // async operation as well
//
//   work->wait()
//
//   // Now continue on other work in the current stream.
class ProcessGroupNCCL : public ProcessGroup {
 public:
  class WorkNCCL : public ProcessGroup::Work,
                   public std::enable_shared_from_this<WorkNCCL> {
   public:
    // Constructor takes a list of CUDA devices
    WorkNCCL(const std::vector<at::Device>& devices);
    virtual ~WorkNCCL();

    // Checks if request has completed. In this specific case of NCCL, it checks
    // if the NCCL operation has completed on the GPU in its own NCCL stream.
    // Non-blocking operation.
    bool isCompleted() override;

    bool isSuccess() const override;

    // Same as calling synchronize() for NCCL work.
    bool wait(std::chrono::milliseconds timeout = kNoTimeout) override;

    void abort() override;

    // Let current stream wait on the completing of the NCCL work
    // Throws on exceptions. Blocking operation, which will wait for work
    // completion.
    void synchronize() override;

    // Synchronize streams by blocking each on the NCCL stream
    void synchronizeStreams();

    // Helper function that checks if the NCCL kernels have finished
    // execution on the GPUs
    bool finishedGPUExecution();

    // Get a Future object that will be marked as completed internally.
    // It actually returns a FutureNCCL object which is a sub class Future.
    c10::intrusive_ptr<c10::ivalue::Future> getFuture() override;

   protected:
    // The cached list of CUDA devices to operate on
    std::vector<at::Device> devices_;

    // The CUDA events tracking this work item on multiple CUDA devices
    std::shared_ptr<std::vector<at::cuda::CUDAEvent>> cudaEvents_;

    // The NCCL communicators used for this work item.
    std::vector<std::shared_ptr<NCCLComm>> ncclComms_;

    // Tensors used for barrier op
    std::vector<at::Tensor> barrierTensors_;

    // Clone of blockingWait_ from ProcessGroupNCCL.
    bool blockingWait_ = false;

    // Clone of opTimeout_ from ProcessGroupNCCL.
    std::chrono::milliseconds opTimeout_;

    // Time point representing when the work started.
    std::chrono::time_point<std::chrono::steady_clock> workStartTime_;

    // Wrapper method for the static checkForNCCLErrors which can be overridden
    // for tests.
    virtual std::exception_ptr checkForNCCLErrors(
        const std::vector<std::shared_ptr<NCCLComm>>& ncclComms) const;

   private:
    // Helper function for synchronize
    void synchronizeInternal(std::chrono::milliseconds timeout);
    // Checks for NCCL errors and sets an appropriate exception_ptr.
    void checkAndSetException();

    // Checks for NCCL errors and throws an appropriate exception.
    void checkAndThrowException();

    // Just checks whether GPU execution has completed, without modifying
    // exception_ptr.
    bool finishedGPUExecutionInternal() const;

    // Reference to the store so that we can write aborted communicators
    // to the store.
    std::shared_ptr<Store> store_;

    // Store a reference to NCCL collective's outputs to be used by getFuture.
    std::shared_ptr<std::vector<at::Tensor>> outputs_;

    friend class ProcessGroupNCCL;
  };

  // FutureNCCL is a subclass of ivalue's Future. The goal is to use
  // this class in getFuture API of WorkNCCL. This Future is mostly a
  // wrapper to synchronize streams appropriately and it mostly enables
  // the async programming model of CUDA while trying to adhere to the
  // Future interface. FutureNCCL does not support NCCL_BLOCKING_WAIT flag
  // or NCCL's barrier().
  //
  // If created by WorkNCCL's getFuture API, FutureNCCL has a reference to
  // WorkNCCL's cudaEvents, NCCL collective's outputs, and device index of
  // outputs' device. Its value is NCCL collective's outputs. FutureNCCL
  // only supports single-process single-device mode where the size of outputs
  // is equal to 1.
  //
  // If created by FutureNCCL's then callback, its value becomes the value of
  // callback() and its cudaEvents will record the NCCL stream that runs that
  // callback. Before invoking the callback, FutureNCCL will synchronize its
  // own cudaEvents with the stream that runs the callback. This design
  // enables synchronizing the appropriate streams and avoids stalling PyTorch's
  // default stream while running the callback. In case of multiple then
  // callbacks, the design will work like a chain such that FutureNCCL n will
  // wait on the cudaEvents from FutureNCCL n - 1.
  struct FutureNCCL : at::ivalue::Future {
   public:
    explicit FutureNCCL(
        at::IValue value,
        c10::DeviceIndex deviceIndex,
        std::shared_ptr<std::vector<at::cuda::CUDAEvent>> cudaEvents)
        : at::ivalue::Future(c10::ListType::create(c10::TensorType::get())),
          value_(std::move(value)),
          deviceIndex_(deviceIndex),
          cudaEvents_(cudaEvents) {
      TORCH_INTERNAL_ASSERT(
          cudaEvents_->size() == 1,
          "FutureNCCL only supports single-process single-device mode.");
    }

    // This constructor is used by then callback, it skips setting the value at
    // the beginning. Later, the value will be set using markCompleted with the
    // return value of callback.
    explicit FutureNCCL(
        c10::DeviceIndex deviceIndex,
        std::shared_ptr<std::vector<at::cuda::CUDAEvent>> cudaEvents)
        : at::ivalue::Future(c10::ListType::create(c10::TensorType::get())),
          deviceIndex_(deviceIndex),
          cudaEvents_(cudaEvents) {
      TORCH_INTERNAL_ASSERT(
          cudaEvents_->size() == 1,
          "FutureNCCL only supports single-process single-device mode.");
    }

    // Gets the current stream of the device and synchronizes recorded streams
    // with that. It will return after synchronizing the correct GPU streams to
    // ensure we can have async CUDA execution and it does not wait for the
    // entire operation to complete on GPU.
    void wait() override {
      if (error_) {
        throw *error_;
      }
      auto stream = at::cuda::getCurrentCUDAStream(deviceIndex_);
      (*cudaEvents_)[0].block(stream);
    }

    // If FutureNCCL was created by FutureNCCL::then, its value would be empty
    // initially. FutureNCCL::then will later use this method to set its value
    // to the return value of the callback.
    void markCompleted(at::IValue value) override {
      TORCH_INTERNAL_ASSERT(
          value_.isNone(),
          "Attempting to set value of a FutureNCCL which has a value."
          "FutureNCCL's value was internally set to NCCL collective's "
          "outputs or the return value of the callback.");
      value_ = std::move(value);
    }

    void setError(std::string err) override {
      error_ = FutureError(std::move(err));
    }

    // Just returns FutureNCCL's value after wait returns.
    at::IValue value() override {
      TORCH_INTERNAL_ASSERT(hasValue(), "FutureNCCL's value is None.")
      wait();
      return value_;
    }

    const at::IValue& constValue() override {
      TORCH_INTERNAL_ASSERT(hasValue(), "FutureNCCL's value is None.")
      wait();
      return value_;
    }

    // Adds a callback to FutureNCCL. It invokes the callback inline after
    // synchronizing FutureNCCL's own cudaEvents with the stream that runs
    // this callback. This new FutureNCCL's cudaEvents will record the
    // callback's stream and will have the result value of the callback.
    void addCallbackWithStream(
        std::function<void(void)> callback,
        const c10::cuda::CUDAStream& stream,
        std::shared_ptr<std::vector<at::cuda::CUDAEvent>> thenFutCudaEvents) {
      (*cudaEvents_)[0].block(stream);
      c10::OptionalStreamGuard streamGuard{c10::Stream(stream)};
      callback();
      (*thenFutCudaEvents)[0].record(stream);
    }

    // We use addCallbackWithStream instead of addCallback.
    void addCallback(std::function<void(void)> /* unused */) override {
      C10_THROW_ERROR(
          Error,
          "FutureNCCL uses addCallbackWithStream instead of addCallback.");
    }

    // Adds a callback to FutureNCCL, and returns another FutureNCCL to hold
    // the return value of the callback and new cudaEvents that recorded the
    // stream that runs this callback.
    c10::intrusive_ptr<Future> then(
        std::function<at::IValue(void)> callback,
        at::TypePtr type) override {
      // Get a new stream from pool that will run the callback.
      const c10::cuda::CUDAStream stream =
          at::cuda::getStreamFromPool(deviceIndex_);
      // Create a new cudaEvents object of size 1 that will record callback's
      // stream and will be used by the new FutureNCCL.
      auto thenFutCudaEvents =
          std::make_shared<std::vector<at::cuda::CUDAEvent>>(1);
      // Create a FutureNCCL without setting a value.
      auto fut =
          c10::make_intrusive<FutureNCCL>(deviceIndex_, thenFutCudaEvents);

      // Cannot move capture std::function in lambda, because it cannot deduce
      // the template type for std::function. Hence use std::bind to explicitly
      // specify types.
      addCallbackWithStream(
          std::bind(
              [&](std::function<at::IValue(void)> cb) {
                try {
                  fut->markCompleted(at::IValue(cb()));
                } catch (const std::exception& e) {
                  fut->setError(e.what());
                }
              },
              std::move(callback)),
          stream,
          thenFutCudaEvents);
      return fut;
    }

    // Checks cudaEventQuery with cudaEvents. Returns true if a FutureError was
    // recorded or the entire operation is completed on the GPU.
    bool completed() const override {
      if (error_) {
        return true;
      }
      // Checking the work's corresponding CUDA events' status
      auto ret = cudaEventQuery((*cudaEvents_)[0]);
      return ret != cudaErrorNotReady || ret == cudaSuccess;
    }

    bool hasValue() const override {
      return !value_.isNone();
    }

   private:
    at::IValue value_;
    c10::DeviceIndex deviceIndex_;
    std::shared_ptr<std::vector<at::cuda::CUDAEvent>> cudaEvents_;
    c10::optional<FutureError> error_;
  };

  // If you wish to create multiple process groups, each with a potentially
  // different rank and size, you can do so by passing a new store instance
  // to each one. If you have only a single store object, you can
  // use the `c10d::PrefixStore` to derive scoped instances.
  // This is also what the Python API in torch.distributed does.
  //
  // The process group instance keeps a reference to the store because
  // it may be used long after the constructor runs. In fact, the constructor
  // doesn't create any NCCL communicators. A single NCCL communicator can
  // only be used on a specific set of devices, and are therefore created
  // on-demand when a collective runs. If another collective is executed later,
  // against a different set of devices, the process group creates another NCCL
  // communicator. These NCCL communicators are cached and reused if possible.
  //
  ProcessGroupNCCL(
      const std::shared_ptr<Store>& store,
      int rank,
      int size,
      const std::chrono::milliseconds& opTimeout =
          std::chrono::milliseconds(kProcessGroupNCCLOpTimeoutMillis));

  // This constructor includes the deprecated `groupName` argument.
  // If you have existing code that uses the `groupName`, you can replace
  // it by specifying a `c10d::PrefixStore(groupName, store)` for store.
  C10_DEPRECATED ProcessGroupNCCL(
      const std::shared_ptr<Store>& store,
      int rank,
      int size,
      const std::string& groupName,
      const std::chrono::milliseconds& opTimeout =
          std::chrono::milliseconds(kProcessGroupNCCLOpTimeoutMillis))
      : ProcessGroupNCCL(store, rank, size, opTimeout) {}

  virtual ~ProcessGroupNCCL();

  std::shared_ptr<ProcessGroup::Work> broadcast(
      std::vector<at::Tensor>& tensors,
      const BroadcastOptions& opts = BroadcastOptions()) override;

  std::shared_ptr<ProcessGroup::Work> allreduce(
      std::vector<at::Tensor>& tensors,
      const AllreduceOptions& opts = AllreduceOptions()) override;

  std::shared_ptr<ProcessGroup::Work> allreduce_coalesced(
      std::vector<at::Tensor>& tensors,
      const AllreduceCoalescedOptions& opts =
          AllreduceCoalescedOptions()) override;

  std::shared_ptr<ProcessGroup::Work> reduce(
      std::vector<at::Tensor>& tensors,
      const ReduceOptions& opts = ReduceOptions()) override;

  std::shared_ptr<ProcessGroup::Work> allgather(
      std::vector<std::vector<at::Tensor>>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const AllgatherOptions& opts = AllgatherOptions()) override;

  std::shared_ptr<ProcessGroup::Work> allgather_base(
      at::Tensor& outputbuffer,
      at::Tensor& inputbuffer,
      const AllgatherOptions& opts = AllgatherOptions()) override;

  std::shared_ptr<ProcessGroup::Work> allgather_coalesced(
      std::vector<std::vector<at::Tensor>>& outputTensorLists,
      std::vector<at::Tensor>& inputTensors,
      const AllgatherOptions& opts = AllgatherOptions()) override;

  std::shared_ptr<ProcessGroup::Work> reduce_scatter(
      std::vector<at::Tensor>& outputTensors,
      std::vector<std::vector<at::Tensor>>& inputTensors,
      const ReduceScatterOptions& opts = ReduceScatterOptions()) override;

  std::shared_ptr<ProcessGroup::Work> barrier(
      const BarrierOptions& opts = BarrierOptions()) override;

  std::shared_ptr<ProcessGroup::Work> alltoall_base(
      at::Tensor& outputTensor,
      at::Tensor& inputTensor,
      std::vector<int64_t>& outputSplitSizes,
      std::vector<int64_t>& inputSplitSizes,
      const AllToAllOptions& opts = AllToAllOptions()) override;

  std::shared_ptr<ProcessGroup::Work> alltoall(
      std::vector<at::Tensor>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const AllToAllOptions& opts = AllToAllOptions()) override;

  // Unsupported Ops
  std::shared_ptr<ProcessGroup::Work> gather(
      std::vector<std::vector<at::Tensor>>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const GatherOptions& opts = GatherOptions()) override;

  std::shared_ptr<ProcessGroup::Work> scatter(
      std::vector<at::Tensor>& outputTensors,
      std::vector<std::vector<at::Tensor>>& inputTensors,
      const ScatterOptions& opts = ScatterOptions()) override;

  std::shared_ptr<ProcessGroup::Work> send(
      std::vector<at::Tensor>& tensors,
      int dstRank,
      int tag) override;

  std::shared_ptr<ProcessGroup::Work> recv(
      std::vector<at::Tensor>& tensors,
      int srcRank,
      int tag) override;

  std::shared_ptr<ProcessGroup::Work> recvAnysource(
      std::vector<at::Tensor>& tensors,
      int tag) override;

  static const int64_t kProcessGroupNCCLOpTimeoutMillis;

 protected:
  // Helper that broadcasts nccl unique ID to all ranks through the store
  void broadcastUniqueNCCLID(ncclUniqueId* ncclID);

  // Helper that either looks up the cached NCCL communicators or creates
  // a new set of NCCL communicators as a cache entry
  std::vector<std::shared_ptr<NCCLComm>>& getNCCLComm(
      const std::string& devicesKey,
      const std::vector<at::Device>& devices);

  // Wrapper method which can be overridden for tests.
  virtual std::exception_ptr checkForNCCLErrors(
      const std::vector<std::shared_ptr<NCCLComm>>& ncclComms);

  virtual std::shared_ptr<ProcessGroupNCCL::WorkNCCL> initWork(
      std::vector<at::Device> devices);

 private:
  // Helper that encapsulates work shared across all collective communication
  // primitives.  The callbacks have the following signatures:
  //
  //    ncclResult_t fn(at::Tensor& input, at::Tensor& output,
  //                    ncclComm_t, at::cuda::CUDAStream&);
  //    void {pre,post}(std::vector<at::cuda::CUDAStream&>);
  template <typename Fn>
  std::shared_ptr<ProcessGroup::Work> collective(
      std::vector<at::Tensor>& input,
      std::vector<at::Tensor>& output,
      Fn fn);
  template <typename Fn, typename PreProcess, typename PostProcess>
  std::shared_ptr<ProcessGroup::Work> collective(
      std::vector<at::Tensor>& input,
      std::vector<at::Tensor>& output,
      Fn fn,
      PreProcess pre,
      PostProcess post);

  // Checks for NCCL errors on each of the communicators and returns an
  // appropriate exception_ptr (nullptr if no errors).
  static std::exception_ptr checkForNCCLErrorsInternal(
      const std::vector<std::shared_ptr<NCCLComm>>& ncclComms);

  // Function that runs as part of a separate thread and checks for errors on
  // NCCL communicators. We need a separate thread to check for NCCL errors
  // since we can't rely on the user calling certain methods like wait(),
  // isCompleted() etc. to detect and remediate errors. In addition to this, we
  // need a mechanism to safely abort and remove NCCL communicators from our
  // cache. This can be done cleanly by having a thread for the ProcessGroupNCCL
  // class. Attempting to modify the communicator cache from the WorkNCCL class
  // might run into issues with object lifetime since the ProcessGroupNCCL
  // object might get destroyed before the WorkNCCL object.
  void ncclCommWatchdog();

  void ncclCommWatchdogInternal();

  // Reads the NCCL_BLOCKING_WAIT environment variable and sets blockingWait_
  // accordingly.
  void parseNcclBlockingWait();

 protected:
  static const int64_t kWatchdogThreadSleepMillis;

  // The store is used to broadcast the NCCL unique ID of rank 0.
  std::shared_ptr<Store> store_;

  // The number of NCCL communicators that have been created during
  // the lifetime of this process group. This sequence number is
  // used to scope keys used in the store.
  uint64_t ncclCommCounter_{0};

  // The NCCL communicator that the process group has cached.
  // The key is a list of GPU devices that an operation is operating on
  // The GPU devices are stored in a device sequence and the cache NCCL
  // communicator is associated with this GPU device sequence
  //
  // e.g. If the process group op only uses device 0, then the value of
  // the used device string stored (value of the hashmap) would be "0".
  //
  //      If the process group op uses device 0 - 7 and the each tensor of the
  //      input tensor list is on device, 0, 1, 2, 3, 4, 5, 6, 7 separately,
  //      then the value of the used device string (key) stored would be
  //      "0,1,2,3,4,5,6,7"
  //
  //      If the process group op uses device 0 - 7 and the each tensor of the
  //      input tensor list is on device, 0, 4, 5, 6, 7, 1, 2, 3 separately,
  //      then the value of the used device string stored would be
  //      "0,4,5,6,7,1,2,3"
  //
  //      Note that the order of the device for the tensor list matters.
  std::unordered_map<std::string, std::vector<std::shared_ptr<NCCLComm>>>
      devNCCLCommMap_;

  // Map from ncclUniqueId to appropriate communicator.
  std::unordered_map<std::string, std::vector<std::shared_ptr<NCCLComm>>>
      ncclIdToCommMap_;

  // Mutex to guard maps like devNCCLCommMap_ and ncclIdToCommMap_.
  std::mutex mutex_;

  // Watchdog thread which looks for errors on the cached NCCL communicators.
  std::thread ncclCommWatchdogThread_;

  // Whether or not we should terminate the watchdog thread.
  std::atomic<bool> terminateWatchdog_;

  // Condition variable to control how long the watchdog thread waits.
  std::condition_variable watchdogCV_;

  // Mutex for watchdog.
  std::mutex watchdogCVMutex_;

  // The CUDA steams used by NCCL kernels
  std::unordered_map<std::string, std::vector<at::cuda::CUDAStream>>
      ncclStreams_;

  // The CUDA events used to sync NCCL streams
  std::unordered_map<std::string, std::vector<at::cuda::CUDAEvent>> ncclEvents_;

  // Device Indexes used for all collectives in this group
  std::set<int> usedDeviceIdxs_;

  // map from the key: "group name + pg counter (ID)" to the
  // unique NCCL ID count. This needs to be group and pg specific
  //
  // For each process group, we need a uniform unique NCCL ID counter to ensure
  // that NCCL operation in this process group can be completed successfully.
  // Since each process group ID belongs to a group name, the key to this map
  // is a combination of group name and ProcessGroupNCCL ID.
  static std::unordered_map<std::string, ssize_t> pgUniqueNCCLIDCnt_;

  // map from group name to the pg counter (ID) within that group
  //
  // For each group with the "group name" (which is the key), we need to
  // keep track of a unique process group ID when creating a new
  // ProcessGroupNCCL for this "group name". Therefore, the value of this
  // map keeps the unique ProcessGroupNCCL's ID for a specific group with
  // the "group name". The reason we need a per-group process group ID counter
  // is that different group can have different ranks and we need ensure that
  // each group has its own uniform process group ID for all its ranks.
  static std::unordered_map<std::string, ssize_t> processGroupCounterMap_;

  // Whether or not wait() and synchronize() are blocking operations that wait
  // for the operation to complete.
  bool blockingWait_ = false;

  // Timeout for operations. This is only used when blockingWait_ is enabled.
  std::chrono::milliseconds opTimeout_;

  // Set of communicators that this process group has aborted and their
  // ncclUniqueId has been written to the store. We don't need a lock
  // for this map since only the watchdog thread accesses this set. The
  // set contains the string representation of ncclUniqueId.
  std::unordered_set<std::string> abortedComms_;
};

} // namespace c10d
