#include "reduction_ops.h"
#include "core/providers/common.h"
#include "core/providers/cuda/cudnn_common.h"
#include "core/providers/cuda/math/unary_elementwise_ops_impl.h"
using namespace Lotus::Common;
namespace Lotus {
namespace Cuda {

#define REGISTER_KERNEL_TYPED(name, T)                                          \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                                \
      name,                                                                     \
      kOnnxDomain,                                                              \
      1,                                                                        \
      T,                                                                        \
      kCudaExecutionProvider,                                                   \
      KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      name<T>);

class CudnnReduceDescriptor final {
 public:
  CudnnReduceDescriptor() : desc_(nullptr) {
  }

  ~CudnnReduceDescriptor() {
    if (desc_ != nullptr) {
      cudnnDestroyReduceTensorDescriptor(desc_);
      desc_ = nullptr;
    }
  }

  Status Set(cudnnReduceTensorOp_t op, cudnnDataType_t type, cudnnReduceTensorIndices_t indices) {
    if (!desc_)
      CUDNN_RETURN_IF_ERROR(cudnnCreateReduceTensorDescriptor(&desc_));

    CUDNN_RETURN_IF_ERROR(cudnnSetReduceTensorDescriptor(
        desc_,
        op,
        type,
        CUDNN_PROPAGATE_NAN,
        indices,
        CUDNN_32BIT_INDICES));  // currently only the 32-bit (unsigned int) type is supported.
    return Status::OK();
  }

  operator cudnnReduceTensorDescriptor_t() const { return desc_; }

 private:
  cudnnReduceTensorDescriptor_t desc_;
};

template <typename T, cudnnReduceTensorIndices_t ReduceTensorIndices>
Status ReduceKernel::ComputeImpl(OpKernelContext* ctx, cudnnReduceTensorOp_t cudnnReduceOp) const {
  typedef typename ToCudaType<T>::MappedType CudaT;
  const Tensor& X = *ctx->Input<Tensor>(0);
  const TensorShape input_shape{X.Shape()};
  const auto rank = input_shape.NumDimensions();

  if (rank > 8) {
    return LOTUS_MAKE_STATUS(LOTUS, FAIL, "cuDNN only supports up to 8-D tensors in reduction");
  }

  const auto& input_dims = input_shape.GetDims();
  std::vector<int64_t> output_dims;
  std::vector<bool> reduced(rank, false);
  std::vector<int64_t> squeezed_output_dims;
  if (axes_.size() > 0) {
    output_dims = input_dims;
    for (auto reduced_axis : axes_) {
      const int64_t axis = HandleNegativeAxis(reduced_axis, rank);
      output_dims[axis] = 1;
      reduced[axis] = true;
    }
  } else {
    output_dims = std::vector<int64_t>(rank, 1);
  }

  if (keepdims_) {
    squeezed_output_dims = output_dims;
  } else {
    for (size_t i = 0; i < rank; ++i) {
      if (!reduced[i])
        squeezed_output_dims.push_back(input_dims[i]);
    }
  }

  Tensor* Y = ctx->Output(0, TensorShape(squeezed_output_dims));

  // CUDNN requires at least 3D input, so pad 1s if needed
  std::vector<int64_t> input_dims_cudnn = input_dims;
  std::vector<int64_t> output_dims_cudnn = output_dims;
  if (rank < 3) {
    std::vector<int64_t> pads(3 - rank, 1);
    input_dims_cudnn.insert(input_dims_cudnn.end(), pads.begin(), pads.end());
    output_dims_cudnn.insert(output_dims_cudnn.end(), pads.begin(), pads.end());
  }

  CudnnReduceDescriptor reduce_desc;
  LOTUS_RETURN_IF_ERROR(reduce_desc.Set(cudnnReduceOp, CudnnTensor::GetDataType<CudaT>(), ReduceTensorIndices));
  const auto alpha = Consts<CudaT>::One;
  const auto beta = Consts<CudaT>::Zero;
  CudnnTensor input_tensor;
  CudnnTensor output_tensor;
  LOTUS_RETURN_IF_ERROR(input_tensor.Set(input_dims_cudnn, CudnnTensor::GetDataType<CudaT>()));
  LOTUS_RETURN_IF_ERROR(output_tensor.Set(output_dims_cudnn, CudnnTensor::GetDataType<CudaT>()));
  size_t indices_bytes = 0;
  CUDNN_RETURN_IF_ERROR(cudnnGetReductionIndicesSize(CudnnHandle(), reduce_desc, input_tensor, output_tensor, &indices_bytes));
  size_t workspace_bytes = 0;
  CUDNN_RETURN_IF_ERROR(cudnnGetReductionWorkspaceSize(CudnnHandle(), reduce_desc, input_tensor, output_tensor, &workspace_bytes));
  BookScratchBuffer<void>(indices_bytes);
  BookScratchBuffer<void>(workspace_bytes);

  if (ReduceTensorIndices == CUDNN_REDUCE_TENSOR_NO_INDICES) {
    PrepareScratchBuffer();
    void* indices_cuda = GetScratchBuffer<void>(indices_bytes);
    void* workspace_cuda = GetScratchBuffer<void>(workspace_bytes);
    CUDNN_RETURN_IF_ERROR(cudnnReduceTensor(
        CudnnHandle(), reduce_desc, indices_cuda, indices_bytes, workspace_cuda, workspace_bytes,
        &alpha, input_tensor, reinterpret_cast<const CudaT*>(X.Data<T>()),
        &beta, output_tensor, reinterpret_cast<CudaT*>(Y->MutableData<T>())));
  } else {
    // need to allocate a separate buffer for ArgMin/ArgMax comparsion output
    auto output_count = Y->Shape().Size();
    BookScratchBuffer<CudaT>(output_count);
    PrepareScratchBuffer();
    void* indices_cuda = GetScratchBuffer<void>(indices_bytes);
    void* workspace_cuda = GetScratchBuffer<void>(workspace_bytes);
    CudaT* temp_output = GetScratchBuffer<CudaT>(output_count);
    CUDNN_RETURN_IF_ERROR(cudnnReduceTensor(
        CudnnHandle(), reduce_desc, indices_cuda, indices_bytes, workspace_cuda, workspace_bytes,
        &alpha, input_tensor, reinterpret_cast<const CudaT*>(X.Data<T>()),
        &beta, output_tensor, temp_output));
    // CUDA reduction index is uint32_t for now, cast it to int64_t according to ONNX spec
    Impl_Cast<uint32_t, int64_t>(reinterpret_cast<uint32_t*>(indices_cuda), Y->MutableData<int64_t>(), output_count);
  }

  return Status::OK();
}

#define SPECIALIZED_COMPUTE_IMPL(T)                                                                                                               \
  template Status ReduceKernel::ComputeImpl<T, CUDNN_REDUCE_TENSOR_NO_INDICES>(OpKernelContext * ctx, cudnnReduceTensorOp_t cudnnReduceOp) const; \
  template Status ReduceKernel::ComputeImpl<T, CUDNN_REDUCE_TENSOR_FLATTENED_INDICES>(OpKernelContext * ctx, cudnnReduceTensorOp_t cudnnReduceOp) const;

SPECIALIZED_COMPUTE_IMPL(MLFloat16)
SPECIALIZED_COMPUTE_IMPL(float)
SPECIALIZED_COMPUTE_IMPL(double)

#define REGISTER_KERNEL_HFD(name)        \
  REGISTER_KERNEL_TYPED(name, MLFloat16) \
  REGISTER_KERNEL_TYPED(name, float)     \
  REGISTER_KERNEL_TYPED(name, double)

REGISTER_KERNEL_HFD(ArgMax)
REGISTER_KERNEL_HFD(ArgMin)
REGISTER_KERNEL_HFD(ReduceL1)
REGISTER_KERNEL_HFD(ReduceL2)
REGISTER_KERNEL_HFD(ReduceMax)
REGISTER_KERNEL_HFD(ReduceMean)
REGISTER_KERNEL_HFD(ReduceMin)
REGISTER_KERNEL_HFD(ReduceProd)
REGISTER_KERNEL_HFD(ReduceSum)

}  // namespace Cuda
}  // namespace Lotus
